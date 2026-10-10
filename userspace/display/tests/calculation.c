// SPDX-License-Identifier: MIT
#include "dml32_plan.h"
#include "reference_profile.h"
#include <math.h>
#include <stdio.h>

#include "mode_fixture.h"

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    soc_bounding_box_st soc = reference_soc;
    // Driver policy in dcn32_determine_max_vratio_prefetch; not a clock value.
    soc.max_vratio_pre = 4;
    ip_params_st ip = reference_ip;
    display_e2e_pipe_params_st pipe = mode1080();
    struct hn_dml_plan plan;
    enum hn_dml_status status = hn_dml32_calculate(&soc, &ip, &pipe, &plan);
    printf("1080p calculation status=%d\n", status);
    ASSERT(status == HN_DML_OK);
    ASSERT(plan.det_kbytes == 1152 && plan.compbuf_kbytes == 128);
    ASSERT(plan.urgent_us > 0 && plan.dispclk_mhz >= 148.5 && plan.dppclk_mhz > 0);
    ASSERT(plan.pipe.pipe.dest.vstartup_start > 0 && plan.ttu.min_ttu_vblank > 0);
    ASSERT(plan.urgent_latency_us > 0 && plan.fraction_urgent_bandwidth > 0);
    ASSERT(plan.rq.rq_regs_l.dpte_group_size == 0 && plan.rq.rq_regs_l.mpte_group_size == 0);
    display_data_rq_regs_st no_chroma = {0};
    ASSERT(memcmp(&plan.rq.rq_regs_c, &no_chroma, sizeof(no_chroma)) == 0);
    printf("DML32: DET=%u KiB compbuf=%u KiB DISPCLK=%.9f DPPCLK=%.9f MHz urgent=%.9f us\n",
        plan.det_kbytes, plan.compbuf_kbytes, plan.dispclk_mhz, plan.dppclk_mhz, plan.urgent_us);
    const double first_urgent = plan.urgent_us;
    const double first_disp = plan.dispclk_mhz;
    soc.urgent_latency_pixel_data_only_us += 10;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_OK);
    ASSERT(plan.urgent_us >= first_urgent + 9.99);
    soc = reference_soc; soc.max_vratio_pre = 4;
    pipe.pipe.src.surface_width_y = pipe.pipe.src.viewport_width = pipe.pipe.src.viewport_width_max = 3840;
    pipe.pipe.src.surface_height_y = pipe.pipe.src.viewport_height = pipe.pipe.src.viewport_height_max = 2160;
    pipe.pipe.src.data_pitch = 3840;
    pipe.pipe.dest.recout_width = pipe.pipe.dest.full_recout_width = pipe.pipe.dest.hactive = 3840;
    pipe.pipe.dest.recout_height = pipe.pipe.dest.full_recout_height = pipe.pipe.dest.vactive = 2160;
    pipe.pipe.dest.htotal = 4400; pipe.pipe.dest.hblank_start = 4224; pipe.pipe.dest.hblank_end = 384;
    pipe.pipe.dest.vtotal = pipe.pipe.dest.vtotal_min = pipe.pipe.dest.vtotal_max = 2250;
    pipe.pipe.dest.vblank_start = 2242; pipe.pipe.dest.vblank_end = 82;
    pipe.pipe.dest.vfront_porch = 8; pipe.pipe.dest.vblank_nom = 90;
    pipe.pipe.dest.pixel_rate_mhz = 594;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_OK);
    ASSERT(plan.dispclk_mhz > first_disp * 3.9);
    ASSERT(plan.det_kbytes == 1152 && plan.compbuf_kbytes == 128);
    printf("2160p calculation: DISPCLK=%.9f DPPCLK=%.9f MHz\n", plan.dispclk_mhz, plan.dppclk_mhz);
    ASSERT(plan.rq.rq_regs_l.dpte_group_size == 0 && plan.rq.rq_regs_l.mpte_group_size == 0);
    ASSERT(memcmp(&plan.rq.rq_regs_c, &no_chroma, sizeof(no_chroma)) == 0);
    // DML's automatic link-bpp selection used to accept this as 18bpp even
    // though the requested 594MHz RGB24 stream exceeds two HBR3 lanes.
    pipe.dout.output_type = dm_dp; pipe.dout.dp_lanes = 2; pipe.dout.dp_rate = dm_dp_rate_hbr3;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_MODE_REJECTED);
    struct hn_dml_plan empty = {0}; ASSERT(memcmp(&plan, &empty, sizeof(plan)) == 0);
    pipe.dout.dp_lanes = 4;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_OK);
    pipe = mode1080(); pipe.dout.output_type = dm_dp; pipe.dout.dp_lanes = 4; pipe.dout.dp_rate = dm_dp_rate_hbr2;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_OK);
    pipe = mode1080();
    soc = reference_soc; soc.max_vratio_pre = 4;
    soc.clock_limits[0].dcfclk_mhz = 0;
    memset(&plan, 0xa5, sizeof(plan));
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_INVALID_INPUT);
    ASSERT(memcmp(&plan, &empty, sizeof(plan)) == 0);
    soc = reference_soc; soc.max_vratio_pre = 4; pipe.pipe.dest.pixel_rate_mhz = NAN;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_INVALID_INPUT);
    pipe = mode1080(); pipe.pipe.src.gpuvm = true;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_UNSUPPORTED);
    pipe = mode1080(); pipe.pipe.src.dcc_use_global = true;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_UNSUPPORTED);
    pipe = mode1080(); pipe.pipe.src.cur0_src_width = 64;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_UNSUPPORTED);
    pipe = mode1080(); pipe.pipe.src.gpuvm_min_page_size_kbytes = 0;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_INVALID_INPUT);
    pipe = mode1080(); pipe.dout.dsc_slices = 0;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_INVALID_INPUT);
    pipe = mode1080(); pipe.pipe.src.gpuvm_min_page_size_kbytes = 2;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_INVALID_INPUT);
    pipe = mode1080(); pipe.dout.output_bpc = dm_out_10;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_UNSUPPORTED);
    pipe = mode1080(); ++pipe.pipe.dest.vfront_porch;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_INVALID_INPUT);
    pipe = mode1080(); ++pipe.pipe.dest.vtotal_max;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_INVALID_INPUT);
    pipe = mode1080(); --pipe.pipe.dest.vtotal_min;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_INVALID_INPUT);
    pipe = mode1080(); soc.downspread_percent = 100;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_INVALID_INPUT);
    soc = reference_soc; soc.max_vratio_pre = 4;
    pipe = mode1080(); soc.num_states = DC__VOLTAGE_STATES;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_INVALID_INPUT);
    soc = reference_soc; soc.max_vratio_pre = 4; soc.sr_exit_time_us = NAN;
    ASSERT(hn_dml32_calculate(&soc, &ip, &pipe, &plan) == HN_DML_INVALID_INPUT);
    puts("Real unmodified DML32 calculation, latency response, and input rejection passed; no hardware programmed.");
    return 0;
}
