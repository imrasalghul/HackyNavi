// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DML_OS_TYPES_H
#define HACKYNAVI_DML_OS_TYPES_H
#ifdef KERNEL
#error The floating-point DML port is userspace only
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define __maybe_unused __attribute__((unused))
#ifdef __cplusplus
extern "C" {
[[noreturn]] void hn_dml_assert_failed(const char*, const char*, unsigned);
#else
_Noreturn void hn_dml_assert_failed(const char*, const char*, unsigned);
#endif
void hn_dml_log(const char* format, ...);
#define ASSERT(condition) ((condition) ? (void)0 : hn_dml_assert_failed(#condition, __FILE__, __LINE__))
#define ASSERT_CRITICAL(condition) ASSERT(condition)
#define DC_LOG_DML(...) hn_dml_log(__VA_ARGS__)
// Userspace receives a real OS-managed FP context. Enforce the rounding mode
// used by the upstream formulas; never compile those formulas into the kext.
void dc_assert_fp_enabled(void);
#ifdef __cplusplus
}
#endif
#endif
