// SPDX-License-Identifier: MIT
// Offline protocol simulation only. No firmware power command reaches a GPU.
#include "hackynavi/early_media.hpp"
#include <iostream>
#include <map>
#include <type_traits>
#include <vector>
namespace em=hackynavi::earlymedia;
namespace ca=hackynavi::coldaux;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<"line "<<__LINE__<<": " #x "\n";}}while(0)
const em::Binding binding={3,4,5};
const em::Limits limits={100000,10000,10,10};
const uint32_t scratch=0x15f00;
void put16(std::vector<uint8_t>& b,size_t i,uint16_t v){b[i]=uint8_t(v);b[i+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t i,uint32_t v){put16(b,i,uint16_t(v));put16(b,i+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t p){uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s;}
void fix(std::vector<uint8_t>& b){put16(b,14,sum(b,60));put16(b,8,sum(b,10));}
std::vector<uint8_t> fixture(){
    std::vector<uint8_t> b(288,0);put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);
    put16(b,10,288);put16(b,12,60);put16(b,16,228);put32(b,60,0x53445049);
    put16(b,64,3);put16(b,66,228);put16(b,72,1);put16(b,76,140);put16(b,142,9);
    const uint16_t id[]={11,34,255,1,42,43,12,12,91};
    const uint8_t major[]={11,3,13,13,6,6,4,4,6};
    const uint32_t b0[]={0x1260,0x1a000,0x16000,0x16000,0x1260,0x1260,0x7800,0x7b00,0x11400};
    const uint32_t b1[]={0xa000,0x2408c00,0xdc0000,0xdc0000,0xa000,0xa000,0x7e00,0x12000,0x2456800};
    for(unsigned i=0;i<9;++i){size_t at=144+i*16;put16(b,at,id[i]);b[at+3]=2;b[at+4]=major[i];
        if(i==7){b[at+2]=1;b[at+6]=1;b[at+7]=0x10;}put32(b,at+8,b0[i]);put32(b,at+12,b1[i]);}
    fix(b);return b;
}
em::Layout layout(){auto b=fixture();em::Layout l;CHECK(em::buildLayout(b.data(),b.size(),0x100000,scratch,l)==em::Status::Ok);return l;}
struct Event {char kind;uint32_t reg,value;};
struct Model {
    em::Layout l=layout();em::State state;em::Result result={};std::map<uint32_t,uint32_t> regs;
    std::vector<Event> events;std::vector<uint32_t> commands,args;
    uint64_t clock=0,clockStep=1;unsigned timeCalls=0,validCalls=0,reads=0,writes=0,smnReads=0,enters=0,leaves=0,holds=0;
    unsigned failRead=0,onesRead=0,failWrite=0,failSmn=0,failTime=0,regressTime=0,revokeAt=0,revokeAfterRead=0,revokeAfterWrite=0;
    unsigned failCommand=0,responseZeros=0,remainingZeros=0;uint32_t badResponse=0,smuVersion=0x004e7900,ifVersion=0x3d,flags=1;
    bool entered=false,revoked=false,denyEnter=false,holdFails=false,unreadableMedia=false,activeMedia=false,held=false;
    Model(){
        const auto& a=l.auxiliary();regs[a.sos]=0;regs[(scratch+7)*4]=0x200;
        regs[a.vcnPowerConfig[0]]=0;regs[a.vcnPowerConfig[1]]=0;
        const uint32_t vcn[]={0,0,0x10000000,0x2004,8,0x100,0x24f,0};
        const uint32_t jpeg[]={0,1,1,3,0x1bfff};
        const uint32_t lsdma[]={0,0,0,0,0,0,0,0x90000000,0x000e2001,0x27f,0x001001ff,0x501001ff};
        for(unsigned v=0;v<2;++v){regs[a.vcnPower[v]]=UINT32_MAX;for(size_t i=0;i<ca::kVcnRegisters;++i)regs[a.vcn[v][i]]=vcn[i];}
        for(size_t i=0;i<ca::kJpegRegisters;++i)regs[a.jpeg[i]]=jpeg[i];
        for(size_t i=0;i<ca::kLsdmaRegisters;++i)regs[a.lsdma[i]]=lsdma[i];
        auto r=l.registers();regs[r.message]=0;regs[r.argument]=0;regs[r.response]=0;
    }
    em::Io io(){return {this,enter,leave,valid,hold,read,write,smn,now,delay};}
    em::Status run(const em::Limits& p=limits){return em::prepare(l,binding,io(),p,state,result);}
    static bool enter(void* c,const em::Binding& b,em::Operation){auto& m=*static_cast<Model*>(c);++m.enters;
        CHECK(!m.entered);CHECK(b.ownerInstance==3 && b.powerGeneration==4 && b.resetGeneration==5);
        if(m.denyEnter)return false;m.entered=true;return true;}
    static void leave(void* c){auto& m=*static_cast<Model*>(c);CHECK(m.entered);m.entered=false;++m.leaves;}
    static bool valid(void* c,const em::Binding&){auto& m=*static_cast<Model*>(c);CHECK(m.entered);++m.validCalls;
        if(m.validCalls==m.revokeAt)m.revoked=true;return !m.revoked;}
    static bool hold(void* c){auto& m=*static_cast<Model*>(c);CHECK(m.entered && !m.revoked);++m.holds;
        if(m.holdFails)return false;m.held=true;return true;}
    static bool read(void* c,uint32_t a,uint32_t& v){auto& m=*static_cast<Model*>(c);CHECK(m.entered && !m.revoked);
        CHECK(m.regs.count(a)==1);v=m.regs[a];++m.reads;auto r=m.l.registers();
        if(a==r.response && m.remainingZeros){--m.remainingZeros;v=0;}
        if(m.reads==m.onesRead)v=UINT32_MAX;m.events.push_back({'r',a,v});
        if(m.reads==m.revokeAfterRead)m.revoked=true;return m.reads!=m.failRead;}
    static bool write(void* c,uint32_t a,uint32_t v){auto& m=*static_cast<Model*>(c);CHECK(m.entered && !m.revoked && m.held);
        auto r=m.l.registers();CHECK(a==r.message || a==r.argument || a==r.response);++m.writes;m.regs[a]=v;
        m.events.push_back({'w',a,v});
        if(a==r.message){m.commands.push_back(v);m.args.push_back(m.regs[r.argument]);m.remainingZeros=m.responseZeros;
            m.regs[r.response]=m.commands.size()==m.failCommand?m.badResponse:1;
            if(v==2)m.regs[r.argument]=m.smuVersion;if(v==3)m.regs[r.argument]=m.ifVersion;
            const auto& aux=m.l.auxiliary();
            if(v==0x2a && !m.unreadableMedia){unsigned instance=m.args.back()>>16;CHECK(instance<2);
                m.regs[aux.vcnPower[instance]]=instance?0x02200800:0;}
            if(v==0x2c && m.activeMedia)m.regs[aux.vcn[0][1]]=1;
        }
        if(m.writes==m.revokeAfterWrite)m.revoked=true;return m.writes!=m.failWrite;
    }
    static bool smn(void* c,uint32_t a,uint32_t& v){auto& m=*static_cast<Model*>(c);CHECK(m.entered && !m.revoked && m.held);
        CHECK(a==0x03b10024);++m.smnReads;v=m.flags;m.events.push_back({'s',a,v});return m.smnReads!=m.failSmn;}
    static bool now(void* c,uint64_t& t){auto& m=*static_cast<Model*>(c);++m.timeCalls;
        t=m.clock;m.clock+=m.clockStep;if(m.timeCalls==m.regressTime)t=0;return m.timeCalls!=m.failTime;}
    static void delay(void* c,uint32_t t){auto& m=*static_cast<Model*>(c);CHECK(m.entered && !m.revoked && t && t<=1000);
        m.clock+=t;m.events.push_back({'d',0,t});}
};
static_assert(!std::is_copy_constructible<em::State>::value,"owner must not copy");
static_assert(!std::is_move_constructible<em::State>::value,"owner must not move");
void golden(){
    Model m;CHECK(m.run()==em::Status::Ok);CHECK(m.state.phase()==em::Phase::Stopped);
    CHECK(m.state.requiresOwnerRetention() && m.held && m.holds==1 && m.enters==1 && m.leaves==1);
    CHECK(m.commands==std::vector<uint32_t>({2,3,0x2a,0x2a,0x2c}));
    CHECK(m.args==std::vector<uint32_t>({0,0,0,0x10000,0}));
    CHECK(m.result.attemptedDomains==7 && m.result.acknowledgedDomains==7 && m.writes==15);
    CHECK(m.result.smuVersion==m.smuVersion && m.result.interfaceVersion==m.ifVersion);
    CHECK(m.result.auxiliary.status==ca::Status::Ok && m.result.auxiliary.reads==180);
    // No PGFSM/client register read until all three real power ACKs. The first
    // mailbox query must not await an uninitialized response register.
    unsigned writes=0;bool media=false;for(auto e:m.events){if(e.kind=='w')++writes;
        if(e.kind=='r' && e.reg==m.l.registers().response)CHECK(writes>=3);
        if(e.kind=='r' && e.reg==m.l.auxiliary().vcnPower[0]){media=true;CHECK(writes==15);}}
    CHECK(media);
    const auto prior=m.writes;CHECK(em::verify(m.io(),limits,m.state,m.result)==em::Status::Ok);
    CHECK(m.writes==prior && m.holds==1 && m.state.requiresOwnerRetention() && m.leaves==2);
    CHECK(m.run()==em::Status::InvalidState && m.writes==prior);
}
void preflight(){
    auto b=fixture();em::Layout l;for(size_t n=0;n<b.size();++n)CHECK(em::buildLayout(b.data(),n,0x100000,scratch,l)!=em::Status::Ok && !l.valid());
    for(uint32_t s:{0U,UINT32_MAX,0x16000U+0x91-7,0x7e00U-7,0x16000U+0x282-7})
        CHECK(em::buildLayout(b.data(),b.size(),0x100000,s,l)==em::Status::OutsideBar && !l.valid());
    auto bad=b;put32(bad,144+3*16+8,0x7e00-0x282);fix(bad);
    CHECK(em::buildLayout(bad.data(),bad.size(),0x100000,scratch,l)==em::Status::OutsideBar);
    for(unsigned field=0;field<11;++field){Model m;auto io=m.io();
        if(field==0)io.context=nullptr;if(field==1)io.enter=nullptr;if(field==2)io.leave=nullptr;
        if(field==3)io.valid=nullptr;if(field==4)io.holdOwner=nullptr;if(field==5)io.read32=nullptr;
        if(field==6)io.write32=nullptr;if(field==7)io.readSmn32=nullptr;if(field==8)io.nowUs=nullptr;
        if(field==9)io.delayUs=nullptr;auto p=limits;if(field==10)p.totalTimeoutUs=0;
        CHECK(em::prepare(m.l,binding,io,p,m.state,m.result)==em::Status::InvalidArgument);
        CHECK(m.enters==0 && m.reads==0 && m.writes==0 && m.state.phase()==em::Phase::Fresh);}
    for(unsigned field=0;field<8;++field){Model m;auto p=limits;
        if(field==0)p.totalTimeoutUs=10000001;if(field==1)p.responseTimeoutUs=0;
        if(field==2)p.responseTimeoutUs=p.totalTimeoutUs+1;if(field==3)p.maxPolls=0;
        if(field==4)p.maxPolls=1000001;if(field==5)p.pollDelayUs=0;
        if(field==6)p.pollDelayUs=1001;if(field==7)p.totalTimeoutUs=0;
        CHECK(m.run(p)==em::Status::InvalidArgument && !m.enters && !m.reads && !m.writes);}
    Model m;m.denyEnter=true;CHECK(m.run()==em::Status::OwnerUnavailable && !m.leaves && !m.reads && !m.writes);
    Model p;p.regs[(scratch+7)*4]=0;CHECK(p.run()==em::Status::PostedStateRejected && !p.holds && p.writes==0);
    Model s;s.regs[s.l.auxiliary().sos]=1;CHECK(s.run()==em::Status::PostedStateRejected && s.reads==1 && s.writes==0);
    Model h;h.holdFails=true;CHECK(h.run()==em::Status::OwnerHoldFailed && !h.state.requiresOwnerRetention() && !h.smnReads && !h.writes);
    CHECK(!h.held && !h.result.ownerHeld && h.leaves==1);
    for(uint32_t flags:{0U,2U,UINT32_MAX}){Model f;f.flags=flags;CHECK(f.run()==(flags==UINT32_MAX?em::Status::DeviceUnavailable:em::Status::FirmwareNotRunning));
        CHECK(f.writes==0 && f.state.requiresOwnerRetention());}
}
void failuresAtEveryAccess(){
    Model base;CHECK(base.run()==em::Status::Ok);
    for(unsigned i=1;i<=base.reads;++i)for(unsigned mode=0;mode<3;++mode){Model m;
        if(mode==0)m.failRead=i;if(mode==1)m.onesRead=i;if(mode==2)m.revokeAfterRead=i;
        CHECK(m.run()==(mode==0?em::Status::ReadFailed:mode==1?em::Status::DeviceUnavailable:em::Status::OwnerRevoked));
        CHECK(m.reads==i && m.state.phase()==em::Phase::Faulted && m.leaves==1);
        const unsigned r=m.reads,w=m.writes;CHECK(m.run()==em::Status::InvalidState && m.reads==r && m.writes==w);}
    for(unsigned i=1;i<=base.writes;++i)for(bool revoke:{false,true}){Model m;
        if(revoke)m.revokeAfterWrite=i;else m.failWrite=i;
        CHECK(m.run()==(revoke?em::Status::OwnerRevoked:em::Status::WriteFailed));
        CHECK(m.writes==i && m.state.requiresOwnerRetention() && m.result.mailboxTouched && m.leaves==1);
        // Domain command may land even if the write callback returns false.
        const uint8_t wanted=i>=15?7:i>=12?3:i>=9?1:0;CHECK(m.result.attemptedDomains==wanted);}
    for(unsigned i=1;i<=base.smnReads;++i){Model m;m.failSmn=i;CHECK(m.run()==em::Status::ReadFailed);
        CHECK(m.smnReads==i && m.state.requiresOwnerRetention() && m.leaves==1);}
    for(unsigned i=1;i<=base.validCalls;++i){Model m;m.revokeAt=i;CHECK(m.run()==em::Status::OwnerRevoked);
        CHECK(m.validCalls==i && m.state.phase()==em::Phase::Faulted && m.leaves==1);}
    for(unsigned i=1;i<=base.timeCalls;++i){Model m;m.failTime=i;CHECK(m.run()==em::Status::ClockFailed);
        CHECK(m.timeCalls==i && m.leaves==1);}
    for(unsigned i=2;i<=base.timeCalls;++i){Model m;m.regressTime=i;
        if(i==2)continue;CHECK(m.run()==em::Status::ClockRegressed && m.timeCalls==i && m.leaves==1);}
}
void semanticFailures(){
    const uint32_t replies[]={0,0xff,0xfe,0xfd,0xfc,2};
    const em::Status status[]={em::Status::ResponseTimeout,em::Status::CommandFailed,em::Status::UnknownCommand,
        em::Status::BadPrerequisite,em::Status::Busy,em::Status::UnknownResponse};
    for(unsigned c=1;c<=5;++c)for(unsigned i=0;i<6;++i){Model m;m.failCommand=c;m.badResponse=replies[i];
        CHECK(m.run()==status[i] && m.commands.size()==c && m.leaves==1 && m.state.requiresOwnerRetention());}
    for(bool first:{false,true}){Model m;if(first)m.smuVersion=0;else m.ifVersion=0;
        CHECK(m.run()==em::Status::InvalidVersion && m.commands.size()==(first?1U:2U));}
    Model u;u.unreadableMedia=true;CHECK(u.run()==em::Status::DeviceUnavailable && u.result.attemptedDomains==7);
    CHECK(u.result.auxiliary.lastRegister==0x1f804 && u.result.auxiliary.lastValue==UINT32_MAX);
    Model a;a.activeMedia=true;CHECK(a.run()==em::Status::AuxiliaryNotStopped && a.result.auxiliary.status==ca::Status::VcnNotStopped);
    Model slow;slow.responseZeros=2;CHECK(slow.run()==em::Status::Ok);
    Model stuck;stuck.responseZeros=20;CHECK(stuck.run()==em::Status::ResponseTimeout && stuck.result.polls==limits.maxPolls);
    Model wall;wall.clockStep=10;auto p=limits;p.responseTimeoutUs=20;
    CHECK(wall.run(p)==em::Status::ResponseTimeout && wall.commands.size()==1);
    Model total;total.clockStep=1000;p=limits;p.totalTimeoutUs=100;p.responseTimeoutUs=50;
    CHECK(total.run(p)==em::Status::DeadlineExpired && total.reads==0 && total.writes==0);
    Model overflow;overflow.clock=UINT64_MAX-2;CHECK(overflow.run()==em::Status::ClockFailed && overflow.reads==0);
    Model revoke;CHECK(revoke.run()==em::Status::Ok);revoke.denyEnter=true;
    CHECK(em::verify(revoke.io(),limits,revoke.state,revoke.result)==em::Status::OwnerUnavailable);
    CHECK(revoke.state.phase()==em::Phase::Faulted && revoke.state.requiresOwnerRetention());
}
}
int main(){golden();preflight();failuresAtEveryAccess();semanticFailures();
    std::cout<<checks<<" early media checks; "<<failures<<" failures\n";return failures?1:0;}
