// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_GFX11_VERTEX_BUFFER_H
#define HACKYNAVI_GFX11_VERTEX_BUFFER_H
#include <stdint.h>
#if defined(_WIN32) && defined(HN_GFX11_VERTEX_BUFFER_SHARED)
#define HN_GFX11_VERTEX_BUFFER_API __declspec(dllexport)
#else
#define HN_GFX11_VERTEX_BUFFER_API
#endif
#ifdef __cplusplus
extern "C" {
#endif
// Offline compiler only. Caller must first validate immutable SPIR-V with
// actual SPIRV-Tools Vulkan1.2 and serialize calls sharing Mesa's globals.
// Output directory must be private and empty. Discard partial output on error.
// Genuine input must read position float4 from readonly buffer0 at VertexIndex.
// Its 48-byte bound requires the later trusted draw owner to issue exactly
// vertices 0..2 with base_vertex0; this function grants no GPU binding authority.
HN_GFX11_VERTEX_BUFFER_API int hn_gfx11_vertex_buffer_compile_validated_file(const char *spirv_path,
                                                 const char *entry,
                                                 const char *private_empty_output_dir);
// Reference using actual Mesa raw descriptor generation, not an input to the
// shader compiler. Nonzero aligned48-bit address, exactly48 bytes; failure2
// preserves output. The kernel must independently derive its owned descriptor.
HN_GFX11_VERTEX_BUFFER_API int hn_gfx11_vertex_buffer_descriptor(uint64_t address, uint32_t output[4]);
#ifdef __cplusplus
}
#endif
#endif
