// SPDX-License-Identifier: MIT
// Host-only register model; not evidence of GPU execution.
#include "hackynavi/display_clock.hpp"
#include <iostream>
#include <map>
#include <vector>
namespace d=hackynavi::displayclock;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v){put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t p){uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s;}
std::vector<uint8_t> discovery(){
    std::vector<uint8_t> b(164,0);put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,164);
    put16(b,12,60);put16(b,16,104);put32(b,60,0x53445049);put16(b,64,3);put16(b,66,104);
    put16(b,72,1);put16(b,76,140);put16(b,142,1);put16(b,144,271);b[147]=3;b[148]=3;b[149]=2;
    put32(b,156,0x1234);put32(b,160,0x34c0);put16(b,14,sum(b,60));put16(b,8,sum(b,10));return b;
}
const d::Config config={600000,594000,1,2};
struct Device {
    d::Layout layout={};d::Controller controller;std::map<uint32_t,uint32_t> regs;
    size_t calls=0,failAt=0,onesAt=0,writes=0;bool drop=false;
    explicit Device(uint8_t pipe){auto b=discovery();CHECK(d::buildLayout(b.data(),b.size(),0x100000,layout)==d::Status::Ok);
        for(size_t n=0;n<4;++n)for(size_t r=0;r<d::kRegisterCount;++r)regs[layout.registers[n][r]]=0;
        reg(pipe,d::Register::DtoControl)=0x1111&~(3u<<(pipe*4));reg(pipe,d::Register::PixelDiv)=0xaaa;
        reg(pipe,d::Register::DtoParam)=0x55aa55bb;reg(pipe,d::Register::HubpControl)=0xb;}
    uint32_t& reg(uint8_t p,d::Register r){return regs[layout.registers[p][size_t(r)]];}
    d::Io io(){return {this,read,write};}
    static bool read(void* c,uint32_t a,uint32_t& v){auto& x=*static_cast<Device*>(c);++x.calls;
        CHECK(x.regs.count(a)==1);v=x.calls==x.onesAt?UINT32_MAX:x.regs[a];return x.calls!=x.failAt;}
    static bool write(void* c,uint32_t a,uint32_t v){auto& x=*static_cast<Device*>(c);++x.calls;++x.writes;
        CHECK(x.regs.count(a)==1);if(x.calls==x.failAt)return false;if(!x.drop)x.regs[a]=v;return true;}
};
}
int main(){
    d::Plan p={};CHECK(d::plan(config,p)==d::Status::Ok);CHECK(p.phase==253&&p.dtoParam==0xff00fd&&p.pixelDivBits==2);
    for(uint32_t ref:{1u,255u,256u,600000u,UINT32_MAX})for(uint32_t step=1;step<=255;++step){
        const uint32_t req=uint32_t((uint64_t(ref)*step+254)/255);
        CHECK(d::plan({ref,req,2,4},p)==d::Status::Ok);
        CHECK(uint64_t(p.phase)*ref>=uint64_t(req)*255);
        CHECK(uint64_t(p.phase-1)*ref<uint64_t(req)*255);
        CHECK(p.minimumOutputKHz>=req&&p.pixelDivBits==7);
    }
    for(d::Config bad: {d::Config{0,1,1,1},d::Config{1,0,1,1},d::Config{1,2,1,1},d::Config{1,1,4,1},d::Config{1,1,1,3}}){
        p.dtoParam=123;CHECK(d::plan(bad,p)==d::Status::InvalidArgument);CHECK(p.dtoParam==123);}
    for(uint8_t pipe=0;pipe<4;++pipe){
        Device x(pipe);d::Result r={};const auto saved=x.regs;
        CHECK(x.controller.configure(x.layout,x.io(),pipe,config,r)==d::Status::Ok);
        CHECK(x.reg(pipe,d::Register::DtoParam)==0x55ff55fd);
        CHECK(x.reg(pipe,d::Register::PixelDiv)==((0xaaau&~(7u<<(pipe*3)))|(2u<<(pipe*3))));
        const size_t configureCalls=x.calls;x.calls=0;
        CHECK(x.controller.stop(r)==d::Status::Ok);CHECK(x.regs==saved);const size_t stopCalls=x.calls;
        for(size_t at=1;at<=configureCalls;++at){Device f(pipe);f.failAt=at;
            CHECK(f.controller.configure(f.layout,f.io(),pipe,config,r)==d::Status::TransportError);
            CHECK(f.controller.requiresHardwareRetirement()==(f.writes!=0));f.failAt=0;
            if(f.controller.requiresHardwareRetirement())CHECK(f.controller.stop(r)==d::Status::Ok);
            CHECK(f.regs==saved);
        }
        for(size_t at=1;at<=stopCalls;++at){Device f(pipe);
            CHECK(f.controller.configure(f.layout,f.io(),pipe,config,r)==d::Status::Ok);f.calls=0;f.failAt=at;
            CHECK(f.controller.stop(r)==d::Status::TransportError);CHECK(f.controller.requiresHardwareRetirement());
            f.failAt=0;CHECK(f.controller.stop(r)==d::Status::Ok);CHECK(f.regs==saved);
        }
        Device n(pipe);CHECK(n.controller.configure(n.layout,n.io(),pipe,config,r)==d::Status::Ok);
        const uint32_t neighbor=1u<<(((pipe+1)%4)*3);n.reg(pipe,d::Register::PixelDiv)^=neighbor;
        CHECK(n.controller.stop(r)==d::Status::Ok);CHECK(n.reg(pipe,d::Register::PixelDiv)==(0xaaa^neighbor));
        for(uint32_t active:{1u,0x10000u}){Device f(pipe);f.reg(pipe,d::Register::OtgControl)=active;
            CHECK(f.controller.configure(f.layout,f.io(),pipe,config,r)==d::Status::ActivePipe);CHECK(f.writes==0);}
        for(uint32_t missing:{1u,2u,8u}){Device f(pipe);f.reg(pipe,d::Register::HubpControl)&=~missing;
            CHECK(f.controller.configure(f.layout,f.io(),pipe,config,r)==d::Status::ActivePipe);CHECK(f.writes==0);}
        Device reset(pipe);reset.reg(pipe,d::Register::HubpControl)|=4;
        CHECK(reset.controller.configure(reset.layout,reset.io(),pipe,config,r)==d::Status::ActivePipe&&reset.writes==0);
        for(uint32_t xrq=0;xrq<16;++xrq){Device f(pipe);f.reg(pipe,d::Register::HubpControl)|=xrq<<16;
            CHECK(f.controller.configure(f.layout,f.io(),pipe,config,r)==d::Status::Ok);
            CHECK(f.controller.stop(r)==d::Status::Ok);}
        for(uint32_t owned:{1u,2u}){Device f(pipe);f.reg(pipe,d::Register::DtoControl)|=owned<<(pipe*4);
            CHECK(f.controller.configure(f.layout,f.io(),pipe,config,r)==d::Status::ForeignControl);CHECK(f.writes==0);}
        Device f(pipe);f.drop=true;CHECK(f.controller.configure(f.layout,f.io(),pipe,config,r)==d::Status::VerifyFailed);
        CHECK(f.controller.requiresHardwareRetirement());f.drop=false;CHECK(f.controller.stop(r)==d::Status::Ok);
        Device gone(pipe);gone.onesAt=1;CHECK(gone.controller.configure(gone.layout,gone.io(),pipe,config,r)==d::Status::DeviceUnavailable);CHECK(gone.writes==0);
        Device malformed(pipe);malformed.layout.registers[3][2]+=4;
        CHECK(malformed.controller.configure(malformed.layout,malformed.io(),pipe,config,r)==d::Status::InvalidArgument);CHECK(malformed.calls==0);
    }
    auto b=discovery();d::Layout l={};CHECK(d::buildLayout(b.data(),b.size(),4,l)==d::Status::OutsideBar);CHECK(!l.valid);
    for(size_t n=0;n<b.size();++n)CHECK(d::buildLayout(b.data(),n,0x100000,l)==d::Status::DiscoveryRejected);
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
