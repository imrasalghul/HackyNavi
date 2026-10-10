// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DML32_PLAN_H
#define HACKYNAVI_DML32_PLAN_H
#include "vendor/dml/display_mode_lib.h"
#ifdef __cplusplus
extern "C" {
#endif
enum hn_dml_status {
    HN_DML_OK, HN_DML_INVALID_INPUT, HN_DML_UNSUPPORTED, HN_DML_NO_MEMORY,
    HN_DML_MODE_REJECTED, HN_DML_SPLIT_REQUIRED, HN_DML_INVALID_RESULT
};
struct hn_dml_plan {
    unsigned voltage_level;
    enum dm_validation_status validation;
    display_e2e_pipe_params_st pipe;
    display_rq_regs_st rq;
    display_dlg_regs_st dlg;
    display_ttu_regs_st ttu;
    unsigned det_kbytes, compbuf_kbytes;
    double dispclk_mhz, dppclk_mhz, dcfclk_mhz, socclk_mhz;
    double dram_speed_mts, fabricclk_mhz, dcfclk_deep_sleep_mhz;
    // Selected-state reference; legacy DP/TMDS do not require a DTB clock.
    double ref_dtbclk_mhz;
    bool dtbclk_required;
    enum clock_change_support dram_clock_change_support;
    enum dm_fclock_change_support fabric_clock_change_support;
    bool retraining_supported;
    double urgent_us, stutter_enter_exit_us, stutter_exit_us, memory_trip_us;
    double dram_clock_change_us, fabric_clock_change_us, retraining_us;
    double urgent_latency_us, fraction_urgent_bandwidth, fraction_urgent_bandwidth_imm_flip;
};
// In-process, trusted-profile calculation interface, not a wire/kernel ABI.
// Supply the real DCN3.2 IP and board/firmware bounding box. This port does not
// populate missing clocks from the reference spreadsheet. Inputs and outputs
// are CPU data only; output confers no power, memory, or MMIO authority.
// Initially supports one progressive, unscaled RGB32 linear direct-VRAM pipe,
// without DCC, MALL, GPUVM, DSC, writeback, cursor, ODM or MPC splitting.
// Upstream arithmetic invariants remain fatal assertions. Execute a future
// external mode-request service in a separate process with a timeout; this
// internal API is not yet an untrusted-input boundary.
enum hn_dml_status hn_dml32_calculate(const soc_bounding_box_st*,
    const ip_params_st*, const display_e2e_pipe_params_st*, struct hn_dml_plan*);
#ifdef __cplusplus
}
#endif
#endif
