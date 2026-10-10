/* MIT License; see repository LICENSE. */
#ifndef HACKYNAVI_SHADER_H
#define HACKYNAVI_SHADER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct HNShaderResult HNShaderResult;
/* Version 1; struct_size must equal sizeof(HNShaderTools). Paths must name
 * trusted absolute executable files. They are canonicalized and copied; no
 * caller pointer is retained. The caller owns the installation and must keep
 * those executable files immutable through compilation and pipeline builds.
 * This pins names, not file descriptors/signatures, and is not a sandbox. */
typedef struct HNShaderTools {
    uint32_t struct_size;
    uint32_t version;
    const char *air_dis_path;
    const char *spirv_val_path;
} HNShaderTools;
enum HNShaderInput { HN_SHADER_AIR = 0, HN_SHADER_LLVM_TEXT = 1, HN_SHADER_METALLIB = 2 };
enum HNShaderStage { HN_SHADER_COMPUTE = 0, HN_SHADER_VERTEX = 1, HN_SHADER_FRAGMENT = 2 };
enum HNShaderDispatch { HN_SHADER_WHOLE_WORKGROUPS = 0, HN_SHADER_EXACT_THREADS = 1 };
uint32_t hn_shader_abi_version(void);
/* Input memory is borrowed during this call. Tool paths are configured with
 * METAL2VULKAN_LLVM_DIS / METAL2VULKAN_SPIRV_VAL before starting any threads.
 * Exact-thread dispatch requires consuming reflection.kernel_dispatch, its
 * local-size specialization IDs and 48-byte push-constant ABI. Whole-workgroup
 * mode is only for dispatchThreadgroups. raster_samples=0 means unspecified.
 * METALLIB selects the exact entry name and requires matching table/AIR stages.
 * Supported containers are documented in METALLIB.md; other versions fail.
 * Errors return an owned result with error text and zero words. */
HNShaderResult *hn_shader_compile(const uint8_t *input, size_t input_size,
    uint32_t format, uint32_t stage, const char *entry,
    uint32_t local_x, uint32_t local_y, uint32_t local_z,
    uint32_t exact_threads, uint32_t raster_samples);
/* Explicit tools path for an embedded driver. No environment or PATH tool
 * lookup, process-global setting, or thread-local override is used. Both
 * paths are required even for LLVM text. Successful shader results retain
 * their copied paths for later hn_pipeline_build validation. Ordinary errors
 * return the same owned error-result contract as hn_shader_compile. */
HNShaderResult *hn_shader_compile_with_tools(const HNShaderTools *tools,
    const uint8_t *input, size_t input_size, uint32_t format, uint32_t stage,
    const char *entry, uint32_t local_x, uint32_t local_y, uint32_t local_z,
    uint32_t exact_threads, uint32_t raster_samples);
/* Returned pointers are borrowed until result_free; do not free them separately. */
const uint32_t *hn_shader_result_words(const HNShaderResult *, size_t *word_count);
const char *hn_shader_result_reflection(const HNShaderResult *);
/* Actual SPIR-V execution entry for VkPipelineShaderStageCreateInfo::pName. */
const char *hn_shader_result_spirv_entry_point(const HNShaderResult *);
const char *hn_shader_result_error(const HNShaderResult *);
void hn_shader_result_free(HNShaderResult *);
#ifdef __cplusplus
}
#endif
#endif
