#ifndef HACKYNAVI_GFX11_H
#define HACKYNAVI_GFX11_H

#include <stdint.h>

#if defined(_WIN32)
#define HN_GFX11_API __declspec(dllexport)
#else
#define HN_GFX11_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

HN_GFX11_API uint32_t hn_gfx11_abi_version(void);
HN_GFX11_API int hn_gfx11_raw_buffer_descriptor(uint64_t gpu_va, uint32_t size, uint32_t descriptor[4]);

/* Input must already have passed real SPIRV-Tools validation for Vulkan 1.2.
 * The caller owns all strings and the three local-size values for the call.
 * The output directory must be an existing private temporary directory.
 * Return 0 means shader.bin, shader.json and aco.txt are complete; publish them
 * together only after success. Any nonzero return requires discarding outputs.
 * The function retains no caller memory and never submits GPU work.
 */
HN_GFX11_API int hn_gfx11_compile_validated_file(const char *spirv_path, const char *entry,
                                               const uint32_t local[3], const char *temporary_output_directory);

#ifdef __cplusplus
}
#endif

#endif
