// SPDX-License-Identifier: MIT
// Actual resource-free position-only vertex-to-NGG prerequisite.
#include "hackynavi_gfx11_ngg.h"
#include "aco_interface.h"
#include "ac_gpu_info.h"
#include "ac_nir.h"
#include "ac_shader_args.h"
#include "amdgfxregs.h"
#include "nir.h"
#include "nir_builder.h"
#include "spirv/nir_spirv.h"
#include "spirv/spirv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ngg_output { const char *directory; bool success; };

static bool reject(const char *message)
{
   fprintf(stderr, "NGG profile: %s\n", message);
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

static bool interface_contract(nir_shader *nir)
{
   unsigned outputs = 0;
   nir_foreach_variable_in_shader(var, nir) {
      if (var->data.mode == nir_var_shader_out) {
         if (++outputs != 1 || var->data.location != VARYING_SLOT_POS ||
             var->data.location_frac || var->data.index ||
             !glsl_type_is_vector(var->type) || glsl_get_vector_elements(var->type) != 4 ||
             glsl_get_base_type(var->type) != GLSL_TYPE_FLOAT)
            return reject("requires exactly one float32 vec4 Position output");
         var->data.driver_location = 0;
      } else if (var->data.mode == nir_var_system_value) {
         if (var->data.location != SYSTEM_VALUE_VERTEX_ID)
            return reject("only VertexIndex system input is supported");
      } else if (var->data.mode != nir_var_shader_temp) {
         return reject("resources and vertex attribute inputs are unsupported");
      }
   }
   return outputs == 1 || reject("missing Position output");
}

static bool body_contract(nir_shader *nir)
{
   unsigned blocks = 0, stores = 0;
   nir_foreach_block(block, nir_shader_get_entrypoint(nir)) {
      if (++blocks != 1)
         return reject("source control flow is unsupported");
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_load_const) {
            if (nir_instr_as_load_const(instr)->def.bit_size > 32)
               return reject("wide source constants are unsupported");
            continue;
         }
         if (instr->type == nir_instr_type_alu) {
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            if (alu->def.bit_size > 32 ||
                (alu->op != nir_op_mov && alu->op != nir_op_vec2 && alu->op != nir_op_vec3 &&
                 alu->op != nir_op_vec4 && alu->op != nir_op_iadd && alu->op != nir_op_ieq &&
                 alu->op != nir_op_bcsel))
               return reject("only constant/VertexIndex position selection is supported");
            continue;
         }
         if (instr->type != nir_instr_type_intrinsic)
            return reject("unsupported source instruction");
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         if (in->intrinsic == nir_intrinsic_load_vertex_id_zero_base ||
             in->intrinsic == nir_intrinsic_load_first_vertex)
            continue;
         if (in->intrinsic != nir_intrinsic_store_output || ++stores != 1 ||
             nir_intrinsic_io_semantics(in).location != VARYING_SLOT_POS ||
             nir_intrinsic_component(in) || nir_intrinsic_write_mask(in) != 15 ||
             in->num_components != 4 || in->src[0].ssa->bit_size != 32 ||
             !nir_src_is_const(in->src[1]) || nir_src_as_uint(in->src[1]) != 0)
            return reject("requires one full Position store without other side effects");
      }
   }
   return stores == 1 || reject("missing full Position store");
}

static bool lowered_contract(nir_shader *nir)
{
   unsigned pos = 0, prim = 0;
   nir_foreach_block(block, nir_shader_get_entrypoint(nir)) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_alu || instr->type == nir_instr_type_load_const ||
             instr->type == nir_instr_type_phi || instr->type == nir_instr_type_undef)
            continue;
         if (instr->type != nir_instr_type_intrinsic)
            return reject("unsupported lowered instruction");
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         switch (in->intrinsic) {
         case nir_intrinsic_load_scalar_arg_amd:
         case nir_intrinsic_load_scalar_arg_wg_div_amd:
         case nir_intrinsic_load_vector_arg_amd:
         case nir_intrinsic_is_subgroup_invocation_lt_amd:
            break;
         case nir_intrinsic_export_amd:
            if (nir_intrinsic_target(in) == V_008DFC_SQ_EXP_POS)
               ++pos;
            else if (nir_intrinsic_target(in) == V_008DFC_SQ_EXP_PRIM)
               ++prim;
            else
               return reject("unexpected NGG export target");
            break;
         default:
            fprintf(stderr, "unexpected intrinsic %s\n", nir_intrinsic_infos[in->intrinsic].name);
            return reject("lowered resource, memory or unknown operation is unsupported");
         }
      }
   }
   return (pos == 1 && prim == 1) || reject("missing unique Position/primitive exports");
}

static bool write_bytes(const char *directory, const char *name, const void *data, size_t size)
{
   char path[4096];
   int count = snprintf(path, sizeof(path), "%s/%s", directory, name);
   if (count < 0 || (size_t)count >= sizeof(path)) return false;
   FILE *file = fopen(path, "wb");
   if (!file) return false;
   bool ok = fwrite(data, 1, size, file) == size;
   return fclose(file) == 0 && ok;
}

static void emit_ngg(void **opaque, const aco_callback_params *p)
{
   struct ngg_output *out = *opaque;
   const struct ac_shader_config *c = &p->config;
   if (!p->code || !p->code_dw || p->code_dw > 262144 || !p->exec_size ||
       p->exec_size > p->code_dw * 4 || p->exec_size % 4 || p->wave_size != 32 ||
       p->num_symbols || p->constants_size || c->scratch_bytes_per_wave || c->lds_size ||
       c->spilled_sgprs || c->spilled_vgprs || c->num_shared_vgprs || c->wgp_mode ||
       c->spi_ps_input_ena || c->spi_ps_input_addr || !c->num_vgprs || c->num_vgprs > 256 ||
       c->num_sgprs > 128 || c->float_mode > 255) {
      reject("ACO product exceeds the no-resource/no-scratch NGG contract");
      return;
   }
   // RADV postprocess_config: passthrough GS_VGPR_COMP_CNT=0, only VertexID
   // gives ES_VGPR_COMP_CNT=0. Hardware still places ES VertexID in v5.
   unsigned vgprs = MAX2(c->num_vgprs, 6);
   uint32_t rsrc1 = S_00B228_VGPRS((vgprs - 1) / 8) | S_00B228_DX10_CLAMP(1) |
      S_00B228_FLOAT_MODE(c->float_mode) | S_00B228_MEM_ORDERED(c->mem_ordered);
   uint32_t rsrc2 = S_00B22C_USER_SGPR(1); // s8: actual base vertex, no address.
   // radv_emit_vgt_shader_config_gfx6's non-tessellated, no-user-GS NGG path.
   // This stage selection is only part of the required graphics draw state.
   uint32_t stages = S_028B54_ES_EN(V_028B54_ES_STAGE_REAL) | S_028B54_PRIMGEN_EN(1) |
      S_028B54_PRIMGEN_PASSTHRU_EN(1) | S_028B54_PRIMGEN_PASSTHRU_NO_MSG(1) |
      S_028B54_MAX_PRIMGRP_IN_WAVE(2) | S_028B54_GS_W32_EN(1);
   if (!write_bytes(out->directory, "shader.bin", p->code, p->code_dw * 4) ||
       !p->ir_str || !p->ir_size || !write_bytes(out->directory, "aco.txt", p->ir_str, p->ir_size))
      return;
   if (p->disasm_str && p->disasm_size &&
       !write_bytes(out->directory, "aco-diagnostics.txt", p->disasm_str, p->disasm_size))
      return;
   char metadata[4096];
   int count = snprintf(metadata, sizeof(metadata),
      "{\n  \"abi_version\":1, \"profile\":\"gfx1100-position-only-ngg-v1\",\n"
      "  \"compiler\":\"Mesa NIR/ACO\", \"mesa_commit\":\"35b085c4c067f198ce0b6437d94678e47b9c645f\",\n"
      "  \"target\":\"gfx1100\", \"stage\":\"vertex\", \"hardware_stage\":\"next_gen_geometry\",\n"
      "  \"code_bytes\":%u, \"executable_bytes\":%u, \"code_alignment\":256, \"wave_size\":32,\n"
      "  \"num_sgprs\":%u, \"num_vgprs\":%u, \"input_vgprs\":6, \"float_mode\":%u,\n"
      "  \"mem_ordered\":%s, \"wgp_mode\":false, \"spi_shader_pgm_rsrc1_gs\":%u, \"spi_shader_pgm_rsrc2_gs\":%u,\n"
      "  \"ngg_passthrough\":true, \"primgen_passthru_no_msg\":true, \"early_primitive_export\":false,\n"
      "  \"vgt_shader_stages_en\":%u, \"spi_shader_pos_format\":%u,\n"
      "  \"max_workgroup_size\":256, \"vertices_per_primitive\":3, \"position_exports\":1, \"parameter_exports\":0,\n"
      "  \"resource_count\":0, \"user_sgprs\":1, \"base_vertex_sgpr\":8, \"merged_wave_info_sgpr\":3,\n"
      "  \"packed_primitive_vgpr\":0, \"vertex_id_vgpr\":5, \"gs_vgpr_comp_cnt\":0, \"es_vgpr_comp_cnt\":0,\n"
      "  \"scratch_bytes_per_wave\":0, \"lds_bytes\":0, \"attribute_ring_writes\":0, \"constants_bytes\":0, \"relocations\":[],\n"
      "  \"gpu_executed\":false, \"draw_state_complete\":false\n}\n",
      p->code_dw * 4, p->exec_size, c->num_sgprs, vgprs, c->float_mode,
      c->mem_ordered ? "true" : "false", rsrc1, rsrc2, stages,
      S_02870C_POS0_EXPORT_FORMAT(V_02870C_SPI_SHADER_4COMP));
   out->success = count > 0 && (size_t)count < sizeof(metadata) &&
      write_bytes(out->directory, "shader.json", metadata, (size_t)count);
}

int hn_gfx11_ngg_compile_validated_file(const char *path, const char *entry, const char *directory)
{
   if (!path || !entry || !directory || !*entry || strlen(entry) > 255) return 2;
   FILE *file = fopen(path, "rb");
   if (!file) return 2;
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
          op == SpvOpExecutionMode || op == SpvOpExecutionModeId ||
          op == SpvOpSpecConstant || op == SpvOpSpecConstantComposite || op == SpvOpSpecConstantOp ||
          op == SpvOpSpecConstantTrue || op == SpvOpSpecConstantFalse) {
         reject("unsupported capability, execution mode or specialization"); valid = false; break;
      }
      at += count;
   }
   if (!valid) { free(words); return 2; }
   struct radeon_info target = {0}; target.gfx_level = GFX11; target.family = CHIP_NAVI31;
   ac_fill_compiler_info(&target, NULL, false);
   if (!target.compiler_info.has_ngg_passthru_no_msg) { free(words); return 2; }
   struct nir_shader_compiler_options options = {0};
   ac_nir_set_options(&target.compiler_info, false, &options);
   const struct spirv_to_nir_options spv = {.environment = NIR_SPIRV_VULKAN};
   int result = 2;
   glsl_type_singleton_init_or_ref();
   nir_shader *nir = spirv_to_nir(words, (size_t)bytes / 4, NULL, MESA_SHADER_VERTEX, entry, &spv, &options);
   free(words);
   if (!nir) { reject("vertex entrypoint translation failed"); goto cleanup; }
   if (!interface_contract(nir)) goto cleanup;
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   nir_remove_non_cmat_call_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_system_values);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_out, glsl_count_attribute_slots, (nir_lower_io_options)0);
   optimize(nir);
   if (!body_contract(nir)) goto cleanup;
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   uint8_t param_offsets[NUM_TOTAL_VARYING_SLOTS]; memset(param_offsets, 0xff, sizeof(param_offsets));
   const ac_nir_lower_ngg_options ngg = {.compiler_info = &target.compiler_info,
      .max_workgroup_size = 256, .wave_size = 32, .vs_output_param_offset = param_offsets,
      .num_vertices_per_primitive = 3, .passthrough = true, .disable_streamout = true};
   uint32_t lds_vertex = 0; uint8_t lds_scratch = 0;
   NIR_PASS(_, nir, ac_nir_lower_ngg_nogs, &ngg, &lds_vertex, &lds_scratch);
   if (lds_vertex || lds_scratch || nir->info.shared_size) { reject("NGG unexpectedly requires LDS"); goto cleanup; }
   struct ac_shader_args args = {0};
   // Real merged gfx11 ABI: system registers s0..s7, user SGPR0 at s8.
   // Unused address/system slots stay reserved, never dereferenced by this profile.
   ac_add_arg(&args, AC_ARG_SGPR, 2, AC_ARG_CONST_ADDR, NULL);
   ac_add_arg(&args, AC_ARG_SGPR, 1, AC_ARG_VALUE, &args.gs_tg_info);
   ac_add_arg(&args, AC_ARG_SGPR, 1, AC_ARG_VALUE, &args.merged_wave_info);
   ac_add_arg(&args, AC_ARG_SGPR, 1, AC_ARG_VALUE, &args.tess_offchip_offset);
   ac_add_arg(&args, AC_ARG_SGPR, 1, AC_ARG_VALUE, &args.gs_attr_offset);
   ac_add_arg(&args, AC_ARG_SGPR, 1, AC_ARG_VALUE, NULL);
   ac_add_arg(&args, AC_ARG_SGPR, 1, AC_ARG_VALUE, NULL);
   ac_add_arg(&args, AC_ARG_SGPR, 1, AC_ARG_VALUE, &args.base_vertex);
   ac_add_arg(&args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args.gs_vtx_offset[0]);
   ac_add_arg(&args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args.gs_vtx_offset[1]);
   ac_add_arg(&args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args.gs_prim_id);
   ac_add_arg(&args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args.gs_invocation_id);
   ac_add_arg(&args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args.gs_vtx_offset[2]);
   ac_add_arg(&args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &args.vertex_id);
   const ac_nir_lower_intrinsics_to_args_options abi = {.gfx_level = GFX11,
      .hw_stage = AC_HW_NEXT_GEN_GEOMETRY_SHADER, .wave_size = 32, .workgroup_size = 256};
   NIR_PASS(_, nir, ac_nir_lower_intrinsics_to_args, &args, &abi);
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS(_, nir, nir_lower_phis_to_scalar, ac_nir_lower_phis_to_scalar_cb, NULL);
   optimize(nir);
   NIR_PASS(_, nir, nir_lower_load_const_to_scalar);
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_all, NULL);
   NIR_PASS(_, nir, nir_opt_dce);
   if (!lowered_contract(nir)) goto cleanup;
   nir->info.max_subgroup_size = 32; nir->info.min_subgroup_size = 32;
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   nir_validate_shader(nir, "HackyNavi position-only NGG before ACO");
   struct aco_shader_info info = {.hw_stage = AC_HW_NEXT_GEN_GEOMETRY_SHADER,
      .wave_size = 32, .workgroup_size = 256};
   const struct aco_compiler_options aco = {.compiler_info = &target.compiler_info,
      .family = CHIP_NAVI31, .gfx_level = GFX11, .record_ir = true, .record_asm = true};
   struct ngg_output out = {.directory = directory}; void *opaque = &out;
   aco_compile_shader(&aco, &info, 1, &nir, &args, emit_ngg, &opaque);
   result = out.success ? 0 : 2;
cleanup:
   if (nir) ralloc_free(nir);
   glsl_type_singleton_decref();
   return result;
}

#ifndef HN_GFX11_NGG_SHARED
int main(int argc, char **argv)
{
   if (argc != 4) {
      fprintf(stderr, "usage: hackynavi-aco-ngg validated.spv entry private-empty-output-directory\n");
      return 2;
   }
   return hn_gfx11_ngg_compile_validated_file(argv[1], argv[2], argv[3]);
}
#endif
