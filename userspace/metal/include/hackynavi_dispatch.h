/* SPDX-License-Identifier: MIT */
#ifndef HACKYNAVI_DISPATCH_H
#define HACKYNAVI_DISPATCH_H
#include "hackynavi_shader.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct HNShaderDispatchPlan HNShaderDispatchPlan;
/* All dimensions/counts are u32. The planner has its own additive ABI version;
 * it does not alter compiler ABI v1 or authorize shader execution. */
typedef struct HNShaderDispatchRegion {
    uint32_t local_size[3];
    uint32_t group_count[3];
    uint32_t thread_base[3];
    uint32_t threadgroup_base[3];
    /* Exact-thread variants use0,1,2. Fixed whole-group modules report
     * UINT32_MAX in all three slots: their local size is not specializable. */
    uint32_t local_size_spec_ids[3];
    uint32_t push_constant_offset;
    uint32_t push_constant_size;
    uint32_t push_constants[12];
} HNShaderDispatchRegion;
uint32_t hn_shader_dispatch_abi_version(void);
/* shader must be a live, successful compiler result for a compute kernel.
 * mode uses HNShaderDispatch: grid means GROUP counts in WHOLE_WORKGROUPS and
 * THREAD counts in EXACT_THREADS. It must match the actual reflection contract.
 * local is the nominal threadsPerThreadgroup. Whole-group modules require the
 * exact local size recorded in reflection. Zero local dimensions, overflow,
 * incompatible mode, or AIR max-workgroup violations produce an error plan.
 * A zero grid dimension is an empty successful plan, with no GPU dispatch.
 * No pointers are retained: shader/grid/local may be freed after this call.
 * Returns an owned result on ordinary failure too; inspect error before count.
 * Each exact-thread region needs the REAL ACO variant compiled using its local
 * size/spec IDs. Changing hardware NUM_THREAD alone is never sufficient.
 * Current native fixture accepts only4096threads/local64,1,1; this planner does
 * not expand its admission or claim that tail variants have run on HackyNavi. */
HNShaderDispatchPlan *hn_shader_plan_dispatch(const HNShaderResult *shader,
    uint32_t mode, const uint32_t grid[3], const uint32_t local[3]);
const char *hn_shader_dispatch_plan_error(const HNShaderDispatchPlan *);
uint32_t hn_shader_dispatch_plan_region_count(const HNShaderDispatchPlan *);
/* Copy one region. Returns1 on success,0 otherwise; output is unchanged on
 * failure. Pass exactly sizeof(HNShaderDispatchRegion). Region data has no
 * borrowed memory and remains valid after the plan is freed. Max8regions. */
int hn_shader_dispatch_plan_region(const HNShaderDispatchPlan *, uint32_t index,
    HNShaderDispatchRegion *output, size_t output_size);
void hn_shader_dispatch_plan_free(HNShaderDispatchPlan *);
#ifdef __cplusplus
}
static_assert(sizeof(HNShaderDispatchRegion)==116,"dispatch region ABI");
#endif
#endif
