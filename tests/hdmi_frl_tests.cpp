// SPDX-License-Identifier: MIT
#include "hackynavi/hdmi_frl.hpp"
#include <iostream>
#include <vector>
namespace f=hackynavi::hdmifrl;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
struct Event{char kind;uint8_t offset,value;};
struct Bus {
    uint8_t config=0,version=1,test=0,updateExtra=4;bool ready=true,enabled=false,packets=false;
    bool admitted=true,alive=true,held=false,healthy=true,stuck=false,stopFails=false,freeze=false;
    bool backward=false,start=true,retrain=false,verifyBad=false,poisonFailure=false;
    unsigned failAt=0,revokeAt=0,ops=0,postRevoke=0,enters=0,leaves=0,frame=0,driveCalls=0;
    uint64_t time=100,ioDelay=0,ackDelay=0;uint8_t ffe=0;f::Config accepted={};
    std::vector<uint16_t> requests={0x1111,0xeeee,0x2222,0};
    std::vector<Event> log;
    bool operation(char kind,uint8_t off=0,uint8_t value=0){
        CHECK(held);if(!alive)++postRevoke;log.push_back({kind,off,value});++ops;time+=ioDelay;
        if(ops==revokeAt)alive=false;
        if(ops==failAt){if(poisonFailure)healthy=false;return false;}return true;}
    static Bus& b(void* p){return *static_cast<Bus*>(p);}
    static bool enter(void* p,f::Operation,const f::Config& c){auto& x=b(p);++x.enters;
        if(!x.admitted||x.held)return false;x.held=true;x.accepted=c;return true;}
    static void leave(void* p){auto& x=b(p);CHECK(x.held);x.held=false;++x.leaves;}
    static bool valid(void* p){return b(p).alive;}
    static bool now(void* p,uint64_t& t){auto& x=b(p);t=x.backward&&x.ops>3?0:x.time;return true;}
    static void delay(void* p,uint32_t us){auto& x=b(p);if(!x.freeze)x.time+=us;}
    static bool health(void* p){return b(p).healthy;}
    static bool read(void* p,uint8_t off,uint8_t* out,size_t n){auto& x=b(p);bool ok=x.operation('r',off);
        if(off==1)*out=x.version;
        else if(off==0x35)*out=x.test;
        else if(off==0x31)*out=uint8_t(x.config^(x.verifyBad&&x.config?1:0));
        else if(off==0x40)*out=x.ready?0x40:0;
        else if(off==0x10)*out=uint8_t(x.updateExtra|(x.packets?(x.retrain?0x20:x.start?0x10:0):x.config&&!x.stuck?0x20:0));
        else if(off==0x41){CHECK(n==2);const auto r=x.requests[x.frame<x.requests.size()?x.frame:x.requests.size()-1];out[0]=uint8_t(r);out[1]=uint8_t(r>>8);}
        else CHECK(false);
        return ok;}
    static bool write(void* p,uint8_t off,uint8_t value){auto& x=b(p);bool ok=x.operation('w',off,value);
        if(off==0x31)x.config=value;
        else if(off==0x10){CHECK(value==0x20||value==0x10);if(x.config&&!x.packets){++x.frame;x.time+=x.ackDelay;}}
        else CHECK(off==2&&value==1);return ok;}
    static bool prepare(void* p,const f::Config& c){auto& x=b(p);CHECK(c.rateCode==x.accepted.rateCode);
        x.enabled=true;return x.operation('p');}
    static bool drive(void* p,uint8_t v){auto& x=b(p);CHECK(v<=x.accepted.maxFfe);x.ffe=v;++x.driveCalls;return x.operation('d',0,v);}
    static bool patterns(void* p,const uint8_t v[4]){auto& x=b(p);for(unsigned i=0;i<4;++i)CHECK(v[i]<8||v[i]==255);return x.operation('t',0,v[0]);}
    static bool packet(void* p){auto& x=b(p);CHECK(x.enabled);x.packets=true;return x.operation('g');}
    static bool stop(void* p){auto& x=b(p);bool ok=x.operation('s');if(x.stopFails)return false;x.enabled=x.packets=false;return ok;}
    f::Io io(){return {this,enter,leave,valid,now,delay,read,write,health,prepare,drive,patterns,packet,stop};}
};
f::Config config(uint8_t rate=6){return {rate,6,6,3,0x12345678,42};}
void success(){
    for(uint8_t rate=1;rate<=6;++rate){Bus b;f::Link l;f::Result r={};
        CHECK(l.train(config(rate),b.io(),r)==f::Status::Ok);
        CHECK(l.phase()==f::Phase::Trained&&r.trainingObserved&&!r.startObserved&&r.retentionRequired);
        CHECK(b.config==uint8_t(0x30|rate)&&b.ffe==1&&b.driveCalls==2&&!b.packets);
        CHECK(l.startPackets(r)==f::Status::Ok&&r.startObserved&&l.phase()==f::Phase::Started);
        CHECK(l.configuration().modePlanIdentity==0x12345678&&l.configuration().rateCode==rate);
        CHECK(l.stop(r)==f::Status::Ok&&r.sourceOff&&r.sinkRateOff&&!r.retentionRequired);
        CHECK(!b.enabled&&!b.config&&!b.held&&b.enters==b.leaves);
        size_t off=0,rateOff=0;for(size_t i=0;i<b.log.size();++i){if(b.log[i].kind=='s')off=i;if(b.log[i].kind=='w'&&b.log[i].offset==0x31&&!b.log[i].value)rateOff=i;}
        CHECK(off<rateOff);
    }
    Bus b;b.requests={0xf000};f::Link l;f::Result r={};
    CHECK(l.train(config(2),b.io(),r)==f::Status::Ok); // inactive fourth lane ignored
    CHECK(l.stop(r)==f::Status::Ok);
}
void admission(){
    for(unsigned n=0;n<7;++n){Bus b;f::Link l;f::Result r={};auto c=config();
        if(n==0)c.rateCode=0;if(n==1)c.rateCode=7;if(n==2)c.sourceMaxRateCode=5;
        if(n==3)c.sinkMaxRateCode=5;if(n==4)c.maxFfe=4;if(n==5)c.modePlanIdentity=0;if(n==6)c.routeGeneration=0;
        CHECK(l.train(c,b.io(),r)==f::Status::InvalidArgument&&b.ops==0&&b.enters==0);}
    Bus b;b.admitted=false;f::Link l;f::Result r={};
    CHECK(l.train(config(),b.io(),r)==f::Status::AdmissionFailed&&b.ops==0&&!r.retentionRequired);
    CHECK(l.startPackets(r)==f::Status::InvalidState);
}
void rejection(){
    for(unsigned n=0;n<12;++n){Bus b;f::Link l;f::Result r={};f::Status expected=f::Status::Ok;
        switch(n){
        case 0:b.version=0;expected=f::Status::UnsupportedReceiver;break;
        case 1:b.test=0x20;expected=f::Status::UnsupportedTestRequest;break;
        case 2:b.ready=false;expected=f::Status::ReadyTimeout;break;
        case 3:b.stuck=true;expected=f::Status::TrainingTimeout;break;
        case 4:b.requests={0x1111,0xffff};expected=f::Status::LowerRateRequired;break;
        case 5:b.requests={0x9999};expected=f::Status::InvalidPattern;break;
        case 6:b.requests={0xeeee};expected=f::Status::FfeExhausted;break;
        case 7:b.requests={0x3333};expected=f::Status::TrainingTimeout;break;
        case 8:b.freeze=true;expected=f::Status::ClockFailed;break;
        case 9:b.backward=true;expected=f::Status::ClockFailed;break;
        case 10:b.verifyBad=true;expected=f::Status::VerifyFailed;break;
        case 11:b.requests={0};b.ackDelay=210001;expected=f::Status::TrainingTimeout;break;
        }
        CHECK(l.train(config(),b.io(),r)==expected);
        CHECK(!r.retentionRequired&&l.phase()==f::Phase::Closed&&!b.config&&!b.enabled&&!b.held);
    }
    for(unsigned n=0;n<3;++n){Bus b;f::Link l;f::Result r={};CHECK(l.train(config(),b.io(),r)==f::Status::Ok);
        if(n==0)b.start=false;if(n==1)b.retrain=true;if(n==2)b.updateExtra|=8;
        const auto expect=n==0?f::Status::StartTimeout:n==1?f::Status::RetrainRequired:f::Status::UnsupportedTestRequest;
        CHECK(l.startPackets(r)==expect&&l.phase()==f::Phase::Closed&&!r.startObserved);
    }
}
void failuresAtEveryOperation(){
    Bus baseline;f::Link base;f::Result r={};CHECK(base.train(config(),baseline.io(),r)==f::Status::Ok);
    const unsigned trainOps=baseline.ops;CHECK(base.stop(r)==f::Status::Ok);
    for(unsigned op=1;op<=trainOps;++op){
        Bus b;b.failAt=op;f::Link l;auto s=l.train(config(),b.io(),r);
        CHECK(s!=f::Status::Ok&&!b.held&&b.postRevoke==0);
        // Every source failure receives shutdown before any receiver rate reset.
        CHECK(!r.retentionRequired&&!b.enabled&&!b.config&&l.phase()==f::Phase::Closed);
        Bus q;q.revokeAt=op;f::Link revoked;s=revoked.train(config(),q.io(),r);
        CHECK(s==f::Status::Revoked&&q.postRevoke==0&&!q.held);
        if(r.retentionRequired)CHECK(revoked.phase()==f::Phase::Faulted);
        revoked.clearAfterVerifiedReset();CHECK(!revoked.requiresHardwareRetirement());
    }
    Bus b;b.failAt=1;b.poisonFailure=true;f::Link l;
    CHECK(l.train(config(),b.io(),r)==f::Status::ScdcFailed);
    CHECK(r.ddcRecoveryRequired&&r.retentionRequired&&l.phase()==f::Phase::Faulted&&b.ops==1);
    CHECK(l.train(config(),b.io(),r)==f::Status::InvalidState);
    b.healthy=true;CHECK(l.stop(r)==f::Status::Ok&&!r.retentionRequired);
    Bus q;f::Link retained;CHECK(retained.train(config(),q.io(),r)==f::Status::Ok);
    q.stopFails=true;CHECK(retained.stop(r)==f::Status::SourceFailed&&r.retentionRequired&&q.config);
    const auto ops=q.ops;q.alive=false;CHECK(retained.stop(r)==f::Status::Revoked&&q.ops==ops);
    q.alive=true;q.stopFails=false;CHECK(retained.stop(r)==f::Status::Ok&&!r.retentionRequired);
}
}
int main(){success();admission();rejection();failuresAtEveryOperation();
    std::cout<<checks<<" FRL protocol checks, "<<failures<<" failures\n";return failures?1:0;}
