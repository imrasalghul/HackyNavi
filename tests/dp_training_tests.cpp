// SPDX-License-Identifier: MIT
// Offline receiver/source protocol model. No hardware success is claimed.
#include "hackynavi/dp_training.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>
namespace t=hackynavi::dptrain;
namespace l=hackynavi::dio;
namespace d=hackynavi::dmcub;
namespace a=hackynavi::dpaux;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
const t::Config config={4,810000,{4,810000},false};
struct Op {char type;uint32_t at;std::vector<uint8_t> bytes;};
struct Monitor {
    std::map<uint32_t,uint8_t> dpcd;
    std::vector<Op> ops;std::vector<uint32_t> waits;
    std::vector<std::vector<uint8_t>> statuses={{0x11,0x11,0,0,0,0},{0x77,0x77,1,0,0,0},{0x77,0x77,1,0,0,0}};
    size_t failAt=0,statusIndex=0;bool auxOk=true,sourceOk=true,optionalNack=true;
    bool stickyFailure=false,ignorePower=false,ignoreD3=false,ignoreExit=false,enable=true,disabled=false,video=false;
    uint8_t pattern=0,vs=0,pre=0;
    Monitor() {
        for(uint32_t n=0;n<15;++n)dpcd[n]=0;
        dpcd[0]=0x14;dpcd[1]=0x1e;dpcd[2]=0xc4;dpcd[3]=0x81;dpcd[6]=1;
        dpcd[0x600]=0xa2;dpcd[0x102]=0;
    }
    t::Io io(){return {this,read,write,start,training,drive,idle,stop,auxHealthy,sourceHealthy,delay};}
    bool operation(char type,uint32_t at,const uint8_t* data,size_t n) {
        ops.push_back({type,at,std::vector<uint8_t>(data,data+n)});
        if(ops.size()!=failAt)return true;
        if(stickyFailure){if(type=='R'||type=='W')auxOk=false;else sourceOk=false;}
        return false;
    }
    static t::AuxStatus read(void* p,uint32_t at,uint8_t* out,uint8_t n) {
        auto& s=*static_cast<Monitor*>(p);CHECK(s.auxOk);
        if(at==0xf0000 && s.optionalNack) {
            uint8_t ignored=0;return s.operation('R',at,&ignored,0)?t::AuxStatus::Nack:t::AuxStatus::Failed;
        }
        if(at==0x202) {
            CHECK(n==6);const auto& b=s.statuses[std::min(s.statusIndex++,s.statuses.size()-1)];
            std::copy(b.begin(),b.end(),out);
        } else for(uint8_t i=0;i<n;++i)out[i]=s.dpcd[at+i];
        return s.operation('R',at,out,n)?t::AuxStatus::Ok:t::AuxStatus::Failed;
    }
    static t::AuxStatus write(void* p,uint32_t at,const uint8_t* data,uint8_t n) {
        auto& s=*static_cast<Monitor*>(p);CHECK(s.auxOk);
        const bool ignore=(at==0x600&&(s.ignorePower||(s.ignoreD3&&(*data&7)==2))) ||
            (at==0x102&&s.ignoreExit&&*data==0&&s.statusIndex>=2);
        if(!ignore)for(uint8_t i=0;i<n;++i)s.dpcd[at+i]=data[i];
        return s.operation('W',at,data,n)?t::AuxStatus::Ok:t::AuxStatus::Failed;
    }
    static bool start(void* p,uint32_t rate){auto& s=*static_cast<Monitor*>(p);CHECK(s.sourceOk);uint8_t b=uint8_t(rate/27000);return s.operation('E',rate,&b,1)&&s.enable;}
    static bool training(void* p,l::Pattern pattern){auto& s=*static_cast<Monitor*>(p);CHECK(s.sourceOk);s.pattern=uint8_t(pattern);return s.operation('P',0,&s.pattern,1);}
    static bool drive(void* p,uint8_t vs,uint8_t pre){auto& s=*static_cast<Monitor*>(p);CHECK(s.sourceOk);CHECK(vs<=3&&pre<=3&&vs+pre<=3);s.vs=vs;s.pre=pre;const uint8_t b[]={vs,pre};return s.operation('D',0,b,2);}
    static bool idle(void* p){auto& s=*static_cast<Monitor*>(p);CHECK(s.sourceOk);uint8_t b=0;CHECK(s.dpcd[0x102]==0);s.video=true;return s.operation('V',0,&b,0);}
    static bool stop(void* p){auto& s=*static_cast<Monitor*>(p);CHECK(s.sourceOk);uint8_t b=0;const bool ok=s.operation('X',0,&b,0);s.disabled=ok;return ok;}
    static bool auxHealthy(void* p){return static_cast<Monitor*>(p)->auxOk;}
    static bool sourceHealthy(void* p){return static_cast<Monitor*>(p)->sourceOk;}
    static void delay(void* p,uint32_t us){auto& s=*static_cast<Monitor*>(p);CHECK(us&&us<=64000);s.waits.push_back(us);}
};
bool written(const Monitor& m,uint32_t address,const std::vector<uint8_t>& data) {
    for(const auto& op:m.ops)if(op.type=='W'&&op.at==address&&op.bytes==data)return true;return false;
}
void normal() {
    for(uint8_t lanes:{1,2,4})for(uint32_t rate:{162000,270000,540000,810000})for(unsigned pattern=0;pattern<3;++pattern) {
        if(rate==810000 && pattern!=2)continue;
        Monitor m;t::Result r{};auto c=config;c.lanes=lanes;c.symbolClockKHz=rate;
        m.dpcd[2]=uint8_t(0x84|(pattern?0x40:0));m.dpcd[3]=uint8_t(1|(pattern==2?0x80:0));
        CHECK(t::train(m.io(),c,r)==t::Status::Ok && r.trained && !r.recoveryRequired && !r.cleanupAttempted);
        CHECK(r.crAttempts==1 && r.eqAttempts==1 && r.stage==t::Stage::Complete);
        CHECK(m.video && !m.disabled && m.statusIndex==3);
        CHECK(m.waits==std::vector<uint32_t>({1000,100,400,5000}));
        CHECK(written(m,0x600,{0xa1})&&written(m,0x108,{1})&&written(m,0x101,{uint8_t(0x80|lanes)}));
        CHECK(written(m,0x100,{uint8_t(rate/27000)})&&written(m,0x107,{0}));
        std::vector<uint8_t> first(lanes+1,0);first[0]=0x21;CHECK(written(m,0x102,first));
        first[0]=pattern==2?7:pattern==1?0x23:0x22;CHECK(written(m,0x102,first));
        CHECK(r.eqPattern==(pattern==2?l::Pattern::Tps4:pattern==1?l::Pattern::Tps3:l::Pattern::Tps2));
    }
    // Only active lanes participate: stale failures on unused lanes are ignored.
    Monitor one;t::Result r{};auto c=config;c.lanes=1;one.statuses={{1,0,0,0,0,0},{7,0,1,0,0,0},{7,0,1,0,0,0}};
    CHECK(t::train(one.io(),c,r)==t::Status::Ok);
    // Extended receiver capabilities supersede the base only at equal/newer revision.
    Monitor extended;extended.dpcd[14]=0x80;extended.dpcd[1]=6;
    for(uint32_t n=0;n<15;++n)extended.dpcd[0x2200+n]=extended.dpcd[n];
    extended.dpcd[0x2201]=0x1e;extended.dpcd[0x220e]=0x80;
    CHECK(t::train(extended.io(),config,r)==t::Status::Ok && r.sink.extended);
    CHECK(r.sink.crDelayUs==400);
    Monitor auxOn;auxOn.dpcd[0x600]=0xa5;
    CHECK(t::train(auxOn.io(),config,r)==t::Status::Ok && written(auxOn,0x600,{0xa1}));
    Monitor auxOnFailure;auxOnFailure.dpcd[0x600]=0xa5;auxOnFailure.statuses={{0,0,0,0,0,0}};
    CHECK(t::train(auxOnFailure.io(),config,r)==t::Status::ClockRecoveryFailed && written(auxOnFailure,0x600,{0xa2}) && r.sinkPoweredDown);
    Monitor stuckAux;stuckAux.dpcd[0x600]=0xa5;stuckAux.ignorePower=true;
    CHECK(t::train(stuckAux.io(),config,r)==t::Status::VerifyFailed && !r.trained && r.recoveryRequired);
    for(uint8_t interval=0;interval<=6;++interval) {
        Monitor m;m.dpcd[14]=interval;CHECK(t::train(m.io(),config,r)==t::Status::Ok);
        const uint32_t eq[]={400,4000,8000,12000,16000,32000,64000};CHECK(r.sink.eqDelayUs==eq[interval]);
        CHECK(r.sink.crDelayUs==(interval?uint32_t(interval)*4000:100));
    }
}
void adjustments() {
    Monitor m;t::Result r{};
    // Lane3 requests stronger levels than lane0. All lanes must match the
    // maximum requested swing, with pre-emphasis limited by 3 - swing.
    m.statuses={{0,0,0,0,0x04,0xea},{0x11,0x11,0,0,0,0},{0x77,0x77,1,0,0,0},{0x77,0x77,1,0,0,0}};
    CHECK(t::train(m.io(),config,r)==t::Status::Ok && r.crAttempts==2);
    CHECK(written(m,0x103,{0x0a,0x0a,0x0a,0x0a})); // VS2/PRE1, not illegal VS2/PRE3
    CHECK(r.swing==2 && r.preEmphasis==1);
    Monitor maximum;maximum.statuses={{0,0,0,0,3,0},{0x11,0x11,0,0,0,0},{0x77,0x77,1,0,0,0},{0x77,0x77,1,0,0,0}};
    CHECK(t::train(maximum.io(),config,r)==t::Status::Ok && written(maximum,0x103,{7,7,7,7}));
    Monitor pre;pre.statuses={{0,0,0,0,0x0c,0},{0x11,0x11,0,0,0,0},{0x77,0x77,1,0,0,0},{0x77,0x77,1,0,0,0}};
    CHECK(t::train(pre.io(),config,r)==t::Status::Ok && written(pre,0x103,{0x38,0x38,0x38,0x38}));
    Monitor zero;zero.statuses={{0,0,0,0,0,0}};
    CHECK(t::train(zero.io(),config,r)==t::Status::ClockRecoveryFailed && r.crAttempts==5 && !r.trained);
    CHECK(r.cleanupAttempted&&r.sourceDisabled&&r.sinkPoweredDown&&!r.recoveryRequired&&zero.dpcd[0x600]==0xa2);
    Monitor oscillating;oscillating.statuses.clear();
    for(unsigned n=0;n<101;++n)oscillating.statuses.push_back({0,0,0,0,uint8_t(n%2?0:1),0});
    CHECK(t::train(oscillating.io(),config,r)==t::Status::ClockRecoveryFailed && r.crAttempts==100);
    Monitor maxFail;maxFail.statuses={{0,0,0,0,3,0},{0,0,0,0,3,0}};
    CHECK(t::train(maxFail.io(),config,r)==t::Status::ClockRecoveryFailed && r.crAttempts==2);
    Monitor eqFail;eqFail.statuses={{0x11,0x11,0,0,0,0}};
    CHECK(t::train(eqFail.io(),config,r)==t::Status::EqualizationFailed && r.eqAttempts==6 && !eqFail.video);
    Monitor lostCr;lostCr.statuses={{0x11,0x11,0,0,0,0},{0x77,0x70,1,0,0,0}};
    CHECK(t::train(lostCr.io(),config,r)==t::Status::ClockRecoveryFailed && r.eqAttempts==1);
    for(unsigned lane=0;lane<4;++lane)for(uint8_t bit:{1,2,4}) {
        Monitor lost;lost.statuses.back()[lane/2]&=uint8_t(~(bit<<((lane%2)*4)));
        CHECK(t::train(lost.io(),config,r)==t::Status::LinkLost && !r.trained && r.sourceDisabled);
    }
    Monitor align;align.statuses.back()[2]=0;CHECK(t::train(align.io(),config,r)==t::Status::LinkLost);
}
void rejects() {
    for(unsigned n=0;n<17;++n) {
        Monitor m;t::Result r{};auto c=config;
        if(n==0)m.dpcd[0]=0xff;if(n==1)m.dpcd[1]=0;if(n==2)m.dpcd[2]=0xc3;if(n==3)m.dpcd[2]&=0x7f;
        if(n==4)m.dpcd[6]=2;if(n==5)m.dpcd[13]=1;if(n==6)m.dpcd[5]=1;if(n==7)m.dpcd[0x111]=1;
        if(n==8){m.optionalNack=false;m.dpcd[0xf0002]=0x80;}if(n==9)m.dpcd[0x120]=1;if(n==10)m.dpcd[0x160]=1;
        if(n==11)m.dpcd[14]=7;if(n==12)c.source.maxLanes=2;if(n==13)c.source.maxSymbolClockKHz=540000;
        if(n==14)m.dpcd[1]=0x14;if(n==15)m.dpcd[3]&=0x7f;if(n==16){m.dpcd[3]&=0xfe;c.downspread=true;}
        CHECK(t::train(m.io(),c,r)!=t::Status::Ok && !r.trained && !r.cleanupAttempted);
        for(const auto& op:m.ops)CHECK(op.type=='R');
    }
    Monitor power;t::Result r{};power.ignorePower=true;
    CHECK(t::train(power.io(),config,r)==t::Status::VerifyFailed && !r.trained && r.sourceDisabled);
    Monitor m;auto c=config;c.symbolClockKHz=1000000;CHECK(t::train(m.io(),c,r)==t::Status::InvalidArgument && m.ops.empty());
    c=config;c.lanes=3;CHECK(t::train(m.io(),c,r)==t::Status::InvalidArgument && m.ops.empty());
    auto io=m.io();io.readDpcd=nullptr;CHECK(t::train(io,config,r)==t::Status::InvalidArgument);
    m.auxOk=false;CHECK(t::train(m.io(),config,r)==t::Status::InvalidState && m.ops.empty());
}
void failuresTest() {
    Monitor baseline;t::Result r{};CHECK(t::train(baseline.io(),config,r)==t::Status::Ok);
    const auto ops=baseline.ops;
    for(size_t n=1;n<=ops.size();++n)for(bool sticky:{false,true}) {
        Monitor m;m.failAt=n;m.stickyFailure=sticky;
        CHECK(t::train(m.io(),config,r)!=t::Status::Ok && !r.trained);
        CHECK(r.status==(ops[n-1].type=='R'||ops[n-1].type=='W'?t::Status::AuxFailed:t::Status::SourceFailed));
        CHECK(m.ops.size()<=ops.size()+5);
        if(r.cleanupAttempted)CHECK(r.recoveryRequired==sticky);
    }
    Monitor failEnable;failEnable.enable=false;
    CHECK(t::train(failEnable.io(),config,r)==t::Status::SourceFailed && !r.trained && r.sourceDisabled);
    Monitor lostPower;lostPower.statuses={{0,0,0,0,0,0}};lostPower.ignoreD3=true;
    CHECK(t::train(lostPower.io(),config,r)==t::Status::ClockRecoveryFailed && r.recoveryRequired && !r.sinkPoweredDown);
    Monitor noExit;noExit.ignoreExit=true;
    CHECK(t::train(noExit.io(),config,r)==t::Status::VerifyFailed && !r.trained && !noExit.video && r.recoveryRequired);
}
// Full AUX FIFO + DMCUB ring + DIO register adapter test. The emulated receiver
// supplies status replies; production train() still drives the actual modules.
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v){put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16));}
std::vector<uint8_t> discovery(){
    std::vector<uint8_t> b(164,0);put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,164);
    put16(b,12,60);put16(b,16,104);put32(b,60,0x53445049);put16(b,64,3);put16(b,66,104);
    put16(b,72,1);put16(b,76,140);put16(b,142,1);put16(b,144,271);b[147]=3;b[148]=3;b[149]=2;put32(b,160,0x34c0);
    uint16_t sum=0;for(size_t n=60;n<b.size();++n)sum=uint16_t(sum+b[n]);put16(b,14,sum);
    sum=0;for(size_t n=10;n<b.size();++n)sum=uint16_t(sum+b[n]);put16(b,8,sum);return b;
}
struct Hardware {
    Monitor sink;std::map<uint32_t,uint32_t> regs;
    a::Layout al{};d::Layout dl{};l::Layout ll{};a::Engine aux;d::Inbox inbox;l::Link link;
    std::vector<uint8_t> memory=std::vector<uint8_t>(16384,0),tx,rx;size_t rxIndex=0,commands=0,packets=0;
    const d::Route route={0x3113,0x211e,0,3,2,true};
    const d::PollOptions polls={4,10};
    uint32_t ar(a::Register r)const{return al.registers[3][size_t(r)];}
    uint32_t dr(d::Register r)const{return dl.registers[size_t(r)];}
    Hardware(){
        const auto b=discovery();CHECK(a::buildLayout(b.data(),b.size(),0x100000,al)==a::Status::Ok);
        CHECK(d::buildLayout(b.data(),b.size(),0x100000,dl)==d::Status::Ok);
        CHECK(l::buildLayout(b.data(),b.size(),0x100000,ll)==l::Status::Ok);
        for(auto reg:dl.registers)regs[reg]=0;for(const auto& rows:al.registers)for(auto reg:rows)regs[reg]=0;
        for(const auto& rows:ll.registers)for(auto reg:rows)regs[reg]=0;
        regs[dr(d::Register::Control)]=0x10000;regs[dr(d::Register::BootStatus)]=3;regs[dr(d::Register::AxiSpace)]=3;
        regs[dr(d::Register::Cw4Base)]=0x04000000;regs[dr(d::Register::Cw4Top)]=0x84004000;
        regs[dr(d::Register::Cw4Offset)]=0x40000000;regs[dr(d::Register::InboxBase)]=0x64000000;regs[dr(d::Register::InboxSize)]=8192;
        d::Result ir{};CHECK(inbox.open(dl,{this,read,write,readMemory,writeMemory,flush,delay},0x40000000,ir)==d::Status::Ok);
        a::Result auxResult{};CHECK(aux.open(al,{this,read,write,delay},3,2,auxResult)==a::Status::Ok);
        l::Result lr{};CHECK(link.prepare(ll,{this,read,write,delay},route,4,4,lr)==l::Status::Ok);
    }
    static bool read(void* p,uint32_t reg,uint32_t& value){
        auto& h=*static_cast<Hardware*>(p);CHECK(h.regs.count(reg));value=h.regs[reg];
        if(reg==h.ar(a::Register::Data)&&(value&1))value=(value&~0xff00U)|(uint32_t(h.rxIndex<h.rx.size()?h.rx[h.rxIndex++]:0)<<8);
        return true;
    }
    static bool write(void* p,uint32_t reg,uint32_t value){
        auto& h=*static_cast<Hardware*>(p);CHECK(h.regs.count(reg));const uint32_t old=h.regs[reg];h.regs[reg]=value;
        if(reg==h.ar(a::Register::Control))h.regs[reg]=(value&~0x20U)|((value&0x10)?0x20U:0);
        else if(reg==h.ar(a::Register::Arbitration))h.regs[reg]=(value&0x20000)?value&~0x3000cU:(value&0x10000)?(value&~0xcU)|4:value;
        else if(reg==h.ar(a::Register::Interrupt)){if(value&2)h.regs[h.ar(a::Register::Status)]=0;}
        else if(reg==h.ar(a::Register::Data)){
            if(!(value&1)){if(value&0x80000000)h.tx.clear();h.tx.push_back(uint8_t(value>>8));}
            else if(!(old&1))h.rxIndex=0;
        }else if(reg==h.ar(a::Register::SwControl)&&(value&1)){
            ++h.packets;CHECK(h.tx.size()==((value>>16)&31));CHECK(h.tx.size()>=4);
            const uint32_t at=(uint32_t(h.tx[0]&15)<<16)|(uint32_t(h.tx[1])<<8)|h.tx[2];
            const uint8_t n=uint8_t(h.tx[3]+1);uint8_t response[16]={};t::AuxStatus status;
            if((h.tx[0]&0xf0)==0x90)status=Monitor::read(&h.sink,at,response,n);
            else {CHECK((h.tx[0]&0xf0)==0x80&&h.tx.size()==size_t(n+4));status=Monitor::write(&h.sink,at,h.tx.data()+4,n);}
            h.rx={uint8_t(status==t::AuxStatus::Ok?0:0x10)};
            if(status==t::AuxStatus::Ok&&(h.tx[0]&0xf0)==0x90)h.rx.insert(h.rx.end(),response,response+n);
            h.rxIndex=0;h.regs[h.ar(a::Register::Status)]=1|(uint32_t(h.rx.size())<<24);h.regs[reg]&=~1U;
        }else if(reg==h.dr(d::Register::Wptr)){
            ++h.commands;CHECK(value==(old+64)%8192);const auto* c=h.memory.data()+old;
            CHECK(c[0]==0x80&&c[1]==1&&c[4]==0);
            if(c[5]==0||c[5]==1)h.regs[h.ll.registers[0][size_t(l::Register::BackendEnable)]]=c[5];
            else CHECK(c[5]==11);
            h.regs[h.dr(d::Register::Rptr)]=value;
        }
        return true;
    }
    static bool readMemory(void* p,uint64_t at,uint8_t* out,size_t n){auto& h=*static_cast<Hardware*>(p);CHECK(at>=0x40000000&&at+n<=0x40004000);std::memcpy(out,h.memory.data()+size_t(at-0x40000000),n);return true;}
    static bool writeMemory(void* p,uint64_t at,const uint8_t* in,size_t n){auto& h=*static_cast<Hardware*>(p);CHECK(at>=0x40000000&&at+n<=0x40004000);std::memcpy(h.memory.data()+size_t(at-0x40000000),in,n);return true;}
    static bool flush(void*){return true;}
    static void delay(void*,uint32_t){}
};
void adapter(){
    Hardware h;t::HardwareResult r{};
    CHECK(t::train(h.aux,h.link,h.inbox,config,h.polls,{&h,Hardware::delay},r)==t::Status::Ok);
    CHECK(r.training.trained&&h.link.phase()==l::Phase::VideoPattern&&h.aux.isOpen()&&h.inbox.isOpen());
    CHECK(h.commands==9&&h.packets>15&&h.sink.statusIndex==3);
    CHECK(t::train(h.aux,h.link,h.inbox,config,h.polls,{&h,Hardware::delay},r)==t::Status::InvalidState);
    Hardware mismatch;a::Result ar{};CHECK(mismatch.aux.close(ar)==a::Status::Ok);
    CHECK(mismatch.aux.open(mismatch.al,{&mismatch,Hardware::read,Hardware::write,Hardware::delay},3,1,ar)==a::Status::Ok);
    CHECK(t::train(mismatch.aux,mismatch.link,mismatch.inbox,config,mismatch.polls,{&mismatch,Hardware::delay},r)==t::Status::InvalidState);
    Hardware failing;failing.sink.statuses={{0,0,0,0,0,0}};
    CHECK(t::train(failing.aux,failing.link,failing.inbox,config,failing.polls,{&failing,Hardware::delay},r)==t::Status::ClockRecoveryFailed);
    CHECK(r.training.sourceDisabled&&!r.training.recoveryRequired&&failing.link.phase()==l::Phase::Closed);
}
}
int main(){normal();adjustments();rejects();failuresTest();adapter();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
