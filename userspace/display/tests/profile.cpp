// SPDX-License-Identifier: MIT
// Synthetic observation inputs exercise real profile construction and vendor
// DML arithmetic. These clocks/registers are not a capture from the target GPU.
#include "dml32_profile.hpp"
#include <iostream>
namespace p=hackynavi::displayprofile;
namespace d=hackynavi::displaysmu;
namespace m=hackynavi::displaymodel;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
m::BoardInfo board(){m::BoardInfo b={};b.vendorId=0x1002;b.deviceId=0x744c;b.subsystemVendorId=0x148c;b.subsystemDeviceId=0x2422;
    b.firmwareTable={0x600,108,3,4};b.displayControllerTable={0x800,212,4,5};b.vramTable={0x200,132,3,0};
    b.crystalKHz=100000;b.i2cKHz=50000;b.memoryType=0x70;b.channelCount=24;b.channelWidthRaw=4;b.channelWidthBytes=2;b.maxDisplayPipes=4;return b;}
m::ReferenceInfo refs(){m::ReferenceInfo r={};CHECK(m::decodeReferences(board(),43,0x1002,r)==m::Status::Ok);r.pllRequestRegister=0x16e37*4;r.timerControlRegister=(0x6000+0x525)*4;return r;}
d::DpmInventory inventory(){d::DpmInventory i={};i.firmwareVersion=0x4e6a00;
    const d::DpmClock clock[]={d::DpmClock::Soc,d::DpmClock::Memory,d::DpmClock::Fabric,d::DpmClock::Disp,d::DpmClock::Dpp,d::DpmClock::Dcf,d::DpmClock::Dtb};
    const uint16_t low[]={800,100,300,600,600,300,600},high[]={1200,1250,2500,2150,2150,1564,1564},dc[]={1100,1000,2000,2100,2100,1200,1200};
    for(unsigned n=0;n<7;++n){auto& t=i.clocks[n];t.clock=clock[n];t.fineGrained=true;t.metadata=0x90000000;t.levelCount=2;t.dcMaxMHz=dc[n];
        t.frequencyMHz[0]=low[n];t.frequencyMHz[1]=high[n];t.rawFrequency[0]=low[n];t.rawFrequency[1]=high[n];}
    return i;}
p::Mode mode(bool fourK=false){p::Mode mode={};auto& t=mode.timing;
    t={148500,1920,1080,2200,2008,2052,1125,1084,1089,true,true,true,true,16,1};
    mode.pitchPixels=1920;mode.output=p::Output::HdmiTmds;mode.hdmiTmdsLimitKHz=600000;
    if(fourK){t={594000,3840,2160,4400,4016,4104,2250,2168,2178,true,true,true,true,97,1};mode.pitchPixels=3840;}
    return mode;}
const auto policy=p::Policy::ColdFixedDcCeilingV1;
}
int main(){
    p::Profile profile={};const auto b=board();const auto r=refs();const auto dpm=inventory();
    CHECK(p::build(b,r,dpm,policy,profile)==p::Status::Ok);
    CHECK(profile.soc.num_states==1&&profile.soc.clock_limits[0].state==0);
    const uint16_t selected[]={1100,1000,2000,1950,1950,1200,1200};
    for(unsigned n=0;n<7;++n)CHECK(profile.selectedClockMHz[n]==selected[n]);
    CHECK(profile.soc.clock_limits[0].dram_speed_mts==16000&&profile.soc.num_chans==24&&profile.soc.dram_channel_width_bytes==2);
    CHECK(profile.soc.dispclk_dppclk_vco_speed_mhz==4300&&profile.soc.mall_allocated_for_dcn_mbytes==0);
    CHECK(profile.soc.sr_exit_time_us==42.97&&profile.soc.sr_enter_plus_exit_time_us==49.94&&profile.soc.dram_clock_change_latency_us==400);
    CHECK(profile.ip.config_return_buffer_size_in_kbytes==1280&&profile.ip.min_prefetch_in_strobe_us==60);
    for(unsigned n=0;n<7;++n){auto x=dpm;x.clocks[n].fineGrained=false;x.clocks[n].metadata=2;
        CHECK(p::build(b,r,x,policy,profile)==p::Status::Ok);
        CHECK(profile.selectedClockMHz[n]==x.clocks[n].frequencyMHz[0]);}
    for(unsigned n=0;n<7;++n){auto x=dpm;x.clocks[n].dcMaxMHz=x.clocks[n].frequencyMHz[0]-1;
        CHECK(p::build(b,r,x,policy,profile)==p::Status::NoClockWithinLimit);CHECK(profile.soc.num_states==0);}
    for(unsigned n=0;n<7;++n)for(unsigned field=0;field<8;++field){auto x=dpm;auto& t=x.clocks[n];
        if(field==0)t.levelCount=17;if(field==1)t.clock=d::DpmClock(0);if(field==2)t.metadata|=0x100;if(field==3)t.dcMaxMHz=0;
        if(field==4)t.frequencyMHz[0]++;if(field==5)t.rawFrequency[2]=1;if(field==6)t.fineGrained=false;if(field==7)t.dcMaxMHz=65536;
        CHECK(p::build(b,r,x,policy,profile)==p::Status::InvalidInventory);CHECK(profile.soc.num_states==0);}
    for(unsigned field=0;field<3;++field){auto x=b;if(field==0)x.rawMclkChange100ns=1;if(field==1)x.rawSrExit100ns=1;if(field==2)x.rawSrEnterExit100ns=1;
        CHECK(p::build(x,r,dpm,policy,profile)==p::Status::AmbiguousLatency);CHECK(profile.soc.num_states==0);}
    for(unsigned field=0;field<5;++field){auto x=r;if(field==0)x.programmedVcoKHz++;if(field==1)x.dccgReferenceKHz++;if(field==2)x.dchubReferenceKHz++;
        if(field==3)x.timerDivider++;if(field==4)x.pllRequestRegister++;
        CHECK(p::build(b,x,dpm,policy,profile)==p::Status::InvalidReference);}
    {auto x=b;x.channelWidthRaw=5;CHECK(p::build(x,r,dpm,policy,profile)==p::Status::InvalidBoard);}
    {auto x=b;x.displayControllerTable.size=211;CHECK(p::build(x,r,dpm,policy,profile)==p::Status::InvalidBoard);}
    {auto x=b;x.firmwareTable.contentRevision=5;CHECK(p::build(x,r,dpm,policy,profile)==p::Status::InvalidBoard);}
    {auto x=dpm;x.firmwareVersion=0;CHECK(p::build(b,r,x,policy,profile)==p::Status::InvalidInventory);}
    CHECK(p::build(b,r,dpm,p::Policy(0),profile)==p::Status::UnsupportedPolicy);
    for(bool fourK:{false,true}) {
        hn_dml_plan plan={};hn_dml_status status=HN_DML_INVALID_INPUT;const auto request=mode(fourK);
        CHECK(p::calculate(b,r,dpm,policy,request,plan,status)==p::Status::Ok);CHECK(status==HN_DML_OK);
        CHECK(plan.det_kbytes==1152&&plan.compbuf_kbytes==128&&plan.dispclk_mhz>=request.timing.pixelClockKHz/1000.0);
        CHECK(plan.dram_speed_mts==16000&&plan.fabricclk_mhz==2000&&plan.socclk_mhz==1100);
        CHECK(plan.pipe.clks_cfg.refclk_mhz==50&&plan.pipe.pipe.src.data_pitch==request.pitchPixels);
        std::cout<<(fourK?"4K60":"1080p60")<<" actual DML from modeled observations: DCF="<<plan.dcfclk_mhz<<" DISP="<<plan.dispclk_mhz<<" MHz\n";
    }
    {auto request=mode(true);request.output=p::Output::DisplayPortLegacy;request.hdmiTmdsLimitKHz=0;request.dpRateMbps=8100;request.dpLanes=2;
        hn_dml_plan plan={};hn_dml_status status=HN_DML_INVALID_INPUT;
        CHECK(p::calculate(b,r,dpm,policy,request,plan,status)==p::Status::CalculationFailed);CHECK(status==HN_DML_MODE_REJECTED);
        request.dpLanes=4;CHECK(p::calculate(b,r,dpm,policy,request,plan,status)==p::Status::Ok);}
    for(unsigned field=0;field<6;++field){auto request=mode();if(field==0)request.hdmiTmdsLimitKHz=0;if(field==1)request.pitchPixels=1919;
        if(field==2)request.timing.hSyncEnd=request.timing.hSyncStart;if(field==3)request.timing.vSyncStart=request.timing.vTotal;
        if(field==4)request.timing.pixelClockKHz=1629250;if(field==5)request.dpLanes=4;
        hn_dml_plan plan={};hn_dml_status status=HN_DML_OK;
        CHECK(p::calculate(b,r,dpm,policy,request,plan,status)==p::Status::InvalidMode);CHECK(status==HN_DML_INVALID_INPUT);}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
