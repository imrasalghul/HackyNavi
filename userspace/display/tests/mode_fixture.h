// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DML_MODE_FIXTURE_H
#define HACKYNAVI_DML_MODE_FIXTURE_H
#include "dml32_plan.h"
static display_e2e_pipe_params_st mode1080(void) {
    display_e2e_pipe_params_st p = {0};
    p.pipe.src.source_format = dm_444_32;
    p.pipe.src.sw_mode = dm_sw_linear;
    p.pipe.src.surface_width_y = p.pipe.src.viewport_width = p.pipe.src.viewport_width_max = 1920;
    p.pipe.src.surface_height_y = p.pipe.src.viewport_height = p.pipe.src.viewport_height_max = 1080;
    p.pipe.src.viewport_stationary = true;
    p.pipe.src.data_pitch = 1920;
    p.pipe.src.det_size_override = 1152;
    // DML requires a positive arithmetic granule even with all VM flags off.
    p.pipe.src.gpuvm_min_page_size_kbytes = 4;
    p.pipe.dest.recout_width = p.pipe.dest.full_recout_width = p.pipe.dest.hactive = 1920;
    p.pipe.dest.recout_height = p.pipe.dest.full_recout_height = p.pipe.dest.vactive = 1080;
    p.pipe.dest.htotal = 2200;
    p.pipe.dest.vtotal = p.pipe.dest.vtotal_min = p.pipe.dest.vtotal_max = 1125;
    p.pipe.dest.hblank_start = 2112; p.pipe.dest.hblank_end = 192;
    p.pipe.dest.vblank_start = 1121; p.pipe.dest.vblank_end = 41;
    p.pipe.dest.vfront_porch = 4;
    p.pipe.dest.vblank_nom = 45;
    p.pipe.dest.pixel_rate_mhz = 148.5;
    p.pipe.dest.refresh_rate = 60;
    p.pipe.scale_ratio_depth.hscl_ratio = p.pipe.scale_ratio_depth.vscl_ratio = 1;
    p.pipe.scale_ratio_depth.hscl_ratio_c = p.pipe.scale_ratio_depth.vscl_ratio_c = 1;
    p.pipe.scale_ratio_depth.lb_depth = dm_lb_19;
    p.pipe.scale_taps.htaps = p.pipe.scale_taps.vtaps = 1;
    p.pipe.scale_taps.htaps_c = p.pipe.scale_taps.vtaps_c = 1;
    p.dout.output_type = dm_hdmi;
    p.dout.output_format = dm_444;
    p.dout.output_bpc = dm_out_8;
    p.dout.output_bpp = 24;
    p.dout.dsc_input_bpc = 8;
    p.dout.dsc_slices = 1; // DML computes slice width even when DSC is off.
    p.clks_cfg.refclk_mhz = 50;
    return p;
}

#endif
