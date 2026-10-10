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
#include "hackynavi_gfx11.h"

struct lowering {
   struct ac_shader_args args;
   struct ac_arg descriptors;
   bool failed;
   uint32_t bindings;
};

struct output {
   const char *directory;
   const uint32_t *local;
   uint32_t bindings;
   bool success;
};

static void fail(struct lowering *state, const char *message)
{
   fprintf(stderr, "%s\n", message);
   state->failed = true;
}

static bool lower_resources(nir_builder *b, nir_intrinsic_instr *intrin, void *opaque)
{
   struct lowering *state = opaque;
   b->cursor = nir_before_instr(&intrin->instr);
   nir_def *replacement = NULL;
   unsigned source = 0;
   switch (intrin->intrinsic) {
   case nir_intrinsic_vulkan_resource_index: {
      unsigned binding = nir_intrinsic_binding(intrin);
      if (nir_intrinsic_desc_set(intrin) != 0 || binding >= 32 ||
          nir_intrinsic_desc_type(intrin) != nir_descriptor_type_storage_buffer ||
          !nir_src_is_const(intrin->src[0]) || nir_src_as_uint(intrin->src[0]) != 0) {
         fail(state, "Only singleton storage buffers in set 0, bindings 0..31 are supported");
         return false;
      }
      nir_def *values[NIR_MAX_VEC_COMPONENTS];
      for (unsigned i = 0; i < intrin->def.num_components; i++)
         values[i] = nir_imm_int(b, i == 0 ? binding : 0);
      replacement = nir_vec(b, values, intrin->def.num_components);
      state->bindings |= 1u << binding;
      break;
   }
   case nir_intrinsic_load_vulkan_descriptor:
      replacement = nir_vector_insert_imm(b, intrin->src[0].ssa, nir_imm_int(b, 0), intrin->def.num_components - 1);
      break;
   case nir_intrinsic_vulkan_resource_reindex:
      fail(state, "Descriptor reindexing is unsupported");
      return false;
   case nir_intrinsic_store_ssbo:
      source = 1;
      FALLTHROUGH;
   case nir_intrinsic_load_ssbo: {
      if (nir_intrinsic_access(intrin) & ACCESS_NON_UNIFORM) {
         fail(state, "Nonuniform buffer descriptors are unsupported");
         return false;
      }
      nir_def *base = nir_pack_64_2x32(b, ac_nir_load_arg(b, &state->args, state->descriptors));
      nir_def *index = nir_channel(b, intrin->src[source].ssa, 0);
      nir_def *descriptor = ac_nir_load_smem(b, 4, base, nir_imul_imm(b, index, 16), 16, 0);
      nir_src_rewrite(&intrin->src[source], descriptor);
      return true;
   }
   case nir_intrinsic_load_push_constant: {
      if (intrin->def.bit_size != 32 || intrin->def.num_components > 4 ||
          !nir_src_is_const(intrin->src[0])) {
         fail(state, "Only constant-offset 32-bit push constant loads are supported");
         return false;
      }
      uint64_t offset = (uint64_t)nir_intrinsic_base(intrin) + nir_src_as_uint(intrin->src[0]);
      if ((offset & 3) || offset + intrin->def.num_components * 4 > 48) {
         fail(state, "Push constant access exceeds the 48-byte dispatch payload");
         return false;
      }
      nir_def *base = nir_pack_64_2x32(b, ac_nir_load_arg(b, &state->args, state->args.push_constants));
      replacement = ac_nir_load_smem(b, intrin->def.num_components, base, nir_imm_int(b, offset), 4, 0);
      break;
   }
   default:
      return false;
   }
   nir_def_replace(&intrin->def, replacement);
   return true;
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

static bool supported_final(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_tex || instr->type == nir_instr_type_call || instr->type == nir_instr_type_deref) {
               fprintf(stderr, "Unsupported residual texture, call, or pointer operation\n");
               return false;
            }
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            switch (intrin->intrinsic) {
            case nir_intrinsic_load_scalar_arg_amd:
            case nir_intrinsic_load_vector_arg_amd:
            case nir_intrinsic_load_ssbo:
            case nir_intrinsic_store_ssbo:
            case nir_intrinsic_load_global_amd:
               break;
            default:
               fprintf(stderr, "Unsupported residual intrinsic: %s\n", nir_intrinsic_infos[intrin->intrinsic].name);
               return false;
            }
         }
      }
   }
   return true;
}

static FILE *open_output(const char *directory, const char *name, const char *mode)
{
   char path[4096];
   if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= sizeof(path))
      return NULL;
   return fopen(path, mode);
}

static void emit_binary(void **opaque, const aco_callback_params *params)
{
   struct output *out = *opaque;
   const struct ac_shader_config *config = &params->config;
   if (!params->code || !params->code_dw || !params->exec_size || params->num_symbols ||
       config->scratch_bytes_per_wave || config->lds_size || params->constants_size) {
      fprintf(stderr, "This launch ABI does not yet support scratch, LDS, constants, or relocations\n");
      return;
   }
   FILE *code = open_output(out->directory, "shader.bin", "wb");
   if (!code)
      return;
   bool written = fwrite(params->code, 4, params->code_dw, code) == params->code_dw;
   if (fclose(code) || !written)
      return;
   FILE *metadata = open_output(out->directory, "shader.json", "w");
   if (!metadata)
      return;
   unsigned vgprs = MAX2(config->num_vgprs, 1);
   uint32_t rsrc1 = S_00B848_VGPRS((vgprs - 1) / 8) | S_00B848_DX10_CLAMP(1) |
      S_00B848_FLOAT_MODE(config->float_mode) | S_00B848_MEM_ORDERED(config->mem_ordered) | S_00B848_WGP_MODE(config->wgp_mode);
   uint32_t rsrc2 = S_00B84C_USER_SGPR(4) | S_00B84C_TGID_X_EN(1) | S_00B84C_TGID_Y_EN(1) |
      S_00B84C_TGID_Z_EN(1) | S_00B84C_TG_SIZE_EN(1) | S_00B84C_TIDIG_COMP_CNT(out->local[2] > 1 ? 2 : out->local[1] > 1 ? 1 : 0);
   fprintf(metadata,
      "{\n  \"abi_version\": 1, \"compiler\": \"Mesa NIR/ACO\",\n"
      "  \"mesa_commit\": \"35b085c4c067f198ce0b6437d94678e47b9c645f\",\n"
      "  \"target\": \"gfx1100\", \"target_kind\": \"explicit_offline_compiler_target\",\n"
      "  \"wave_size\": %u, \"workgroup_size\": [%u, %u, %u],\n"
      "  \"code_bytes\": %u, \"executable_bytes\": %u, \"code_alignment\": 256,\n"
      "  \"num_sgprs\": %u, \"num_vgprs\": %u, \"num_shared_vgprs\": %u,\n"
      "  \"spilled_sgprs\": %u, \"spilled_vgprs\": %u, \"lds_bytes\": %u, \"scratch_bytes_per_wave\": %u,\n"
      "  \"float_mode\": %u, \"mem_ordered\": %s, \"wgp_mode\": %s,\n"
      "  \"compute_pgm_rsrc1\": %u, \"compute_pgm_rsrc2\": %u, \"compute_pgm_rsrc3\": 0,\n"
      "  \"dispatch_initiator_required_bits\": %u,\n"
      "  \"descriptor_binding_mask\": %u, \"descriptor_set\": 0, \"descriptor_stride_bytes\": 16,\n"
      "  \"descriptor_format\": \"GFX11 raw buffer descriptor, byte bounds\",\n"
      "  \"user_sgprs\": {\"descriptor_table_gpu_va\": [0, 1], \"push_constants_gpu_va\": [2, 3]},\n"
      "  \"system_sgprs\": {\"workgroup_x\": 4, \"workgroup_y\": 5, \"workgroup_z\": 6, \"tg_size\": 7},\n"
      "  \"system_vgprs\": {\"packed_local_invocation_id\": 0},\n"
      "  \"push_constant_bytes\": 48, \"relocations\": [], \"constants_bytes\": 0\n}\n",
      params->wave_size, out->local[0], out->local[1], out->local[2], params->code_dw * 4,
      params->exec_size, config->num_sgprs, vgprs, config->num_shared_vgprs,
      config->spilled_sgprs, config->spilled_vgprs, config->lds_size, config->scratch_bytes_per_wave,
      config->float_mode, config->mem_ordered ? "true" : "false", config->wgp_mode ? "true" : "false", rsrc1, rsrc2,
      S_00B800_CS_W32_EN(1), out->bindings);
   if (fclose(metadata))
      return;
   if (params->ir_str && params->ir_size) {
      FILE *ir = open_output(out->directory, "aco.txt", "wb");
      if (!ir)
         return;
      written = fwrite(params->ir_str, 1, params->ir_size, ir) == params->ir_size;
      if (fclose(ir) || !written)
         return;
   }
   out->success = true;
}

static int backend_main(int argc, char **argv)
{
   if (argc == 4 && !strcmp(argv[1], "--raw-buffer-descriptor")) {
      char *end_va, *end_size;
      uint64_t va = strtoull(argv[2], &end_va, 0);
      unsigned long long size = strtoull(argv[3], &end_size, 0);
      if (*end_va || *end_size || !va || va >= (1ull << 48) || !size || size > UINT32_MAX || va + size > (1ull << 48))
         return 2;
      uint32_t descriptor[4];
      ac_build_raw_buffer_descriptor(GFX11, false, va, (uint32_t)size, descriptor);
      printf("[%u, %u, %u, %u]\n", descriptor[0], descriptor[1], descriptor[2], descriptor[3]);
      return 0;
   }
   if (argc != 7) {
      fprintf(stderr, "usage: hackynavi-aco validated.spv entry local_x local_y local_z output-directory\n");
      return 2;
   }
   uint32_t local[3];
   for (unsigned i = 0; i < 3; i++) {
      char *end;
      unsigned long value = strtoul(argv[i + 3], &end, 10);
      if (*end || !value || value > 1024)
         return 2;
      local[i] = value;
   }
   if ((uint64_t)local[0] * local[1] * local[2] > 1024)
      return 2;
   FILE *input = fopen(argv[1], "rb");
   if (!input)
      return 2;
   if (fseek(input, 0, SEEK_END)) {
      fclose(input);
      return 2;
   }
   long bytes = ftell(input);
   if (bytes < 20 || bytes > 64 * 1024 * 1024 || (bytes % 4)) {
      fclose(input);
      return 2;
   }
   rewind(input);
   uint32_t *words = malloc(bytes);
   if (!words || fread(words, 1, bytes, input) != bytes) {
      free(words);
      fclose(input);
      return 2;
   }
   fclose(input);
   if (words[0] != SpvMagicNumber) {
      free(words);
      return 2;
   }
   for (unsigned offset = 5; offset < bytes / 4;) {
      unsigned count = words[offset] >> 16, opcode = words[offset] & 0xffff;
      if (!count || count > bytes / 4 - offset) {
         free(words);
         return 2;
      }
      if (opcode == SpvOpCapability && (count != 2 || (words[offset + 1] != SpvCapabilityShader && words[offset + 1] != SpvCapabilityInt64))) {
         fprintf(stderr, "Unsupported SPIR-V capability\n");
         free(words);
         return 2;
      }
      if (opcode == SpvOpDecorate && count >= 3 && words[offset + 2] == SpvDecorationSpecId &&
          (count != 4 || words[offset + 3] > 2)) {
         fprintf(stderr, "Only local-size specialization IDs 0, 1, and 2 are supported\n");
         free(words);
         return 2;
      }
      offset += count;
   }
   struct radeon_info target = {0};
   target.gfx_level = GFX11;
   target.family = CHIP_NAVI31;
   ac_fill_compiler_info(&target, NULL, false);
   struct nir_shader_compiler_options options = {0};
   ac_nir_set_options(&target.compiler_info, false, &options);
   const struct spirv_to_nir_options spirv_options = {
      .environment = NIR_SPIRV_VULKAN,
      .ubo_addr_format = nir_address_format_vec2_index_32bit_offset,
      .ssbo_addr_format = nir_address_format_vec2_index_32bit_offset,
      .push_const_addr_format = nir_address_format_logical,
      .shared_addr_format = nir_address_format_32bit_offset,
   };
   struct nir_spirv_specialization_entry entries[3];
   for (unsigned i = 0; i < 3; i++)
      entries[i] = (struct nir_spirv_specialization_entry){.id = i, .size = 4, .data = (uint8_t *)&local[i]};
   struct nir_spirv_specialization specialization = {.num_entries = 3, .entries = entries};
   int result = 2;
   glsl_type_singleton_init_or_ref();
   nir_shader *nir = spirv_to_nir(words, bytes / 4, &specialization, MESA_SHADER_COMPUTE, argv[2], &spirv_options, &options);
   free(words);
   if (!nir)
      goto cleanup;
   for (unsigned i = 0; i < 3; i++) {
      if (nir->info.workgroup_size[i] != local[i]) {
         fprintf(stderr, "SPIR-V workgroup size does not match explicit specialization\n");
         goto cleanup;
      }
   }
   nir_foreach_variable_in_shader(var, nir) {
      if (var->data.mode == nir_var_mem_ssbo) {
         if (var->data.descriptor_set || var->data.binding >= 32 || glsl_type_is_array(var->type))
            goto cleanup;
      } else if (var->data.mode != nir_var_mem_push_const && var->data.mode != nir_var_system_value &&
                 var->data.mode != nir_var_shader_in && var->data.mode != nir_var_shader_temp) {
         fprintf(stderr, "Unsupported shader variable mode\n");
         goto cleanup;
      }
   }
   NIR_PASS(_, nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS(_, nir, nir_lower_returns);
   NIR_PASS(_, nir, nir_inline_functions);
   nir_remove_non_cmat_call_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_system_values);
   nir_lower_compute_system_values_options csv = {.lower_local_invocation_index = true};
   NIR_PASS(_, nir, nir_lower_compute_system_values, &csv);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_push_const, nir_address_format_32bit_offset);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo, nir_address_format_vec2_index_32bit_offset);
   optimize(nir);
   struct lowering state = {0};
   ac_add_arg(&state.args, AC_ARG_SGPR, 2, AC_ARG_CONST_ADDR, &state.descriptors);
   ac_add_arg(&state.args, AC_ARG_SGPR, 2, AC_ARG_CONST_ADDR, &state.args.push_constants);
   for (unsigned i = 0; i < 3; i++)
      ac_add_arg(&state.args, AC_ARG_SGPR, 1, AC_ARG_VALUE, &state.args.workgroup_ids[i]);
   ac_add_arg(&state.args, AC_ARG_SGPR, 1, AC_ARG_VALUE, &state.args.tg_size);
   ac_add_arg(&state.args, AC_ARG_VGPR, 1, AC_ARG_VALUE, &state.args.local_invocation_ids_packed);
   nir_shader_intrinsics_pass(nir, lower_resources, nir_metadata_control_flow, &state);
   if (state.failed)
      goto cleanup;
   const ac_nir_lower_intrinsics_to_args_options args_options = {
      .gfx_level = GFX11, .hw_stage = AC_HW_COMPUTE_SHADER, .wave_size = 32,
      .workgroup_size = local[0] * local[1] * local[2],
   };
   NIR_PASS(_, nir, ac_nir_lower_intrinsics_to_args, &state.args, &args_options);
   NIR_PASS(_, nir, ac_nir_lower_global_access, GFX11);
   NIR_PASS(_, nir, nir_lower_int64);
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS(_, nir, nir_lower_phis_to_scalar, ac_nir_lower_phis_to_scalar_cb, NULL);
   optimize(nir);
   NIR_PASS(_, nir, nir_lower_load_const_to_scalar);
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_all, NULL);
   NIR_PASS(_, nir, nir_opt_dce);
   if (!supported_final(nir))
      goto cleanup;
   nir->info.max_subgroup_size = 32;
   nir->info.min_subgroup_size = 32;
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   nir_validate_shader(nir, "HackyNavi before ACO");
   struct aco_shader_info info = {
      .hw_stage = AC_HW_COMPUTE_SHADER, .wave_size = 32,
      .workgroup_size = local[0] * local[1] * local[2],
   };
   const struct aco_compiler_options aco_options = {
      .compiler_info = &target.compiler_info, .family = CHIP_NAVI31, .gfx_level = GFX11,
      .wgp_mode = true, .record_ir = true,
   };
   struct output out = {.directory = argv[6], .local = local, .bindings = state.bindings};
   void *out_ptr = &out;
   aco_compile_shader(&aco_options, &info, 1, &nir, &state.args, emit_binary, &out_ptr);
   result = out.success ? 0 : 2;
cleanup:
   if (nir)
      ralloc_free(nir);
   glsl_type_singleton_decref();
   return result;
}

uint32_t hn_gfx11_abi_version(void)
{
   return 1;
}

int hn_gfx11_raw_buffer_descriptor(uint64_t gpu_va, uint32_t size, uint32_t descriptor[4])
{
   if (!descriptor || !gpu_va || gpu_va >= (1ull << 48) || !size || gpu_va + size > (1ull << 48))
      return 2;
   ac_build_raw_buffer_descriptor(GFX11, false, gpu_va, size, descriptor);
   return 0;
}

int hn_gfx11_compile_validated_file(const char *spirv_path, const char *entry, const uint32_t local[3],
                                   const char *temporary_output_directory)
{
   if (!spirv_path || !entry || !entry[0] || !local || !temporary_output_directory || strlen(entry) > 255)
      return 2;
   char dimensions[3][16];
   for (unsigned i = 0; i < 3; i++)
      snprintf(dimensions[i], sizeof(dimensions[i]), "%u", local[i]);
   char *argv[] = {"hackynavi-aco", (char *)spirv_path, (char *)entry, dimensions[0], dimensions[1], dimensions[2],
                   (char *)temporary_output_directory};
   return backend_main(7, argv);
}

#ifndef HN_GFX11_SHARED
int main(int argc, char **argv)
{
   return backend_main(argc, argv);
}
#endif
