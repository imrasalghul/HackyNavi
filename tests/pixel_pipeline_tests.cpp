// SPDX-License-Identifier: MIT
// Host-only MMIO model. These checks are not evidence of GPU execution.
#include "hackynavi/pixel_pipeline.hpp"
#include <iostream>
#include <map>
#include <vector>
namespace d=hackynavi::pixelpipeline;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v){put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t p){uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s;}
void checksum(std::vector<uint8_t>& b){put16(b,14,sum(b,60));put16(b,8,sum(b,10));}
std::vector<uint8_t> discovery(){
    std::vector<uint8_t> b(168,0);put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,168);
    put16(b,12,60);put16(b,16,108);put32(b,60,0x53445049);put16(b,64,3);put16(b,66,108);
    put16(b,72,1);put16(b,76,140);put16(b,142,1);put16(b,144,271);b[147]=4;b[148]=3;b[149]=2;
    put32(b,156,0x1200);put32(b,160,0x3400);put32(b,164,0x6000);checksum(b);return b;
}
const d::Poll poll={3,1};
const d::Config config={1920,1080,d::Format::Argb8888,8};
struct Access { uint32_t address,value;bool write; };
struct Device {
    d::Layout layout={};d::Controller controller;std::map<uint32_t,uint32_t> regs;
    std::vector<Access> trace;uint8_t pipe;size_t failAt=0,onesAt=0,dropAt=0;
    size_t writes=0,delays=0,changeAt=0;bool changeConfig=false;
    bool ambiguous=false,busy=false,noClock=false;
    explicit Device(uint8_t p=0):pipe(p){
        auto b=discovery();CHECK(d::buildLayout(b.data(),b.size(),0x100000,layout)==d::Status::Ok);
        for(size_t n=0;n<4;++n)for(size_t r=0;r<d::kRegisterCount;++r)regs[layout.registers[n][r]]=0;
        for(uint8_t n=0;n<4;++n){
            for(d::Register r:{d::Register::Top,d::Register::Bottom,d::Register::OppId,d::Register::OutMux,d::Register::LockSelect})reg(n,r)=15;
            reg(n,d::Register::MpccStatus)=5;reg(n,d::Register::HubpControl)=0x100b;
        }
        reg(p,d::Register::DwbMux)=0xff;
    }
    uint32_t& reg(uint8_t n,d::Register r){return regs[layout.registers[n][size_t(r)]];}
    uint32_t& reg(d::Register r){return reg(pipe,r);}
    d::Io io(){return {this,read,write,delay};}
    static bool read(void* c,uint32_t a,uint32_t& v){auto& x=*static_cast<Device*>(c);
        CHECK(x.regs.count(a)==1);
        if(x.trace.size()+1==x.changeAt)x.reg(x.changeConfig?d::Register::PowerConfig:d::Register::Power)^=x.changeConfig?1u:0x10000000u;
        v=x.regs[a];x.trace.push_back({a,v,false});
        if(x.trace.size()==x.onesAt)v=UINT32_MAX;return x.trace.size()!=x.failAt;
    }
    static bool write(void* c,uint32_t a,uint32_t v){auto& x=*static_cast<Device*>(c);
        CHECK(x.regs.count(a)==1);x.trace.push_back({a,v,true});++x.writes;
        const bool failed=x.trace.size()==x.failAt;
        if((!failed||x.ambiguous)&&x.trace.size()!=x.dropAt){
            x.regs[a]=v;
            if(a==x.layout.registers[x.pipe][size_t(d::Register::OppControl)]&&!x.noClock)
                x.reg(d::Register::OppControl)=(v&~2u)|((v&1)?2:0);
            if(a==x.layout.registers[x.pipe][size_t(d::Register::Top)])
                x.reg(d::Register::MpccStatus)=x.busy?2:((v&15)==15?5:0);
        }
        return !failed;
    }
    static void delay(void* c,uint32_t us){auto& x=*static_cast<Device*>(c);CHECK(us==1);++x.delays;}
    d::Status configure(d::Result& r){return controller.configure(layout,io(),pipe,config,poll,r);}
};
bool changesValue(const std::vector<Access>& trace,size_t writeIndex){
    for(size_t i=writeIndex;i>0;--i)if(trace[i-1].address==trace[writeIndex].address)
        return trace[i-1].value!=trace[writeIndex].value;
    return false;
}
void checkDisconnected(Device& x){
    for(d::Register r:{d::Register::Top,d::Register::Bottom,d::Register::OppId,d::Register::OutMux,d::Register::LockSelect})CHECK((x.reg(r)&15)==15);
    CHECK((x.reg(d::Register::HubpClock)&1)==0);
    CHECK((x.reg(d::Register::DppControl)&0x10)==0);CHECK((x.reg(d::Register::OppControl)&3)==0);
}
}
void powerStates(){
    for(uint8_t pipe=0;pipe<4;++pipe)for(uint32_t status:{0u,0x10000000u})for(uint32_t config:{0u,1u}){
        Device x(pipe);x.reg(d::Register::Power)=status;x.reg(d::Register::PowerConfig)=config;d::Result r={};
        CHECK(x.configure(r)==d::Status::Ok);CHECK(x.controller.stop(poll,r)==d::Status::Ok);
        CHECK(x.reg(d::Register::Power)==status&&x.reg(d::Register::PowerConfig)==config);
        for(bool cfg:{false,true}){Device changed(pipe);changed.reg(d::Register::Power)=status;changed.reg(d::Register::PowerConfig)=config;
            CHECK(changed.configure(r)==d::Status::Ok);const auto writes=changed.writes;
            changed.reg(cfg?d::Register::PowerConfig:d::Register::Power)^=cfg?1u:0x10000000u;
            CHECK(changed.controller.stop(poll,r)==d::Status::PowerUnavailable&&changed.writes==writes&&r.retentionRequired);}
    }
    Device baseline;d::Result r={};CHECK(baseline.configure(r)==d::Status::Ok);
    std::vector<size_t> points={3,4,5,7};
    for(size_t n=0;n<baseline.trace.size();++n)if(baseline.trace[n].write){points.push_back(n+2);break;}
    for(bool config:{false,true})for(size_t at:points){Device x;x.changeAt=at;x.changeConfig=config;
        CHECK(x.configure(r)==d::Status::PowerUnavailable);CHECK(r.retentionRequired==(x.writes!=0));}
    for(unsigned bit=0;bit<32;++bit)if(bit!=28){Device x;x.reg(d::Register::Power)=1u<<bit;
        CHECK(x.configure(r)==d::Status::PowerUnavailable&&x.writes==0);}
    for(unsigned bit=1;bit<32;++bit){Device x;x.reg(d::Register::PowerConfig)=1u<<bit;
        CHECK(x.configure(r)==d::Status::PowerUnavailable&&x.writes==0);}
}
int main(){
    powerStates();
    d::Plan p={};CHECK(d::plan(config,p)==d::Status::Ok);
    CHECK(p.size==0x04380780&&p.surfaceFormat==8&&p.formatControl==0x24000000&&p.lbMemory==0x1f01);
    for(auto format:{d::Format::Argb8888,d::Format::Abgr8888,d::Format::Argb2101010,d::Format::Abgr2101010})
        for(uint8_t bpc:{8,10}){CHECK(d::plan({3840,2160,format,bpc},p)==d::Status::Ok);
            CHECK(p.surfaceFormat==(uint32_t(format)<2?8u:10u));CHECK(p.denorm==(bpc==8?2u:4u));CHECK(p.expansion==(bpc==8?0x11u:1u));}
    for(uint32_t width:{1u,5820u,5821u,7740u,7741u,16383u}){
        CHECK(d::plan({width,1,d::Format::Argb8888,8},p)==d::Status::Ok);
        CHECK(p.lbMemory==(0x1f00|(width<=5820?1u:width<=7740?2u:0u)));
    }
    for(d::Config c:{d::Config{0,1,d::Format::Argb8888,8},d::Config{1,0,d::Format::Argb8888,8},
        d::Config{16384,1,d::Format::Argb8888,8},d::Config{1,16384,d::Format::Argb8888,8},
        d::Config{1,1,d::Format(4),8},d::Config{1,1,d::Format::Argb8888,12}}){
        p.size=123;CHECK(d::plan(c,p)==d::Status::InvalidArgument);CHECK(p.size==123);}
    for(uint8_t pipe=0;pipe<4;++pipe){
        Device x(pipe);d::Result r={};const auto initial=x.regs;
        CHECK(x.layout.registers[pipe][size_t(d::Register::DppControl)]==(0x3400+0xcc5+pipe*0x16b)*4u);
        CHECK(x.layout.registers[pipe][size_t(d::Register::HubpClock)]==(0x3400+0x5f4+pipe*0xdc)*4u);
        CHECK(x.layout.registers[pipe][size_t(d::Register::HubpRequestDebug)]==(0x3400+0x5f9+pipe*0xdc)*4u);
        CHECK(x.layout.registers[pipe][size_t(d::Register::Top)]==(0x6000+pipe*0x15)*4u);
        CHECK(x.layout.registers[pipe][size_t(d::Register::Power)]==(0x3400+0x81+pipe*2)*4u);
        CHECK(x.configure(r)==d::Status::Ok);CHECK(x.controller.phase()==d::Phase::Configured);
        CHECK(r.retentionRequired&&r.writeAttempted);CHECK(x.reg(d::Register::RecoutSize)==0x04380780);
        CHECK(x.reg(d::Register::ScalerMode)==0);CHECK(x.reg(d::Register::LbMemory)==0x1f01);
        CHECK(x.reg(d::Register::SurfaceFormat)==8);CHECK(x.reg(d::Register::FormatControl)==0x24000000);
        CHECK(x.reg(d::Register::MpccControl)==0xffff0422);CHECK(x.reg(d::Register::TopGain)==0x1f000);
        CHECK(x.reg(d::Register::Denorm)==0x02fff000);CHECK(x.reg(d::Register::FmtExpansion)==0x11);
        CHECK(x.reg(d::Register::FmtClamp)==1);CHECK((x.reg(d::Register::OutMux)&15)==pipe);
        CHECK((x.reg(d::Register::HubpClock)&1)==1);
        CHECK(x.reg(d::Register::HubpRequestDebug)==0x04000000);CHECK(x.reg(d::Register::HubpControl)==0xb);
        const auto configTrace=x.trace;
        size_t hubpClockOn=0,hubpDebugInit=0,hubpTtuInit=0,dppClockOn=0;
        for(size_t n=0;n<configTrace.size();++n)if(configTrace[n].write){
            if(configTrace[n].address==x.layout.registers[pipe][size_t(d::Register::HubpClock)])hubpClockOn=n;
            if(configTrace[n].address==x.layout.registers[pipe][size_t(d::Register::HubpRequestDebug)])hubpDebugInit=n;
            if(configTrace[n].address==x.layout.registers[pipe][size_t(d::Register::HubpControl)])hubpTtuInit=n;
            if(configTrace[n].address==x.layout.registers[pipe][size_t(d::Register::DppControl)])dppClockOn=n;
        }
        CHECK(hubpClockOn<hubpDebugInit&&hubpDebugInit<hubpTtuInit&&hubpTtuInit<dppClockOn);
        for(auto format:{d::Format::Argb8888,d::Format::Abgr8888,d::Format::Argb2101010,d::Format::Abgr2101010})
            for(uint8_t bpc:{8,10}){Device f(pipe);
                CHECK(f.controller.configure(f.layout,f.io(),pipe,{3840,2160,format,bpc},poll,r)==d::Status::Ok);
                CHECK(f.reg(d::Register::SurfaceFormat)==(uint32_t(format)<2?8u:10u));
                CHECK(f.reg(d::Register::FormatControl)==0x24000000);CHECK(f.reg(d::Register::RecoutSize)==0x08700f00);
                CHECK(f.reg(d::Register::Denorm)==((bpc==8?2u:4u)<<24|0xfff000));
                CHECK(f.reg(d::Register::FmtExpansion)==(bpc==8?0x11u:1u));
                CHECK(f.controller.stop(poll,r)==d::Status::Ok);}
        // Every write targets this pipe; shared DWB selector is never written.
        for(const auto& a:configTrace)if(a.write){bool own=false;
            for(size_t n=0;n<d::kRegisterCount;++n)own|=a.address==x.layout.registers[pipe][n];CHECK(own);}
        for(uint8_t n=0;n<4;++n)if(n!=pipe)for(size_t i=0;i<d::kRegisterCount;++i){
            const auto a=x.layout.registers[n][i];CHECK(x.regs[a]==initial.at(a));}
        x.trace.clear();CHECK(x.controller.stop(poll,r)==d::Status::Ok);const auto stopTrace=x.trace;
        size_t idleBeforeClock=0,hubpClockOff=0,dppClockOff=0;
        for(size_t n=0;n<stopTrace.size();++n){const auto& a=stopTrace[n];
            if(!a.write&&a.address==x.layout.registers[pipe][size_t(d::Register::MpccStatus)]&&(a.value&7)==5)idleBeforeClock=n;
            if(a.write&&a.address==x.layout.registers[pipe][size_t(d::Register::HubpClock)])hubpClockOff=n;
            if(a.write&&a.address==x.layout.registers[pipe][size_t(d::Register::DppControl)])dppClockOff=n;
        }
        CHECK(idleBeforeClock<hubpClockOff&&hubpClockOff<dppClockOff);
        CHECK(r.disconnected&&r.idle&&!r.retentionRequired);checkDisconnected(x);
        CHECK(!x.controller.requiresHardwareRetirement());CHECK(x.controller.phase()==d::Phase::Closed);
        // Fail every configure transport operation, including writes that
        // reach the device even though the transport reports failure.
        for(size_t at=1;at<=configTrace.size();++at)for(bool ambiguous:{false,true}){
            Device f(pipe);f.failAt=at;f.ambiguous=ambiguous;
            CHECK(f.configure(r)==d::Status::TransportError);
            CHECK(f.controller.requiresHardwareRetirement()==(f.writes!=0));
            CHECK(r.retentionRequired==(f.writes!=0));f.failAt=0;
            if(f.writes){CHECK(f.controller.phase()==d::Phase::Faulted);
                const auto count=f.trace.size();CHECK(f.configure(r)==d::Status::InvalidState);CHECK(f.trace.size()==count);
                CHECK(f.controller.stop(poll,r)==d::Status::Ok);checkDisconnected(f);}
        }
        for(size_t at=1;at<=configTrace.size();++at)if(!configTrace[at-1].write){
            Device f(pipe);f.onesAt=at;CHECK(f.configure(r)==d::Status::DeviceUnavailable);
            CHECK(f.controller.requiresHardwareRetirement()==(f.writes!=0));}
        // Suppress each write that changes a programmed field and require
        // readback failure, retention and a successful independent stop.
        for(size_t at=1;at<configTrace.size();++at)if(configTrace[at-1].write&&
            changesValue(configTrace,at-1)){Device f(pipe);f.dropAt=at;
            CHECK(f.configure(r)==d::Status::VerifyFailed);CHECK(f.controller.requiresHardwareRetirement());
            f.dropAt=0;CHECK(f.controller.stop(poll,r)==d::Status::Ok);checkDisconnected(f);}
        for(size_t at=1;at<=stopTrace.size();++at){Device f(pipe);CHECK(f.configure(r)==d::Status::Ok);
            f.trace.clear();f.failAt=at;f.ambiguous=true;
            CHECK(f.controller.stop(poll,r)==d::Status::TransportError);CHECK(f.controller.requiresHardwareRetirement());
            f.failAt=0;CHECK(f.controller.stop(poll,r)==d::Status::Ok);checkDisconnected(f);}
        for(size_t at=1;at<stopTrace.size();++at)if(stopTrace[at-1].write&&
            changesValue(stopTrace,at-1)){Device f(pipe);CHECK(f.configure(r)==d::Status::Ok);
            f.trace.clear();f.dropAt=at;CHECK(f.controller.stop(poll,r)==d::Status::VerifyFailed);
            CHECK(f.controller.requiresHardwareRetirement());f.dropAt=0;
            CHECK(f.controller.stop(poll,r)==d::Status::Ok);checkDisconnected(f);}
        // Foreign references from ANY other MPCC or OPP must prevent writes.
        for(uint8_t n=0;n<4;++n)for(auto reg:{d::Register::Top,d::Register::Bottom,d::Register::OppId,d::Register::OutMux,d::Register::LockSelect}){
            Device f(pipe);f.reg(n,reg)=n==pipe?uint32_t((pipe+1)%4):pipe;
            CHECK(f.configure(r)==d::Status::ForeignControl);CHECK(f.writes==0);}
        for(uint32_t dwb:{uint32_t(0xf0|pipe),uint32_t((pipe<<4)|15)}){
            Device f(pipe);f.reg(d::Register::DwbMux)=dwb;CHECK(f.configure(r)==d::Status::ForeignControl);CHECK(f.writes==0);}
        for(auto reg:{d::Register::HubpClock,d::Register::DppControl,d::Register::OppControl,d::Register::Stereo,d::Register::Cursor,d::Register::Pattern,
            d::Register::OtgLock,d::Register::LockSelect}){
            Device f(pipe);f.reg(reg)=reg==d::Register::DppControl?0x10:reg==d::Register::LockSelect?0x10:1;
            CHECK(f.configure(r)==d::Status::ForeignControl);CHECK(f.writes==0);}
        for(uint32_t mask:{0x20000000u,0x40000000u,0x80000000u}){Device f(pipe);f.reg(d::Register::Power)=mask;
            CHECK(f.configure(r)==d::Status::PowerUnavailable);CHECK(f.writes==0);}
        for(uint32_t mask:{1u,2u,8u}){Device f(pipe);f.reg(d::Register::HubpControl)&=~mask;
            CHECK(f.configure(r)==d::Status::ActivePipe);CHECK(f.writes==0);}
        Device reset(pipe);reset.reg(d::Register::HubpControl)|=4;
        CHECK(reset.configure(r)==d::Status::ActivePipe);CHECK(reset.writes==0);
        for(uint32_t xrq=0;xrq<16;++xrq){Device f(pipe);f.reg(d::Register::HubpControl)=0xb|(xrq<<16);
            CHECK(f.configure(r)==d::Status::Ok);CHECK(f.controller.stop(poll,r)==d::Status::Ok);checkDisconnected(f);}
        for(uint32_t mask:{1u,0x10000u}){Device f(pipe);f.reg(d::Register::OtgControl)=mask;
            CHECK(f.configure(r)==d::Status::ActivePipe);CHECK(f.writes==0);}
        Device bad(pipe);bad.layout.registers[3][size_t(d::Register::Shaper)]+=4;
        CHECK(bad.configure(r)==d::Status::InvalidArgument);CHECK(bad.trace.empty());
        Device readback(pipe);size_t firstWrite=0;
        for(size_t n=0;n<configTrace.size();++n)if(configTrace[n].write){firstWrite=n+1;break;}
        readback.dropAt=firstWrite;CHECK(readback.configure(r)==d::Status::VerifyFailed);
        CHECK(readback.controller.requiresHardwareRetirement());CHECK(readback.controller.stop(poll,r)==d::Status::Ok);
        Device timeout(pipe);timeout.noClock=true;CHECK(timeout.configure(r)==d::Status::Timeout);
        CHECK(timeout.controller.requiresHardwareRetirement());timeout.noClock=false;
        CHECK(timeout.controller.stop(poll,r)==d::Status::Ok);
        Device blocked(pipe);CHECK(blocked.configure(r)==d::Status::Ok);blocked.busy=true;
        CHECK(blocked.controller.stop(poll,r)==d::Status::Timeout);CHECK(blocked.controller.requiresHardwareRetirement());
        CHECK((blocked.reg(d::Register::DppControl)&0x10)!=0);CHECK((blocked.reg(d::Register::HubpClock)&1)!=0);blocked.busy=false;
        CHECK(blocked.controller.stop(poll,r)==d::Status::Ok);
        Device stolen(pipe);CHECK(stolen.configure(r)==d::Status::Ok);stolen.reg((pipe+1)%4,d::Register::Bottom)=pipe;
        const size_t before=stolen.writes;CHECK(stolen.controller.stop(poll,r)==d::Status::ForeignControl);
        CHECK(stolen.writes==before&&stolen.controller.requiresHardwareRetirement());
        stolen.controller.clearAfterVerifiedReset();CHECK(!stolen.controller.requiresHardwareRetirement());
        Device preserved(pipe);preserved.reg(d::Register::DppControl)=0x100;
        preserved.reg(d::Register::HubpClock)=0x10000;
        const uint32_t hubpControl=0x030ee0fb;
        preserved.reg(d::Register::HubpControl)=hubpControl|0x1000|0x84000000;
        preserved.reg(d::Register::HubpRequestDebug)=0x87654321;
        preserved.reg(d::Register::FormatControl)=0x00800000;preserved.reg(d::Register::MpccControl)=0x4000;
        preserved.reg(d::Register::FmtControl)=0x4000;preserved.reg(d::Register::OutMux)|=0x80;
        CHECK(preserved.configure(r)==d::Status::Ok);
        CHECK(preserved.reg(d::Register::DppControl)==0x110);CHECK(preserved.reg(d::Register::FormatControl)==0x24800000);
        CHECK(preserved.reg(d::Register::HubpClock)==0x10001);
        CHECK(preserved.reg(d::Register::HubpControl)==hubpControl);
        CHECK(preserved.reg(d::Register::HubpRequestDebug)==0x04000000);
        CHECK(preserved.reg(d::Register::MpccControl)==0xffff4422);CHECK(preserved.reg(d::Register::FmtControl)==0x4000);
        CHECK(preserved.controller.stop(poll,r)==d::Status::Ok);CHECK(preserved.reg(d::Register::DppControl)==0x100);
        CHECK(preserved.reg(d::Register::HubpClock)==0x10000);
        for(const auto& a:preserved.trace)if(a.write&&a.address==preserved.layout.registers[pipe][size_t(d::Register::OutMux)])
            CHECK((a.value&0x80)==0); // Never replay the rate-error ACK bit.
        for(const auto& a:preserved.trace)if(a.write&&a.address==preserved.layout.registers[pipe][size_t(d::Register::HubpControl)])
            CHECK((a.value&0x84000000)==0); // TTU initialization does not acknowledge errors.
    }
    auto b=discovery();d::Layout l={};
    for(size_t size=0;size<b.size();++size)CHECK(d::buildLayout(b.data(),size,0x100000,l)==d::Status::DiscoveryRejected);
    CHECK(d::buildLayout(b.data(),b.size(),4,l)==d::Status::OutsideBar);CHECK(!l.valid);
    b[149]=1;checksum(b);CHECK(d::buildLayout(b.data(),b.size(),0x100000,l)==d::Status::UnsupportedDmu);
    b=discovery();put32(b,164,0x40000000);checksum(b);CHECK(d::buildLayout(b.data(),b.size(),UINT64_MAX,l)==d::Status::OutsideBar);
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
