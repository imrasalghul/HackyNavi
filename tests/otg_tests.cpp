// SPDX-License-Identifier: MIT
// Host-only register model: fault coverage, never evidence of GPU execution.
#include "hackynavi/otg.hpp"
#include <iostream>
#include <map>
#include <vector>
namespace o=hackynavi::otg;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures;std::cerr<<__LINE__<<": " #x "\n"; } } while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v){put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t p){uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s;}
std::vector<uint8_t> discovery(){
    std::vector<uint8_t> b(164,0);put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,164);
    put16(b,12,60);put16(b,16,104);put32(b,60,0x53445049);put16(b,64,3);put16(b,66,104);
    put16(b,72,1);put16(b,76,140);put16(b,142,1);put16(b,144,271);b[147]=3;b[148]=3;b[149]=2;
    put32(b,160,0x34c0);put16(b,14,sum(b,60));put16(b,8,sum(b,10));return b;
}
hackynavi::edid::Timing timing(){
    hackynavi::edid::Timing t={};t.pixelClockKHz=148500;t.width=1920;t.height=1080;
    t.hTotal=2200;t.hSyncStart=2008;t.hSyncEnd=2052;t.vTotal=1125;t.vSyncStart=1084;t.vSyncEnd=1089;
    t.hPositive=t.vPositive=true;return t;
}
const o::Poll poll={3,10};const o::GlobalSync sync={40,100,20,200};
struct Device {
    o::Layout layout={};o::Controller controller;std::map<uint32_t,uint32_t> registers;
    size_t calls=0,failAt=0,onesAt=0,writes=0,delays=0;
    bool clocks=true,frames=true,enableAck=true,disableAck=true,underflow=false;
    Device(){auto b=discovery();CHECK(o::buildLayout(b.data(),b.size(),0x100000,layout)==o::Status::Ok);
        for(size_t p=0;p<o::kPipeCount;++p)for(size_t r=0;r<o::kRegisterCount;++r)registers[layout.registers[p][r]]=0;}
    uint32_t address(size_t p,o::Register r)const{return layout.registers[p][size_t(r)];}
    uint32_t& reg(size_t p,o::Register r){return registers[address(p,r)];}
    o::Io io(){return {this,read,write,delay};}
    static bool read(void* context,uint32_t address,uint32_t& value){
        auto& d=*static_cast<Device*>(context);++d.calls;CHECK(d.registers.count(address)==1);
        for(size_t p=0;p<o::kPipeCount;++p)if(address==d.address(p,o::Register::FrameCount)&&d.frames&&(d.reg(p,o::Register::Control)&0x10000))++d.registers[address];
        value=d.registers[address];if(d.calls==d.onesAt)value=UINT32_MAX;return d.calls!=d.failAt;
    }
    static bool write(void* context,uint32_t address,uint32_t value){
        auto& d=*static_cast<Device*>(context);++d.calls;++d.writes;CHECK(d.registers.count(address)==1);
        if(d.calls==d.failAt)return false;
        const auto old=d.registers[address];d.registers[address]=value;
        for(size_t p=0;p<o::kPipeCount;++p){
            if(address==d.address(p,o::Register::InputClock))d.registers[address]=(value&~4u)|((d.clocks&&(value&3))?4u:0u);
            if(address==d.address(p,o::Register::Clock))d.registers[address]=(value&~0x100u)|((d.clocks&&(value&3))?0x100u:0u);
            if(address==d.address(p,o::Register::Control)){
                const bool current=(value&1)?d.enableAck:(!d.disableAck&&(old&0x10000));
                d.registers[address]=(value&~0x10000u)|(current?0x10000u:0u);
                if((value&1)&&d.underflow)d.reg(p,o::Register::InputGlobal)|=0x400;
            }
            if(address==d.address(p,o::Register::InputGlobal)&&(value&0x1000))d.registers[address]=value&~0x1400u;
        }
        return true;
    }
    static void delay(void* context,uint32_t us){auto& d=*static_cast<Device*>(context);CHECK(us==10);++d.delays;}
    o::Status configure(o::Result& r,uint8_t pipe=0){return controller.configure(layout,io(),pipe,timing(),sync,o::Signal::Hdmi,poll,r);}
};
void encoding(){
    o::TimingValues v={};auto t=timing();CHECK(o::encode(t,sync,o::Signal::Hdmi,v)==o::Status::Ok);
    CHECK(v.hTotal==2199&&v.vTotal==1124);CHECK(v.hBlank==(192u<<16|2112u));CHECK(v.vBlank==(41u<<16|1121u));
    CHECK(v.hSync==44u<<16);CHECK(v.vSync==5u<<16);CHECK(v.vtg==1121u<<16);
    CHECK(v.vStartup==40&&v.vUpdate==(20u<<16|100u)&&v.vReady==200&&v.startPoint==0);
    t.hPositive=t.vPositive=false;CHECK(o::encode(t,sync,o::Signal::DisplayPort,v)==o::Status::Ok);
    CHECK(v.startPoint==0x1000&&v.hPolarity==1&&v.vPolarity==1);
    t.hSyncStart=t.width;CHECK(o::encode(t,sync,o::Signal::Hdmi,v)==o::Status::InvalidTiming);
    t=timing();t.hSyncEnd=t.hSyncStart+3;CHECK(o::encode(t,sync,o::Signal::Hdmi,v)==o::Status::InvalidTiming);
    CHECK(o::encode(timing(),{0,0,0,0},o::Signal::Hdmi,v)==o::Status::InvalidSync);
    CHECK(o::encode(timing(),{40,0,1024,0},o::Signal::Hdmi,v)==o::Status::InvalidSync);
}
void lifecycle(){
    for(uint8_t p=0;p<4;++p){Device d;o::Result r={};CHECK(d.configure(r,p)==o::Status::Ok);
        CHECK(d.reg(p,o::Register::HTotal)==2199);CHECK(d.reg(p,o::Register::DataSource)==(0xfff00000u|(uint32_t(p)<<16)));
        CHECK(d.controller.phase()==o::Phase::Programmed&&d.controller.requiresHardwareRetirement());
        CHECK(d.controller.enable(poll,r)==o::Status::Ok&&r.frameAdvanced&&r.currentEnabled);
        CHECK(d.controller.disable(poll,r)==o::Status::Ok&&r.busyCleared&&r.clockControlsDisabled);
        CHECK(d.controller.phase()==o::Phase::Stopped&&!d.controller.requiresHardwareRetirement());
        CHECK(d.reg(p,o::Register::DataSource)==0xffff0000);CHECK(d.configure(r,p)==o::Status::Ok);
    }
    Device a;o::Result r={};a.reg(0,o::Register::Control)=1;CHECK(a.configure(r)==o::Status::ActivePipe&&a.writes==0);
    Device b;b.reg(1,o::Register::Control)=0x10001;b.reg(1,o::Register::DataSource)=0xff0f0000;
    CHECK(b.configure(r)==o::Status::ForeignControl&&b.writes==0);
    Device c;c.layout.registers[3][2]+=4;CHECK(c.configure(r)==o::Status::InvalidArgument&&c.calls==0);
    Device e;e.clocks=false;CHECK(e.configure(r)==o::Status::ClockTimeout&&e.controller.requiresHardwareRetirement());
    Device f;CHECK(f.configure(r)==o::Status::Ok);f.frames=false;CHECK(f.controller.enable(poll,r)==o::Status::FrameTimeout);
    CHECK(f.controller.phase()==o::Phase::Faulted);CHECK(f.controller.disable(poll,r)==o::Status::Ok);
    Device g;CHECK(g.configure(r)==o::Status::Ok);g.underflow=true;CHECK(g.controller.enable(poll,r)==o::Status::Underflow);
    CHECK(g.controller.disable(poll,r)==o::Status::Ok);
    Device h;CHECK(h.configure(r)==o::Status::Ok);h.enableAck=false;CHECK(h.controller.enable(poll,r)==o::Status::EnableTimeout);
    Device i;CHECK(i.configure(r)==o::Status::Ok);CHECK(i.controller.enable(poll,r)==o::Status::Ok);i.disableAck=false;
    CHECK(i.controller.disable(poll,r)==o::Status::DisableTimeout&&i.controller.requiresHardwareRetirement());
}
void admission(){
    auto bytes=discovery();o::Layout layout={};
    for(size_t n=0;n<bytes.size();++n)CHECK(o::buildLayout(bytes.data(),n,0x100000,layout)!=o::Status::Ok);
    CHECK(o::buildLayout(bytes.data(),bytes.size(),0x100,layout)==o::Status::OutsideBar);
    CHECK(o::buildLayout(bytes.data(),bytes.size(),0x100000,layout)==o::Status::Ok);
    for(size_t p=0;p<4;++p){
        CHECK(layout.registers[p][size_t(o::Register::HTotal)]==(0x34c0+0x1b2a+p*0x80)*4);
        CHECK(layout.registers[p][size_t(o::Register::Vtg)]==(0x34c0+0x52e + p)*4);
        CHECK(layout.registers[p][size_t(o::Register::InputClock)]==(0x34c0+0x1acf+p*0x10)*4);
    }
    struct Guard {o::Register reg;uint32_t value;};
    const Guard guards[]={{o::Register::Vtg,0x80000000},{o::Register::UpdateLock,0x101},
        {o::Register::GlobalControl,0x40070400},{o::Register::Gsl,0x8000000f},
        {o::Register::Stereo,0x01000000},{o::Register::Structure3d,0x1101},
        {o::Register::Interlace,0x30001},{o::Register::DataFormat,0x30},
        {o::Register::InputGlobal,0x80000101},{o::Register::DoubleBuffer,0x6f1},
        {o::Register::Interrupt0,0x100},{o::Register::Interrupt1,0x100},
        {o::Register::Interrupt2,0x100},{o::Register::HSyncControl,0x30000},
        {o::Register::VSyncControl,0x100}};
    for(const auto& guard:guards)for(unsigned bit=0;bit<32;++bit)if(guard.value&(1u<<bit)){
        Device d;o::Result r={};d.reg(0,guard.reg)=1u<<bit;
        CHECK(d.configure(r)==o::Status::ForeignControl);CHECK(d.writes==0&&!d.controller.requiresHardwareRetirement());
    }
    for(unsigned segment=0;segment<4;++segment){
        Device d;o::Result r={};d.reg(2,o::Register::Control)=0x10001;
        d.reg(2,o::Register::DataSource)=0xffff0000u&~(15u<<(16+segment*4));
        CHECK(d.configure(r)==o::Status::ForeignControl&&d.writes==0);
    }
    Device d;o::Result r={};CHECK(d.configure(r)==o::Status::Ok);
    d.reg(0,o::Register::HTotal)^=1;const size_t writes=d.writes;
    CHECK(d.controller.enable(poll,r)==o::Status::VerifyFailed&&d.writes==writes);
    CHECK(d.controller.requiresHardwareRetirement());
}
void faults(){
    Device baseline;o::Result r={};CHECK(baseline.configure(r)==o::Status::Ok);const size_t configured=baseline.calls;
    CHECK(baseline.controller.enable(poll,r)==o::Status::Ok);const size_t enabled=baseline.calls;
    CHECK(baseline.controller.disable(poll,r)==o::Status::Ok);const size_t stopped=baseline.calls;
    for(size_t mode=0;mode<2;++mode)for(size_t point=1;point<=stopped;++point){
        Device d;if(mode==0)d.failAt=point;else d.onesAt=point;
        auto status=d.configure(r);
        if(point>configured&&status==o::Status::Ok)status=d.controller.enable(poll,r);
        if(point>enabled&&status==o::Status::Ok)status=d.controller.disable(poll,r);
        // All-ones injection applies to reads only; write positions remain valid.
        if(mode==0)CHECK(status==o::Status::TransportError);
        if(status!=o::Status::Ok){CHECK(d.controller.requiresHardwareRetirement()==(d.writes!=0));
            CHECK(d.controller.phase()==(d.writes?o::Phase::Faulted:o::Phase::Closed));}
    }
}
}
int main(){encoding();lifecycle();admission();faults();std::cout<<checks<<" OTG checks, "<<failures<<" failures\n";return failures?1:0;}

