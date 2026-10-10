// SPDX-License-Identifier: MIT
#include "dml32_profile.hpp"
#include <cstring>
extern "C" void hn_dml32_asic_base(soc_bounding_box_st*,ip_params_st*);
namespace hackynavi { namespace displayprofile { namespace {
const displaysmu::DpmClock clocks[]={displaysmu::DpmClock::Soc,displaysmu::DpmClock::Memory,
    displaysmu::DpmClock::Fabric,displaysmu::DpmClock::Disp,displaysmu::DpmClock::Dpp,
    displaysmu::DpmClock::Dcf,displaysmu::DpmClock::Dtb};
Status select(const displaysmu::DpmTable& t,size_t which,uint16_t& selected) {
    if(t.clock!=clocks[which]||(t.metadata&0x0fffff00)||!t.dcMaxMHz||t.dcMaxMHz>UINT16_MAX||
       !t.levelCount||t.levelCount>displaysmu::kDpmLevelCapacity||
       t.fineGrained!=bool(t.metadata&0x80000000)||
       t.levelCount!=(t.fineGrained?2:(t.metadata&0xff)))return Status::InvalidInventory;
    uint32_t ceiling=t.dcMaxMHz;
    if((which==3||which==4)&&ceiling>1950)ceiling=1950;
    uint16_t best=0;
    for(size_t n=0;n<displaysmu::kDpmLevelCapacity;++n) {
        if(n>=t.levelCount) {
            if(t.rawFrequency[n]||t.frequencyMHz[n])return Status::InvalidInventory;
            continue;
        }
        if(t.rawFrequency[n]==UINT32_MAX||!t.frequencyMHz[n]||
           t.frequencyMHz[n]!=uint16_t(t.rawFrequency[n]))return Status::InvalidInventory;
        if(t.frequencyMHz[n]<=ceiling&&t.frequencyMHz[n]>best)best=t.frequencyMHz[n];
    }
    if(t.fineGrained) {
        if(t.frequencyMHz[0]>t.frequencyMHz[1])return Status::InvalidInventory;
        if(ceiling<t.frequencyMHz[0])return Status::NoClockWithinLimit;
        best=uint16_t(ceiling<t.frequencyMHz[1]?ceiling:t.frequencyMHz[1]);
    }
    if(!best)return Status::NoClockWithinLimit;
    selected=best;return Status::Ok;
}
} // namespace
Status build(const displaymodel::BoardInfo& b,const displaymodel::ReferenceInfo& ref,
    const displaysmu::DpmInventory& inventory,Policy policy,Profile& out) {
    out={};if(policy!=Policy::ColdFixedDcCeilingV1)return Status::UnsupportedPolicy;
    if(b.vendorId!=0x1002||b.deviceId!=0x744c||b.memoryType!=0x70||!b.channelCount||
       b.channelCount>24||b.channelWidthRaw!=4||b.channelWidthBytes!=2||b.maxDisplayPipes!=4||
       b.firmwareTable.formatRevision!=3||(b.firmwareTable.contentRevision!=4&&b.firmwareTable.contentRevision!=5)||
       !b.firmwareTable.offset||b.firmwareTable.size<(b.firmwareTable.contentRevision==4?108:172)||
       b.displayControllerTable.formatRevision!=4||b.displayControllerTable.contentRevision!=5||
       !b.displayControllerTable.offset||b.displayControllerTable.size<212||
       b.vramTable.formatRevision!=3||b.vramTable.contentRevision!=0||!b.vramTable.offset||b.vramTable.size<132)
        return Status::InvalidBoard;
    if(b.rawMclkChange100ns||b.rawSrExit100ns||b.rawSrEnterExit100ns)return Status::AmbiguousLatency;
    displaymodel::ReferenceInfo decoded={};
    if(displaymodel::decodeReferences(b,ref.rawPllRequest,ref.rawTimerControl,decoded)!=displaymodel::Status::Ok||
       ref.pllRequestRegister!=0x16e37*4||!ref.timerControlRegister||(ref.timerControlRegister&3)||
       ref.timerControlRegister==ref.pllRequestRegister||
       decoded.dccgReferenceKHz!=ref.dccgReferenceKHz||decoded.dchubReferenceKHz!=ref.dchubReferenceKHz||
       decoded.programmedVcoKHz!=ref.programmedVcoKHz||decoded.timerDivider!=ref.timerDivider)
        return Status::InvalidReference;
    if(inventory.firmwareVersion<0x4e6a00||inventory.firmwareVersion==UINT32_MAX)return Status::InvalidInventory;
    Profile p={};p.policy=policy;p.reference=ref;
    for(size_t n=0;n<displaysmu::kDpmClockCount;++n) {
        const auto s=select(inventory.clocks[n],n,p.selectedClockMHz[n]);if(s!=Status::Ok)return s;
    }
    hn_dml32_asic_base(&p.soc,&p.ip);
    p.soc.num_chans=b.channelCount;p.soc.dram_channel_width_bytes=b.channelWidthBytes;
    p.soc.dispclk_dppclk_vco_speed_mhz=ref.programmedVcoKHz/1000.0;
    p.soc.num_states=1;
    auto& c=p.soc.clock_limits[0];c.state=0;
    c.socclk_mhz=p.selectedClockMHz[0];c.dram_speed_mts=uint32_t(p.selectedClockMHz[1])*16;
    c.fabricclk_mhz=p.selectedClockMHz[2];c.dispclk_mhz=p.selectedClockMHz[3];
    c.dppclk_mhz=p.selectedClockMHz[4];c.dcfclk_mhz=p.selectedClockMHz[5];c.dtbclk_mhz=p.selectedClockMHz[6];
    // Pinned dcn3_2_soc PHY link ceilings, not observed running clocks. DSC is
    // excluded by the pipe profile; this derived value preserves DML arithmetic.
    c.phyclk_mhz=810;c.phyclk_d18_mhz=667;c.phyclk_d32_mhz=625;c.dscclk_mhz=c.dispclk_mhz/3;
    out=p;return Status::Ok;
}
Status makePipe(const Profile& profile,const Mode& m,display_e2e_pipe_params_st& out) {
    out={};const auto& t=m.timing;
    if(profile.policy!=Policy::ColdFixedDcCeilingV1||!t.width||!t.height||t.width>16383||t.height>16383||
       !t.pixelClockKHz||t.pixelClockKHz>2000000||m.pitchPixels<t.width||m.pitchPixels>16384||(m.pitchPixels&63)||
       t.hTotal<=t.width||t.hTotal>32768||t.vTotal<=t.height||t.vTotal>32768||
       t.hSyncStart<t.width||t.hSyncEnd<=t.hSyncStart||t.hSyncEnd>t.hTotal||
       t.vSyncStart<t.height||t.vSyncEnd<=t.vSyncStart||t.vSyncEnd>t.vTotal)
        return Status::InvalidMode;
    if(m.output==Output::HdmiTmds) {
        if(!m.hdmiTmdsLimitKHz||t.pixelClockKHz>m.hdmiTmdsLimitKHz||t.pixelClockKHz>600000||m.dpLanes||m.dpRateMbps)
            return Status::InvalidMode;
    } else if(m.output==Output::DisplayPortLegacy) {
        if(m.hdmiTmdsLimitKHz||(m.dpLanes!=1&&m.dpLanes!=2&&m.dpLanes!=4)||
           (m.dpRateMbps!=2700&&m.dpRateMbps!=5400&&m.dpRateMbps!=8100))return Status::InvalidMode;
    } else return Status::InvalidMode;
    display_e2e_pipe_params_st p={};auto& s=p.pipe.src;auto& d=p.pipe.dest;
    s.source_format=dm_444_32;s.sw_mode=dm_sw_linear;
    s.surface_width_y=m.pitchPixels;s.viewport_width=s.viewport_width_max=t.width;
    s.surface_height_y=s.viewport_height=s.viewport_height_max=t.height;
    s.viewport_stationary=true;s.data_pitch=m.pitchPixels;s.det_size_override=1152;s.gpuvm_min_page_size_kbytes=4;
    d.recout_width=d.full_recout_width=d.hactive=t.width;d.recout_height=d.full_recout_height=d.vactive=t.height;
    d.htotal=t.hTotal;d.vtotal=d.vtotal_min=d.vtotal_max=t.vTotal;
    d.hblank_start=t.hTotal-(t.hSyncStart-t.width);d.hblank_end=d.hblank_start-t.width;
    d.vfront_porch=t.vSyncStart-t.height;d.vblank_start=t.vTotal-d.vfront_porch;d.vblank_end=d.vblank_start-t.height;
    d.vblank_nom=t.vTotal-t.height;d.pixel_rate_mhz=t.pixelClockKHz/1000.0;
    d.refresh_rate=uint32_t(uint64_t(t.pixelClockKHz)*1000/(uint64_t(t.hTotal)*t.vTotal));
    p.pipe.scale_ratio_depth.hscl_ratio=p.pipe.scale_ratio_depth.vscl_ratio=1;
    p.pipe.scale_ratio_depth.hscl_ratio_c=p.pipe.scale_ratio_depth.vscl_ratio_c=1;p.pipe.scale_ratio_depth.lb_depth=dm_lb_19;
    p.pipe.scale_taps.htaps=p.pipe.scale_taps.vtaps=p.pipe.scale_taps.htaps_c=p.pipe.scale_taps.vtaps_c=1;
    p.dout.output_type=m.output==Output::HdmiTmds?dm_hdmi:dm_dp;
    p.dout.output_format=dm_444;p.dout.output_bpc=dm_out_8;p.dout.output_bpp=24;
    p.dout.dsc_input_bpc=8;p.dout.dsc_slices=1;
    if(m.output==Output::DisplayPortLegacy) {
        p.dout.dp_lanes=m.dpLanes;p.dout.dp_rate=m.dpRateMbps==2700?dm_dp_rate_hbr:
            m.dpRateMbps==5400?dm_dp_rate_hbr2:dm_dp_rate_hbr3;
    }
    p.clks_cfg.refclk_mhz=profile.reference.dchubReferenceKHz/1000.0;out=p;return Status::Ok;
}
Status calculate(const displaymodel::BoardInfo& board,const displaymodel::ReferenceInfo& refs,
    const displaysmu::DpmInventory& dpm,Policy policy,const Mode& mode,hn_dml_plan& out,hn_dml_status& calculation) {
    out={};calculation=HN_DML_INVALID_INPUT;Profile profile={};
    auto status=build(board,refs,dpm,policy,profile);if(status!=Status::Ok)return status;
    display_e2e_pipe_params_st pipe={};status=makePipe(profile,mode,pipe);if(status!=Status::Ok)return status;
    calculation=hn_dml32_calculate(&profile.soc,&profile.ip,&pipe,&out);
    return calculation==HN_DML_OK?Status::Ok:Status::CalculationFailed;
}
} }
