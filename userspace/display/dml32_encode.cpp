// SPDX-License-Identifier: MIT
#include "dml32_encode.hpp"
#include <cmath>
#include <limits>
namespace hackynavi { namespace displaycalculation { namespace {
namespace b=displaybandwidth;
static_assert(b::kFieldCount==70,"Update the complete named DML field mapping");
bool valid(const Calculation& c) {
    return c.status==HN_DML_OK&&c.result&&c.result->validation==DML_VALIDATION_OK&&
        c.identity.deviceGeneration&&c.identity.modeGeneration&&
        c.identity.clockGeneration&&c.identity.calculationId;
}
bool sameSnapshot(const b::Identity& a,const b::Identity& z) {
    return a.deviceGeneration==z.deviceGeneration&&a.modeGeneration==z.modeGeneration&&
        a.clockGeneration==z.clockGeneration;
}
bool scale(double v,double factor,bool roundUp,uint32_t& out) {
    if(!std::isfinite(v)||v<0)return false;
    const double product=v*factor;
    if(!std::isfinite(product)||product>UINT32_MAX)return false;
    out=uint32_t(roundUp?std::ceil(product):std::floor(product));
    return true;
}
bool clock(double v,uint32_t& out) { return v>0&&scale(v,1000,true,out)&&out; }
bool clocks(const hn_dml_plan& p,Clocks& c) {
    c.voltageLevel=p.voltage_level;c.dtbRequired=p.dtbclk_required;
    return p.voltage_level<DC__VOLTAGE_STATES&&
        p.pipe.clks_cfg.voltage==int(p.voltage_level)&&
        p.pipe.clks_cfg.dispclk_mhz==p.dispclk_mhz&&p.pipe.clks_cfg.dppclk_mhz==p.dppclk_mhz&&
        p.pipe.clks_cfg.dcfclk_mhz==p.dcfclk_mhz&&p.pipe.clks_cfg.socclk_mhz==p.socclk_mhz&&
        clock(p.dispclk_mhz,c.dispKHz)&&clock(p.dppclk_mhz,c.dppKHz)&&
        clock(p.dcfclk_mhz,c.dcfKHz)&&clock(p.socclk_mhz,c.socKHz)&&
        clock(p.fabricclk_mhz,c.fabricKHz)&&clock(p.dcfclk_deep_sleep_mhz,c.deepSleepDcfKHz)&&
        clock(p.dram_speed_mts,c.dramKTransfersPerSecond)&&scale(p.ref_dtbclk_mhz,1000,true,c.referenceDtbKHz);
}
bool supported(const hn_dml_plan& p) {
    const auto& s=p.pipe.pipe.src;const auto& d=p.pipe.pipe.dest;const auto& o=p.pipe.dout;
    const auto& r=p.pipe.pipe.scale_ratio_depth;const auto& t=p.pipe.pipe.scale_taps;
    return !p.dtbclk_required&&s.source_format==dm_444_32&&s.sw_mode==dm_sw_linear&&
        !s.vm&&!s.gpuvm&&!s.hostvm&&!s.dcc&&!s.dcc_use_global&&!s.source_rotation&&!s.source_scan&&
        !s.is_hsplit&&!s.num_cursors&&!s.cur0_src_width&&!s.cur1_src_width&&!s.dynamic_metadata_enable&&
        !s.xfc_enable&&!s.unbounded_req_mode&&!s.immediate_flip&&!s.use_mall_for_pstate_change&&
        !s.use_mall_for_static_screen&&!d.interlaced&&!d.odm_combine&&!d.drr_display&&
        !o.dsc_enable&&!o.wb_enable&&!o.dp_multistream_en&&!o.is_virtual&&
        (o.output_type==dm_hdmi||o.output_type==dm_dp)&&o.output_format==dm_444&&
        o.output_bpp==24&&o.output_bpc==dm_out_8&&!r.scl_enable&&r.hscl_ratio==1&&r.vscl_ratio==1&&
        r.lb_depth==dm_lb_19&&t.htaps==1&&t.vtaps==1;
}
bool sameMode(const hn_dml_plan& a,const hn_dml_plan& z) {
    // Compare named input values, never structure padding or calculated
    // per-state DLG/clock/global-sync values that legitimately differ.
#define SAME(member) if(a.pipe.member!=z.pipe.member)return false
    SAME(pipe.src.viewport_width);SAME(pipe.src.viewport_height);SAME(pipe.src.viewport_x_y);
    SAME(pipe.src.viewport_y_y);SAME(pipe.src.viewport_stationary);SAME(pipe.src.surface_width_y);
    SAME(pipe.src.surface_height_y);SAME(pipe.src.data_pitch);SAME(pipe.src.det_size_override);
    SAME(pipe.dest.hactive);SAME(pipe.dest.vactive);SAME(pipe.dest.htotal);SAME(pipe.dest.vtotal);
    SAME(pipe.dest.hblank_start);SAME(pipe.dest.hblank_end);SAME(pipe.dest.vblank_start);
    SAME(pipe.dest.vblank_end);SAME(pipe.dest.vfront_porch);SAME(pipe.dest.vblank_nom);
    SAME(pipe.dest.vtotal_min);SAME(pipe.dest.vtotal_max);SAME(pipe.dest.pixel_rate_mhz);
    SAME(pipe.dest.recout_width);SAME(pipe.dest.recout_height);SAME(pipe.dest.full_recout_width);
    SAME(pipe.dest.full_recout_height);SAME(clks_cfg.refclk_mhz);
    SAME(dout.output_type);SAME(dout.dp_lanes);SAME(dout.dp_rate);SAME(dout.output_bpc);SAME(dout.output_bpp);
#undef SAME
    return true;
}
bool watermarks(const Calculation& c,b::Watermarks& w) {
    const auto& p=*c.result;w.sourceId=c.identity.calculationId;
    return p.fraction_urgent_bandwidth<=1&&p.fraction_urgent_bandwidth_imm_flip<=1&&
        scale(p.urgent_us,1000,false,w.urgentNs)&&
        scale(p.fraction_urgent_bandwidth_imm_flip,1000,false,w.fracUrgentFlipPerThousand)&&
        scale(p.fraction_urgent_bandwidth,1000,false,w.fracUrgentNomPerThousand)&&
        scale(p.urgent_latency_us,1000,false,w.urgentLatencyNs)&&
        scale(p.stutter_enter_exit_us,1000,false,w.stutterEnterExitNs)&&
        scale(p.stutter_exit_us,1000,false,w.stutterExitNs)&&
        scale(p.dram_clock_change_us,1000,false,w.dramChangeNs)&&
        scale(p.fabric_clock_change_us,1000,false,w.fabricChangeNs)&&
        scale(p.retraining_us,1000,false,w.retrainingNs);
}
} // namespace
Status encode(const Calculation& timing,const Calculation (&sets)[4],const Binding& binding,Encoding& out) {
    out={};
    if(!valid(timing))return Status::InvalidCalculation;
    const auto& p=*timing.result;
    if(!supported(p))return Status::UnsupportedProfile;
    if(!std::isfinite(p.pipe.clks_cfg.refclk_mhz)||
       p.pipe.clks_cfg.refclk_mhz!=double(binding.dchubReferenceKHz)/1000)
        return Status::InvalidReferenceClock;
    Encoding result={};auto& in=result.registers;
    in.profile=b::Profile::SingleLinearRgb32DirectVram;in.identity=timing.identity;
    in.fieldCount=b::kFieldCount;in.pipe=binding.pipe;
    in.dccgReferenceKHz=binding.dccgReferenceKHz;in.dchubReferenceKHz=binding.dchubReferenceKHz;
    in.timerDivider=binding.timerDivider;in.detKbytes=p.det_kbytes;in.compbufKbytes=p.compbuf_kbytes;
    const auto& d=p.pipe.pipe.dest;const auto& s=p.pipe.pipe.src;
    in.width=d.hactive;in.height=d.vactive;in.htotal=d.htotal;in.vtotal=d.vtotal;in.vblankEnd=d.vblank_end;
    in.vstartupStart=d.vstartup_start;in.vreadyOffset=d.vready_offset;
    in.vupdateWidth=d.vupdate_width;in.vupdateOffset=d.vupdate_offset;
    result.mode={d.hactive,d.vactive,s.data_pitch,s.surface_width_y,s.surface_height_y,
        s.viewport_x_y,s.viewport_y_y,d.htotal,d.vtotal,d.hblank_start,d.hblank_end,
        d.vblank_start,d.vblank_end,d.vfront_porch,0,p.pipe.dout.output_type,p.pipe.dout.dp_rate,p.pipe.dout.dp_lanes};
    const double hz=p.pipe.pipe.dest.pixel_rate_mhz*1000000;
    // Supported hardware never approaches 2^53 Hz: retaining integer-Hz mode
    // identity must be exact, rather than rounding a fractional request.
    if(!std::isfinite(hz)||hz<=0||hz>1000000000000.0||std::floor(hz)!=hz)
        return Status::NumericOverflow;
    result.mode.pixelClockHz=uint64_t(hz);
    if(!clocks(p,result.timingClocks))return Status::NumericOverflow;
    for(size_t n=0;n<4;++n) {
        if(!valid(sets[n]))return Status::InvalidCalculation;
        if(!sameSnapshot(timing.identity,sets[n].identity))return Status::StaleCalculation;
        if(!supported(*sets[n].result))return Status::UnsupportedProfile;
        if(!sameMode(p,*sets[n].result))return Status::ModeMismatch;
        if(!watermarks(sets[n],in.watermarks[n])||!clocks(*sets[n].result,result.watermarkClocks[n]))
            return Status::NumericOverflow;
        result.transitions[n]={sets[n].result->dram_clock_change_support,
            sets[n].result->fabric_clock_change_support,sets[n].result->retraining_supported};
    }
    // Explicit, compile-checked mapping: enum order or upstream member layout
    // changes cannot silently reinterpret raw structures as register words.
#define FIELD(name,member) in.fields[size_t(b::Field::name)]=p.member
#include "register_fields.inc"
#undef FIELD
    b::Plan checked;
    if(b::plan(in,checked)!=b::Status::Ok)return Status::InvalidRegisterPlan;
    out=result;return Status::Ok;
}
} }
