// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_GFX11_COLOR_TARGET_H
#define HACKYNAVI_GFX11_COLOR_TARGET_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Numeric inputs only: this structure does not attest a hardware observation. */
typedef struct HNColorTopology {
    uint32_t gb_addr_config;
    uint32_t revision_strap;
} HNColorTopology;
typedef struct HNColorRequest { uint32_t width, height; } HNColorRequest;
/* Fixed RGBA8_UNORM, linear, one layer/mip/sample, no auxiliary metadata.
 * Address-free and pointer-free. No allocation or submission authority. */
typedef struct HNColorLayout {
    uint32_t width, height, pitch_pixels, pitch_bytes;
    uint64_t slice_bytes, total_bytes;
    uint32_t base_alignment, swizzle_mode;
    uint32_t cb_color_info, cb_color_view, cb_color_attrib;
    uint32_t cb_color_attrib2, cb_color_attrib3, cb_dcc_control;
} HNColorLayout;
typedef enum HNColorStatus {
    HN_COLOR_OK = 0,
    HN_COLOR_INVALID_ARGUMENT = 1,
    HN_COLOR_UNSUPPORTED_TOPOLOGY = 2,
    HN_COLOR_INVALID_DIMENSIONS = 3,
    HN_COLOR_CALCULATION_FAILED = 4,
    HN_COLOR_INVALID_LAYOUT = 5
} HNColorStatus;

/* Portable fixed-profile input/layout admission. No MMIO or allocation. */
HNColorStatus hn_color_target_validate_inputs(const HNColorTopology*, const HNColorRequest*);
HNColorStatus hn_color_target_validate(const HNColorTopology*, const HNColorRequest*, const HNColorLayout*);
/* Userspace adapter invokes actual pinned Mesa/addrlib. A valid aligned output
 * is cleared on every failure. Null/unaligned output is rejected untouched.
 * Input/output overlap is rejected before input reads; output is cleared.
 * No environment, device open, hardware access or global mutable state. */
HNColorStatus hn_color_target_calculate(const HNColorTopology*, const HNColorRequest*, HNColorLayout*);
#ifdef __cplusplus
}
static_assert(sizeof(HNColorTopology) == 8, "topology ABI");
static_assert(sizeof(HNColorRequest) == 8, "request ABI");
static_assert(sizeof(HNColorLayout) == 64, "layout ABI");
static_assert(offsetof(HNColorLayout, slice_bytes) == 16, "size field ABI");
static_assert(offsetof(HNColorLayout, base_alignment) == 32, "alignment field ABI");
static_assert(offsetof(HNColorLayout, cb_dcc_control) == 60, "CB field ABI");
#endif
#endif
