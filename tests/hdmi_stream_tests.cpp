// SPDX-License-Identifier: MIT
// Offline register model; no hardware execution claim.
#include "hackynavi/hdmi_stream.hpp"
#include <iostream>
#include <map>
#include <vector>
namespace h=hackynavi::hdmistream;namespace v=hackynavi::vpg;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v){put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t p){uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s;}
std::vector<uint8_t> discovery(){
    std::vector<uint8_t> b(164,0);put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,164);
    put16(b,12,60);put16(b,16,104);put32(b,60,0x53445049);put16(b,64,3);put16(b,66,104);
    put16(b,72,1);put16(b,76,140);put16(b,142,1);put16(b,144,271);b[147]=3;b[148]=3;b[149]=2;
    put32(b,160,0x34c0);put16(b,14,sum(b,60));put16(b,8,sum(b,10));return b;
}

const v::Poll poll={3,10};
struct Device {
    h::Layout layout={};h::Controller engine;std::map<uint32_t,uint32_t> regs;
    std::vector<uint32_t> data;size_t calls=0,failAt=0,onesAt=0,writes=0;
    bool stuckPower=false,stuckUpdate=false,conflict=false,frames=true,reset=true,clock=true;
    uint8_t frontend=0,backend=0,pipe=0;
    uint32_t hr(h::Register r){return layout.registers[frontend][size_t(r)];}
    uint32_t tr(hackynavi::otg::Register r){return layout.timing.registers[pipe][size_t(r)];}
    Device(uint8_t fe=0,uint8_t be=0,uint8_t otg=0):frontend(fe),backend(be),pipe(otg){auto b=discovery();CHECK(h::buildLayout(b.data(),b.size(),0x100000,layout)==h::Status::Ok);
        for(size_t i=0;i<v::kInstanceCount;++i)for(size_t r=0;r<v::kRegisterCount;++r)regs[layout.packets.registers[i][r]]=0;
        for(size_t i=0;i<5;++i)for(size_t r=0;r<h::kRegisterCount;++r)regs[layout.registers[i][r]]=0;
        for(size_t i=0;i<4;++i)for(size_t r=0;r<hackynavi::otg::kRegisterCount;++r)regs[layout.timing.registers[i][r]]=0;
        regs[hr(h::Register::Frontend)]=0x1000000|pipe;
        regs[layout.registers[backend][size_t(h::Register::Backend)]]=0x30000|(0x100u<<frontend);regs[layout.registers[backend][size_t(h::Register::BackendEnable)]]=1;
        regs[hr(h::Register::Hdmi)]=0x19;regs[hr(h::Register::Vbi)]=0x31;regs[hr(h::Register::General)]=1;
        regs[tr(hackynavi::otg::Register::Control)]=0x10001;regs[tr(hackynavi::otg::Register::Clock)]=0x103;}
    uint32_t address(size_t i,v::Register r)const{return layout.packets.registers[i][size_t(r)];}
    uint32_t& reg(size_t i,v::Register r){return regs[address(i,r)];}
    v::Io io(){return {this,read,write,delay};}
    static bool read(void* context,uint32_t address,uint32_t& value){auto& d=*static_cast<Device*>(context);
        ++d.calls;CHECK(d.regs.count(address));if(address==d.tr(hackynavi::otg::Register::FrameCount)&&d.frames)++d.regs[address];value=d.regs[address];if(d.calls==d.onesAt)value=UINT32_MAX;return d.calls!=d.failAt;}
    static bool write(void* context,uint32_t address,uint32_t value){auto& d=*static_cast<Device*>(context);
        ++d.calls;++d.writes;CHECK(d.regs.count(address));if(d.calls==d.failAt)return false;d.regs[address]=value;
        if(address==d.hr(h::Register::Fifo))d.regs[address]=(value&~0x100000u)|((d.reset&&(value&2))?0x100000u:0u);
        if(address==d.hr(h::Register::Clock))d.regs[address]=(value&~0x100u)|((d.clock&&(value&1))?0x100u:0u);
        for(size_t i=0;i<v::kInstanceCount;++i){
            if(address==d.address(i,v::Register::Data)){d.data.push_back(value);++d.reg(i,v::Register::Access);if(d.conflict)d.reg(i,v::Register::Status)|=2;}
            if(address==d.address(i,v::Register::MemoryPower))d.regs[address]=value|(d.stuckPower?0x100u:0u);
            if(address==d.address(i,v::Register::Status))d.regs[address]=0;
            if(address==d.address(i,v::Register::ImmediateUpdate))d.regs[address]=d.stuckUpdate?(value<<16):0;
        }return true;}
    static void delay(void*,uint32_t us){CHECK(us==10);}
    h::Status open(h::Result& r){hackynavi::edid::Monitor sink={};sink.hdmi=true;
        h::Config c={};c.frontend=frontend;c.backend=backend;c.pipe=pipe;c.infoframe.encoding=hackynavi::avi::Encoding::Rgb;
        return engine.prepare(layout,io(),c,sink,poll,r);}
};

void tests(){
    for(uint8_t fe=0;fe<5;++fe)for(uint8_t be=0;be<5;++be)for(uint8_t pipe=0;pipe<4;++pipe){
        Device routed(fe,be,pipe);h::Result out={};CHECK(routed.open(out)==h::Status::Ok);
        CHECK(routed.engine.start(poll,out)==h::Status::Ok);CHECK(routed.engine.stop(poll,out)==h::Status::Ok);
    }
    Device d;h::Result r={};CHECK(d.open(r)==h::Status::Ok&&d.engine.phase()==h::Phase::Prepared);
    CHECK(d.data.size()==9&&d.data[0]==0x000d0282);CHECK(d.regs[d.hr(h::Register::General)]==1);
    CHECK(d.engine.start(poll,r)==h::Status::Ok&&r.framesAdvanced);
    CHECK((d.regs[d.hr(h::Register::Fifo)]&0x7f)==0x1d);CHECK(d.regs[d.hr(h::Register::Packets)]==3);
    CHECK(d.regs[d.hr(h::Register::General)]==0);
    CHECK(d.engine.stop(poll,r)==h::Status::Ok&&r.muted&&r.fifoDisabled);
    CHECK(!d.engine.requiresHardwareRetirement()&&d.regs[d.hr(h::Register::Packets)]==0);
    CHECK((d.regs[d.hr(h::Register::Clock)]&1)==0);
    Device a;a.frames=false;CHECK(a.open(r)==h::Status::Ok);CHECK(a.engine.start(poll,r)==h::Status::Timeout);
    CHECK(a.regs[a.hr(h::Register::General)]==1&&a.engine.requiresHardwareRetirement());CHECK(a.engine.stop(poll,r)==h::Status::Ok);
    Device b;b.reset=false;CHECK(b.open(r)==h::Status::Ok);CHECK(b.engine.start(poll,r)==h::Status::Timeout);CHECK(b.engine.stop(poll,r)==h::Status::Ok);
    Device c;c.clock=false;CHECK(c.open(r)==h::Status::Timeout);CHECK(c.engine.stop(poll,r)==h::Status::Ok);
    Device e;e.regs[e.hr(h::Register::Packets)]=0x10;CHECK(e.open(r)==h::Status::ForeignControl&&e.writes==0);
    Device f;f.regs[f.hr(h::Register::Backend)]=0x30200;CHECK(f.open(r)==h::Status::ForeignControl&&f.writes==0);
    Device g;CHECK(g.open(r)==h::Status::Ok);g.regs[g.hr(h::Register::Fifo)]|=0x10000000;
    CHECK(g.engine.start(poll,r)==h::Status::FifoError&&g.regs[g.hr(h::Register::General)]==1);
}
void faults(){
    Device base;h::Result r={};CHECK(base.open(r)==h::Status::Ok);const auto prepared=base.calls;
    CHECK(base.engine.start(poll,r)==h::Status::Ok);const auto started=base.calls;
    CHECK(base.engine.stop(poll,r)==h::Status::Ok);const auto stopped=base.calls;
    for(size_t at=1;at<=stopped;++at){Device d;d.failAt=at;auto s=d.open(r);
        if(at>prepared&&s==h::Status::Ok)s=d.engine.start(poll,r);
        if(at>started&&s==h::Status::Ok)s=d.engine.stop(poll,r);
        CHECK(s==h::Status::TransportError||s==h::Status::PacketFailed);
        CHECK(d.engine.requiresHardwareRetirement()==(d.writes!=0));
        CHECK(d.engine.phase()==(d.writes?h::Phase::Faulted:h::Phase::Closed));
    }
}
}
int main(){tests();faults();std::cout<<checks<<" HDMI stream checks, "<<failures<<" failures\n";return failures?1:0;}
