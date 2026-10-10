// SPDX-License-Identifier: MIT
// Host-only register/transport model, never a production hardware substitute.
#include "hackynavi/display_power.hpp"
#include <iostream>
#include <map>
#include <vector>
namespace d=hackynavi::displaypower;
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
    put32(b,156,0xc0);put32(b,160,0x34c0);put32(b,164,0x9000);checksum(b);return b;
}
const d::Poll poll={5,1};
struct Event { bool write;uint32_t offset,value; };
struct Device {
    d::Layout layout={};d::Controller controller;uint8_t pipe;
    std::map<uint32_t,uint32_t> regs;std::vector<Event> events;
    size_t calls=0,failAt=0,onesAt=0,corruptAt=0,writes=0,dropAt=0,delays=0;
    uint32_t corruption=0;bool acceptFailed=false,stall=false,transitioning=false;
    unsigned lag=0,remaining=0;uint32_t target=0,onValue,offValue;
    explicit Device(uint8_t p,bool on=false,uint32_t onStatus=0,uint32_t offStatus=0x90000000):
        pipe(p),onValue(onStatus),offValue(offStatus){auto b=discovery();
        CHECK(d::buildLayout(b.data(),b.size(),0x100000,layout)==d::Status::Ok);
        for(size_t n=0;n<4;++n)for(size_t r=0;r<d::kRegisterCount;++r)regs[layout.registers[n][r]]=0;
        for(uint8_t n=0;n<4;++n){
            reg(n,d::Register::PowerConfig)=0x100;reg(n,d::Register::PowerStatus)=offValue;
            reg(n,d::Register::MpcTop)=reg(n,d::Register::MpcBottom)=reg(n,d::Register::MpcOpp)=0xf;
            reg(n,d::Register::MpcOutput)=reg(n,d::Register::MpcLock)=0xf;
            reg(n,d::Register::MpcStatus)=1;reg(n,d::Register::HubpControl)=0xb;
        }
        if(on){reg(p,d::Register::PowerConfig)=0;reg(p,d::Register::PowerStatus)=onValue;}
        reg(p,d::Register::RequestControl)=0x40000000;
    }
    uint32_t& reg(uint8_t p,d::Register r){return regs[layout.registers[p][size_t(r)]];}
    d::Io io(){return {this,read,write,delay};}
    static bool read(void* context,uint32_t offset,uint32_t& value){
        auto& x=*static_cast<Device*>(context);++x.calls;CHECK(x.regs.count(offset)==1);
        for(uint8_t p=0;p<4;++p)for(d::Register r:{d::Register::HubpControl,d::Register::HubpClock,d::Register::DppControl,
            d::Register::CursorControl,d::Register::DmdataControl})
            if(offset==x.layout.registers[p][size_t(r)])
                CHECK((x.reg(p,d::Register::PowerStatus)&0xc0000000)==0);
        if(x.transitioning&&offset==x.layout.registers[x.pipe][size_t(d::Register::PowerStatus)]&&!x.stall){
            if(x.remaining)--x.remaining;else{x.reg(x.pipe,d::Register::PowerStatus)=x.target;x.transitioning=false;}
        }
        value=x.regs[offset];if(x.calls==x.onesAt)value=UINT32_MAX;
        if(x.calls==x.corruptAt)value=x.corruption;
        x.events.push_back({false,offset,value});return x.calls!=x.failAt;
    }
    static bool write(void* context,uint32_t offset,uint32_t value){
        auto& x=*static_cast<Device*>(context);++x.calls;++x.writes;CHECK(x.regs.count(offset)==1);
        CHECK(offset==x.layout.registers[x.pipe][size_t(d::Register::PowerConfig)]||
            offset==x.layout.registers[x.pipe][size_t(d::Register::RequestControl)]);
        x.events.push_back({true,offset,value});
        const bool failed=x.calls==x.failAt;
        if((!failed||x.acceptFailed)&&x.writes!=x.dropAt){
            x.regs[offset]=value;
            if(offset==x.layout.registers[x.pipe][size_t(d::Register::PowerConfig)]){
                CHECK(x.reg(x.pipe,d::Register::RequestControl)&1);CHECK(value==0||value==0x100);
                x.target=value?x.offValue:x.onValue;x.transitioning=true;x.remaining=x.lag;
                x.reg(x.pipe,d::Register::PowerStatus)=value?0x50000000:0x40000000;
            }
        }
        return !failed;
    }
    static void delay(void* context,uint32_t us){auto& x=*static_cast<Device*>(context);++x.delays;CHECK(us==1||us==1000);}
    void freshCounters(){calls=writes=delays=0;events.clear();}
};
bool validOn(Device& x,d::Result& r){return x.controller.powerOn(x.layout,x.io(),x.pipe,poll,r)==d::Status::Ok;}
void successAndFailures(){
    for(uint32_t onValue:{0u,0x10000000u})for(uint32_t offValue:{0x80000000u,0x90000000u})
    for(uint8_t pipe=0;pipe<4;++pipe)for(bool requestAlready:{false,true}){
        Device x(pipe,false,onValue,offValue);d::Result r={};if(requestAlready)x.reg(pipe,d::Register::RequestControl)|=1;
        const auto initial=x.regs;x.lag=2;CHECK(validOn(x,r));CHECK(r.powerVerified&&r.powered);
        CHECK(x.controller.powered()&&x.controller.requiresHardwareRetirement());CHECK(r.polls==3);
        CHECK(x.reg(pipe,d::Register::RequestControl)==initial.at(x.layout.registers[pipe][size_t(d::Register::RequestControl)]));
        CHECK(x.writes==(requestAlready?1u:3u));CHECK(x.delays==2);
        for(const auto& pair:initial)if(pair.first!=x.layout.registers[pipe][size_t(d::Register::PowerConfig)]&&
            pair.first!=x.layout.registers[pipe][size_t(d::Register::PowerStatus)])CHECK(x.regs[pair.first]==pair.second);
        CHECK(x.controller.powerOn(x.layout,x.io(),pipe,poll,r)==d::Status::InvalidState);
        x.freshCounters();CHECK(x.controller.powerOff(poll,r)==d::Status::Ok);CHECK(r.powerVerified&&!r.powered);
        CHECK(!r.retentionRequired&&!x.controller.requiresHardwareRetirement());CHECK(x.regs==initial);
        CHECK(x.controller.phase()==d::Phase::Off);CHECK(validOn(x,r));

        Device baseline(pipe,false,onValue,offValue);if(requestAlready)baseline.reg(pipe,d::Register::RequestControl)|=1;
        CHECK(validOn(baseline,r));const auto onEvents=baseline.events;
        for(size_t at=1;at<=onEvents.size();++at)for(bool accepted:{false,true}){
            Device f(pipe,false,onValue,offValue);if(requestAlready)f.reg(pipe,d::Register::RequestControl)|=1;
            f.failAt=at;f.acceptFailed=accepted;
            CHECK(f.controller.powerOn(f.layout,f.io(),pipe,poll,r)==d::Status::TransportError);
            CHECK(f.calls==at);CHECK(!r.powerVerified);CHECK(r.retentionRequired==(f.writes!=0));
            CHECK(f.controller.faulted()==(f.writes!=0));
            const size_t calls=f.calls;f.controller.clearAfterVerifiedReset();CHECK(f.calls==calls);
            CHECK(!f.controller.requiresHardwareRetirement()&&f.controller.phase()==d::Phase::Fresh);
        }
        for(size_t at=1;at<=onEvents.size();++at)if(!onEvents[at-1].write){
            Device f(pipe,false,onValue,offValue);if(requestAlready)f.reg(pipe,d::Register::RequestControl)|=1;f.onesAt=at;
            CHECK(f.controller.powerOn(f.layout,f.io(),pipe,poll,r)==d::Status::DeviceUnavailable);
            CHECK(!r.powerVerified);CHECK(f.controller.requiresHardwareRetirement()==(f.writes!=0));
        }
        baseline.freshCounters();CHECK(baseline.controller.powerOff(poll,r)==d::Status::Ok);
        const auto offEvents=baseline.events;
        for(size_t at=1;at<=offEvents.size();++at)for(bool accepted:{false,true}){
            Device f(pipe,false,onValue,offValue);if(requestAlready)f.reg(pipe,d::Register::RequestControl)|=1;CHECK(validOn(f,r));
            f.freshCounters();f.failAt=at;f.acceptFailed=accepted;
            CHECK(f.controller.powerOff(poll,r)==d::Status::TransportError);CHECK(f.calls==at);
            CHECK(r.retentionRequired&&f.controller.faulted()&&!r.powerVerified);
            const auto calls=f.calls;CHECK(f.controller.powerOff(poll,r)==d::Status::InvalidState);CHECK(f.calls==calls);
        }
        for(size_t at=1;at<=offEvents.size();++at)if(!offEvents[at-1].write){
            Device f(pipe,false,onValue,offValue);if(requestAlready)f.reg(pipe,d::Register::RequestControl)|=1;CHECK(validOn(f,r));
            f.freshCounters();f.onesAt=at;CHECK(f.controller.powerOff(poll,r)==d::Status::DeviceUnavailable);
            CHECK(r.retentionRequired&&f.controller.faulted()&&!r.powerVerified);
        }
    }
}
void admission(){
    for(uint8_t pipe=0;pipe<4;++pipe){
        d::Result r={};
        for(uint32_t onValue:{0u,0x10000000u})for(uint32_t config:{0u,1u}){Device x(pipe,true,onValue);x.reg(pipe,d::Register::PowerConfig)=config;
            CHECK(validOn(x,r));CHECK(x.writes==0&&r.powerVerified&&r.powered&&r.retentionRequired);
            CHECK(x.controller.powerOff(poll,r)==d::Status::Ok);}
        for(uint32_t status:{1u,0x10000000u,0x40000000u,0x50000000u,0xc0000000u,0xd0000000u}){
            Device x(pipe);x.reg(pipe,d::Register::PowerStatus)=status;
            CHECK(x.controller.powerOn(x.layout,x.io(),pipe,poll,r)==d::Status::UnsettledPower);CHECK(x.writes==0);}
        for(uint32_t config:{2u,0x101u,0x200u,0u,1u}){Device x(pipe);x.reg(pipe,d::Register::PowerConfig)=config;
            CHECK(x.controller.powerOn(x.layout,x.io(),pipe,poll,r)==d::Status::UnsettledPower);CHECK(x.writes==0);}
        const d::Register regs[]={d::Register::OtgControl,d::Register::OtgClock,d::Register::DppControl,
            d::Register::DtoControl,d::Register::MpcTop,d::Register::MpcBottom,d::Register::MpcOpp,
            d::Register::MpcStatus,d::Register::MpcOutput,d::Register::MpcLock,d::Register::HubpControl,
            d::Register::HubpClock,d::Register::CursorControl,d::Register::DmdataControl};
        const uint32_t bad[]={1,0x10000,0x10,1u<<(pipe*4),0,0,0,0,0,0x1f,0,1,1,4};
        for(uint32_t on:{0u,0x10000000u})for(uint32_t cfg:{0u,1u})
        for(size_t i=0;i<sizeof(regs)/sizeof(regs[0]);++i){Device x(pipe,true,on);
            x.reg(pipe,d::Register::PowerConfig)=cfg;CHECK(validOn(x,r));
            const auto old=x.reg(pipe,regs[i]);x.reg(pipe,regs[i])=bad[i];x.freshCounters();
            CHECK(x.controller.powerOff(poll,r)!=d::Status::Ok);CHECK(x.writes==0&&r.retentionRequired);
            CHECK(x.controller.powered());x.reg(pipe,regs[i])=old;CHECK(x.controller.powerOff(poll,r)==d::Status::Ok);}
        {Device x(pipe,true);CHECK(validOn(x,r));x.reg(pipe,d::Register::HubpControl)|=4;
            CHECK(x.controller.powerOff(poll,r)==d::Status::OutstandingFetch);CHECK(x.writes==0);}
        for(uint32_t bit:{0u,0x10000u,0x20000u,0x40000u,0x80000u,0xf0000u}){
            Device x(pipe,true);x.reg(pipe,d::Register::HubpControl)|=bit;CHECK(validOn(x,r));
            CHECK(x.controller.powerOff(poll,r)==d::Status::Ok); // XRQ field has no guessed completion encoding.
        }
        for(uint8_t other=0;other<4;++other)if(other!=pipe){
            for(d::Register reg:{d::Register::MpcTop,d::Register::MpcBottom,d::Register::MpcOutput}){
                Device x(pipe);x.reg(other,reg)=pipe;CHECK(x.controller.powerOn(x.layout,x.io(),pipe,poll,r)==d::Status::ConnectedCombiner);
                CHECK(x.writes==0);
            }
            Device x(pipe);x.reg(other,d::Register::MpcTop)=other;x.reg(other,d::Register::MpcOutput)=other;
            x.reg(other,d::Register::MpcOpp)=other;x.reg(other,d::Register::MpcLock)=other;
            x.reg(other,d::Register::MpcStatus)=2;CHECK(validOn(x,r)); // Unrelated active pipe allowed.
        }
    }
}
void timeoutsAndChanges(){
    d::Result r={};
    for(uint8_t pipe=0;pipe<4;++pipe){
        Device timeout(pipe);timeout.stall=true;
        CHECK(timeout.controller.powerOn(timeout.layout,timeout.io(),pipe,poll,r)==d::Status::Timeout);
        CHECK(r.polls==poll.iterations&&timeout.delays==poll.iterations-1&&r.retentionRequired);
        CHECK(timeout.controller.faulted()&&!r.powerVerified);CHECK(timeout.reg(pipe,d::Register::RequestControl)&1);
        Device base(pipe);CHECK(validOn(base,r));const auto events=base.events;bool firstAfterCommand=false;
        for(size_t at=1;at<=events.size();++at){const auto& e=events[at-1];
            if(e.write){if(e.offset==base.layout.registers[pipe][size_t(d::Register::PowerConfig)])firstAfterCommand=true;continue;}
            if(e.offset==base.layout.registers[pipe][size_t(d::Register::PowerStatus)]){
                // A non-final state in the wait loop is allowed to settle on
                // the next poll; stable admission/final samples are not.
                if(firstAfterCommand){firstAfterCommand=false;continue;}
                Device x(pipe);x.corruptAt=at;x.corruption=e.value?0:0x90000000;
                CHECK(x.controller.powerOn(x.layout,x.io(),pipe,poll,r)!=d::Status::Ok);
                CHECK(!r.powerVerified);CHECK(r.retentionRequired==(x.writes!=0));
            }
        }
        for(size_t dropped=1;dropped<=3;++dropped){Device x(pipe);x.dropAt=dropped;
            CHECK(x.controller.powerOn(x.layout,x.io(),pipe,poll,r)==d::Status::VerifyFailed);
            CHECK(x.controller.faulted()&&r.retentionRequired&&!r.powerVerified);}
        Device x(pipe);CHECK(validOn(x,r));x.reg(pipe,d::Register::PowerConfig)=0x100;x.reg(pipe,d::Register::PowerStatus)=0x90000000;
        CHECK(x.controller.powerOff(poll,r)==d::Status::PowerChanged);CHECK(x.controller.faulted());
    }
}
void currentPowerStates(){
    d::Result r={};
    for(uint8_t pipe=0;pipe<4;++pipe)for(uint32_t state:{0u,0x10000000u,0x80000000u,0x90000000u}){
        const bool on=(state&0xc0000000)==0;
        for(uint32_t cfg:{0u,1u,0x100u,0x101u}){
            Device x(pipe,true,0x10000000,0x80000000);
            x.reg(pipe,d::Register::PowerStatus)=state;x.reg(pipe,d::Register::PowerConfig)=cfg;
            const bool admitted=on?cfg<=1:cfg==0x100;
            const auto status=x.controller.powerOn(x.layout,x.io(),pipe,poll,r);
            CHECK(status==(admitted?d::Status::Ok:d::Status::UnsettledPower));
            if(!admitted)CHECK(!x.writes&&!r.powerVerified&&!r.retentionRequired);
            else if(on)CHECK(!x.writes&&r.powerVerified&&r.powered&&r.retentionRequired);
            else CHECK(x.writes&&r.powerVerified&&r.powered&&r.retentionRequired);
        }
        for(unsigned bit=0;bit<32;++bit){
            Device x(pipe,true);x.reg(pipe,d::Register::PowerStatus)=state^(1U<<bit);
            x.reg(pipe,d::Register::PowerConfig)=on?0:0x100;
            CHECK(x.controller.powerOn(x.layout,x.io(),pipe,poll,r)==(bit==28?d::Status::Ok:d::Status::UnsettledPower));
            Device c(pipe,true);c.reg(pipe,d::Register::PowerStatus)=state;
            c.reg(pipe,d::Register::PowerConfig)=(on?0:0x100)^(1U<<bit);
            CHECK(c.controller.powerOn(c.layout,c.io(),pipe,poll,r)==(on&&bit==0?d::Status::Ok:d::Status::UnsettledPower));
        }
    }
    for(uint8_t pipe=0;pipe<4;++pipe)for(uint32_t on:{0u,0x10000000u})for(uint32_t cfg:{0u,1u}){
        Device base(pipe,true,on);base.reg(pipe,d::Register::PowerConfig)=cfg;
        CHECK(validOn(base,r));CHECK(!base.writes);
        // Every read in adoption still runs with desired bit set, and failure
        // before any ownership/write never creates hardware retirement work.
        for(size_t at=1;at<=base.events.size();++at)for(bool ones:{false,true}){
            Device x(pipe,true,on);x.reg(pipe,d::Register::PowerConfig)=cfg;
            if(ones)x.onesAt=at;else x.failAt=at;
            CHECK(x.controller.powerOn(x.layout,x.io(),pipe,poll,r)==(ones?d::Status::DeviceUnavailable:d::Status::TransportError));
            CHECK(x.calls==at&&!x.writes&&!r.powerVerified&&!r.retentionRequired&&!x.controller.faulted());
        }
        bool firstStatus=true,firstConfig=true;
        for(size_t at=1;at<=base.events.size();++at){const auto& e=base.events[at-1];
            const bool status=e.offset==base.layout.registers[pipe][size_t(d::Register::PowerStatus)];
            const bool config=e.offset==base.layout.registers[pipe][size_t(d::Register::PowerConfig)];
            if(!status&&!config)continue;
            bool& first=status?firstStatus:firstConfig;if(first){first=false;continue;}
            Device x(pipe,true,on);x.reg(pipe,d::Register::PowerConfig)=cfg;
            x.corruptAt=at;x.corruption=e.value^(status?0x10000000u:1u);
            CHECK(x.controller.powerOn(x.layout,x.io(),pipe,poll,r)==d::Status::PowerChanged);
            CHECK(!x.writes&&!r.powerVerified&&!r.retentionRequired);
        }
    }
    for(uint8_t pipe=0;pipe<4;++pipe)for(uint32_t on:{0u,0x10000000u})for(uint32_t off:{0x80000000u,0x90000000u}){
        // Both directions retain the entire raw completion sample. A changed
        // desired bit after polling the correct FSM is still observed change,
        // never permission to drop a domain with an ambiguous transition.
        for(bool goingOn:{false,true}){
            Device base(pipe,false,on,off);CHECK(validOn(base,r));
            if(goingOn){CHECK(base.controller.powerOff(poll,r)==d::Status::Ok);base.freshCounters();CHECK(validOn(base,r));}
            else{base.freshCounters();CHECK(base.controller.powerOff(poll,r)==d::Status::Ok);}
            bool commanded=false,polled=false;
            for(size_t at=1;at<=base.events.size();++at){const auto& e=base.events[at-1];
                if(e.write&&e.offset==base.layout.registers[pipe][size_t(d::Register::PowerConfig)]){commanded=true;continue;}
                if(!commanded||e.write||e.offset!=base.layout.registers[pipe][size_t(d::Register::PowerStatus)])continue;
                if(!polled){polled=true;continue;}
                Device x(pipe,false,on,off);CHECK(validOn(x,r));
                if(goingOn)CHECK(x.controller.powerOff(poll,r)==d::Status::Ok);
                x.freshCounters();x.corruptAt=at;x.corruption=e.value^0x10000000u;
                const auto result=goingOn?x.controller.powerOn(x.layout,x.io(),pipe,poll,r):x.controller.powerOff(poll,r);
                CHECK(result==d::Status::PowerChanged&&!r.powerVerified&&r.retentionRequired&&x.controller.faulted());
            }
        }
    }
}
void layoutsAndArguments(){
    auto b=discovery();d::Layout l={};CHECK(d::buildLayout(b.data(),b.size(),0x100000,l)==d::Status::Ok);
    for(size_t pipe=0;pipe<4;++pipe){
        CHECK(l.registers[pipe][size_t(d::Register::PowerConfig)]==(0x34c0+0x80+pipe*2)*4);
        CHECK(l.registers[pipe][size_t(d::Register::PowerStatus)]==(0x34c0+0x81+pipe*2)*4);
        CHECK(l.registers[pipe][size_t(d::Register::DppControl)]==(0x34c0+0xcc5+pipe*0x16b)*4);
    }
    for(size_t n=0;n<b.size();++n){d::Layout bad=l;CHECK(d::buildLayout(b.data(),n,0x100000,bad)!=d::Status::Ok);CHECK(!bad.valid);}
    for(size_t pos:{size_t(148),size_t(149),size_t(150)}){auto bad=b;bad[pos]^=1;checksum(bad);
        CHECK(d::buildLayout(bad.data(),bad.size(),0x100000,l)==d::Status::UnsupportedDmu);}
    auto alias=b;put32(alias,156,0x34c0+0x80-0xb6);checksum(alias);
    CHECK(d::buildLayout(alias.data(),alias.size(),0x100000,l)==d::Status::OutsideBar);
    for(uint64_t bar:{uint64_t(0),uint64_t(3),uint64_t(0x100)})CHECK(d::buildLayout(b.data(),b.size(),bar,l)==d::Status::OutsideBar);
    Device x(0);d::Result r={};
    for(size_t pipe=0;pipe<4;++pipe)for(size_t reg=0;reg<d::kRegisterCount;++reg){auto bad=x.layout;bad.registers[pipe][reg]+=4;
        CHECK(x.controller.powerOn(bad,x.io(),0,poll,r)==d::Status::InvalidArgument);CHECK(x.calls==0);}
    for(d::Poll bad:{d::Poll{0,1},d::Poll{1,0},d::Poll{1,1001},d::Poll{101,1000},d::Poll{UINT32_MAX,1}}){
        CHECK(x.controller.powerOn(x.layout,x.io(),0,bad,r)==d::Status::InvalidArgument);CHECK(x.calls==0);}
    CHECK(x.controller.powerOn(x.layout,x.io(),4,poll,r)==d::Status::InvalidArgument);
    d::Io io=x.io();io.read32=nullptr;CHECK(x.controller.powerOn(x.layout,io,0,poll,r)==d::Status::InvalidArgument);
    io=x.io();io.write32=nullptr;CHECK(x.controller.powerOn(x.layout,io,0,poll,r)==d::Status::InvalidArgument);
    io=x.io();io.delayUs=nullptr;CHECK(x.controller.powerOn(x.layout,io,0,poll,r)==d::Status::InvalidArgument);
    CHECK(x.calls==0);CHECK(x.controller.powerOff(poll,r)==d::Status::InvalidState);
}
}
int main(){layoutsAndArguments();successAndFailures();admission();timeoutsAndChanges();currentPowerStates();
    std::cout<<checks<<" display power checks, "<<failures<<" failures\n";return failures?1:0;}
