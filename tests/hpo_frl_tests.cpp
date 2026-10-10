// SPDX-License-Identifier: MIT
// Host register protocol tests, not hardware/link-training evidence.
#include "hackynavi/hpo_frl.hpp"
#include <iostream>
#include <map>
#include <vector>
namespace h=hackynavi::hpofrl;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void p16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void p32(std::vector<uint8_t>& b,size_t p,uint32_t v){p16(b,p,uint16_t(v));p16(b,p+2,uint16_t(v>>16));}
void checksum(std::vector<uint8_t>& b){unsigned sum=0;for(size_t i=60;i<b.size();++i)sum+=b[i];p16(b,14,uint16_t(sum));sum=0;for(size_t i=10;i<b.size();++i)sum+=b[i];p16(b,8,uint16_t(sum));}
std::vector<uint8_t> discovery(){std::vector<uint8_t>b(168,0);p32(b,0,0x28211407);p16(b,4,1);p16(b,6,4);p16(b,10,168);
    p16(b,12,60);p16(b,16,108);p32(b,60,0x53445049);p16(b,64,3);p16(b,66,108);p16(b,72,1);p16(b,76,140);
    p16(b,142,1);p16(b,144,271);b[147]=4;b[148]=3;b[149]=2;p32(b,160,0x1000);p32(b,164,0x34c0);checksum(b);return b;}
const h::Poll poll={3,10};
struct Event{bool write;h::Register reg;uint32_t value;};
struct Device{
    h::Link link;h::Layout layout={};std::map<uint32_t,uint32_t> regs;std::vector<Event> log;
    bool live=true,reflect=true;unsigned calls=0,writes=0,failAt=0,dropAt=0,revokeAt=0,postRevoke=0;
    Device(){auto d=discovery();CHECK(h::buildLayout(d.data(),d.size(),0x100000,layout)==h::Status::Ok);
        for(auto addr:layout.registers)regs[addr]=0;
        regs[layout.registers[size_t(h::Register::Config)]]=0x00008000;
        regs[layout.registers[size_t(h::Register::Config2)]]=0x72000000;
    }
    uint32_t& reg(h::Register r){return regs[layout.registers[size_t(r)]];}
    static Device& d(void* p){return *static_cast<Device*>(p);}
    static bool valid(void* p){return d(p).live;}
    h::Register regAt(uint32_t at){for(size_t i=0;i<h::kRegisters;++i)if(layout.registers[i]==at)return h::Register(i);CHECK(false);return h::Register::Count;}
    static bool read(void* p,uint32_t at,uint32_t& v){auto& x=d(p);if(!x.live)++x.postRevoke;++x.calls;
        auto r=x.regAt(at);CHECK(x.regs.count(at));
        if(r==h::Register::StreamFifo||r==h::Register::Tribyte)CHECK((x.reg(h::Register::StreamClock)&0x1111)==0x1111);
        if(r==h::Register::Config||r==h::Register::Config2)CHECK((x.reg(h::Register::Clock)&3)==3);
        v=x.regs[at];x.log.push_back({false,r,v});if(x.calls==x.revokeAt)x.live=false;return x.calls!=x.failAt;}
    static bool write(void* p,uint32_t at,uint32_t v){auto& x=d(p);if(!x.live)++x.postRevoke;++x.calls;++x.writes;
        auto r=x.regAt(at);CHECK(x.regs.count(at));x.log.push_back({true,r,v});
        if(x.calls!=x.dropAt){x.regs[at]=v;if(r==h::Register::Clock&&x.reflect)x.regs[at]=(v&~2u)|((v&1)<<1);}
        if(x.calls==x.revokeAt)x.live=false;return x.calls!=x.failAt;}
    static void delay(void*,uint32_t){}
    h::Io io(){return {this,valid,read,write,delay};}
    h::Status prepare(h::Result& r,uint8_t lanes=4){return link.prepare(layout,io(),lanes,poll,r);}
};
void operation(){for(uint8_t lanes=3;lanes<=4;++lanes){Device d;h::Result r={};CHECK(d.prepare(r,lanes)==h::Status::Ok);
    CHECK(r.retentionRequired&&d.link.phase()==h::Phase::Training);
    CHECK(d.reg(h::Register::Config)==(0x8002u|(lanes==4?1:0))&&d.reg(h::Register::Control)==1);
    const uint8_t values[4]={0,7,4,uint8_t(lanes==4?2:255)};
    CHECK(d.link.patterns(values,r)==h::Status::Ok);
    CHECK((d.reg(h::Register::Config)&0x0fff0000)==0x04700000);
    CHECK(d.link.packetMode(r)==h::Status::Ok&&!(d.reg(h::Register::Config)&2));
    CHECK(d.link.disable(poll,r)==h::Status::Ok&&!r.retentionRequired);
    CHECK(d.reg(h::Register::Clock)==0&&d.reg(h::Register::Control)==0);
    CHECK(d.reg(h::Register::Config)&0x8000); // reserved bit survives all writes
    for(auto event:d.log)if(event.write&&event.reg==h::Register::Config2)CHECK(!(event.value&0x70000000));
}}
void guards(){
    Device d;h::Result r={};CHECK(d.layout.registers[0]==4*(0x34c0+0x95b));
    auto bad=d.layout;bad.registers[1]=bad.registers[0];CHECK(d.link.prepare(bad,d.io(),4,poll,r)==h::Status::InvalidArgument&&d.calls==0);
    for(unsigned n=0;n<7;++n){Device x;
        if(n==0)x.reg(h::Register::Clock)=3;
        if(n==1)x.reg(h::Register::Control)=1;
        if(n==2)x.reg(h::Register::Memory)=0x10;
        if(n==3)x.reg(h::Register::Memory)=2;
        if(n>=4)x.reg(h::Register::StreamClock)=0x1111;
        if(n==4)x.reg(h::Register::StreamFifo)=0x01000000;
        if(n==5)x.reg(h::Register::Tribyte)=1;
        if(n==6)x.reg(h::Register::StreamClock)=1;
        CHECK(x.prepare(r)!=h::Status::Ok&&!x.writes&&!r.retentionRequired);
    }
    Device x;x.reflect=false;CHECK(x.prepare(r)==h::Status::ClockTimeout&&r.retentionRequired);
    CHECK(x.link.disable(poll,r)==h::Status::Ok&&!r.retentionRequired);
    Device y;CHECK(y.prepare(r)==h::Status::Ok);y.reg(h::Register::Config)|=4;
    const uint8_t patterns[]={1,2,3,4};const auto writes=y.writes;
    CHECK(y.link.patterns(patterns,r)==h::Status::ForeignControl&&y.writes==writes&&r.retentionRequired);
    y.reg(h::Register::Config)&=~4u;CHECK(y.link.disable(poll,r)==h::Status::Ok);
    Device z;CHECK(z.prepare(r)==h::Status::Ok);z.reflect=false;
    CHECK(z.link.disable(poll,r)==h::Status::ClockTimeout&&r.retentionRequired);
    z.reflect=true;CHECK(z.link.disable(poll,r)==h::Status::Ok&&!r.retentionRequired);
    auto bytes=discovery();bytes[148]=4;checksum(bytes);h::Layout l={};
    CHECK(h::buildLayout(bytes.data(),bytes.size(),0x100000,l)==h::Status::UnsupportedDmu);
    bytes=discovery();CHECK(h::buildLayout(bytes.data(),bytes.size(),64,l)==h::Status::OutsideBar);
}
void everyFailure(){Device base;h::Result r={};CHECK(base.prepare(r)==h::Status::Ok);const auto count=base.calls;
    for(unsigned n=1;n<=count;++n){
        Device x;x.failAt=n;CHECK(x.prepare(r)!=h::Status::Ok);
        CHECK(r.retentionRequired==bool(x.writes));x.failAt=0;
        CHECK(x.link.disable(poll,r)==h::Status::Ok&&!r.retentionRequired);
        Device y;y.revokeAt=n;CHECK(y.prepare(r)==h::Status::Revoked&&y.postRevoke==0);
        if(y.writes){CHECK(r.retentionRequired&&y.link.phase()==h::Phase::Faulted);const auto calls=y.calls;
            CHECK(y.link.disable(poll,r)==h::Status::Revoked&&y.calls==calls);}
        y.link.clearAfterVerifiedReset();CHECK(!y.link.requiresHardwareRetirement());
    }
    // Lost/partially landed pattern writes retain the link until actual disable.
    Device x;CHECK(x.prepare(r)==h::Status::Ok);x.failAt=x.calls+7;const uint8_t patterns[]={7,6,5,4};
    CHECK(x.link.patterns(patterns,r)==h::Status::TransportError&&r.retentionRequired);
    x.failAt=0;CHECK(x.link.disable(poll,r)==h::Status::Ok&&!r.retentionRequired);
    Device y;CHECK(y.prepare(r)==h::Status::Ok);y.reg(h::Register::StreamClock)=0x1111;y.reg(h::Register::StreamFifo)=1;
    const auto writes=y.writes;CHECK(y.link.disable(poll,r)==h::Status::StreamActive&&r.retentionRequired&&y.writes==writes);
    Device dropped;CHECK(dropped.prepare(r)==h::Status::Ok);dropped.dropAt=dropped.calls+7;
    CHECK(dropped.link.patterns(patterns,r)==h::Status::VerifyFailed&&r.retentionRequired);
    CHECK(dropped.link.disable(poll,r)==h::Status::Ok&&!r.retentionRequired);
}
}
int main(){operation();guards();everyFailure();std::cout<<checks<<" HPO FRL checks, "<<failures<<" failures\n";return failures?1:0;}
