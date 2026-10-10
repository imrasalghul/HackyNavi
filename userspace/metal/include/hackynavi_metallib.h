/* MIT License; see repository LICENSE. */
#ifndef HACKYNAVI_METALLIB_H
#define HACKYNAVI_METALLIB_H
#include "hackynavi_shader.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct HNMetallibResult HNMetallibResult;
/* Fixed 64-byte ABI v1 record. All reserved values are zero. The digest covers
 * the container's wrapped AIR extent, including padding, NOT just copied AIR.
 * Metadata/digests describe the container; they confer no execution authority.
 * A compiler must independently verify the actual AIR entry point and stage. */
typedef struct HNMetallibFunctionInfo {
    uint32_t struct_bytes;
    uint32_t stage;                  /* HNShaderStage: compute=0, vertex=1, fragment=2 */
    uint32_t name_bytes;             /* UTF-8 bytes INCLUDING one trailing NUL */
    uint32_t air_bytes;              /* Exact raw AIR, excluding wrapper/padding */
    uint16_t air_version[2];
    uint16_t language_version[2];
    uint8_t wrapped_air_sha256[32];
    uint32_t reserved[2];
} HNMetallibFunctionInfo;

/* Additive, independently versioned ABI. Existing shader/pipeline ABI stays v1. */
uint32_t hn_metallib_abi_version(void);
/* Borrow input only for this call; return an owned immutable success/error
 * result. No pointer into caller memory is retained. Supported format and
 * 64 MiB input limit are exactly those of metallib::Library (see METALLIB.md).
 * Errors have nonempty error text and zero functions; valid empty libraries
 * are not supported. Allocation failure may abort, as in the compiler ABI. */
HNMetallibResult *hn_metallib_parse(const uint8_t *input, size_t input_bytes);
/* Error string is borrowed until free, empty on success, NULL for NULL result.
 * Count is zero for NULL/error result. Read-only calls may run concurrently if
 * the caller keeps the result alive; free must not race any call. */
const char *hn_metallib_error(const HNMetallibResult *);
uint32_t hn_metallib_function_count(const HNMetallibResult *);
/* Return 1 on complete copy, otherwise 0 with output unchanged. Metadata needs
 * exact sizeof(HNMetallibFunctionInfo) and aligned output; name/AIR require
 * exact nonzero info.name_bytes/info.air_bytes respectively. No truncation or
 * prefix copy. Names are case-sensitive UTF-8 without embedded NUL. Copies
 * remain owned by the caller and valid after the result is freed.
 * Non-NULL handles must be live handles from parse; every supplied span must
 * reference valid accessible memory. Output must not overlap result storage or
 * borrowed error text. Pointer-span arithmetic is checked, not memory access. */
int32_t hn_metallib_function_info(const HNMetallibResult *, uint32_t index,
    HNMetallibFunctionInfo *out, size_t out_bytes);
int32_t hn_metallib_copy_name(const HNMetallibResult *, uint32_t index,
    char *out, size_t out_bytes);
int32_t hn_metallib_copy_air(const HNMetallibResult *, uint32_t index,
    uint8_t *out, size_t out_bytes);
/* NULL is accepted. Any non-NULL handle must be freed exactly once. */
void hn_metallib_free(HNMetallibResult *);

#ifdef __cplusplus
}
static_assert(sizeof(HNMetallibFunctionInfo) == 64, "metallib function ABI");
static_assert(offsetof(HNMetallibFunctionInfo, wrapped_air_sha256) == 24, "metallib digest ABI");
#else
_Static_assert(sizeof(HNMetallibFunctionInfo) == 64, "metallib function ABI");
_Static_assert(offsetof(HNMetallibFunctionInfo, wrapped_air_sha256) == 24, "metallib digest ABI");
#endif
#endif
