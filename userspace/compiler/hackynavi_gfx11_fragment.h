// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_GFX11_FRAGMENT_H
#define HACKYNAVI_GFX11_FRAGMENT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Offline, separate fragment ABI. The file must already have passed the real
// SPIRV-Tools Vulkan1.2 validator. Only one constant float32 vec4 color output
// at location0 is admitted. No resources, varying inputs or GPU submission.
// Output directory is caller-owned, private and empty; discard it on failure.
// This does not alter the existing compute C ABI or its compiler executable.
int hn_gfx11_fragment_compile_validated_file(const char *spirv_path,
                                           const char *entry,
                                           const char *temporary_output_directory);
#ifdef __cplusplus
}
#endif
#endif
