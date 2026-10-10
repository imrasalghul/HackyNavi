// SPDX-License-Identifier: MIT
// Native userspace support for the unmodified AMD DML32 formulas.
#include "vendor/dml/display_mode_lib.h"
#include "vendor/dml/dcn32/display_mode_vba_32.h"
#include "vendor/dml/dcn32/display_rq_dlg_calc_32.h"
#include <stdio.h>
#include <stdarg.h>
#include <fenv.h>

void dc_assert_fp_enabled(void) { ASSERT(fegetround() == FE_TONEAREST); }

void hn_dml_log(const char* format, ...) {
    // Optional upstream diagnostic output; calculations never depend on it.
    if (!getenv("HACKYNAVI_DML_TRACE")) return;
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

_Noreturn void hn_dml_assert_failed(const char* expression, const char* file, unsigned line) {
    fprintf(stderr, "DML assertion failed: %s (%s:%u)\n", expression, file, line);
    abort();
}

// DCN32-only dispatch corresponding to upstream display_mode_lib.c. Other
// ASIC models are not linked into this port and must not silently use DCN32.
void dml_init_instance(struct display_mode_lib* lib,
    const struct _vcs_dpi_soc_bounding_box_st* soc,
    const struct _vcs_dpi_ip_params_st* ip, enum dml_project project) {
    ASSERT(lib && soc && ip && project == DML_PROJECT_DCN32);
    dc_assert_fp_enabled();
    memset(lib, 0, sizeof(*lib));
    lib->soc = *soc;
    lib->ip = *ip;
    lib->project = project;
    lib->funcs.validate = dml32_ModeSupportAndSystemConfigurationFull;
    lib->funcs.recalculate = dml32_recalculate;
    lib->funcs.rq_dlg_get_dlg_reg_v2 = dml32_rq_dlg_get_dlg_reg;
    lib->funcs.rq_dlg_get_rq_reg_v2 = dml32_rq_dlg_get_rq_reg;
}
