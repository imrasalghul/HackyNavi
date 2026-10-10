// SPDX-License-Identifier: MIT
// Host-only register model, not a GPU execution test.
#include "hackynavi/vpg.hpp"
#include <iostream>
#include <map>
#include <vector>
namespace v=hackynavi::vpg;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do { ++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";} }while(0)
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
    v::Layout layout={};v::Engine engine;std::map<uint32_t,uint32_t> regs;
    std::vector<uint32_t> data;size_t calls=0,failAt=0,onesAt=0,writes=0;
    bool stuckPower=false,stuckUpdate=false,conflict=false;
    Device(){auto b=discovery();CHECK(v::buildLayout(b.data(),b.size(),0x100000,layout)==v::Status::Ok);
        for(size_t i=0;i<v::kInstanceCount;++i)for(size_t r=0;r<v::kRegisterCount;++r)regs[layout.registers[i][r]]=0;
        for(size_t i=0;i<v::kInstanceCount;++i)reg(i,v::Register::Clock)=0x101;}
    uint32_t address(size_t i,v::Register r)const{return layout.registers[i][size_t(r)];}
    uint32_t& reg(size_t i,v::Register r){return regs[address(i,r)];}
    v::Io io(){return {this,read,write,delay};}
    static bool read(void* context,uint32_t address,uint32_t& value){auto& d=*static_cast<Device*>(context);
        ++d.calls;CHECK(d.regs.count(address));value=d.regs[address];if(d.calls==d.onesAt)value=UINT32_MAX;return d.calls!=d.failAt;}
    static bool write(void* context,uint32_t address,uint32_t value){auto& d=*static_cast<Device*>(context);
        ++d.calls;++d.writes;CHECK(d.regs.count(address));if(d.calls==d.failAt)return false;d.regs[address]=value;
        for(size_t i=0;i<v::kInstanceCount;++i){
            if(address==d.address(i,v::Register::Data)){d.data.push_back(value);++d.reg(i,v::Register::Access);if(d.conflict)d.reg(i,v::Register::Status)|=2;}
            if(address==d.address(i,v::Register::MemoryPower))d.regs[address]=value|(d.stuckPower?0x100u:0u);
            if(address==d.address(i,v::Register::Status))d.regs[address]=0;
            if(address==d.address(i,v::Register::ImmediateUpdate))d.regs[address]=d.stuckUpdate?(value<<16):0;
        }return true;}
    static void delay(void*,uint32_t us){CHECK(us==10);}
    v::Status open(v::Result& r,uint8_t i=0){return engine.open(layout,io(),i,poll,r);}
};
v::Packet packet(){v::Packet p={};for(size_t i=0;i<4;++i)p.header[i]=uint8_t(i+1);for(size_t i=0;i<32;++i)p.payload[i]=uint8_t(i+10);return p;}
void exercise(){
    for(uint8_t instance=0;instance<5;++instance){Device d;v::Result r={};CHECK(d.open(r,instance)==v::Status::Ok);
        for(uint8_t slot=0;slot<15;++slot){d.data.clear();CHECK(d.engine.upload(slot,packet(),poll,r)==v::Status::Ok);
            CHECK(r.packetWords==9&&r.updateSubmitted&&r.updateCompleted);CHECK(d.data.size()==9&&d.data[0]==0x04030201);
            CHECK(d.data[1]==0x0d0c0b0a&&d.data[8]==0x29282726);CHECK(d.reg(instance,v::Register::Access)==uint32_t(slot)*9+9);}
        const auto calls=d.calls;CHECK(d.engine.upload(15,packet(),poll,r)==v::Status::InvalidArgument&&d.calls==calls);
        CHECK(d.engine.close(poll,r)==v::Status::Ok);CHECK(!d.engine.requiresHardwareRetirement()&&d.engine.phase()==v::Phase::Closed);
        CHECK(d.reg(instance,v::Register::MemoryPower)==0);
    }
    Device a;v::Result r={};a.reg(0,v::Register::Clock)=1;CHECK(a.open(r)==v::Status::ForeignControl&&a.writes==0);
    Device b;b.reg(0,v::Register::MemoryPower)=0x10;CHECK(b.open(r)==v::Status::ForeignControl&&b.writes==0);
    Device c;c.stuckPower=true;CHECK(c.open(r)==v::Status::PowerTimeout&&c.engine.requiresHardwareRetirement());
    Device d;CHECK(d.open(r)==v::Status::Ok);d.stuckUpdate=true;CHECK(d.engine.upload(0,packet(),poll,r)==v::Status::UpdateTimeout);
    CHECK(!r.updateCompleted&&d.engine.phase()==v::Phase::Faulted&&d.engine.requiresHardwareRetirement());
    Device e;CHECK(e.open(r)==v::Status::Ok);e.conflict=true;CHECK(e.engine.upload(0,packet(),poll,r)==v::Status::VerifyFailed&&!r.updateSubmitted);
    for(const auto reg:{v::Register::Status,v::Register::ImmediateUpdate,v::Register::FrameUpdate}){
        Device f;f.reg(0,reg)=1;CHECK(f.open(r)==v::Status::UpdateTimeout&&f.writes==0);}
}
void failuresAtEveryOperation(){
    Device base;v::Result r={};CHECK(base.open(r)==v::Status::Ok);const auto opened=base.calls;
    CHECK(base.engine.upload(14,packet(),poll,r)==v::Status::Ok);const auto uploaded=base.calls;
    CHECK(base.engine.close(poll,r)==v::Status::Ok);const auto closed=base.calls;
    for(size_t mode=0;mode<2;++mode)for(size_t at=1;at<=closed;++at){Device d;if(mode)d.onesAt=at;else d.failAt=at;
        auto s=d.open(r);if(at>opened&&s==v::Status::Ok)s=d.engine.upload(14,packet(),poll,r);
        if(at>uploaded&&s==v::Status::Ok)s=d.engine.close(poll,r);
        if(!mode)CHECK(s==v::Status::TransportError);
        if(s!=v::Status::Ok){CHECK(d.engine.requiresHardwareRetirement()==(d.writes!=0));
            CHECK(d.engine.phase()==(d.writes?v::Phase::Faulted:v::Phase::Closed));}
    }
}
}
int main(){exercise();failuresAtEveryOperation();std::cout<<checks<<" VPG checks, "<<failures<<" failures\n";return failures?1:0;}
