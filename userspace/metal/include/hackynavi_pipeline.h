/* SPDX-License-Identifier: MIT */
#ifndef HACKYNAVI_PIPELINE_H
#define HACKYNAVI_PIPELINE_H
#include "hackynavi_dispatch.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct HNPipelineResult HNPipelineResult;
typedef struct HNPipelineInfo {
    uint32_t variant_count, region_count, resource_count;
    uint32_t descriptor_table_bytes, push_constant_bytes;
    uint32_t descriptor_set, descriptor_stride_bytes, target_gfx;
} HNPipelineInfo;
typedef struct HNPipelineVariant {
    uint32_t local_size[3], code_bytes, executable_bytes, code_alignment;
    uint32_t compute_pgm_rsrc[3], dispatch_initiator_required_bits;
    uint32_t num_sgprs, num_vgprs, descriptor_binding_mask;
    uint32_t descriptor_read_mask, descriptor_write_mask;
    uint32_t descriptor_table_bytes, push_constant_bytes, wave_size;
} HNPipelineVariant;
typedef struct HNPipelineResource {
    uint32_t descriptor_set, binding, metal_index;
    /* Access bits: READ=1, WRITE=2, UNUSED=0. Actual emitted SPIR-V effects
     * are conservative across variants; reflected_access is the AIR contract.
     * declared_element_bytes is metadata, NEVER a bound for an entire buffer.
     * Full footprint/extent reflection is retained in the REFLECTION blob. */
    uint32_t access, declared_element_bytes, reflected_access;
} HNPipelineResource;
typedef struct HNPipelineRegion {
    uint32_t variant_index;
    HNShaderDispatchRegion dispatch;
} HNPipelineRegion;
enum HNPipelineBlob {
    HN_PIPELINE_CODE=0, HN_PIPELINE_ACO_METADATA=1,
    HN_PIPELINE_SPIRV=2, HN_PIPELINE_REFLECTION=3, HN_PIPELINE_SPIRV_ENTRY=4
};
uint32_t hn_pipeline_abi_version(void);
/* Synchronous offline build using the live successful shader's real planner.
 * mode/grid/local have the same meaning as hn_shader_plan_dispatch. Compile
 * every distinct actual local-size region through the trusted ACO executable.
 * An empty grid is rejected: it has no executable product. Any variant failure
 * discards every partial product. This never opens a GPU or creates MTLDevice.
 * compiler is an absolute UTF-8 path to our REAL trusted hackynavi-aco CLI.
 * Tool environment overrides must be fixed before any compiler threads start.
 * The caller must keep all inputs live until return; no input pointer survives
 * the call. Owned successful products survive freeing the original shader.
 * Ordinary failure returns an owned error handle; free it exactly once too.
 * At most8 variants, 1MiB code/variant, 64KiB metadata/variant. ACO compilation
 * shares a120s deadline (validation has its existing separate tool deadlines).
 * Process execution is not a sandbox and metadata is not an ISA safety proof.
 * Supported contract: GFX1100 wave32; singleton ordinary SSBOs at set0/0..31;
 * at most1024 threads/group; zero scratch/LDS/spills/relocations/constants;
 * descriptor16B stride with two64-bit user-SGPR pointers; dispatch push48B at0.
 * Rejected pointer forms include calls, escapes, cycles and atomics. Native
 * code admission is unchanged: this API does NOT make these kernels runnable
 * on the existing fixed-fixture kext transport. */
HNPipelineResult *hn_pipeline_build(const HNShaderResult *, uint32_t mode,
    const uint32_t grid[3], const uint32_t local[3], const char *compiler);
/* Only the error string is borrowed; valid until result is freed. All handles
 * must be live and cannot be freed concurrently with any accessor. Immutable
 * successful products may be read concurrently. NULL error pointer for NULL.
 * An empty error string means success; NULL input handles never mean success. */
const char *hn_pipeline_error(const HNPipelineResult *);
/* Exact-size copies:1 success,0 failure; output remains unchanged on failure. */
int hn_pipeline_info(const HNPipelineResult *, HNPipelineInfo *, size_t);
int hn_pipeline_variant(const HNPipelineResult *, uint32_t, HNPipelineVariant *, size_t);
int hn_pipeline_region(const HNPipelineResult *, uint32_t, HNPipelineRegion *, size_t);
int hn_pipeline_resource(const HNPipelineResult *, uint32_t, HNPipelineResource *, size_t);
/* CODE/ACO_METADATA index a variant; other blobs require index0. Unknown or
 * error result gives size0. Text is UTF-8 without a terminating NUL. Caller
 * owns destination memory; never aliases the result. Query then exact-size
 * copy is stable for the result's lifetime. No borrowed code/data pointers. */
size_t hn_pipeline_blob_size(const HNPipelineResult *, uint32_t kind, uint32_t index);
int hn_pipeline_copy_blob(const HNPipelineResult *, uint32_t kind, uint32_t index,
    void *output, size_t exact_bytes);
void hn_pipeline_free(HNPipelineResult *);
#ifdef __cplusplus
}
static_assert(sizeof(HNPipelineInfo)==32,"pipeline info ABI");
static_assert(sizeof(HNPipelineVariant)==72,"pipeline variant ABI");
static_assert(sizeof(HNPipelineResource)==24,"pipeline resource ABI");
static_assert(sizeof(HNPipelineRegion)==120,"pipeline region ABI");
#endif
#endif
