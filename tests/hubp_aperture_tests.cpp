// SPDX-License-Identifier: MIT
// Host-only protocol fault model. No test claims target GPU execution.
#include "hackynavi/hubp_aperture.hpp"
#include <iostream>
#include <map>
#include <vector>
namespace a=hackynavi::hubpaperture;
namespace h=hackynavi::hubpscanout;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v){put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t p){uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s;}
std::vector<uint8_t> discovery(){
    std::vector<uint8_t> b(168,0);put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,168);
    put16(b,12,60);put16(b,16,108);put32(b,60,0x53445049);put16(b,64,3);put16(b,66,108);
    put16(b,72,1);put16(b,76,140);put16(b,142,1);put16(b,144,271);b[147]=4;b[148]=3;b[149]=2;
    put32(b,156,0x1200);put32(b,160,0x3400);put32(b,164,0x6000);
    put16(b,14,sum(b,60));put16(b,8,sum(b,10));return b;
}
struct Event {bool write;uint32_t address,value;};
struct Device {
    std::vector<uint8_t> bytes=discovery();h::Layout layout={};hackynavi::MemoryLayout memory={};
    std::map<uint32_t,uint32_t> regs;std::vector<Event> trace;uint8_t pipe;
    size_t failAt=0,onesAt=0,dropAt=0,powerOffAt=0,changeAt=0;bool changeConfig=false;
    explicit Device(uint8_t p=0):pipe(p){
        CHECK(h::buildLayout(bytes.data(),bytes.size(),0x100000,layout)==h::Status::Ok);
        const hackynavi::MemoryLayoutInput input={0x100,0x103,0x200,64ULL<<20,0x400000000ULL,64ULL<<20};
        CHECK(hackynavi::deriveMemoryLayout(input,memory)==hackynavi::MemoryLayoutStatus::Ok);
        for(size_t n=0;n<4;++n)for(size_t r=0;r<h::kRegisterCount;++r)regs[layout.registers[n][r]]=0;
        reg(h::Register::Clock)=1;reg(h::Register::Control)=0xb;
        reg(h::Register::ReturnControl)=0x15040010;
        const uint32_t fb[]={0x100,0x103,0x200,0xffffff,0};
        for(size_t n=0;n<5;++n)regs[layout.framebuffer[n]]=fb[n];
    }
    uint32_t address(h::Register r)const{return layout.registers[pipe][size_t(r)];}
    uint32_t& reg(h::Register r){return regs[address(r)];}
    h::Io io(){return {this,read,write,delay};}
    static bool read(void* p,uint32_t offset,uint32_t& value){
        auto& d=*static_cast<Device*>(p);CHECK(d.regs.count(offset)==1);
        if(d.trace.size()+1==d.powerOffAt)d.reg(h::Register::Power)=0x20000000;
        if(d.trace.size()+1==d.changeAt)d.reg(d.changeConfig?h::Register::PowerConfig:h::Register::Power)^=d.changeConfig?1u:0x10000000u;
        value=d.regs[offset];d.trace.push_back({false,offset,value});
        if(d.trace.size()==d.onesAt)value=UINT32_MAX;
        return d.trace.size()!=d.failAt;
    }
    static bool write(void* p,uint32_t offset,uint32_t value){
        auto& d=*static_cast<Device*>(p);CHECK(d.regs.count(offset)==1);d.trace.push_back({true,offset,value});
        // A failed transport can still have delivered the write.
        if(d.trace.size()!=d.dropAt)d.regs[offset]=value;
        return d.trace.size()!=d.failAt;
    }
    static void delay(void*,uint32_t){CHECK(false);}
    a::Status program(a::Result& r){return a::program(bytes.data(),bytes.size(),0x100000,io(),pipe,memory,r);}
    size_t writes()const{size_t n=0;for(const auto& e:trace)if(e.write)++n;return n;}
};
void rejected(h::Register reg,uint32_t value,a::Status status){
    Device d;d.reg(reg)=value;a::Result r={};CHECK(d.program(r)==status);CHECK(!r.writeAttempted);CHECK(d.writes()==0);
}
}
int main(){
    std::vector<Event> baseline;
    for(uint8_t pipe=0;pipe<4;++pipe){
        Device d(pipe);auto original=d.regs;a::Result r={};CHECK(d.program(r)==a::Status::Ok);
        CHECK(r.writeAttempted&&r.writes==3&&d.writes()==3);CHECK(r.reads+3==d.trace.size());
        const h::Register expected[]={h::Register::ApertureLow,h::Register::ApertureHigh,h::Register::TlbControl};
        const uint32_t values[]={0x4000,0x40ff,0x19};size_t n=0;
        for(const auto& event:d.trace)if(event.write){CHECK(n<3);if(n<3){CHECK(event.address==d.address(expected[n]));CHECK(event.value==values[n]);++n;}}
        for(size_t x=0;x<3;++x)original[d.address(expected[x])]=values[x];CHECK(original==d.regs);
        if(pipe==0)baseline=d.trace;
        // Exercise the actual consumer against the newly programmed aperture.
        hackynavi::VramAllocator allocator;hackynavi::VramAllocation allocation={};
        CHECK(allocator.configure(d.memory,{0,64ULL<<20},nullptr,0)==hackynavi::VramAllocatorStatus::Ok);
        CHECK(allocator.allocate(8ULL<<20,4096,allocation)==hackynavi::VramAllocatorStatus::Ok);
        h::Controller hubp;h::Result result={};const h::Surface surface={allocation.handle,0,1920,0,0,1920,1080,h::Format::Argb8888};
        CHECK(hubp.configure(d.layout,d.io(),pipe,d.memory,allocator,surface,result)==h::Status::Ok);
        CHECK(hubp.phase()==h::Phase::Prepared);CHECK(hubp.requiresHardwareRetirement());
        CHECK(hubp.stop({3,1},result)==h::Status::Ok);CHECK(!hubp.requiresHardwareRetirement());
        CHECK(allocator.release(allocation.handle)==hackynavi::VramAllocatorStatus::Ok);
    }
    for(uint8_t pipe=0;pipe<4;++pipe)for(uint32_t status:{0u,0x10000000u})for(uint32_t config:{0u,1u}){
        Device d(pipe);d.reg(h::Register::Power)=status;d.reg(h::Register::PowerConfig)=config;a::Result r={};
        CHECK(d.program(r)==a::Status::Ok&&r.writes==3);
        CHECK(d.reg(h::Register::Power)==status&&d.reg(h::Register::PowerConfig)==config);
    }
    // Change to another individually valid ON value at every subsequent
    // power/config observation; exact raw continuity must reject it.
    for(bool config:{false,true})for(size_t at=3;at<=baseline.size();++at)
        if(!baseline[at-1].write&&baseline[at-1].address==Device().address(
            config?h::Register::PowerConfig:h::Register::Power)){
            Device d;d.changeAt=at;d.changeConfig=config;a::Result r={};
            CHECK(d.program(r)==a::Status::PowerUnavailable);
            for(size_t n=at-1;n<d.trace.size();++n)CHECK(!d.trace[n].write&&
                (d.trace[n].address==d.address(h::Register::Power)||d.trace[n].address==d.address(h::Register::PowerConfig)));
            CHECK(r.writeAttempted==(d.writes()!=0));
        }
    for(unsigned bit=0;bit<32;++bit)if(bit!=28)rejected(h::Register::Power,1u<<bit,a::Status::PowerUnavailable);
    for(unsigned bit=1;bit<32;++bit)rejected(h::Register::PowerConfig,1u<<bit,a::Status::PowerUnavailable);
    // Every transport operation may fail; reads may return all ones; writes may
    // disappear or be delivered despite failure. No path may report success.
    for(size_t at=1;at<=baseline.size();++at){
        Device d;d.failAt=at;a::Result r={};CHECK(d.program(r)==a::Status::TransportError);
        CHECK(d.trace.size()==at);CHECK(r.writeAttempted==(d.writes()!=0));
        if(!baseline[at-1].write){Device x;x.onesAt=at;CHECK(x.program(r)==a::Status::DeviceUnavailable);CHECK(x.trace.size()==at);}
        else {Device x;x.dropAt=at;CHECK(x.program(r)==a::Status::VerifyFailed);CHECK(r.writeAttempted);CHECK(x.writes()==d.writes());}
    }
    // A power transition observed before any gated access must stop access.
    for(size_t at=1;at<=baseline.size();++at)if(!baseline[at-1].write&&baseline[at-1].address==Device().address(h::Register::Power)){
        Device d;d.powerOffAt=at;a::Result r={};CHECK(d.program(r)==a::Status::PowerUnavailable);CHECK(d.trace.size()==at);
    }
    rejected(h::Register::Power,0x20000000,a::Status::PowerUnavailable);
    rejected(h::Register::PowerConfig,0x100,a::Status::PowerUnavailable);
    rejected(h::Register::PowerConfig,2,a::Status::PowerUnavailable);
    rejected(h::Register::Control,1,a::Status::ActivePipe);
    rejected(h::Register::Control,3|4,a::Status::ActivePipe);
    rejected(h::Register::Control,3|0x800,a::Status::ActivePipe);
    rejected(h::Register::Clock,0,a::Status::ClockUnavailable);
    rejected(h::Register::OtgControl,1,a::Status::ActivePipe);
    rejected(h::Register::OtgControl,0x10000,a::Status::ActivePipe);
    rejected(h::Register::Vmid,1,a::Status::AddressMode);
    rejected(h::Register::FlipControl,0x100,a::Status::ActivePipe);
    rejected(h::Register::CursorControl,1,a::Status::ForeignControl);
    rejected(h::Register::DmdataControl,1,a::Status::ForeignControl);
    rejected(h::Register::MallConfig,1,a::Status::ForeignControl);
    for(size_t n=0;n<5;++n){Device d;d.regs[d.layout.framebuffer[n]]|=0x1000000;a::Result r={};CHECK(d.program(r)==a::Status::AddressMode);CHECK(!r.writeAttempted);}
    for(size_t n=0;n<3;++n){Device d;d.regs[d.layout.framebuffer[n]]^=1;a::Result r={};CHECK(d.program(r)==a::Status::AddressMode);CHECK(!r.writeAttempted);}
    {Device d;d.regs[d.layout.framebuffer[3]]=0;a::Result r={};CHECK(d.program(r)==a::Status::AddressMode);CHECK(!r.writeAttempted);}
    {Device d;d.reg(h::Register::PowerConfig)=1;a::Result r={};CHECK(d.program(r)==a::Status::Ok);}
    for(size_t n=0;n<7;++n){
        Device d;a::Result r={};auto io=d.io();auto m=d.memory;uint8_t pipe=0;const uint8_t* data=d.bytes.data();size_t bytes=d.bytes.size();uint64_t bar=0x100000;
        if(n==0)io.read32=nullptr;if(n==1)io.write32=nullptr;if(n==2)pipe=4;
        if(n==3)m.mcWindowEndInclusive=UINT64_C(1)<<48;
        if(n==4)m.mcWindowStart=m.mcWindowEndInclusive+1;
        if(n==5)data=nullptr;if(n==6)bar=4;
        CHECK(a::program(data,bytes,bar,io,pipe,m,r)!=a::Status::Ok);CHECK(d.trace.empty());CHECK(!r.writeAttempted);
    }
    std::cout<<checks<<" HUBP aperture checks; "<<failures<<" failures\n";return failures?1:0;
}
