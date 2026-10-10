// SPDX-License-Identifier: MIT
// Real Mesa NIR/ACO single read-only fragment buffer, separate from existing profiles.
#include "hackynavi_gfx11_fragment_buffer.h"
#include "aco_interface.h"
#include "ac_gpu_info.h"
#include "ac_nir.h"
#include "ac_shader_args.h"
#include "ac_shader_util.h"
#include "ac_descriptors.h"
#include "amdgfxregs.h"
#include "nir.h"
#include "nir_builder.h"
#include "spirv/nir_spirv.h"
#include "spirv/spirv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct fragment_output {
   const char *directory;
   unsigned load_count;
   bool success;
};

static bool reject(const char *message)
{
   fprintf(stderr, "fragment-buffer profile: %s\n", message);
   return false;
}

static void optimize(nir_shader *nir)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
   } while (progress);
}

struct lowering {
   struct ac_shader_args args;
   struct ac_arg descriptor;
   bool failed;
   unsigned resources;
};

static bool output_contract(nir_shader *nir)
{
   unsigned outputs = 0, buffers = 0;
   nir_foreach_variable_in_shader(var, nir) {
      if (var->data.mode == nir_var_shader_out) {
         if (++outputs != 1 || var->data.location != FRAG_RESULT_DATA0 ||
             var->data.location_frac || var->data.index ||
             !glsl_type_is_vector(var->type) || glsl_get_vector_elements(var->type) != 4 ||
             glsl_get_base_type(var->type) != GLSL_TYPE_FLOAT)
            return reject("requires exactly one float32 vec4 output at location0/index0");
         var->data.driver_location = 0;
      } else if (var->data.mode == nir_var_mem_ssbo) {
         if (++buffers != 1 || var->data.descriptor_set || var->data.binding ||
             glsl_type_is_array(var->type) || !(var->data.access & ACCESS_NON_WRITEABLE) ||
             (var->data.access & (ACCESS_VOLATILE | ACCESS_COHERENT)))
            return reject("requires one nonvolatile read-only singleton buffer at set0/binding0");
      } else if (var->data.mode != nir_var_shader_temp) {
         return reject("varyings, system inputs and other resource modes are unsupported");
      }
   }
   return (outputs == 1 && buffers == 1) || reject("missing output or buffer");
}

// Resolve only the explicit singleton index. The descriptor itself is NOT read
// from user memory: the later native owner must place its four words in s0..3.
static bool lower_indices(nir_builder *b, nir_intrinsic_instr *in, void *opaque)
{
   struct lowering *state = opaque;
   b->cursor = nir_before_instr(&in->instr);
   nir_def *value = NULL;
   switch (in->intrinsic) {
   case nir_intrinsic_vulkan_resource_index:
      if (nir_intrinsic_desc_set(in) || nir_intrinsic_binding(in) ||
          nir_intrinsic_desc_type(in) != nir_descriptor_type_storage_buffer ||
          !nir_src_is_const(in->src[0]) || nir_src_as_uint(in->src[0])) {
         state->failed = true;
         return false;
      }
      state->resources++;
      value = nir_imm_zero(b, in->def.num_components, in->def.bit_size);
      break;
   case nir_intrinsic_load_vulkan_descriptor:
      if (nir_intrinsic_desc_type(in) != nir_descriptor_type_storage_buffer) {
         state->failed = true;
         return false;
      }
      value = in->src[0].ssa;
      break;
   case nir_intrinsic_vulkan_resource_reindex:
      state->failed = true;
      return false;
   default:
      return false;
   }
   nir_def_replace(&in->def, value);
   return true;
}

// Bounded bit-preserving provenance, not numerical evaluation or replacement.
// Every output component must originate from that same component in buffer0.
static bool component_offset(nir_def *def, unsigned component, unsigned depth, unsigned *offset)
{
   if (!def || def->bit_size != 32 || component >= def->num_components || depth > 32)
      return false;
   nir_instr *instr = nir_def_instr(def);
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
      if (in->intrinsic != nir_intrinsic_load_ssbo || !nir_src_is_const(in->src[1]))
         return false;
      *offset = nir_src_as_uint(in->src[1]) + component * 4;
      return true;
   }
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   if (alu->op == nir_op_mov)
      return component_offset(alu->src[0].src.ssa, alu->src[0].swizzle[component], depth + 1, offset);
   if (alu->op == nir_op_vec2 || alu->op == nir_op_vec3 || alu->op == nir_op_vec4)
      return component_offset(alu->src[component].src.ssa, alu->src[component].swizzle[0], depth + 1, offset);
   return false;
}

static bool buffer_contract(nir_shader *nir, unsigned *load_count)
{
   unsigned blocks = 0, stores = 0, coverage = 0, loads = 0;
   nir_foreach_block(block, nir_shader_get_entrypoint(nir)) {
      if (++blocks != 1)
         return reject("requires one unconditional buffer-color block");
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_load_const)
            continue;
         if (instr->type == nir_instr_type_alu) {
            nir_op op = nir_instr_as_alu(instr)->op;
            if (op == nir_op_mov || op == nir_op_vec2 || op == nir_op_vec3 || op == nir_op_vec4)
               continue;
            return reject("only bit-preserving moves/assembly of loaded color are supported");
         }
         if (instr->type != nir_instr_type_intrinsic)
            return reject("unexpected buffer-color instruction");
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         if (in->intrinsic == nir_intrinsic_load_ssbo) {
            if (++loads > 4 || in->def.bit_size != 32 ||
                in->def.num_components != 1 || !nir_src_is_const(in->src[0]) ||
                !nir_src_is_const(in->src[1]) ||
                (nir_intrinsic_access(in) & (ACCESS_NON_UNIFORM | ACCESS_VOLATILE | ACCESS_COHERENT)) ||
                !(nir_intrinsic_access(in) & ACCESS_NON_WRITEABLE))
               return reject("requires read-only aligned constant-offset buffer loads");
            for (unsigned i = 0; i < in->src[0].ssa->num_components; ++i)
               if (nir_src_comp_as_uint(in->src[0], i))
                  return reject("buffer descriptor index is not zero");
            unsigned offset = nir_src_as_uint(in->src[1]);
            if (offset > 12 || (offset & 3) || in->def.num_components > (16 - offset) / 4 ||
                nir_intrinsic_align_mul(in) < 4 || (nir_intrinsic_align_offset(in) & 3))
               return reject("buffer access exceeds aligned 16-byte color");
            unsigned mask = ((1u << in->def.num_components) - 1) << (offset / 4);
            if (coverage & mask)
               return reject("duplicate color component access");
            coverage |= mask;
         } else if (in->intrinsic == nir_intrinsic_store_output) {
            if (++stores != 1 || nir_intrinsic_io_semantics(in).location != FRAG_RESULT_DATA0 ||
                nir_intrinsic_component(in) || nir_intrinsic_write_mask(in) != 15 ||
                in->num_components != 4 || in->src[0].ssa->bit_size != 32 ||
                !nir_src_is_const(in->src[1]) || nir_src_as_uint(in->src[1]))
               return reject("requires one full RGBA output");
            for (unsigned i = 0; i < 4; ++i) {
               unsigned offset = UINT32_MAX;
               if (!component_offset(in->src[0].ssa, i, 0, &offset) || offset != i * 4)
                  return reject("color does not preserve buffer0 components in RGBA order");
            }
         } else {
            return reject("unexpected intrinsic in buffer-color shader");
         }
      }
   }
   *load_count = loads;
   return (stores == 1 && loads == 4 && coverage == 15) || reject("missing four scalar color loads/full store");
}

static bool bind_descriptor(nir_builder *b, nir_intrinsic_instr *in, void *opaque)
{
   if (in->intrinsic != nir_intrinsic_load_ssbo)
      return false;
   struct lowering *state = opaque;
   b->cursor = nir_before_instr(&in->instr);
   nir_src_rewrite(&in->src[0], ac_nir_load_arg(b, &state->args, state->descriptor));
   return true;
}

static bool export_contract(nir_shader *nir)
{
   unsigned exports = 0, loads = 0, descriptors = 0;
   nir_foreach_block(block, nir_shader_get_entrypoint(nir)) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_load_const || instr->type == nir_instr_type_alu)
            continue;
         if (instr->type != nir_instr_type_intrinsic)
            return reject("unexpected post-lowering instruction");
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         if (in->intrinsic == nir_intrinsic_load_ssbo) { ++loads; continue; }
         if (in->intrinsic == nir_intrinsic_load_scalar_arg_amd) {
            if (nir_intrinsic_base(in) || in->def.num_components != 4 || in->def.bit_size != 32)
               return reject("descriptor argument differs from four s0..3 words");
            ++descriptors;
            continue;
         }
         if (in->intrinsic != nir_intrinsic_export_amd || ++exports != 1 ||
             nir_intrinsic_target(in) != V_008DFC_SQ_EXP_MRT ||
             nir_intrinsic_enabled_channels(in) != 15 ||
             nir_intrinsic_flags(in) != (AC_EXP_FLAG_DONE | AC_EXP_FLAG_VALID_MASK))
            return reject("lowered export differs from single uncompressed MRT0 RGBA contract");
      }
   }
   return (exports == 1 && loads == 4 && descriptors == 1) || reject("missing real descriptor/load/export");
}

static FILE *open_output(const char *directory, const char *name, const char *mode)
{
   char path[4096];
   int count = snprintf(path, sizeof(path), "%s/%s", directory, name);
   return count < 0 || (size_t)count >= sizeof(path) ? NULL : fopen(path, mode);
}

static bool write_bytes(const char *directory, const char *name, const void *data, size_t bytes)
{
   FILE *file = open_output(directory, name, "wb");
   if (!file)
      return false;
   bool ok = fwrite(data, 1, bytes, file) == bytes;
   return fclose(file) == 0 && ok;
}

static void emit_fragment(void **opaque, const aco_callback_params *p)
{
   struct fragment_output *out = *opaque;
   const struct ac_shader_config *c = &p->config;
   const uint32_t inputs = S_0286CC_LINE_STIPPLE_TEX_ENA(1);
   if (!p->code || !p->code_dw || p->code_dw > 262144 || !p->exec_size ||
       p->exec_size > p->code_dw * 4 || p->exec_size % 4 || p->wave_size != 32 ||
       p->num_symbols || p->constants_size || c->scratch_bytes_per_wave || c->lds_size ||
       c->spilled_sgprs || c->spilled_vgprs || c->num_shared_vgprs || c->wgp_mode ||
       c->spi_ps_input_ena != inputs || c->spi_ps_input_addr != inputs ||
       !c->num_vgprs || c->num_vgprs > 24 || c->num_sgprs != 108 ||
       c->float_mode != 192 || c->mem_ordered) {
      reject("ACO product exceeds the single-buffer/no-scratch PS contract");
      return;
   }
   // radv_postprocess_config: actual PS allocation, float mode and MEM_ORDERED;
   // four descriptor user SGPRs; no scratch/shared VGPR/trap/collision state.
   unsigned vgprs = MAX2(c->num_vgprs, ac_get_fs_input_vgpr_cnt(c));
   uint32_t rsrc1 = S_00B028_VGPRS((vgprs - 1) / 8) | S_00B028_DX10_CLAMP(1) |
      S_00B028_FLOAT_MODE(c->float_mode) | S_00B028_MEM_ORDERED(c->mem_ordered);
   if (!write_bytes(out->directory, "shader.bin", p->code, p->code_dw * 4) ||
       !p->ir_str || !p->ir_size || !write_bytes(out->directory, "aco.txt", p->ir_str, p->ir_size))
      return;
   // This build has no LLVM AMDGPU disassembler. ACO labels its print-program
   // fallback explicitly; retain it as compiler diagnostics, never ISA proof.
   if (p->disasm_str && p->disasm_size &&
       !write_bytes(out->directory, "aco-diagnostics.txt", p->disasm_str, p->disasm_size))
      return;
   FILE *meta = open_output(out->directory, "shader.json", "w");
   if (!meta)
      return;
   int count = fprintf(meta,
      "{\n  \"abi_version\":1, \"profile\":\"gfx1100-buffer-rgba32-fragment-v1\",\n"
      "  \"compiler\":\"Mesa NIR/ACO\", \"mesa_commit\":\"35b085c4c067f198ce0b6437d94678e47b9c645f\",\n"
      "  \"target\":\"gfx1100\", \"stage\":\"fragment\", \"hardware_stage\":\"pixel_shader\",\n"
      "  \"code_bytes\":%u, \"executable_bytes\":%u, \"code_alignment\":256, \"wave_size\":32,\n"
      "  \"num_sgprs\":%u, \"num_vgprs\":%u, \"input_vgprs\":%u, \"float_mode\":%u,\n"
      "  \"mem_ordered\":%s, \"wgp_mode\":false, \"spi_shader_pgm_rsrc1_ps\":%u, \"spi_shader_pgm_rsrc2_ps\":%u,\n"
      "  \"spi_ps_input_ena\":%u, \"spi_ps_input_addr\":%u, \"spi_shader_col_format\":%u, \"cb_shader_mask\":15,\n"
      "  \"spi_ps_in_control\":%u, \"spi_shader_z_format\":0,\n"
      "  \"resource_count\":1, \"varying_count\":0, \"user_sgprs\":4, \"descriptor_sgpr_base\":0,\n"
      "  \"buffer_set\":0, \"buffer_binding\":0, \"buffer_read_offset\":0, \"buffer_read_bytes\":16, \"nir_load_count\":%u,\n"
      "  \"scratch_bytes_per_wave\":0, \"lds_bytes\":0, \"constants_bytes\":0, \"relocations\":[],\n"
      "  \"gpu_executed\":false, \"draw_state_complete\":false\n}\n",
      p->code_dw * 4, p->exec_size, c->num_sgprs, vgprs, ac_get_fs_input_vgpr_cnt(c), c->float_mode,
      c->mem_ordered ? "true" : "false", rsrc1, S_00B02C_USER_SGPR(4), inputs, inputs, V_028714_SPI_SHADER_32_ABGR,
      S_0286D8_PS_W32_EN(1),
      out->load_count);
   out->success = fclose(meta) == 0 && count > 0;
}

int hn_gfx11_fragment_buffer_compile_validated_file(const char *path, const char *entry, const char *directory)
{
   if (!path || !entry || !directory || !*entry || strlen(entry) > 255)
      return 2;
   FILE *file = fopen(path, "rb");
   if (!file)
      return 2;
   if (fseek(file, 0, SEEK_END)) { fclose(file); return 2; }
   long bytes = ftell(file);
   if (bytes < 20 || bytes > 64 * 1024 * 1024 || bytes % 4) { fclose(file); return 2; }
   rewind(file);
   uint32_t *words = malloc((size_t)bytes);
   bool read_ok = words && fread(words, 1, (size_t)bytes, file) == (size_t)bytes;
   fclose(file);
   if (!read_ok) { free(words); return 2; }
   bool valid = words[0] == SpvMagicNumber;
   for (unsigned at = 5; valid && at < (unsigned)bytes / 4;) {
      unsigned count = words[at] >> 16, op = words[at] & 0xffff;
      if (!count || count > (unsigned)bytes / 4 - at) { valid = false; break; }
      if ((op == SpvOpCapability && (count != 2 || words[at+1] != SpvCapabilityShader)) ||
          (op == SpvOpExecutionMode && (count != 3 || words[at+2] != SpvExecutionModeOriginUpperLeft)) ||
          op == SpvOpExecutionModeId || op == SpvOpSpecConstant || op == SpvOpSpecConstantComposite ||
          op == SpvOpSpecConstantOp || op == SpvOpSpecConstantTrue || op == SpvOpSpecConstantFalse) {
         reject("unsupported capability, execution mode or specialization"); valid = false; break;
      }
      at += count;
   }
   if (!valid) { free(words); return 2; }
   struct radeon_info target = {0}; target.gfx_level = GFX11; target.family = CHIP_NAVI31;
   ac_fill_compiler_info(&target, NULL, false);
   struct nir_shader_compiler_options options = {0};
   ac_nir_set_options(&target.compiler_info, false, &options);
   const struct spirv_to_nir_options spv = {.environment = NIR_SPIRV_VULKAN,
      .ssbo_addr_format = nir_address_format_vec2_index_32bit_offset};
   int result = 2;
   glsl_type_singleton_init_or_ref();
   nir_shader *nir = spirv_to_nir(words, (size_t)bytes / 4, NULL, MESA_SHADER_FRAGMENT, entry, &spv, &options);
   free(words);
   if (!nir) {
      reject("fragment entrypoint translation failed");
      goto cleanup;
   }
   if (!output_contract(nir))
      goto cleanup;
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   nir_remove_non_cmat_call_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_system_values);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_out, glsl_count_attribute_slots, (nir_lower_io_options)0);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo, nir_address_format_vec2_index_32bit_offset);
   optimize(nir);
   struct lowering state = {0};
   ac_add_arg(&state.args, AC_ARG_SGPR, 4, AC_ARG_VALUE, &state.descriptor);
   ac_add_arg(&state.args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &state.args.line_stipple_tex_ena);
   nir_shader_intrinsics_pass(nir, lower_indices, nir_metadata_control_flow, &state);
   if (state.failed || !state.resources) {
      reject("unsupported descriptor index/type");
      goto cleanup;
   }
   optimize(nir);
   struct fragment_output out = {.directory = directory};
   if (!buffer_contract(nir, &out.load_count))
      goto cleanup;
   nir_shader_intrinsics_pass(nir, bind_descriptor, nir_metadata_control_flow, &state);
   ac_nir_lower_ps_late_options ps = {.gfx_level = GFX11, .use_aco = true,
      .spi_shader_col_format = V_028714_SPI_SHADER_32_ABGR};
   NIR_PASS(_, nir, ac_nir_lower_ps_late, &ps);
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS(_, nir, nir_lower_phis_to_scalar, ac_nir_lower_phis_to_scalar_cb, NULL);
   optimize(nir);
   NIR_PASS(_, nir, nir_lower_load_const_to_scalar);
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_all, NULL);
   NIR_PASS(_, nir, nir_opt_dce);
   if (!export_contract(nir))
      goto cleanup;
   nir->info.max_subgroup_size = 32; nir->info.min_subgroup_size = 32;
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   nir_validate_shader(nir, "HackyNavi buffer fragment before ACO");
   /* RADV radv_shader_info.c:radv_nir_shader_info_pass: FS has no workgroups and uses
    * wave_size for ACO's scheduling/occupancy metadata. */
   struct aco_shader_info info = {.hw_stage = AC_HW_PIXEL_SHADER, .wave_size = 32, .workgroup_size = 32,
      .ps = {.spi_ps_input_ena = S_0286CC_LINE_STIPPLE_TEX_ENA(1),
             .spi_ps_input_addr = S_0286CC_LINE_STIPPLE_TEX_ENA(1)}};
   const struct aco_compiler_options aco = {.compiler_info = &target.compiler_info,
      .family = CHIP_NAVI31, .gfx_level = GFX11, .record_ir = true, .record_asm = true};
   void *opaque = &out;
   aco_compile_shader(&aco, &info, 1, &nir, &state.args, emit_fragment, &opaque);
   result = out.success ? 0 : 2;
cleanup:
   if (nir) ralloc_free(nir);
   glsl_type_singleton_decref();
   return result;
}

#ifndef HN_GFX11_FRAGMENT_BUFFER_SHARED
int main(int argc, char **argv)
{
   if (argc == 3 && !strcmp(argv[1], "--descriptor")) {
      char *end = NULL;
      errno = 0;
      unsigned long long address = strtoull(argv[2], &end, 0);
      uint32_t descriptor[4];
      if (errno || !*argv[2] || *end || argv[2][0] == '-' ||
          hn_gfx11_fragment_buffer_descriptor(address, descriptor))
         return 2;
      printf("[%u,%u,%u,%u]\n", descriptor[0], descriptor[1], descriptor[2], descriptor[3]);
      return 0;
   }
   if (argc != 4) {
      fprintf(stderr, "usage: hackynavi-aco-fragment-buffer validated.spv entry private-empty-output-directory\n");
      return 2;
   }
   return hn_gfx11_fragment_buffer_compile_validated_file(argv[1], argv[2], argv[3]);
}
#endif

int hn_gfx11_fragment_buffer_descriptor(uint64_t address, uint32_t output[4])
{
   if (!output || !address || (address & 15) || address > (UINT64_C(1) << 48) - 16)
      return 2;
   uint32_t local[4];
   ac_build_raw_buffer_descriptor(GFX11, false, address, 16, local);
   memcpy(output, local, sizeof(local));
   return 0;
}
