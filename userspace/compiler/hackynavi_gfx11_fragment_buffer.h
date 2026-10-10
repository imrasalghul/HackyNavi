// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_GFX11_FRAGMENT_BUFFER_H
#define HACKYNAVI_GFX11_FRAGMENT_BUFFER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Additive offline fragment-buffer ABI. Input must pass real SPIRV-Tools
// Vulkan1.2 validation before calling. Exactly one read-only buffer0/set0,
// bytes [0,16), copied component-for-component to full RGBA32 MRT0. Native
// owner must bind a validated 16-byte raw descriptor in s0..3; USER_SGPR=4.
// No native admission/submission or resource-free profile expansion is made.
// Caller owns a private empty output directory and discards it on any failure.
// Calls into Mesa must be serialized with other adapter calls.
int hn_gfx11_fragment_buffer_compile_validated_file(const char *spirv_path,
                                           const char *entry,
                                           const char *temporary_output_directory);
// Offline reference for the exact descriptor ABI, using actual Mesa. Requires
// nonzero 16-byte-aligned 48-bit address and the exact 16-byte resource extent.
// Returns 0 on success, 2 otherwise; output is unchanged on failure. This is
// not permission to submit caller-provided descriptors to the kernel.
int hn_gfx11_fragment_buffer_descriptor(uint64_t address, uint32_t output[4]);
#ifdef __cplusplus
}
#endif
#endif
