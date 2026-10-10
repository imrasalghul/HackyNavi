// SPDX-License-Identifier: MIT
// Numerical host oracle. Calls actual pinned Mesa functions; no GPU/backend.
// Every topology value is an explicit test input, NOT a hardware observation.
#include "ac_barrier.h"
#include "ac_binary.h"
#include "ac_cmdbuf.h"
#include "ac_cmdbuf_cp.h"
#include "ac_gpu_info.h"
#include "ac_formats.h"
#include "ac_shader_util.h"
#include "ac_surface.h"
#include "ac_descriptors.h"
#include "ac_drm_fourcc.h"
#include "addrlib/src/amdgpu_asic_addr.h"
#include "util/format/u_formats.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
// Verbatim pinned radv_get_viewport_xform, extracted and hash-bound by runner.
#include "mesa_viewport_xform.h"

static unsigned number(const char *s) {
   char *end = NULL;
   errno = 0;
   unsigned long value = strtoul(s, &end, 0);
   if (errno || !*s || *end || value > UINT32_MAX) exit(2);
   return value;
}
static void words(const char *name, const uint32_t *data, unsigned count) {
   printf("\"%s\":[", name);
   for (unsigned i = 0; i < count; ++i) printf("%s%u", i ? "," : "", data[i]);
   printf("]");
}
static void reg(const char *name, uint32_t address, uint32_t value, bool *first) {
   printf("%s{\"name\":\"%s\",\"address\":%u,\"value\":%u}",
          *first ? "" : ",", name, address, value);
   *first = false;
}
#define REG(name, value) reg(#name, name, (value), &first)

int main(int argc, char **argv) {
   // se rb min-cu-per-sa gb-addr-config tile-steering address32-hi vs-exec ps-exec [width height]
   if (argc != 9 && argc != 11) return 2;
   struct radeon_info info = {0};
   info.gfx_level = GFX11;
   info.family = CHIP_NAVI31;
   info.max_se = number(argv[1]);
   info.max_render_backends = number(argv[2]);
   info.min_good_cu_per_sa = number(argv[3]);
   info.gb_addr_config = number(argv[4]);
   info.pa_sc_tile_steering_override = number(argv[5]);
   info.address32_hi = number(argv[6]);
   const unsigned vs_exec = number(argv[7]), ps_exec = number(argv[8]);
   const unsigned width = argc == 11 ? number(argv[9]) : 64;
   const unsigned height = argc == 11 ? number(argv[10]) : 64;
   if (!info.max_se || info.max_se > 6 || !info.max_render_backends ||
       info.max_render_backends > 24 || info.min_good_cu_per_sa < 2 ||
       info.min_good_cu_per_sa > 16 || (info.min_good_cu_per_sa & 1) ||
       info.address32_hi > 0x7fff || !vs_exec || vs_exec > 1024 ||
       !ps_exec || ps_exec > 1024 || !width || width > 16384 || !height || height > 16384) return 2;
   // Real GFX11 addrlib + CB descriptor calculation, then the exact named
   // ordinary-register path of radv_gfx11_emit_fb_color_state:4931-4969.
   // This is a numerical host input; it does not assert target residency.
   info.family_id = FAMILY_NV3;
   info.chip_external_rev = 1;
   info.has_dedicated_vram = true;
   info.has_graphics = true;
   info.has_image_opcodes = true;
   ac_fill_compiler_info(&info, NULL, false);
   struct ac_addrlib *lib = ac_addrlib_create(&info, NULL);
   if (!lib) return 6;
   const struct ac_surf_config config = {
      .info = {.width=width,.height=height,.depth=1,.array_size=1,.samples=1,
               .storage_samples=1,.levels=1,.num_channels=4},
      .blk_w=1,.blk_h=1,.bpe=4,
      .surf_flags=RADEON_SURF_DISABLE_DCC|RADEON_SURF_NO_TEXTURE,
      .modifier=DRM_FORMAT_MOD_INVALID
   };
   struct radeon_surf surface = {0};
   const int surface_status = ac_compute_surface(lib,&info,&config,RADEON_SURF_MODE_LINEAR_ALIGNED,&surface);
   ac_addrlib_destroy(lib);
   if (surface_status || !surface.is_linear || surface.meta_size || surface.cmask_size || surface.fmask_size) return 6;
   const struct ac_cb_state color_state = {
      .surf=&surface,.format=PIPE_FORMAT_R8G8B8A8_UNORM,.width=width,.height=height,
      .num_samples=1,.num_storage_samples=1,.num_levels=1
   };
   struct ac_cb_surface initial = {0}, cb = {0};
   ac_init_cb_surface(&info,&color_state,&initial);
   const struct ac_mutable_cb_state mutable = {
      .surf=&surface,.cb=&initial,.va=UINT64_C(0x400000000000),.num_samples=1
   };
   ac_set_mutable_cb_surface_fields(&info,&mutable,&cb);
   // Exact fixed GFX11 policy facts, not a simulated device query:
   // ac_gpu_info.c:80,1337-1340,1664-1665,1737-1756.
   info.spi_cu_en = UINT32_MAX; // no user CU restriction in this profile
   info.pc_lines = 1024;
   info.pbb_max_alloc_count = 16;
   info.instr_prefetch_distance = 3;
   info.attribute_ring_size_per_se = align(1400 * 1024, 65536);
   info.num_tile_pipes = 1u << G_0098F8_NUM_PIPES(info.gb_addr_config);
   // 4K VM pages, no 64K BIG_PAGE mapping. Ordinary SET_* packets, no
   // optional packed-state optimization. No CB/DB L2 cache preference.
   info.discardable_allows_big_page = false;
   struct ac_pm4_state *pm4 = ac_pm4_create_sized(&info, false, 512, false);
   if (!pm4) return 3;
   const struct ac_preamble_state preamble = {.border_color_va = 0};
   ac_init_graphics_preamble_state(&preamble, pm4);
   ac_pm4_finalize(pm4);

   ac_ngg_subgroup_info subgroup = {0};
   if (!ac_ngg_compute_subgroup_info(GFX11, MESA_SHADER_VERTEX, false,
          MESA_PRIM_TRIANGLES, 0, 1, 128, 256, 32, 0, 0, 0, false, 0, &subgroup)) {
      ac_pm4_free_state(pm4); return 4;
   }
   unsigned late = 0, cu = 0;
   ac_compute_late_alloc(&info, true, false, false, &late, &cu);
   const unsigned ps_cu = ac_gfx103_get_cu_mask_ps(&info);
   const unsigned lines = late ? info.pc_lines / 4 : 0;
   const unsigned vs_pref = ac_get_instr_prefetch_size(GFX11, 3, vs_exec);
   const unsigned ps_pref = ac_get_instr_prefetch_size(GFX11, 3, ps_exec);
   uint32_t ring[64] = {0}, before[64] = {0}, after[64] = {0};
   struct ac_cmdbuf ring_cs = {.buf=ring,.max_dw=64,.reserved_dw=64};
   struct ac_cmdbuf before_cs = {.buf=before,.max_dw=64,.reserved_dw=64};
   struct ac_cmdbuf after_cs = {.buf=after,.max_dw=64,.reserved_dw=64};
   const uint64_t attribute_va = ((uint64_t)info.address32_hi << 32) | 0x1000000;
   // Exact real radv_emit_ge_rings sequence, including CP_ME wait BEFORE writes.
   ac_emit_cp_release_mem_pws(&ring_cs,GFX11,AMD_IP_GFX,V_028A90_BOTTOM_OF_PIPE_TS,0);
   ac_emit_cp_acquire_mem_pws(&ring_cs,GFX11,AMD_IP_GFX,V_028A90_BOTTOM_OF_PIPE_TS,V_581B_CP_ME,0,0);
   ac_emit_cp_gfx11_ge_rings(&ring_cs,&info,attribute_va,false);
   const struct ac_barrier_state before_state = {
      .flags=AC_BARRIER_INV_ICACHE|AC_BARRIER_INV_SMEM|AC_BARRIER_INV_VMEM|
             AC_BARRIER_INV_L2|AC_BARRIER_PFP_SYNC_ME,
      .pws_acquire_point=AC_PWS_ACQUIRE_POINT_PFP
   };
   const struct ac_barrier_state after_state = {
      .flags=AC_BARRIER_SYNC_AND_INV_CB|AC_BARRIER_SYNC_AND_INV_DB|
             AC_BARRIER_INV_L2|AC_BARRIER_PFP_SYNC_ME,
      .pws_acquire_point=AC_PWS_ACQUIRE_POINT_PFP
   };
   bool roll=false; enum ac_rgp_flush_bits flush=0;
   ac_emit_barrier(&before_cs,GFX11,AMD_IP_GFX,&before_state,&roll,&flush);
   ac_emit_barrier(&after_cs,GFX11,AMD_IP_GFX,&after_state,&roll,&flush);
   if (roll) { ac_pm4_free_state(pm4); return 5; }
   printf("{\"schema\":1,\"hardware_observation\":false,\"input\":{");
   printf("\"max_se\":%u,\"max_render_backends\":%u,\"min_good_cu_per_sa\":%u,",
          info.max_se,info.max_render_backends,info.min_good_cu_per_sa);
   printf("\"gb_addr_config\":%u,\"tile_steering\":%u,\"address32_hi\":%u,\"vs_exec\":%u,\"ps_exec\":%u},",
          info.gb_addr_config,info.pa_sc_tile_steering_override,info.address32_hi,vs_exec,ps_exec);
   printf("\"derived\":{\"attribute_per_se\":%u,\"attribute_total\":%u,\"attribute_va\":%llu,",
          info.attribute_ring_size_per_se,info.attribute_ring_size_per_se*info.max_se,(unsigned long long)attribute_va);
   printf("\"num_tile_pipes\":%u,\"late_alloc\":%u,\"ngg_cu_mask\":%u,\"ps_cu_mask\":%u,",
          info.num_tile_pipes,late,cu,ps_cu);
   printf("\"es_vertices\":%u,\"gs_primitives\":%u,\"output_vertices\":%u,\"esgs_lds_dwords\":%u,\"output_lds_dwords\":%u,",
          subgroup.hw_max_esverts,subgroup.max_gsprims,subgroup.max_out_verts,subgroup.esgs_lds_size,subgroup.ngg_out_lds_size);
   printf("\"vs_prefetch_bytes\":%u,\"ps_prefetch_bytes\":%u,\"vs_prefetch_field\":%u,\"ps_prefetch_field\":%u},",
          ac_align_shader_binary_for_prefetch(GFX11,3,vs_exec),ac_align_shader_binary_for_prefetch(GFX11,3,ps_exec),vs_pref,ps_pref);
   words("common_preamble",pm4->pm4,pm4->ndw); printf(",");
   words("attribute_idle_and_ring",ring,ring_cs.cdw); printf(",");
   words("instruction_visibility",before,before_cs.cdw); printf(",");
   words("graphics_completion",after,after_cs.cdw); printf(",\"registers\":[");
   bool first=true;
   // Named fixed-profile specialization of radv_shader.c:1856-2068.
   REG(R_0286C4_SPI_VS_OUT_CONFIG,S_0286C4_NO_PC_EXPORT(1));
   REG(R_00B21C_SPI_SHADER_PGM_RSRC3_GS,ac_apply_cu_en(S_00B21C_CU_EN(cu)|S_00B21C_WAVE_LIMIT(63),C_00B21C_CU_EN,0,&info));
   REG(R_00B204_SPI_SHADER_PGM_RSRC4_GS,ac_apply_cu_en(S_00B204_CU_EN_GFX11(1)|S_00B204_SPI_SHADER_LATE_ALLOC_GS_GFX10(late)|S_00B204_INST_PREF_SIZE(vs_pref),C_00B204_CU_EN_GFX11,16,&info));
   REG(R_030980_GE_PC_ALLOC,S_030980_OVERSUB_EN(lines>0)|S_030980_NUM_PC_LINES(lines-1));
   REG(R_028708_SPI_SHADER_IDX_FORMAT,S_028708_IDX0_EXPORT_FORMAT(V_028708_SPI_SHADER_1COMP));
   REG(R_02870C_SPI_SHADER_POS_FORMAT,S_02870C_POS0_EXPORT_FORMAT(V_02870C_SPI_SHADER_4COMP));
   REG(R_02881C_PA_CL_VS_OUT_CNTL,0);
   REG(R_028A84_VGT_PRIMITIVEID_EN,0);
   REG(R_0287FC_GE_MAX_OUTPUT_PER_SUBGROUP,S_0287FC_MAX_VERTS_PER_SUBGROUP(subgroup.max_out_verts));
   REG(R_028B4C_GE_NGG_SUBGRP_CNTL,S_028B4C_PRIM_AMP_FACTOR(1)|S_028B4C_THDS_PER_SUBGRP(0));
   REG(R_028B90_VGT_GS_INSTANCE_CNT,S_028B90_CNT(1));
   REG(R_03096C_GE_CNTL,S_03096C_PRIMS_PER_SUBGRP(subgroup.max_gsprims)|S_03096C_VERTS_PER_SUBGRP(subgroup.hw_max_esverts)|S_03096C_PRIM_GRP_SIZE_GFX11(252));
   REG(R_02880C_DB_SHADER_CONTROL,S_02880C_CONSERVATIVE_Z_EXPORT(V_02880C_EXPORT_ANY_Z)|S_02880C_Z_ORDER(V_02880C_EARLY_Z_THEN_LATE_Z));
   REG(R_00B004_SPI_SHADER_PGM_RSRC4_PS,ac_apply_cu_en(S_00B004_CU_EN(ps_cu>>16)|S_00B004_INST_PREF_SIZE(ps_pref),C_00B004_CU_EN,16,&info));
   // Named raster/cache-independent fixed triangle decisions. Source table is
   // maintained in docs/gfx11-graphics-draw-contract.md; this is NOT a complete
   // render IB and none of these host numbers establish allocation authority.
   REG(R_028A4C_PA_SC_MODE_CNTL_1,S_028A4C_WALK_FENCE_ENABLE(1)|S_028A4C_WALK_FENCE_SIZE(info.num_tile_pipes==2?2:3)|S_028A4C_OUT_OF_ORDER_WATER_MARK(7)|S_028A4C_SUPERTILE_WALK_ORDER_ENABLE(1)|S_028A4C_TILE_WALK_ORDER_ENABLE(1)|S_028A4C_MULTI_SHADER_ENGINE_PRIM_DISCARD_ENABLE(1)|S_028A4C_FORCE_EOV_CNTDWN_ENABLE(1)|S_028A4C_FORCE_EOV_REZ_ENABLE(1)|S_028A4C_WALK_ALIGN8_PRIM_FITS_ST(1));
   REG(R_028C44_PA_SC_BINNER_CNTL_0,S_028C44_BINNING_MODE(V_028C44_BINNING_DISABLED)|S_028C44_BIN_SIZE_X_EXTEND(2)|S_028C44_BIN_SIZE_Y_EXTEND(2)|S_028C44_DISABLE_START_OF_PRIM(1)|S_028C44_FLUSH_ON_BINNING_TRANSITION(1));
   REG(R_028808_CB_COLOR_CONTROL,S_028808_ROP3(V_028808_ROP3_COPY)|S_028808_MODE(V_028808_CB_NORMAL));
   REG(R_028804_DB_EQAA,S_028804_HIGH_QUALITY_INTERSECTIONS(1)|S_028804_INCOHERENT_EQAA_READS(1)|S_028804_STATIC_ANCHOR_ASSOCIATIONS(1));
   REG(R_028C4C_PA_SC_CONSERVATIVE_RASTERIZATION_CNTL,S_028C4C_NULL_SQUAD_AA_MASK_ENABLE(1));
   REG(R_028A48_PA_SC_MODE_CNTL_0,S_028A48_ALTERNATE_RBS_PER_TILE(1)|S_028A48_VPORT_SCISSOR_ENABLE(1));
   REG(R_028760_SX_MRT0_BLEND_OPT,S_028760_COLOR_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED)|S_028760_ALPHA_COMB_FCN(V_028760_OPT_COMB_BLEND_DISABLED));
   // RADV preamble additions (radv_queue.c:718-827), specialized only by the
   // explicitly fixed resource-free Navi31 triangle profile.
   REG(R_028230_PA_SC_EDGERULE,0xaaaaaaaa);
   REG(R_028838_PA_CL_NGG_CNTL,S_028838_VERTEX_REUSE_DEPTH(30));
   REG(R_028A00_PA_SU_POINT_SIZE,S_028A00_HEIGHT(8)|S_028A00_WIDTH(8));
   REG(R_028A04_PA_SU_POINT_MINMAX,S_028A04_MIN_SIZE(0)|S_028A04_MAX_SIZE(65535));
   REG(R_028830_PA_SU_SMALL_PRIM_FILTER_CNTL,S_028830_SMALL_PRIM_FILTER_ENABLE(1)|S_028830_SC_1XMSAA_COMPATIBLE_DISABLE(1));
   REG(R_0286D4_SPI_INTERP_CONTROL_0,S_0286D4_FLAT_SHADE_ENA(1)|S_0286D4_PNT_SPRITE_ENA(1)|S_0286D4_PNT_SPRITE_OVRD_X(V_0286D4_SPI_PNT_SPRITE_SEL_S)|S_0286D4_PNT_SPRITE_OVRD_Y(V_0286D4_SPI_PNT_SPRITE_SEL_T)|S_0286D4_PNT_SPRITE_OVRD_Z(V_0286D4_SPI_PNT_SPRITE_SEL_0)|S_0286D4_PNT_SPRITE_OVRD_W(V_0286D4_SPI_PNT_SPRITE_SEL_1));
   REG(R_028BE4_PA_SU_VTX_CNTL,S_028BE4_PIX_CENTER(1)|S_028BE4_ROUND_MODE(V_028BE4_X_ROUND_TO_EVEN)|S_028BE4_QUANT_MODE(V_028BE4_X_16_8_FIXED_POINT_1_256TH));
   REG(R_028818_PA_CL_VTE_CNTL,S_028818_VTX_W0_FMT(1)|S_028818_VPORT_X_SCALE_ENA(1)|S_028818_VPORT_X_OFFSET_ENA(1)|S_028818_VPORT_Y_SCALE_ENA(1)|S_028818_VPORT_Y_OFFSET_ENA(1)|S_028818_VPORT_Z_SCALE_ENA(1)|S_028818_VPORT_Z_OFFSET_ENA(1));
   REG(R_028828_PA_SU_LINE_STIPPLE_SCALE,0x3f800000);
   REG(R_028424_CB_FDCC_CONTROL,S_028424_DISABLE_CONSTANT_ENCODE_SINGLE(1));
   // Single-sample center location; radv_device.c:970-972,1009-1069.
   REG(R_028BF8_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0,0);
   REG(R_028C08_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y0_0,0);
   REG(R_028C18_PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y1_0,0);
   REG(R_028C28_PA_SC_AA_SAMPLE_LOCS_PIXEL_X1Y1_0,0);
   REG(R_028BD4_PA_SC_CENTROID_PRIORITY_0,0);
   REG(R_028BD8_PA_SC_CENTROID_PRIORITY_1,0);
   REG(R_02882C_PA_SU_PRIM_FILTER_CNTL,S_02882C_XMAX_RIGHT_EXCLUSION(1)|S_02882C_YMAX_BOTTOM_EXCLUSION(1));
   REG(R_028B70_DB_ALPHA_TO_MASK,S_028B70_ALPHA_TO_MASK_OFFSET0(3)|S_028B70_ALPHA_TO_MASK_OFFSET1(1)|S_028B70_ALPHA_TO_MASK_OFFSET2(0)|S_028B70_ALPHA_TO_MASK_OFFSET3(2)|S_028B70_OFFSET_ROUND(1));
   REG(R_028C38_PA_SC_AA_MASK_X0Y0_X1Y0,UINT32_MAX);
   REG(R_028C3C_PA_SC_AA_MASK_X0Y1_X1Y1,UINT32_MAX);
   REG(R_028BE0_PA_SC_AA_CONFIG,0);
   REG(R_0286E0_SPI_BARYC_CNTL,S_0286E0_FRONT_FACE_ALL_BITS(0));
   // No cull/offset/depth/stencil/query, clipping to [0,1], first provoking
   // vertex. Triangle polygon-mode enums are NOT zero-valued.
   REG(R_028814_PA_SU_SC_MODE_CNTL,S_028814_POLYMODE_FRONT_PTYPE(V_028814_X_DRAW_TRIANGLES)|S_028814_POLYMODE_BACK_PTYPE(V_028814_X_DRAW_TRIANGLES));
   REG(R_028810_PA_CL_CLIP_CNTL,S_028810_DX_CLIP_SPACE_DEF(1)|S_028810_DX_LINEAR_ATTR_CLIP_ENA(1));
   REG(R_028A08_PA_SU_LINE_CNTL,S_028A08_WIDTH(8));
   REG(R_028A0C_PA_SC_LINE_STIPPLE,S_028A0C_LINE_PATTERN(0xffff)|S_028A0C_AUTO_RESET_CNTL(2));
   REG(R_028BDC_PA_SC_LINE_CNTL,0);
   REG(R_028040_DB_Z_INFO,S_028040_FORMAT(V_028040_Z_INVALID)|S_028040_NUM_SAMPLES(0));
   REG(R_028044_DB_STENCIL_INFO,S_028044_FORMAT(V_028044_STENCIL_INVALID));
   REG(R_028000_DB_RENDER_CONTROL,S_028000_MAX_ALLOWED_TILES_IN_WAVE(0));
   REG(R_028004_DB_COUNT_CONTROL,0);
   REG(R_028010_DB_RENDER_OVERRIDE2,S_028010_CENTROID_COMPUTATION_MODE(1));
   REG(R_02800C_DB_RENDER_OVERRIDE,S_02800C_DISABLE_VIEWPORT_CLAMP(0));
   REG(R_028800_DB_DEPTH_CONTROL,0);
   REG(R_02842C_DB_STENCIL_CONTROL,0);
   REG(R_02820C_PA_SC_CLIPRECT_RULE,0xffff);
   // VRS is explicitly disabled through the three real state paths.
   REG(R_03098C_GE_VRS_RATE,0);
   REG(R_028848_PA_CL_VRS_CNTL,0);
   REG(R_0283D0_PA_SC_VRS_OVERRIDE_CNTL,S_0283D0_VRS_OVERRIDE_RATE_COMBINER_MODE(V_028064_SC_VRS_COMB_MODE_PASSTHRU));
   REG(R_028A98_VGT_DRAW_PAYLOAD_CNTL,S_028A98_EN_VRS_RATE(1));
   REG(R_030908_VGT_PRIMITIVE_TYPE,V_008958_DI_PT_TRILIST);
   REG(R_030998_VGT_GS_OUT_PRIM_TYPE,V_030998_TRISTRIP);
   // Call real common RB+ logic for the actual admitted 32_ABGR export and
   // fixed color product. RADV applies its source-visible zero-result fallback.
   uint32_t down=0, epsilon=0, control=0;
   ac_set_sx_downconvert_state_for_mrt(GFX11,false,0x2800a,0,9,15,0,&down,&epsilon,&control,NULL);
   if (!down) down=V_028754_SX_RT_EXPORT_32_R;
   REG(R_028754_SX_PS_DOWNCONVERT,down);
   REG(R_028758_SX_BLEND_OPT_EPSILON,epsilon);
   REG(R_02875C_SX_BLEND_OPT_CONTROL,control);
   printf("],\"color_registers\":["); first=true;
   REG(R_028C6C_CB_COLOR0_VIEW,cb.cb_color_view);
   REG(R_028C70_CB_COLOR0_INFO,cb.cb_color_info);
   REG(R_028C74_CB_COLOR0_ATTRIB,cb.cb_color_attrib);
   REG(R_028C78_CB_COLOR0_DCC_CONTROL,cb.cb_dcc_control & C_028C78_FDCC_ENABLE);
   REG(R_028C60_CB_COLOR0_BASE,(uint32_t)cb.cb_color_base);
   REG(R_028E40_CB_COLOR0_BASE_EXT,S_028E40_BASE_256B(cb.cb_color_base>>32));
   REG(R_028C94_CB_COLOR0_DCC_BASE,(uint32_t)cb.cb_dcc_base);
   REG(R_028EA0_CB_COLOR0_DCC_BASE_EXT,S_028EA0_BASE_256B(cb.cb_dcc_base>>32));
   REG(R_028EC0_CB_COLOR0_ATTRIB2,cb.cb_color_attrib2);
   REG(R_028EE0_CB_COLOR0_ATTRIB3,cb.cb_color_attrib3);
   printf("],\"viewport_registers\":["); first=true;
   // Metal's positive logical viewport corresponds to a Vulkan viewport
   // beginning at its bottom with negative height. Call actual RADV math.
   const VkViewport viewport = {0.0f,(float)height,(float)width,-(float)height,0.0f,1.0f};
   float scale[3], translate[3];
   radv_get_viewport_xform(&viewport,scale,translate);
   REG(R_02843C_PA_CL_VPORT_XSCALE,fui(scale[0]));
   REG(R_028440_PA_CL_VPORT_XOFFSET,fui(translate[0]));
   REG(R_028444_PA_CL_VPORT_YSCALE,fui(scale[1]));
   REG(R_028448_PA_CL_VPORT_YOFFSET,fui(translate[1]));
   REG(R_02844C_PA_CL_VPORT_ZSCALE,fui(scale[2]));
   REG(R_028450_PA_CL_VPORT_ZOFFSET,fui(translate[2]));
   printf("]}\n");
   ac_pm4_free_state(pm4);
   return 0;
}
