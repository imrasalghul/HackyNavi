// SPDX-License-Identifier: MIT
// Hardware-independent fault-injection tests, never target hardware access.
#include "hackynavi/audio_quiesce.hpp"
#include <iostream>
#include <map>
#include <vector>
using namespace hackynavi::audioquiesce;
namespace {
unsigned checks=0,failures=0;
#define CHECK(c) do{++checks;if(!(c)){++failures;std::cerr<<__LINE__<<": " #c "\n";}}while(0)
struct Access {bool write;uint32_t offset;uint8_t width;uint32_t value;};
struct Sim {
    std::map<uint32_t,uint32_t> regs;
    std::vector<Access> accesses;
    uint64_t time=1000;
    unsigned reads=0,writes=0,valids=0,clocks=0,delays=0;
    unsigned failRead=0,onesRead=0,failWrite=0,revokeAt=0,clockFail=0,clockBack=0;
    bool landed=false,revoked=false,delayFail=false,lateDelay=false,frozen=false;
    uint32_t sticky=UINT32_MAX,mutateOffset=UINT32_MAX,mutateValue=0;
    unsigned mutateAt=0;
    uint8_t count;
    explicit Sim(unsigned in=0,unsigned out=6,unsigned bi=0):count(uint8_t(in+out+bi)) {
        regs[0]=(in<<8)|(out<<12)|(bi<<3)|1;
        regs[2]=0;regs[3]=1;regs[8]=0x101;regs[0xc]=0x803f;
        regs[0x20]=0xc0000000|((uint32_t(1)<<count)-1);
        regs[0x4c]=0x83;regs[0x5c]=0x87;regs[0x70]=0x34567001;
        for(unsigned i=0;i<count;++i)regs[0x80+i*0x20]=0x3e;
    }
    static uint8_t width(uint32_t offset) {
        if(offset==0||offset==0xc)return 2;
        if(offset==8||offset==0x20||offset==0x70)return 4;
        if(offset==2||offset==3||offset==0x4c||offset==0x5c||
           (offset>=0x80 && !(offset&0x1f)))return 1;
        CHECK(false);return 0;
    }
    static bool valid(void* c){auto& s=*static_cast<Sim*>(c);++s.valids;
        if(s.revokeAt==s.valids)s.revoked=true;return !s.revoked;}
    static bool now(void* c,uint64_t& out){auto& s=*static_cast<Sim*>(c);++s.clocks;
        out=s.clockBack==s.clocks?s.time-100:s.time;return s.clockFail!=s.clocks;}
    static bool delay(void* c,uint32_t us){auto& s=*static_cast<Sim*>(c);++s.delays;
        CHECK(!s.revoked);if(!s.frozen)s.time+=s.lateDelay?1000001:us;return !s.delayFail;}
    static bool read(void* c,uint32_t off,uint8_t n,uint32_t& v){auto& s=*static_cast<Sim*>(c);
        CHECK(!s.revoked&&width(off)==n&&s.regs.count(off));++s.reads;
        if(s.mutateAt==s.reads)s.regs[s.mutateOffset]=s.mutateValue;
        v=s.onesRead==s.reads?(n==1?0xff:n==2?0xffff:UINT32_MAX):s.regs[off];
        s.accesses.push_back({false,off,n,v});if(!s.frozen)++s.time;return s.reads!=s.failRead;}
    static bool r8(void*c,uint32_t o,uint8_t&v){uint32_t x=0;bool ok=read(c,o,1,x);v=uint8_t(x);return ok;}
    static bool r16(void*c,uint32_t o,uint16_t&v){uint32_t x=0;bool ok=read(c,o,2,x);v=uint16_t(x);return ok;}
    static bool r32(void*c,uint32_t o,uint32_t&v){return read(c,o,4,v);}
    static bool write(void*c,uint32_t o,uint8_t n,uint32_t v){auto&s=*static_cast<Sim*>(c);
        CHECK(!s.revoked&&width(o)==n&&s.regs.count(o));++s.writes;
        CHECK(o!=0&&o!=2&&o!=3); // No capability/status/address writes.
        if(o==8&&!(v&1)) {
            CHECK(!(s.regs[0x4c]&3)&&!(s.regs[0x5c]&7)&&!(s.regs[0x70]&1));
            for(unsigned i=0;i<s.count;++i)CHECK(!(s.regs[0x80+i*0x20]&0x1e));
        }
        if(o==8)CHECK(!(v&2)); // Never initiates/replays a flush.
        s.accesses.push_back({true,o,n,v});if(!s.frozen)++s.time;
        if(s.sticky!=o&&(s.writes!=s.failWrite||s.landed))s.regs[o]=v;
        return s.writes!=s.failWrite;
    }
    static bool w8(void*c,uint32_t o,uint8_t v){return write(c,o,1,v);}
    static bool w16(void*c,uint32_t o,uint16_t v){return write(c,o,2,v);}
    static bool w32(void*c,uint32_t o,uint32_t v){return write(c,o,4,v);}
    Io io(){return {this,valid,r8,r16,r32,w8,w16,w32,now,delay};}
};
void complete(Sim& s,const Result& r) {
    CHECK(r.status==Status::Ok&&r.complete&&r.stage==Stage::Complete);
    CHECK(!(s.regs[8]&0x103)&&!(s.regs[0xc]&0x7fff));
    CHECK(r.elapsedUs>=100&&r.elapsedUs<1000000);
    CHECK(r.reads==s.reads&&r.writes==s.writes);
}
void golden() {
    for(unsigned in=0;in<=15;++in)for(unsigned out=0;out<=15;++out)for(unsigned bi=0;bi<=30;++bi) {
        const unsigned n=in+out+bi;if(!n||n>30)continue;
        Sim s(in,out,bi);Result r{};
        // INTCTL all-ones is deliberately rejected even for30 engines.
        if(n==30)s.regs[0x20]&=~1U;
        CHECK(stop(s.io(),0x80+n*0x20,1000000,r)==Status::Ok);complete(s,r);
        CHECK(r.streamCountsKnown&&r.streamCount==n&&r.inputStreams==in&&
              r.outputStreams==out&&r.bidirectionalStreams==bi);
        CHECK(r.stoppedStreamMask==((uint32_t(1)<<n)-1));
        CHECK(s.regs[0x4c]==0x80&&s.regs[0x5c]==0x80&&s.regs[0x70]==0x34567000);
        CHECK(s.regs[0xc]==0x8000); // Preserve WAKEEN's reserved bit15.
        for(unsigned i=0;i<n;++i)CHECK(s.regs[0x80+i*0x20]==0x20);
    }
    {Sim s;Result r{};s.regs[8]=0;
        CHECK(stop(s.io(),0x80,1000000,r)==Status::Ok);complete(s,r);
        CHECK(r.initiallyInReset&&!r.streamCountsKnown&&!r.streamCount&&!r.stoppedStreamMask);
        for(const auto&a:s.accesses)CHECK(a.offset==8||a.offset==0xc);}
    {Sim s;Result r{};
        CHECK(stop(s.io(),4096,1000000,r)==Status::Ok);
        s.accesses.clear();s.reads=s.writes=0;
        CHECK(stop(s.io(),4096,1000000,r)==Status::Ok);complete(s,r);
        CHECK(r.initiallyInReset&&!r.touched&&!r.writes);}
}
void faults() {
    Sim baseline;Result r{};CHECK(stop(baseline.io(),4096,1000000,r)==Status::Ok);
    for(unsigned n=1;n<=baseline.reads;++n)for(bool ones:{false,true}) {
        Sim s;if(ones)s.onesRead=n;else s.failRead=n;
        CHECK(stop(s.io(),4096,1000000,r)==(ones?Status::DeviceUnavailable:Status::ReadFailed));
        CHECK(!r.complete&&s.reads==n&&r.lastReadSucceeded==ones);
        CHECK(ones||!r.lastValue);CHECK(r.touched==(s.writes!=0));
    }
    for(unsigned n=1;n<=baseline.writes;++n)for(bool land:{false,true}) {
        Sim s;s.failWrite=n;s.landed=land;
        CHECK(stop(s.io(),4096,1000000,r)==Status::WriteFailed);
        CHECK(!r.complete&&r.touched&&s.writes==n&&s.accesses.back().write);
        CHECK(!r.lastReadSucceeded);
    }
    for(unsigned n=1;n<=baseline.valids;++n) {
        Sim s;s.revokeAt=n;
        CHECK(stop(s.io(),4096,1000000,r)==Status::Revoked);
        CHECK(!r.complete&&s.revoked&&r.touched==(s.writes!=0));
    }
    for(unsigned n=1;n<=baseline.clocks;++n)for(bool backwards:{false,true}) {
        Sim s;if(backwards)s.clockBack=n;else s.clockFail=n;
        // First clock defines the origin; its value cannot regress yet.
        if(backwards&&n==1)continue;
        CHECK(stop(s.io(),4096,1000000,r)==Status::ClockFailure);
        CHECK(!r.complete&&s.clocks==n);
    }
    for(uint32_t off:{8U,0xcU,0x20U,0x80U,0x4cU,0x5cU,0x70U}) {
        Sim s;s.sticky=off;
        CHECK(stop(s.io(),4096,1000000,r)==Status::Timeout);
        CHECK(!r.complete&&r.touched&&r.elapsedUs>=100000&&r.elapsedUs<100500);
        if(off!=8)CHECK(s.regs[8]&1); // Never shortcuts a failed stop through reset.
    }
    {Sim s;s.delayFail=true;CHECK(stop(s.io(),4096,1000000,r)==Status::ClockFailure&&!r.complete);}
    {Sim s;s.lateDelay=true;CHECK(stop(s.io(),4096,1000000,r)==Status::Timeout&&!r.complete);}
    {Sim s;s.frozen=true;CHECK(stop(s.io(),4096,1000000,r)==Status::ClockFailure&&!r.complete);}
    {Sim s;s.frozen=true;s.sticky=0x80;
        CHECK(stop(s.io(),4096,1000000,r)==Status::ClockFailure&&!r.complete);CHECK(s.reads<10100);}
    {Sim s;CHECK(stop(s.io(),4096,1,r)==Status::Timeout&&!r.complete&&!r.writes);}
    // Change observed stream inventory or re-enable a verified stream before
    // the final verification. Neither may be converted into reset success.
    unsigned nth=0;
    for(const auto&a:baseline.accesses)if(!a.write){++nth;
        if(a.offset==0&&nth>4){Sim s;s.mutateAt=nth;s.mutateOffset=0;s.mutateValue=0x5001;
            CHECK(stop(s.io(),4096,1000000,r)==Status::StateChanged&&!r.complete);CHECK(s.regs[8]&1);}
        if(a.offset==0x80&&nth>20){Sim s;s.mutateAt=nth;s.mutateOffset=0x80;s.mutateValue=2;
            CHECK(stop(s.io(),4096,1000000,r)==Status::StateChanged&&!r.complete);CHECK(s.regs[8]&1);}
    }
}
void rejection() {
    Result r{};
    for(unsigned defect=0;defect<9;++defect){Sim s;Io io=s.io();
        switch(defect){case 0:io.valid=nullptr;break;case 1:io.read8=nullptr;break;
        case 2:io.read16=nullptr;break;case 3:io.read32=nullptr;break;
        case 4:io.write8=nullptr;break;case 5:io.write16=nullptr;break;
        case 6:io.write32=nullptr;break;case 7:io.nowUs=nullptr;break;case 8:io.delayUs=nullptr;break;}
        CHECK(stop(io,4096,1000000,r)==Status::InvalidArgument&&s.accesses.empty());}
    for(uint32_t timeout:{0U,1000001U,UINT32_MAX}){Sim s;
        CHECK(stop(s.io(),4096,timeout,r)==Status::InvalidArgument&&s.accesses.empty());}
    for(uint64_t bytes:{uint64_t(0),uint64_t(0x7f),uint64_t(0x13f)}){Sim s;
        CHECK(stop(s.io(),bytes,1000000,r)==Status::OutsideBar&&!r.touched);}
    for(uint32_t cap:{0U,0xf0f0U,0xffffU}){Sim s;s.regs[0]=cap;
        CHECK(stop(s.io(),4096,1000000,r)==(cap==0xffff?Status::DeviceUnavailable:Status::UnsupportedCapabilities));
        CHECK(!r.touched&&!r.complete);}
    for(uint32_t off:{2U,3U}){Sim s;s.regs[off]=2;
        CHECK(stop(s.io(),4096,1000000,r)==Status::UnsupportedVersion&&!r.touched);}
    {Sim s;s.regs[8]|=2;CHECK(stop(s.io(),4096,1000000,r)==Status::FlushInProgress&&!r.touched);}
}
}
int main(){golden();faults();rejection();std::cout<<checks<<" audio quiescence checks, "<<failures<<" failures\n";return failures?1:0;}
