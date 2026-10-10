/* SPDX-License-Identifier: MIT */
#ifndef HACKYNAVI_GFX11_NGG_H
#define HACKYNAVI_GFX11_NGG_H
#ifdef __cplusplus
extern "C" {
#endif
/* Offline compiler only. The caller must first validate the whole input with
 * actual SPIRV-Tools for Vulkan 1.2. Output must be a private empty directory;
 * discard all its contents on failure. Zero means an actual ACO product was
 * written, never that any shader, draw or GPU operation was executed. */
int hn_gfx11_ngg_compile_validated_file(const char *path, const char *entry,
                                      const char *private_empty_output_directory);
#ifdef __cplusplus
}
#endif
#endif
