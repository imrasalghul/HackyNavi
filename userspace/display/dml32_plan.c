// SPDX-License-Identifier: MIT
#include "dml32_plan.h"
#include <math.h>
#include <limits.h>
#include <fenv.h>

static bool positive(double value) { return isfinite(value) && value > 0 && value <= 1000000; }
static bool nonnegative(double value) { return isfinite(value) && value >= 0 && value <= 1000000; }
static bool unsigned_result(double value) { return nonnegative(value) && value <= UINT_MAX && floor(value) == value; }

enum hn_dml_status hn_dml32_calculate(const soc_bounding_box_st* soc,
    const ip_params_st* ip, const display_e2e_pipe_params_st* input, struct hn_dml_plan* out) {
    if (!out) return HN_DML_INVALID_INPUT;
    memset(out, 0, sizeof(*out));
    if (!soc || !ip || !input) return HN_DML_INVALID_INPUT;
#include "finite_fields.inc"
    if (!soc || !ip || !input || fegetround() != FE_TONEAREST ||
        !soc->num_states || soc->num_states >= DC__VOLTAGE_STATES ||
        !soc->num_chans || soc->num_chans > 24 || ip->max_num_dpp != 4 || ip->max_num_otg != 4 ||
        !positive(soc->dram_channel_width_bytes) || !soc->return_bus_width_bytes ||
        !positive(soc->fabric_datapath_to_dcn_data_return_bytes) ||
        !positive(soc->dispclk_dppclk_vco_speed_mhz) ||
        !positive(soc->max_vratio_pre) || soc->max_vratio_pre > 8 || soc->downspread_percent >= 100 ||
        !positive(soc->urgent_latency_pixel_data_only_us) ||
        !positive(input->pipe.dest.pixel_rate_mhz) || !positive(input->clks_cfg.refclk_mhz))
        return HN_DML_INVALID_INPUT;
    for (unsigned i = 0; i < soc->num_states; ++i) {
        const voltage_scaling_st* c = &soc->clock_limits[i];
        if (c->state != i || !positive(c->dcfclk_mhz) || !positive(c->socclk_mhz) ||
            !positive(c->fabricclk_mhz) || !positive(c->dram_speed_mts) ||
            !positive(c->dispclk_mhz) || !positive(c->dppclk_mhz) || !positive(c->phyclk_mhz) ||
            !nonnegative(c->phyclk_d18_mhz) || !nonnegative(c->phyclk_d32_mhz) ||
            !nonnegative(c->dscclk_mhz) || !nonnegative(c->dtbclk_mhz))
            return HN_DML_INVALID_INPUT;
    }
    const display_pipe_source_params_st* s = &input->pipe.src;
    const display_pipe_dest_params_st* d = &input->pipe.dest;
    const scaler_ratio_depth_st* scale = &input->pipe.scale_ratio_depth;
    if (ip->gpuvm_enable || ip->hostvm_enable || s->gpuvm || s->hostvm || s->vm ||
        s->source_format != dm_444_32 || s->sw_mode != dm_sw_linear || s->dcc || s->dcc_use_global ||
        s->source_rotation || s->source_scan || s->is_hsplit || s->num_cursors ||
        s->dynamic_metadata_enable || s->xfc_enable || s->unbounded_req_mode || s->immediate_flip ||
        s->cur0_src_width || s->cur1_src_width ||
        s->use_mall_for_pstate_change || s->use_mall_for_static_screen ||
        d->interlaced || d->odm_combine || d->drr_display ||
        input->dout.dsc_enable || input->dout.wb_enable || input->dout.dp_multistream_en || input->dout.is_virtual ||
        input->dout.output_format != dm_444 ||
        (input->dout.output_type != dm_hdmi && input->dout.output_type != dm_dp) ||
        input->dout.output_bpp != 24 || input->dout.output_bpc != dm_out_8 || scale->scl_enable ||
        scale->hscl_ratio != 1 || scale->vscl_ratio != 1 || scale->lb_depth != dm_lb_19)
        return HN_DML_UNSUPPORTED;
    if (!s->viewport_width || !s->viewport_height || s->viewport_width > 16384 || s->viewport_height > 16384 ||
        s->viewport_width != d->hactive || s->viewport_height != d->vactive ||
        s->viewport_width != d->recout_width || s->viewport_height != d->recout_height ||
        s->surface_width_y < s->viewport_width || s->surface_height_y < s->viewport_height ||
        s->surface_width_y > 65536 || s->surface_height_y > 65536 ||
        s->viewport_x_y > s->surface_width_y - s->viewport_width ||
        s->viewport_y_y > s->surface_height_y - s->viewport_height ||
        s->data_pitch < s->surface_width_y || s->data_pitch > 65536 || s->det_size_override != 1152 ||
        s->gpuvm_min_page_size_kbytes < 4 || s->gpuvm_min_page_size_kbytes > 256 ||
        (s->gpuvm_min_page_size_kbytes & (s->gpuvm_min_page_size_kbytes - 1)) ||
        d->full_recout_width != d->recout_width || d->full_recout_height != d->recout_height ||
        d->htotal <= d->hactive || d->htotal > 32768 || d->vtotal <= d->vactive || d->vtotal > 32768 ||
        d->hblank_start >= d->htotal || d->hblank_end >= d->htotal ||
        d->vblank_start >= d->vtotal || d->vblank_end >= d->vtotal ||
        d->vfront_porch >= d->vtotal - d->vactive || d->hblank_start < d->hblank_end ||
        d->vblank_start < d->vblank_end || d->hblank_start - d->hblank_end != d->hactive ||
        d->vblank_start - d->vblank_end != d->vactive ||
        d->vblank_start != d->vtotal - d->vfront_porch ||
        d->vtotal_min != d->vtotal || d->vtotal_max != d->vtotal ||
        input->clks_cfg.refclk_mhz / d->pixel_rate_mhz >= 4 ||
        input->pipe.scale_taps.htaps != 1 || input->pipe.scale_taps.vtaps != 1)
        return HN_DML_INVALID_INPUT;
    // DML32 evaluates DSC slice geometry before selecting the non-DSC branch.
    if (input->dout.dsc_slices != 1) return HN_DML_INVALID_INPUT;
    if (input->dout.output_type == dm_dp &&
        ((input->dout.dp_lanes != 1 && input->dout.dp_lanes != 2 && input->dout.dp_lanes != 4) ||
        input->dout.dp_rate < dm_dp_rate_hbr || input->dout.dp_rate > dm_dp_rate_hbr3))
        return HN_DML_INVALID_INPUT;

    struct display_mode_lib* lib = calloc(1, sizeof(*lib));
    if (!lib) return HN_DML_NO_MEMORY;
    dml_init_instance(lib, soc, ip, DML_PROJECT_DCN32);
    display_e2e_pipe_params_st pipe = *input;
    pipe.clks_cfg.voltage = 0;
    pipe.clks_cfg.dppclk_mhz = pipe.clks_cfg.dispclk_mhz = 0;
    pipe.clks_cfg.dcfclk_mhz = pipe.clks_cfg.socclk_mhz = 0;
    const unsigned level = dml_get_voltage_level(lib, &pipe, 1);
    const unsigned comb = lib->vba.maxMpcComb;
    hn_dml_log("HackyNavi: level=%u comb=%u validation=%u support=%u\n", level, comb,
        (unsigned)lib->vba.ValidationStatus[level < DC__VOLTAGE_STATES ? level : 0],
        level < DC__VOLTAGE_STATES && comb < 2 ? lib->vba.ModeSupport[level][comb] : 0);
    if (level >= soc->num_states || comb > 1 || !lib->vba.ModeSupport[level][comb]) {
        free(lib); return HN_DML_MODE_REJECTED;
    }
    if (lib->vba.NoOfDPP[level][comb][0] != 1 || lib->vba.ODMCombineEnablePerState[level][0] != dm_odm_combine_mode_disabled) {
        free(lib); return HN_DML_SPLIT_REQUIRED;
    }
    // Pinned DML32 auto-selects link bpp for non-DSC modes, even when the caller
    // requests RGB24. Admit that exact format, never an automatic lower bpp.
    // CalculateOutputLink:1385,1478-1534; TruncToValidBPP:1754,1796-1804.
    const double phy = soc->clock_limits[level].phyclk_mhz;
    if (input->dout.output_type == dm_dp) {
        const double rate = input->dout.dp_rate == dm_dp_rate_hbr ? 2700 :
            input->dout.dp_rate == dm_dp_rate_hbr2 ? 5400 : 8100;
        const double payload = (1 - soc->downspread_percent / 100) * rate * 0.8 * input->dout.dp_lanes;
        if (phy < rate / 10 || d->pixel_rate_mhz * 24 > payload) {
            free(lib); return HN_DML_MODE_REJECTED;
        }
    } else if (d->pixel_rate_mhz > fmin(600, phy)) {
        free(lib); return HN_DML_MODE_REJECTED;
    }
    pipe.clks_cfg.voltage = (int)level;
    pipe.clks_cfg.dcfclk_mhz = lib->vba.DCFCLKState[level][comb];
    pipe.clks_cfg.socclk_mhz = soc->clock_limits[level].socclk_mhz;
    if (!positive(pipe.clks_cfg.dcfclk_mhz) || pipe.clks_cfg.dcfclk_mhz > soc->clock_limits[level].dcfclk_mhz) {
        free(lib); return HN_DML_INVALID_RESULT;
    }
    const double dpp = get_dppclk_calculated(lib, &pipe, 1, 0);
    const double disp = get_dispclk_calculated(lib, &pipe, 1);
    if (!positive(dpp) || !positive(disp) || dpp > soc->clock_limits[level].dppclk_mhz ||
        disp > soc->clock_limits[level].dispclk_mhz) { free(lib); return HN_DML_INVALID_RESULT; }
    pipe.clks_cfg.dppclk_mhz = dpp;
    pipe.clks_cfg.dispclk_mhz = disp;
    struct hn_dml_plan result = {0};
    result.voltage_level = level;
    result.validation = lib->vba.ValidationStatus[level];
    result.urgent_us = get_wm_urgent(lib, &pipe, 1);
    result.stutter_enter_exit_us = get_wm_stutter_enter_exit(lib, &pipe, 1);
    result.stutter_exit_us = get_wm_stutter_exit(lib, &pipe, 1);
    result.memory_trip_us = get_wm_memory_trip(lib, &pipe, 1);
    result.dram_clock_change_us = get_wm_dram_clock_change(lib, &pipe, 1);
    result.fabric_clock_change_us = get_fclk_watermark(lib, &pipe, 1);
    result.retraining_us = get_usr_retraining_watermark(lib, &pipe, 1);
    result.urgent_latency_us = get_urgent_latency(lib, &pipe, 1);
    result.fraction_urgent_bandwidth = get_fraction_of_urgent_bandwidth(lib, &pipe, 1);
    result.fraction_urgent_bandwidth_imm_flip = get_fraction_of_urgent_bandwidth_imm_flip(lib, &pipe, 1);
    const double startup = get_vstartup(lib, &pipe, 1, 0);
    const double offset = get_vupdate_offset(lib, &pipe, 1, 0);
    const double width = get_vupdate_width(lib, &pipe, 1, 0);
    const double ready = get_vready_offset(lib, &pipe, 1, 0);
    const double det = get_det_buffer_size_kbytes(lib, &pipe, 1, 0);
    const double compbuf = get_comp_buffer_size_kbytes(lib, &pipe, 1);
    if (!unsigned_result(startup) || !unsigned_result(offset) || !unsigned_result(width) ||
        !unsigned_result(ready) || !unsigned_result(det) || !unsigned_result(compbuf) ||
        !positive(result.urgent_us) || !nonnegative(result.stutter_enter_exit_us) ||
        !nonnegative(result.stutter_exit_us) || !nonnegative(result.memory_trip_us) ||
        !nonnegative(result.dram_clock_change_us) || !nonnegative(result.fabric_clock_change_us) ||
        !nonnegative(result.retraining_us) || !nonnegative(result.urgent_latency_us) ||
        !nonnegative(result.fraction_urgent_bandwidth) || result.fraction_urgent_bandwidth > 1 ||
        !nonnegative(result.fraction_urgent_bandwidth_imm_flip) || result.fraction_urgent_bandwidth_imm_flip > 1)
        { free(lib); return HN_DML_INVALID_RESULT; }
    pipe.pipe.dest.vstartup_start = (unsigned)startup;
    pipe.pipe.dest.vupdate_offset = (unsigned)offset;
    pipe.pipe.dest.vupdate_width = (unsigned)width;
    pipe.pipe.dest.vready_offset = (unsigned)ready;
    result.det_kbytes = (unsigned)det;
    result.compbuf_kbytes = (unsigned)compbuf;
    lib->funcs.rq_dlg_get_rq_reg_v2(&result.rq, lib, &pipe, 1, 0);
    lib->funcs.rq_dlg_get_dlg_reg_v2(lib, &result.dlg, &result.ttu, &pipe, 1, 0);
    // This adapter admits neither GPUVM nor a second/chroma plane. Pinned
    // CalculateVMRowAndSwath:2113-2126 returns zero VM/DPTE group bytes when
    // VM is off, but rq_dlg_calc_32:124-127 unconditionally log2-encodes them,
    // yielding wrapped uint32 values. Likewise line149 encodes an absent
    // chroma swath. Canonicalize ONLY these inactive requestor settings;
    // active luma geometry/timing stays exactly as calculated. Hardware owner
    // must enforce the same direct-VRAM, single-plane profile before use.
    result.rq.rq_regs_l.dpte_group_size = 0;
    result.rq.rq_regs_l.mpte_group_size = 0;
    memset(&result.rq.rq_regs_c, 0, sizeof(result.rq.rq_regs_c));
    result.pipe = pipe;
    result.dispclk_mhz = disp;
    result.dppclk_mhz = dpp;
    result.dcfclk_mhz = pipe.clks_cfg.dcfclk_mhz;
    result.socclk_mhz = pipe.clks_cfg.socclk_mhz;
    result.dram_speed_mts = lib->vba.DRAMSpeed;
    result.fabricclk_mhz = lib->vba.FabricClock;
    result.dcfclk_deep_sleep_mhz = lib->vba.DCFCLKDeepSleep;
    result.ref_dtbclk_mhz = lib->vba.DTBCLKPerState[level];
    result.dram_clock_change_support = lib->vba.DRAMClockChangeSupport[level][comb];
    result.fabric_clock_change_support = lib->vba.FCLKChangeSupport[level][comb];
    result.retraining_supported = lib->vba.USRRetrainingSupport[level][comb];
    if (!positive(result.dram_speed_mts) || !positive(result.fabricclk_mhz) ||
        !positive(result.dcfclk_deep_sleep_mhz) || !nonnegative(result.ref_dtbclk_mhz))
        { free(lib); return HN_DML_INVALID_RESULT; }
    free(lib);
    *out = result;
    return HN_DML_OK;
}
