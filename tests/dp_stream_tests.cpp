// SPDX-License-Identifier: MIT
// Disconnected numeric MMIO protocol/fault oracle. No GPU execution claim.
#include "hackynavi/dp_stream.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
namespace d=hackynavi::dpstream;
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
const d::Poll poll={4,10};
struct Event { char type;uint32_t address,value; };
struct Device {
    d::Layout l={};d::Controller stream;d::Config c={};std::map<uint32_t,uint32_t> regs;
    std::vector<Event> events;size_t calls=0,validCalls=0,writes=0,failAt=0,onesAt=0,denyAt=0,dropAt=0;
    bool frames=true,reset=true,enable=true,blank=true,ambiguous=false;
    d::Config* changeConfig=nullptr;d::Layout* changeLayout=nullptr;d::Poll* changePoll=nullptr;
    uint32_t reg(d::Register r,uint8_t n=255)const{return l.registers[n==255?c.frontend:n][size_t(r)];}
    uint32_t timing(d::TimingRegister r)const{return l.timing[c.pipe][size_t(r)];}
    Device(uint8_t fe=0,uint8_t be=0,uint8_t pipe=0){
        const auto bytes=discovery();CHECK(d::buildLayout(bytes.data(),bytes.size(),0x100000,l)==d::Status::Ok);
        c.frontend=fe;c.backend=be;c.pipe=pipe;c.hpd=be;c.lanes=4;c.symbolClockKHz=810000;
        c.timing={148500,1920,1080,2200,2008,2052,1125,1084,1089,true,true,false,false,16,4};
        for(auto& a:l.registers)for(auto v:a)regs[v]=0;for(auto& a:l.timing)for(auto v:a)regs[v]=0;
        regs[reg(d::Register::Backend,be)]=(uint32_t(be)<<28)|(0x100u<<fe);
        regs[reg(d::Register::BackendEnable,be)]=1;regs[reg(d::Register::Config,be)]=3;
        regs[reg(d::Register::Link,be)]=0x10;regs[reg(d::Register::Framing,be)]=0x10002000;
        regs[reg(d::Register::Scrambler,be)]=0x1ff10;
        regs[reg(d::Register::Frontend)]=0x1000000;
        regs[timing(d::TimingRegister::Control)]=0x11001;regs[timing(d::TimingRegister::Clock)]=0x103;
        regs[timing(d::TimingRegister::HTotal)]=2199;regs[timing(d::TimingRegister::VTotal)]=1124;
        regs[timing(d::TimingRegister::HBlank)]=0x00c00840;
        regs[timing(d::TimingRegister::HSync)]=0x002c0000;
        regs[timing(d::TimingRegister::VBlank)]=0x00290461;
        regs[timing(d::TimingRegister::VSync)]=0x00050000;
    }
    d::Io io(){return {this,valid,read,write,delay};}
    static bool valid(void* p){auto& m=*static_cast<Device*>(p);m.events.push_back({'v',0,0});
        if(m.changeConfig){*m.changeConfig={};m.changeConfig=nullptr;}
        if(m.changeLayout){*m.changeLayout={};m.changeLayout=nullptr;}
        if(m.changePoll){*m.changePoll={};m.changePoll=nullptr;}
        return ++m.validCalls!=m.denyAt;}
    static bool read(void* p,uint32_t a,uint32_t& v){auto& m=*static_cast<Device*>(p);++m.calls;CHECK(m.regs.count(a));
        if(a==m.timing(d::TimingRegister::FrameCount)&&m.frames)++m.regs[a];v=m.regs[a];if(m.calls==m.onesAt)v=UINT32_MAX;
        m.events.push_back({'r',a,v});return m.calls!=m.failAt;}
    static bool write(void* p,uint32_t a,uint32_t v){auto& m=*static_cast<Device*>(p);++m.calls;++m.writes;CHECK(m.regs.count(a));
        m.events.push_back({'w',a,v});if(m.calls==m.failAt&&!m.ambiguous)return false;if(m.calls==m.dropAt)return true;
        m.regs[a]=v;
        if(a==m.reg(d::Register::Fifo))m.regs[a]=(v&~0x100000u)|((m.reset&&(v&2))?0x100000u:0);
        if(a==m.reg(d::Register::Stream)){
            if((v&1)&&m.enable)m.regs[a]|=0x10000;
            if(!(v&1)&&m.blank)m.regs[a]&=~0x10000u;
        }
        return m.calls!=m.failAt;
    }
    static void delay(void* p,uint32_t us){auto& m=*static_cast<Device*>(p);CHECK(us==10||us==100);m.events.push_back({'d',0,us});}
    d::Status prepare(d::Result& r){return stream.prepare(l,io(),c,r);}
};
void numeric(){
    Device m;d::Values v={};CHECK(d::encode(m.c,v)==d::Status::Ok);
    CHECK(v.timing[0]==0x08980465&&v.timing[1]==0x00c00029&&v.timing[2]==0x002c0005&&v.timing[3]==0x07800438);
    CHECK(v.n==32768&&v.m==6007);
    m.c.timing.hPositive=m.c.timing.vPositive=false;CHECK(d::encode(m.c,v)==d::Status::Ok&&v.timing[2]==0x802c8005);
    for(uint32_t rate:{162000u,270000u,540000u,810000u})for(uint8_t lane:{uint8_t(1),uint8_t(2),uint8_t(4)}){
        m.c.symbolClockKHz=rate;m.c.lanes=lane;
        for(uint32_t clk:{rate*lane/3,rate*lane/3+1}){m.c.timing.pixelClockKHz=clk;
            CHECK(d::encode(m.c,v)==(uint64_t(clk)*24<=uint64_t(rate)*lane*8?d::Status::Ok:d::Status::LinkCapacity));}
    }
    m.c.timing={594000,3840,2160,4400,4016,4104,2250,2168,2178,true,true,false,false,97,4};
    m.c.symbolClockKHz=810000;m.c.lanes=2;CHECK(d::encode(m.c,v)==d::Status::LinkCapacity);
    m.c.lanes=4;CHECK(d::encode(m.c,v)==d::Status::Ok&&v.m==24029);
    m.c.lanes=3;CHECK(d::encode(m.c,v)==d::Status::InvalidArgument);
}
void routeAndTiming(){
    for(uint8_t fe=0;fe<5;++fe)for(uint8_t be=0;be<5;++be)for(uint8_t pipe=0;pipe<4;++pipe){
        Device m(fe,be,pipe);d::Result r={};CHECK(m.prepare(r)==d::Status::Ok&&r.writes==10);
        CHECK(m.stream.start(poll,r)==d::Status::Ok&&r.streamEnabled&&r.framesAdvanced);
        CHECK(m.stream.stop(poll,r)==d::Status::Ok&&r.streamStopped&&r.fifoDisabled&&!m.stream.requiresHardwareRetirement());
        CHECK(m.stream.phase()==d::Phase::Stopped);
    }
    struct Bad {d::Register r;uint32_t value;bool be;};
    const Bad bad[]={{d::Register::Backend,0,true},{d::Register::BackendEnable,0,true},{d::Register::PhyMux,1,true},
        {d::Register::Config,1,true},{d::Register::Link,0,true},{d::Register::Phy,0x10,true},{d::Register::Framing,0,true},
        {d::Register::Panel,1,true},{d::Register::Prbs,1,true},{d::Register::Frontend,0x40000,false},
        {d::Register::Secondary,1,false},{d::Register::Secondary2,0x10000000,false},{d::Register::Metadata,1,false},
        {d::Register::Pps,0x10,false},{d::Register::Dsc,1,false},{d::Register::DoubleBuffer,0x100,false},
        {d::Register::Vbid,0x2000,false},{d::Register::VidTiming,0x1000,false},
        {d::Register::Stream,0x10000,false},{d::Register::Fifo,1,false},{d::Register::Steer,0x100,false}};
    for(const auto& b:bad){Device m(1,3,2);d::Result r={};m.regs[m.reg(b.r,b.be?m.c.backend:m.c.frontend)]=b.value;
        CHECK(m.prepare(r)==d::Status::ForeignControl&&m.writes==0&&!m.stream.requiresHardwareRetirement());}
    Device shared(1,3,2);d::Result r={};shared.regs[shared.reg(d::Register::Frontend,4)]=2;shared.regs[shared.reg(d::Register::Fifo,4)]=1;
    CHECK(shared.prepare(r)==d::Status::ForeignControl&&shared.writes==0);
    for(auto tr:{d::TimingRegister::Control,d::TimingRegister::Clock,d::TimingRegister::HTotal,d::TimingRegister::VTotal}){
        Device m;CHECK(m.prepare(r)==d::Status::Ok);m.regs[m.timing(tr)]=0;
        CHECK(m.stream.start(poll,r)==d::Status::ForeignControl&&r.writes==0&&m.stream.requiresHardwareRetirement());
    }
    for(auto reg:{d::Register::Timing1,d::Register::Timing2,d::Register::Timing3,d::Register::Timing4,d::Register::Pixel}){
        Device m;CHECK(m.prepare(r)==d::Status::Ok);m.regs[m.reg(reg)]^=1;
        CHECK(m.stream.start(poll,r)==d::Status::VerifyFailed&&r.writes==0);
    }
}
void transitions(){
    Device m;d::Result r={};CHECK(m.prepare(r)==d::Status::Ok);m.events.clear();CHECK(m.stream.start(poll,r)==d::Status::Ok);
    std::vector<Event> writes;for(const auto& e:m.events)if(e.type=='w'||e.type=='d')writes.push_back(e);
    const d::Register regs[]={d::Register::VidTiming,d::Register::N,d::Register::M,d::Register::VidTiming,d::Register::Stream,
        d::Register::Steer,d::Register::Steer,d::Register::Fifo,d::Register::Fifo,d::Register::Fifo,d::Register::Fifo,d::Register::Stream};
    const uint32_t masks[]={0x100,0xffffff,0xffffff,0xd00,1,1,1,0x7c,2,2,1,1};
    const uint32_t values[]={0,32768,6007,0x100,0,1,0,28,2,0,1,1};size_t i=0;
    for(const auto& e:writes)if(e.type=='w'){CHECK(i<12);if(i<12){CHECK(e.address==m.reg(regs[i]));CHECK((e.value&masks[i])==values[i]);}++i;}
    CHECK(i==12);
    CHECK(writes[6].type=='d'&&writes[6].value==10);CHECK(writes[12].type=='d'&&writes[12].value==100);
    m.events.clear();CHECK(m.stream.stop(poll,r)==d::Status::Ok);
    bool sawStopped=false;for(const auto& e:m.events){if(e.type=='r'&&e.address==m.reg(d::Register::Stream)&&!(e.value&0x10001))sawStopped=true;
        if(e.type=='w'&&e.address==m.reg(d::Register::Steer))CHECK(sawStopped);}
    Device pending;CHECK(pending.prepare(r)==d::Status::Ok);CHECK(pending.stream.start(poll,r)==d::Status::Ok);
    pending.blank=false;pending.events.clear();CHECK(pending.stream.stop(poll,r)==d::Status::Timeout&&pending.stream.requiresHardwareRetirement());
    for(const auto& e:pending.events)CHECK(!(e.type=='w'&&e.address==pending.reg(d::Register::Steer)));
    for(unsigned failure=0;failure<4;++failure){Device a;CHECK(a.prepare(r)==d::Status::Ok);
        if(failure==0)a.reset=false;if(failure==1)a.frames=false;if(failure==2)a.enable=false;
        if(failure==3)a.regs[a.reg(d::Register::Frontend)]&=~0x1000000u;
        CHECK(a.stream.start(poll,r)==d::Status::Timeout&&a.stream.requiresHardwareRetirement());
        CHECK(a.stream.stop(poll,r)==d::Status::Ok&&!a.stream.requiresHardwareRetirement());
    }
    Device wrap;CHECK(wrap.prepare(r)==d::Status::Ok);wrap.regs[wrap.timing(d::TimingRegister::FrameCount)]=0xfffffe;
    CHECK(wrap.stream.start(poll,r)==d::Status::Ok&&r.firstFrame==0xffffff&&r.lastFrame==1);
}
void faultSweep(){
    Device baseline;d::Result r={};CHECK(baseline.prepare(r)==d::Status::Ok);const auto prepared=baseline.calls;
    CHECK(baseline.stream.start(poll,r)==d::Status::Ok);const auto started=baseline.calls;
    CHECK(baseline.stream.stop(poll,r)==d::Status::Ok);const auto stopped=baseline.calls;
    for(bool ambiguous:{false,true})for(size_t at=1;at<=stopped;++at){Device m;m.failAt=at;m.ambiguous=ambiguous;
        auto s=m.prepare(r);if(at>prepared&&s==d::Status::Ok)s=m.stream.start(poll,r);if(at>started&&s==d::Status::Ok)s=m.stream.stop(poll,r);
        CHECK(s==d::Status::TransportError&&m.calls==at);
        CHECK(!m.events.empty()&&m.events.back().type=='v');
        CHECK(m.stream.requiresHardwareRetirement()==(m.writes!=0));
        CHECK(m.stream.phase()==(m.writes?d::Phase::Faulted:d::Phase::Closed));
    }
    for(size_t at=1;at<=baseline.validCalls;++at){Device m;m.denyAt=at;
        auto s=m.prepare(r);if(s==d::Status::Ok)s=m.stream.start(poll,r);if(s==d::Status::Ok)s=m.stream.stop(poll,r);
        CHECK(s==d::Status::DeviceUnavailable&&m.validCalls==at);
        CHECK(m.stream.requiresHardwareRetirement()==(m.writes!=0));
    }
    for(const auto& e:baseline.events)if(e.type=='r'){
        Device m;size_t at=0;for(const auto& x:baseline.events){if(x.type=='r'||x.type=='w')++at;if(&x==&e)break;}
        m.onesAt=at;auto s=m.prepare(r);if(s==d::Status::Ok)s=m.stream.start(poll,r);if(s==d::Status::Ok)s=m.stream.stop(poll,r);
        CHECK(s==d::Status::DeviceUnavailable&&m.calls==at);CHECK(m.stream.requiresHardwareRetirement()==(m.writes!=0));
    }
}
void ownership(){
    d::Result r={};Device m(2,3,1);auto config=m.c;auto layout=m.l;
    m.changeConfig=&config;m.changeLayout=&layout;
    CHECK(m.stream.prepare(layout,m.io(),config,r)==d::Status::Ok);
    CHECK(!layout.valid&&config.timing.pixelClockKHz==0);
    auto bounded=poll;m.changePoll=&bounded;
    CHECK(m.stream.start(bounded,r)==d::Status::Ok&&bounded.maxPolls==0);
    bounded=poll;m.changePoll=&bounded;CHECK(m.stream.stop(bounded,r)==d::Status::Ok);
    for(size_t i=0;i<m.events.size();++i)if(m.events[i].type!='v'){
        CHECK(i>0&&i+1<m.events.size());
        CHECK(m.events[i-1].type=='v'&&m.events[i+1].type=='v');
    }
    // A bounded stop permits a fresh transaction, including its retained steer
    // reset. It does not permit re-entering a running or failed transaction.
    CHECK(m.prepare(r)==d::Status::Ok);CHECK(m.stream.start(poll,r)==d::Status::Ok);
    const size_t before=m.calls;CHECK(m.prepare(r)==d::Status::InvalidState&&m.calls==before);
    CHECK(m.stream.requiresHardwareRetirement());CHECK(m.stream.stop(poll,r)==d::Status::Ok);
    for(bool writeFailure:{false,true}){
        Device f;f.failAt=writeFailure?0:1;
        if(writeFailure){CHECK(f.prepare(r)==d::Status::Ok);f.failAt=f.calls+1;}
        CHECK((writeFailure?f.stream.stop(poll,r):f.prepare(r))==d::Status::TransportError);
        CHECK(f.events.back().type=='v');
    }
    Device baseline;CHECK(baseline.prepare(r)==d::Status::Ok);const size_t prepareEnd=baseline.calls;
    CHECK(baseline.stream.start(poll,r)==d::Status::Ok);const size_t startEnd=baseline.calls;
    CHECK(baseline.stream.stop(poll,r)==d::Status::Ok);
    size_t ordinal=0;std::map<uint32_t,uint32_t> last;
    for(const auto& event:baseline.events){
        if(event.type=='r'||event.type=='w')++ordinal;
        if(event.type=='r'){last[event.address]=event.value;continue;}
        // Suppressing a write that already equals readback is unobservable and
        // harmless. Every materially changing write must be detected.
        if(event.type!='w'||last[event.address]==event.value)continue;
        Device f;f.dropAt=ordinal;auto status=f.prepare(r);
        if(ordinal>prepareEnd&&status==d::Status::Ok)status=f.stream.start(poll,r);
        if(ordinal>startEnd&&status==d::Status::Ok)status=f.stream.stop(poll,r);
        CHECK(status==d::Status::VerifyFailed||status==d::Status::Timeout);
        CHECK(f.stream.phase()==d::Phase::Faulted&&f.stream.requiresHardwareRetirement());
        f.stream.clearAfterVerifiedReset();CHECK(f.stream.phase()==d::Phase::Closed&&!f.stream.requiresHardwareRetirement());
    }
    for(size_t i=0;i<d::kTimingRegisterCount;++i){
        const auto reg=static_cast<d::TimingRegister>(i);if(reg==d::TimingRegister::FrameCount)continue;
        Device f;CHECK(f.prepare(r)==d::Status::Ok);
        f.regs[f.timing(reg)]^=reg==d::TimingRegister::Clock?0x100:1;
        CHECK(f.stream.start(poll,r)==d::Status::ForeignControl&&r.writes==0);
    }
    for(uint32_t v:{1u,2u,4u}){Device f;f.regs[f.reg(d::Register::Backend)]|=v;
        CHECK(f.prepare(r)==d::Status::ForeignControl&&f.writes==0);}
    for(uint32_t v:{1u,2u,4u,8u,0x10u,0x20u,0x40u,0x10000u,0x1000000u}){
        Device f;f.regs[f.reg(d::Register::Phy)]|=v;CHECK(f.prepare(r)==d::Status::ForeignControl&&f.writes==0);}
    for(const auto reg:{d::Register::Secondary1,d::Register::Gsp8,d::Register::Gsp9,d::Register::Gsp10}){
        Device f;f.regs[f.reg(reg)]=1;CHECK(f.prepare(r)==d::Status::ForeignControl&&f.writes==0);}
    for(uint32_t mask:{1u,0x10u,0x100u,0x1000000u}){Device f;f.regs[f.reg(d::Register::Scrambler)]^=mask;
        CHECK(f.prepare(r)==d::Status::ForeignControl&&f.writes==0);}
}
void preflight(){
    d::Result r={};for(size_t n=0;n<5;++n)for(size_t j=0;j<d::kRegisterCount;++j){Device m;m.l.registers[n][j]+=4;
        CHECK(m.prepare(r)==d::Status::InvalidArgument&&m.calls==0&&m.validCalls==0);}
    for(size_t n=0;n<4;++n)for(size_t j=0;j<d::kTimingRegisterCount;++j){Device m;m.l.timing[n][j]+=4;
        CHECK(m.prepare(r)==d::Status::InvalidArgument&&m.calls==0);}
    auto bytes=discovery();d::Layout l={};for(size_t n=0;n<bytes.size();++n)CHECK(d::buildLayout(bytes.data(),n,0x100000,l)==d::Status::DiscoveryRejected);
    CHECK(d::buildLayout(bytes.data(),bytes.size(),0x14000,l)==d::Status::OutsideBar&&!l.valid);
    for(unsigned n=0;n<4;++n){Device m;auto io=m.io();
        if(n==0)io.valid=nullptr;if(n==1)io.read32=nullptr;if(n==2)io.write32=nullptr;if(n==3)io.delayUs=nullptr;
        CHECK(m.stream.prepare(m.l,io,m.c,r)==d::Status::InvalidArgument&&m.calls==0&&m.validCalls==0);}
    const d::Poll invalidPolls[]={{0,10},{1,0},{1,101},{2000001,1},{20001,100}};
    for(const auto p:invalidPolls){Device m;
        CHECK(m.prepare(r)==d::Status::Ok);const size_t before=m.calls;
        CHECK(m.stream.start(p,r)==d::Status::InvalidArgument&&m.calls==before&&m.stream.requiresHardwareRetirement());}
}
}
int main(int argc,char** argv){numeric();routeAndTiming();transitions();faultSweep();ownership();preflight();
    if(argc>1){std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t>b((std::istreambuf_iterator<char>(f)),{});d::Layout l={};
        CHECK(!b.empty()&&d::buildLayout(b.data(),b.size(),0x100000,l)==d::Status::Ok);
        CHECK(l.registers[0][size_t(d::Register::Timing1)]==0x15830&&l.timing[0][size_t(d::TimingRegister::HTotal)]==0x13fa8);}
    std::cout<<checks<<" DP stream assertions, "<<failures<<" failures\n";return failures?1:0;}
