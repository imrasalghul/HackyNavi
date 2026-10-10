// SPDX-License-Identifier: MIT
#include "hackynavi/gfx11_color_target.h"
#include "ac_gpu_info.h"
#include "ac_surface.h"
#include "ac_descriptors.h"
#include "ac_drm_fourcc.h"
#include "addrlib/src/amdgpu_asic_addr.h"
#include "util/format/u_formats.h"
#include <string.h>
#include <stddef.h>

HNColorStatus hn_color_target_calculate(const HNColorTopology* topology,
    const HNColorRequest* request, HNColorLayout* output) {
    if (!output || (uintptr_t)output % _Alignof(HNColorLayout) ||
        (uintptr_t)output > UINTPTR_MAX - sizeof(*output) + 1)
        return HN_COLOR_INVALID_ARGUMENT;
    const uintptr_t out = (uintptr_t)output;
    const uintptr_t tptr = (uintptr_t)topology, rptr = (uintptr_t)request;
    if ((topology && ((tptr >= out && tptr - out < sizeof(*output)) ||
                      (out >= tptr && out - tptr < sizeof(*topology)))) ||
        (request && ((rptr >= out && rptr - out < sizeof(*output)) ||
                     (out >= rptr && out - rptr < sizeof(*request))))) {
        memset(output, 0, sizeof(*output)); return HN_COLOR_INVALID_ARGUMENT;
    }
    const HNColorStatus status = hn_color_target_validate_inputs(topology, request);
    if (status != HN_COLOR_OK) { memset(output, 0, sizeof(*output)); return status; }
    const HNColorTopology t = *topology;
    const HNColorRequest r = *request;
    memset(output, 0, sizeof(*output));
    struct radeon_info info = {0};
    info.family = CHIP_NAVI31;
    info.family_id = FAMILY_NV3;
    info.chip_external_rev = ((t.revision_strap >> 24) & 15) + 1;
    info.gfx_level = GFX11;
    info.gb_addr_config = t.gb_addr_config;
    info.has_dedicated_vram = true;
    info.has_graphics = true;
    info.has_image_opcodes = true;
    ac_fill_compiler_info(&info, NULL, false);
    struct ac_addrlib* lib = ac_addrlib_create(&info, NULL);
    if (!lib) return HN_COLOR_CALCULATION_FAILED;
    struct ac_surf_config config = {0};
    config.info.width = r.width; config.info.height = r.height;
    config.info.depth = 1; config.info.array_size = 1;
    config.info.samples = 1; config.info.storage_samples = 1;
    config.info.levels = 1; config.info.num_channels = 4;
    config.blk_w = config.blk_h = 1; config.bpe = 4;
    config.surf_flags = RADEON_SURF_DISABLE_DCC | RADEON_SURF_NO_TEXTURE;
    config.modifier = DRM_FORMAT_MOD_INVALID;
    struct radeon_surf surface = {0};
    const int result = ac_compute_surface(lib, &info, &config, RADEON_SURF_MODE_LINEAR_ALIGNED, &surface);
    ac_addrlib_destroy(lib);
    if (result) return HN_COLOR_CALCULATION_FAILED;
    if (!surface.is_linear || surface.u.gfx9.resource_type != RADEON_RESOURCE_2D ||
        surface.u.gfx9.swizzle_mode || surface.u.gfx9.surf_offset || surface.tile_swizzle ||
        surface.meta_size || surface.meta_offset || surface.fmask_size || surface.fmask_offset ||
        surface.cmask_size || surface.cmask_offset || surface.display_dcc_offset ||
        surface.num_meta_levels || surface.u.gfx9.offset[0] || surface.u.gfx9.uses_custom_pitch ||
        surface.u.gfx9.surf_height != r.height || surface.surf_alignment_log2 != 8 ||
        surface.alignment_log2 != 8)
        return HN_COLOR_INVALID_LAYOUT;
    struct ac_cb_state state = {0};
    state.surf = &surface; state.format = PIPE_FORMAT_R8G8B8A8_UNORM;
    state.width = r.width; state.height = r.height;
    state.num_samples = state.num_storage_samples = state.num_levels = 1;
    struct ac_cb_surface initial = {0}, cb = {0};
    ac_init_cb_surface(&info, &state, &initial);
    const struct ac_mutable_cb_state mutable = {
        .surf = &surface, .cb = &initial, .num_samples = 1
    };
    ac_set_mutable_cb_surface_fields(&info, &mutable, &cb);
    if (cb.cb_color_base || cb.cb_dcc_base || cb.cb_color_cmask || cb.cb_color_fmask)
        return HN_COLOR_INVALID_LAYOUT;
    const HNColorLayout candidate = {
        r.width, r.height, surface.u.gfx9.surf_pitch, surface.u.gfx9.surf_pitch * 4,
        surface.u.gfx9.surf_slice_size, surface.total_size, 1u << surface.alignment_log2,
        surface.u.gfx9.swizzle_mode, cb.cb_color_info, cb.cb_color_view, cb.cb_color_attrib,
        cb.cb_color_attrib2, cb.cb_color_attrib3, cb.cb_dcc_control
    };
    const HNColorStatus validated = hn_color_target_validate(&t, &r, &candidate);
    if (validated != HN_COLOR_OK) return validated;
    *output = candidate;
    return HN_COLOR_OK;
}
