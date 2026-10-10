// SPDX-License-Identifier: MIT
#include "hackynavi/gfx11_color_target.h"
#include <stddef.h>

namespace {
template<class T> bool aligned(const T* p) {
    return p && (reinterpret_cast<uintptr_t>(p) % alignof(T)) == 0 &&
        reinterpret_cast<uintptr_t>(p) <= UINTPTR_MAX - sizeof(T) + 1;
}
}
extern "C" HNColorStatus hn_color_target_validate_inputs(const HNColorTopology* t, const HNColorRequest* r) {
    if (!aligned(t) || !aligned(r)) return HN_COLOR_INVALID_ARGUMENT;
    if (!r->width || !r->height || r->width > 16384 || r->height > 16384)
        return HN_COLOR_INVALID_DIMENSIONS;
    // Pinned Gfx11Lib::HwlInitGlobalParams requires 256B interleave and these
    // pipe/packer relationships. Its 75-entry swizzle tables additionally
    // bound packers to <=5: (pipes + 2*packers - 2)*5 + element <=74.
    // AddrCreate initializes these tables even for linear requests.
    const uint32_t pipes = t->gb_addr_config & 7;
    const uint32_t packers = (t->gb_addr_config >> 8) & 7;
    const uint32_t revision = (t->revision_strap >> 24) & 15;
    if (!t->gb_addr_config || t->gb_addr_config == UINT32_MAX ||
        t->revision_strap == UINT32_MAX || (t->gb_addr_config & 0x38) ||
        pipes > 6 || packers > 5 || packers > pipes || pipes - packers > 2 || revision >= 15)
        return HN_COLOR_UNSUPPORTED_TOPOLOGY;
    return HN_COLOR_OK;
}
extern "C" HNColorStatus hn_color_target_validate(const HNColorTopology* t,
    const HNColorRequest* r, const HNColorLayout* s) {
    if (!aligned(s)) return HN_COLOR_INVALID_ARGUMENT;
    const auto status = hn_color_target_validate_inputs(t, r);
    if (status != HN_COLOR_OK) return status;
    // Actual gfx11addrlib linear one-mip equation: pitch aligns 32bpp pixels
    // to 64, height stays unchanged, base alignment 256 bytes. All products
    // below are bounded by the prior 16384x16384 profile admission.
    const uint32_t pitch = (r->width + 63u) & ~63u;
    const uint64_t bytes = uint64_t(pitch) * 4u * r->height;
    // Pinned ac_init_gfx10_cb_surface + ac_set_mutable_cb_surface_fields:
    // RGBA8 format10, UNORM0/STD0, BLEND_CLAMP15, SIMPLE_FLOAT17;
    // 2D resource1, width/height minus1, no layer/mip/sample/metadata enable;
    // DCC configuration 256B uncomp/128B comp/32B min/128B independent,
    // with FDCC_ENABLE clear. Configuration is not compression permission.
    if (s->width != r->width || s->height != r->height || s->pitch_pixels != pitch ||
        s->pitch_bytes != pitch * 4 || s->slice_bytes != bytes || s->total_bytes != bytes ||
        s->base_alignment != 256 || s->swizzle_mode != 0 ||
        s->cb_color_info != 0x2800a || s->cb_color_view || s->cb_color_attrib ||
        s->cb_color_attrib2 != ((r->width - 1) << 14 | (r->height - 1)) ||
        s->cb_color_attrib3 != 0x01000000 || s->cb_dcc_control != 0x428)
        return HN_COLOR_INVALID_LAYOUT;
    return HN_COLOR_OK;
}
