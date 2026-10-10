// SPDX-License-Identifier: MIT
// Host-only MMIO protocol model. These tests do not execute or validate a GPU.
#include "hackynavi/hubp_scanout.hpp"
#include <iostream>
#include <map>
#include <vector>
namespace h=hackynavi::hubpscanout;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";} } while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v){put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t p){uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s;}
void checksums(std::vector<uint8_t>& b){put16(b,14,sum(b,60));put16(b,8,sum(b,10));}
std::vector<uint8_t> discovery(){
    std::vector<uint8_t> b(164,0);put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,164);
    put16(b,12,60);put16(b,16,104);put32(b,60,0x53445049);put16(b,64,3);put16(b,66,104);
    put16(b,72,1);put16(b,76,140);put16(b,142,1);put16(b,144,271);b[147]=3;b[148]=3;b[149]=2;
    put32(b,160,0x34c0);checksums(b);return b;
}
const h::PollOptions poll={3,10};
struct Event { bool write;uint32_t address,value; };
struct Device {
    h::Layout layout={};h::Controller controller;hackynavi::MemoryLayout memory={};hackynavi::VramAllocator allocator;
    hackynavi::VramAllocation a={},b={};h::Surface surface={},next={};std::map<uint32_t,uint32_t> registers;
    std::vector<Event> events;size_t calls=0,writes=0,delays=0,failAt=0,onesAt=0,dropAt=0;
    size_t changeAt=0;bool changeConfig=false;
    bool reflectInUse=true,reflectEarliest=true,keepPending=false,drainOnBlank=true;
    uint8_t pipe;
    explicit Device(uint8_t p=0):pipe(p){
        auto bytes=discovery();CHECK(h::buildLayout(bytes.data(),bytes.size(),0x100000,layout)==h::Status::Ok);
        const hackynavi::MemoryLayoutInput input={0x100,0x103,0x200,64ULL<<20,0x400000000ULL,64ULL<<20};
        CHECK(hackynavi::deriveMemoryLayout(input,memory)==hackynavi::MemoryLayoutStatus::Ok);
        CHECK(allocator.configure(memory,{0,64ULL<<20},nullptr,0)==hackynavi::VramAllocatorStatus::Ok);
        CHECK(allocator.allocate(8ULL<<20,4096,a)==hackynavi::VramAllocatorStatus::Ok);
        CHECK(allocator.allocate(8ULL<<20,4096,b)==hackynavi::VramAllocatorStatus::Ok);
        surface={a.handle,0,1920,0,0,1920,1080,h::Format::Argb8888};next=surface;next.allocation=b.handle;
        for(size_t q=0;q<h::kPipeCount;++q)for(size_t r=0;r<h::kRegisterCount;++r)registers[layout.registers[q][r]]=0;
        for(uint8_t q=0;q<4;++q){
            reg(q,h::Register::Clock)=1;reg(q,h::Register::Control)=3;reg(q,h::Register::OtgControl)=0x10001;
            reg(q,h::Register::TlbControl)=0x19;reg(q,h::Register::ApertureLow)=0x4000;reg(q,h::Register::ApertureHigh)=0x40ff;
            reg(q,h::Register::ReturnControl)=0x15040010;reg(q,h::Register::RequestSize)=0x4321056;
        }
        const uint32_t fb[]={0x100,0x103,0x200,0xffffff,0};
        for(size_t i=0;i<5;++i)registers[layout.framebuffer[i]]=fb[i];
    }
    uint32_t address(uint8_t p,h::Register r)const{return layout.registers[p][size_t(r)];}
    uint32_t& reg(uint8_t p,h::Register r){return registers[address(p,r)];}
    uint32_t& reg(h::Register r){return reg(pipe,r);}
    h::Io io(){return {this,read,write,delay};}
    void resetEvents(){events.clear();calls=0;writes=0;delays=0;failAt=onesAt=dropAt=0;}
    static bool read(void* ctx,uint32_t address,uint32_t& value){
        auto& d=*static_cast<Device*>(ctx);++d.calls;CHECK(d.registers.count(address)==1);
        if(d.calls==d.changeAt)d.reg(d.changeConfig?h::Register::PowerConfig:h::Register::Power)^=d.changeConfig?1u:0x10000000u;
        value=d.registers[address];if(d.calls==d.onesAt)value=UINT32_MAX;
        d.events.push_back({false,address,value});return d.calls!=d.failAt;
    }
    static bool write(void* ctx,uint32_t address,uint32_t value){
        auto& d=*static_cast<Device*>(ctx);++d.calls;++d.writes;CHECK(d.registers.count(address)==1);
        d.events.push_back({true,address,value});
        if(d.calls!=d.dropAt){
            const uint32_t old=d.registers[address];d.registers[address]=value;
            for(uint8_t p=0;p<4;++p){
                if(address==d.address(p,h::Register::Control)){
                    d.registers[address]=(value&~0x70ff000aU)|(old&0x70ff000aU);
                    if((value&1)&&d.drainOnBlank)d.registers[address]|=2;
                }
                if(address==d.address(p,h::Register::AddressLo)){
                    const uint32_t hi=d.reg(p,h::Register::AddressHi);
                    if(d.reflectInUse){d.reg(p,h::Register::InUseLo)=value;d.reg(p,h::Register::InUseHi)=hi;}
                    if(d.reflectEarliest){d.reg(p,h::Register::EarliestLo)=value;d.reg(p,h::Register::EarliestHi)=hi;}
                    if(d.keepPending)d.reg(p,h::Register::FlipControl)|=0x100;
                    else d.reg(p,h::Register::FlipControl)&=~0x100u;
                }
                if(address==d.address(p,h::Register::Interrupt)){
                    d.registers[address]=(old&~0xfu)|(value&0xf);
                    if(value&0x100)d.registers[address]&=~0x30000u;
                    if(value&0x200)d.registers[address]&=~0xc0000u;
                }
            }
        }
        // A failed write is deliberately delivered, modelling ambiguous I/O.
        return d.calls!=d.failAt;
    }
    static void delay(void* ctx,uint32_t us){auto& d=*static_cast<Device*>(ctx);CHECK(us==10);++d.delays;}
    h::Status configure(h::Result& r){return controller.configure(layout,io(),pipe,memory,allocator,surface,r);}
    void running(h::Result& r){CHECK(configure(r)==h::Status::Ok);CHECK(controller.start(poll,r)==h::Status::Ok);}
};
void plans(){
    Device d;h::Plan p={};CHECK(h::plan(d.memory,d.allocator,d.surface,p)==h::Status::Ok);
    CHECK(p.mcAddress==0x100000000ULL&&p.pitch==1919&&p.footprintBytes==8294400);
    CHECK(p.viewportStart==0&&p.viewportDimension==(1080u<<16|1920u));
    const h::Format formats[]={h::Format::Argb8888,h::Format::Abgr8888,h::Format::Argb2101010,h::Format::Abgr2101010};
    for(size_t i=0;i<4;++i){d.surface.format=formats[i];CHECK(h::plan(d.memory,d.allocator,d.surface,p)==h::Status::Ok);
        CHECK(p.surfaceConfig==(i<2?8u:10u));CHECK(p.crossbar==(i%2?0xb00000u:0xe00000u));}
    auto s=d.surface;s.pitchPixels=1921;CHECK(h::plan(d.memory,d.allocator,s,p)==h::Status::InvalidArgument);
    s=d.surface;s.byteOffset=1;CHECK(h::plan(d.memory,d.allocator,s,p)==h::Status::InvalidArgument);
    s=d.surface;s.width=0;CHECK(h::plan(d.memory,d.allocator,s,p)==h::Status::InvalidArgument);
    s=d.surface;s.x=1;CHECK(h::plan(d.memory,d.allocator,s,p)==h::Status::InvalidArgument);
    s=d.surface;s.format=static_cast<h::Format>(0xff);CHECK(h::plan(d.memory,d.allocator,s,p)==h::Status::InvalidArgument);
    s=d.surface;s.byteOffset=8ULL<<20;CHECK(h::plan(d.memory,d.allocator,s,p)==h::Status::OutsideAllocation);
    s=d.surface;s.y=100;CHECK(h::plan(d.memory,d.allocator,s,p)==h::Status::OutsideAllocation);
    s=d.surface;s.y=10;s.height=100;s.x=64;s.width=1600;s.byteOffset=256;
    CHECK(h::plan(d.memory,d.allocator,s,p)==h::Status::Ok);
    CHECK(p.footprintBytes==844800&&p.viewportStart==(10u<<16|64u)&&p.mcAddress==0x100000100ULL);
    auto memory=d.memory;memory.vramPageBaseOffset+=4096;CHECK(h::plan(memory,d.allocator,s,p)==h::Status::InvalidAllocation);
    Device other;s.allocation=other.a.handle;CHECK(h::plan(d.memory,d.allocator,s,p)==h::Status::InvalidAllocation);
    CHECK(d.allocator.release(d.a.handle)==hackynavi::VramAllocatorStatus::Ok);
    CHECK(h::plan(d.memory,d.allocator,d.surface,p)==h::Status::InvalidAllocation);
    CHECK(p.mcAddress==0&&p.footprintBytes==0);
}
void layouts(){
    auto bytes=discovery();h::Layout l={};
    for(size_t n=0;n<bytes.size();++n)CHECK(h::buildLayout(bytes.data(),n,0x100000,l)!=h::Status::Ok&&!l.valid);
    CHECK(h::buildLayout(bytes.data(),bytes.size(),0x100,l)==h::Status::OutsideBar&&!l.valid);
    CHECK(h::buildLayout(bytes.data(),bytes.size(),0x100000,l)==h::Status::Ok);
    for(size_t p=0;p<4;++p){
        CHECK(l.registers[p][size_t(h::Register::Power)]==(0x34c0+0x81+2*p)*4);
        CHECK(l.registers[p][size_t(h::Register::Pitch)]==(0x34c0+0x607+0xdc*p)*4);
        CHECK(l.registers[p][size_t(h::Register::AddressHi)]==(0x34c0+0x60b+0xdc*p)*4);
        CHECK(l.registers[p][size_t(h::Register::OtgControl)]==(0x34c0+0x1b41+0x80*p)*4);
    }
    bytes[149]=1;checksums(bytes);CHECK(h::buildLayout(bytes.data(),bytes.size(),0x100000,l)==h::Status::UnsupportedDmu);
    Device d;h::Result r={};d.layout.registers[3][4]+=4;CHECK(d.configure(r)==h::Status::InvalidArgument&&d.calls==0);
}
void lifecycle(){
    for(uint8_t pipe=0;pipe<4;++pipe){Device d(pipe);h::Result r={};d.surface.format=static_cast<h::Format>(pipe);
        d.next.format=d.surface.format;CHECK(d.configure(r)==h::Status::Ok);
        CHECK(d.controller.phase()==h::Phase::Prepared&&r.retentionRequired&&d.controller.requiresHardwareRetirement());
        CHECK(d.reg(h::Register::Pitch)==1919&&d.reg(h::Register::SurfaceConfig)==(pipe<2?8u:10u));
        CHECK(d.reg(h::Register::ReturnControl)==(pipe%2?0x15b40010u:0x15e40010u));
        CHECK(d.reg(h::Register::RequestSize)==0x4321050);
        CHECK((d.reg(h::Register::Control)&0xf1)==(uint32_t(pipe)<<4|1));
        size_t high=0,low=0;for(size_t i=0;i<d.events.size();++i)if(d.events[i].write){
            if(d.events[i].address==d.address(pipe,h::Register::AddressHi))high=i;
            if(d.events[i].address==d.address(pipe,h::Register::AddressLo))low=i;
        }
        CHECK(high&&low>high);
        CHECK(d.controller.start(poll,r)==h::Status::Ok&&d.controller.phase()==h::Phase::Running);
        CHECK(d.controller.setFlipInterrupt(true,r)==h::Status::Ok&&d.reg(h::Register::Interrupt)==1);
        CHECK(d.controller.submitFlip(d.next,r)==h::Status::Ok&&d.controller.phase()==h::Phase::FlipPending);
        CHECK(d.controller.current().surface.allocation.serial==d.a.handle.serial);
        CHECK(d.controller.pending().surface.allocation.serial==d.b.handle.serial);
        d.reg(h::Register::Interrupt)|=0xf0000;bool occurred=false;
        CHECK(d.controller.acknowledgeFlipInterrupt(occurred,r)==h::Status::Ok&&occurred);
        CHECK(d.reg(h::Register::Interrupt)==0xc0001); // No accidental away-event acknowledgement.
        CHECK(d.controller.phase()==h::Phase::FlipPending);
        CHECK(d.controller.completeFlip(poll,r)==h::Status::Ok&&d.controller.phase()==h::Phase::Running);
        CHECK(d.controller.current().surface.allocation.serial==d.b.handle.serial);
        CHECK(d.controller.pending().surface.allocation.owner==nullptr);
        CHECK(d.controller.stop(poll,r)==h::Status::Ok&&!r.retentionRequired);
        CHECK(d.controller.phase()==h::Phase::Stopped&&!d.controller.requiresHardwareRetirement());
        CHECK((d.reg(h::Register::Control)&3)==3&&(d.reg(h::Register::Interrupt)&5)==0);
        CHECK(d.configure(r)==h::Status::Ok);CHECK(d.controller.stop(poll,r)==h::Status::Ok);
    }
}
void powerStates(){
    for(uint8_t pipe=0;pipe<4;++pipe)for(uint32_t status:{0u,0x10000000u})for(uint32_t config:{0u,1u}){
        Device d(pipe);d.reg(h::Register::Power)=status;d.reg(h::Register::PowerConfig)=config;h::Result r={};
        d.running(r);CHECK(d.controller.stop(poll,r)==h::Status::Ok&&!r.retentionRequired);
        CHECK(d.reg(h::Register::Power)==status&&d.reg(h::Register::PowerConfig)==config);
        for(bool cfg:{false,true}){Device changed(pipe);changed.reg(h::Register::Power)=status;changed.reg(h::Register::PowerConfig)=config;
            changed.running(r);const auto writes=changed.writes;
            changed.reg(cfg?h::Register::PowerConfig:h::Register::Power)^=cfg?1u:0x10000000u;
            CHECK(changed.controller.stop(poll,r)==h::Status::PowerUnavailable&&changed.writes==writes&&r.retentionRequired);}
    }
    Device baseline;h::Result r={};CHECK(baseline.configure(r)==h::Status::Ok);
    std::vector<size_t> points={3,4,5,7};
    for(size_t n=0;n<baseline.events.size();++n)if(baseline.events[n].write){points.push_back(n+2);break;}
    for(bool config:{false,true})for(size_t at:points){Device d;d.changeAt=at;d.changeConfig=config;
        CHECK(d.configure(r)==h::Status::PowerUnavailable);CHECK(r.retentionRequired==(d.writes!=0));}
    for(unsigned bit=0;bit<32;++bit)if(bit!=28){Device d;d.reg(h::Register::Power)=1u<<bit;
        CHECK(d.configure(r)==h::Status::PowerUnavailable&&d.writes==0);}
    for(unsigned bit=1;bit<32;++bit){Device d;d.reg(h::Register::PowerConfig)=1u<<bit;
        CHECK(d.configure(r)==h::Status::PowerUnavailable&&d.writes==0);}
}
void admission(){
    const struct { h::Register reg;uint32_t value;h::Status status; } cases[]={
        {h::Register::Power,0x90000000,h::Status::PowerUnavailable},
        {h::Register::PowerConfig,0x100,h::Status::PowerUnavailable},
        {h::Register::Clock,0,h::Status::ClockUnavailable},{h::Register::Control,2,h::Status::ActivePipe},
        {h::Register::Control,1,h::Status::ActivePipe},{h::Register::Control,7,h::Status::ForeignControl},
        {h::Register::Control,0x10000003,h::Status::HardwareFault},
        {h::Register::FlipControl,1,h::Status::ForeignControl},{h::Register::FlipControl,0x100,h::Status::ActivePipe},
        {h::Register::FlipControl,0x200,h::Status::ForeignControl},{h::Register::FlipControl2,0x400,h::Status::ForeignControl},
        {h::Register::CursorControl,1,h::Status::ForeignControl},{h::Register::DmdataControl,4,h::Status::ForeignControl},
        {h::Register::MallConfig,1,h::Status::ForeignControl},{h::Register::OtgInterlace,1,h::Status::ForeignControl},
        {h::Register::Interrupt,1,h::Status::ForeignControl},{h::Register::Interrupt,4,h::Status::ForeignControl},
        {h::Register::Vmid,1,h::Status::AddressMode},{h::Register::TlbControl,0,h::Status::AddressMode},
        {h::Register::ApertureLow,0x4001,h::Status::AddressMode},{h::Register::ApertureHigh,0x4000,h::Status::AddressMode}
    };
    for(const auto& c:cases){Device d;h::Result r={};d.reg(c.reg)=c.value;CHECK(d.configure(r)==c.status);
        CHECK(d.writes==0&&!r.retentionRequired&&d.controller.phase()==h::Phase::Closed);}
    for(size_t n=0;n<5;++n){Device d;h::Result r={};d.registers[d.layout.framebuffer[n]]^=0x80000000;
        CHECK(d.configure(r)==h::Status::AddressMode&&d.writes==0);}
    Device d;h::Result r={};CHECK(d.configure(r)==h::Status::Ok);d.reg(h::Register::OtgControl)=0;
    CHECK(d.controller.start(poll,r)==h::Status::InvalidState&&d.controller.phase()==h::Phase::Prepared);
    CHECK(d.controller.start({0,10},r)==h::Status::InvalidArgument);
    CHECK(d.controller.start({100001,1},r)==h::Status::InvalidArgument);
    CHECK(d.controller.start({1,101},r)==h::Status::InvalidArgument);
    CHECK(d.controller.requiresHardwareRetirement());
}
void completion(){
    for(int mode=0;mode<3;++mode){Device d;h::Result r={};d.running(r);
        if(mode==0)d.reflectInUse=false;if(mode==1)d.reflectEarliest=false;if(mode==2)d.keepPending=true;
        CHECK(d.controller.submitFlip(d.next,r)==h::Status::Ok);
        d.reg(h::Register::Interrupt)|=0x10000;bool occurred=false;CHECK(d.controller.acknowledgeFlipInterrupt(occurred,r)==h::Status::Ok&&occurred);
        CHECK(d.controller.completeFlip(poll,r)==h::Status::Timeout&&r.polls==3&&r.retentionRequired);
        CHECK(d.controller.phase()==h::Phase::Faulted);CHECK(d.controller.current().surface.allocation.serial==d.a.handle.serial);
        CHECK(d.controller.pending().surface.allocation.serial==d.b.handle.serial);
        const auto calls=d.calls;CHECK(d.controller.stop(poll,r)==h::Status::InvalidState&&d.calls==calls);
        d.controller.clearAfterVerifiedReset();CHECK(!d.controller.requiresHardwareRetirement()&&d.controller.phase()==h::Phase::Closed);
    }
    Device d;h::Result r={};d.running(r);auto changed=d.next;changed.format=h::Format::Abgr8888;d.resetEvents();
    CHECK(d.controller.submitFlip(changed,r)==h::Status::InvalidArgument&&d.calls==0&&d.controller.phase()==h::Phase::Running);
    d.reg(h::Register::Pitch)=1900;CHECK(d.controller.submitFlip(d.next,r)==h::Status::ForeignControl&&d.controller.requiresHardwareRetirement());
    Device stale;stale.running(r);CHECK(stale.allocator.release(stale.a.handle)==hackynavi::VramAllocatorStatus::Ok);
    CHECK(stale.controller.submitFlip(stale.next,r)==h::Status::InvalidAllocation&&r.retentionRequired);
    Device pending;pending.running(r);pending.keepPending=true;CHECK(pending.controller.submitFlip(pending.next,r)==h::Status::Ok);
    CHECK(pending.controller.stop(poll,r)==h::Status::Timeout&&r.retentionRequired);
    Device drain;drain.running(r);drain.reg(h::Register::Control)&=~2u;
    CHECK(drain.controller.stop(poll,r)==h::Status::Timeout&&r.polls==3&&r.retentionRequired);
}
enum class Operation { Configure,Start,Submit,Complete,Stop };
void prepare(Device& d,Operation op,h::Result& r){
    if(op!=Operation::Configure)CHECK(d.configure(r)==h::Status::Ok);
    if(op==Operation::Submit||op==Operation::Complete||op==Operation::Stop)CHECK(d.controller.start(poll,r)==h::Status::Ok);
    if(op==Operation::Complete)CHECK(d.controller.submitFlip(d.next,r)==h::Status::Ok);
    d.resetEvents();
}
h::Status run(Device& d,Operation op,h::Result& r){switch(op){
    case Operation::Configure:return d.configure(r);case Operation::Start:return d.controller.start(poll,r);
    case Operation::Submit:return d.controller.submitFlip(d.next,r);case Operation::Complete:return d.controller.completeFlip(poll,r);
    case Operation::Stop:return d.controller.stop(poll,r);
    }return h::Status::InvalidArgument;
}
void failuresAtEveryAccess(){
    const Operation operations[]={Operation::Configure,Operation::Start,Operation::Submit,Operation::Complete,Operation::Stop};
    for(const auto op:operations){Device baseline;h::Result r={};prepare(baseline,op,r);CHECK(run(baseline,op,r)==h::Status::Ok);
        for(size_t n=1;n<=baseline.calls;++n){Device d;prepare(d,op,r);d.failAt=n;
            CHECK(run(d,op,r)==h::Status::TransportError);
            CHECK(d.calls==n);CHECK(r.retentionRequired==(op!=Operation::Configure||d.writes!=0));
            if(r.retentionRequired){CHECK(d.controller.phase()==h::Phase::Faulted);CHECK(d.controller.current().surface.allocation.owner==&d.allocator);}
            if(!baseline.events[n-1].write){Device ones;prepare(ones,op,r);ones.onesAt=n;
                CHECK(run(ones,op,r)==h::Status::DeviceUnavailable);CHECK(ones.calls==n);
                CHECK(r.retentionRequired==(op!=Operation::Configure||ones.writes!=0));}
        }
    }
    Device baseline;h::Result r={};baseline.surface.byteOffset=256;CHECK(baseline.configure(r)==h::Status::Ok);
    for(size_t i=0;i<baseline.events.size();++i)if(baseline.events[i].write && baseline.events[i].address==baseline.address(0,h::Register::AddressLo)){
        Device d;d.surface.byteOffset=256;d.dropAt=i+1;CHECK(d.configure(r)==h::Status::VerifyFailed&&r.retentionRequired);
    }
}
}
int main(){powerStates();plans();layouts();lifecycle();admission();completion();failuresAtEveryAccess();
    std::cout<<checks<<" HUBP host protocol checks, "<<failures<<" failures (no hardware execution)\n";return failures?1:0;}
