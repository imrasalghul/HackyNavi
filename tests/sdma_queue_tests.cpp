// SPDX-License-Identifier: MIT
// Offline protocol/packet emulator only. No real GPU execution or I/O occurs.
#include "hackynavi/sdma_queue.hpp"
#include <iostream>
#include <map>
#include <vector>
using namespace hackynavi;
using namespace hackynavi::sdmaqueue;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)){++failures;std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";} } while(0)
const PollOptions polls={4,2};
constexpr uint64_t kRing=0x900000000ULL,kWb=0x910000000ULL;
constexpr uint64_t kSource=0x920000000ULL,kDest=0x930000000ULL;
uint32_t reg(unsigned engine,uint32_t off) {
    return ((off>=0x5880 ? 0xa000 : 0x1260)+off+engine*(off>=0x5880 ? 0x20 : 0x600))*4;
}
Navi31IpLayout layout() {
    Navi31IpLayout l={};l.valid=true;
    const uint16_t ids[6]={11,34,255,1,42,43};const uint8_t majors[6]={11,3,13,13,6,6};
    for(unsigned i=0;i<6;++i) l.blocks[i]={ids[i],0,0,majors[i],0,0,2,{0x1260,0xa000}};
    return l;
}
Config config(unsigned engine=0) { return {engine,256,1,kRing,kWb,kWb+8,kWb+16}; }
struct Event { char kind;uint32_t address,value; };
struct Fake {
    alignas(8) uint32_t ring[16384];
    alignas(8) uint64_t rptr=0xccccccccccccccccULL,wptr=0xccccccccccccccccULL;
    uint32_t fence=0xcccccccc;
    std::map<uint32_t,uint32_t> regs;
    std::vector<Event> events;
    unsigned engine=0,reads=0,writes=0,syncDevice=0,syncCpu=0;
    unsigned failRead=0,failWrite=0,allOnesRead=0,failSyncDevice=0,failSyncCpu=0;
    uint32_t corruptAddress=UINT32_MAX,corruptMask=0;
    uint64_t publishedBytes=0,pendingBytes=0;
    uint32_t pendingFence=0,ringWords=256;
    unsigned completionAfter=1,completionPolls=0;
    bool execute=false,invalidPacket=false,corruptReadback=false,holdRptr=false,holdFence=false;
    explicit Fake(unsigned e=0):engine(e) {
        for(auto& word:ring) word=0xcccccccc;
        regs[reg(e,0x80)]=0x00040800;regs[reg(e,0x8a)]=0x100;
        regs[reg(e,0x589a)]=0x4401;regs[reg(e,0x25)]=1;
        regs[reg(e,0x3f)]=0x010cec00;regs[reg(e,0x92)]=0x10000000;
        regs[reg(e,0xab)]=0x400;regs[reg(e,0x2e)]=0xaa00;
        regs[reg(e,0x3c)]=0x12340020;
    }
    Memory memory() {return {ring,16384,&rptr,&wptr,&fence};}
    Io io() {return {this,read,write,device,cpu,delay};}
    uint32_t word(uint64_t index) const {return ring[index&(ringWords-1)];}
    void decode(uint64_t next) {
        uint64_t pos=publishedBytes/4,end=next/4;
        while(pos<end) {
            const uint32_t op=word(pos)&255;
            unsigned n=1;
            if(op==1)n=7;else if(op==11)n=5;else if(op==17)n=5;else if(op==4)n=6;
            else if(op==5){n=4;pendingFence=word(pos+3);}
            else if(op!=0)invalidPacket=true;
            if(pos+n>end){invalidPacket=true;break;} pos+=n;
        }
        pendingBytes=next;publishedBytes=next;completionPolls=0;
    }
    static bool read(void* p,uint32_t a,uint32_t& v) {
        auto& f=*static_cast<Fake*>(p);++f.reads;v=f.regs[a];
        if(f.corruptReadback&&f.writes&&a==f.corruptAddress)v^=f.corruptMask;
        if(f.reads==f.allOnesRead)v=UINT32_MAX;
        f.events.push_back({'r',a,v});return f.reads!=f.failRead;
    }
    static bool write(void* p,uint32_t a,uint32_t v) {
        auto& f=*static_cast<Fake*>(p);++f.writes;f.regs[a]=v;f.events.push_back({'w',a,v});
        if(a==reg(f.engine,0x86)&&(f.regs[reg(f.engine,0x80)]&1)) {
            const uint64_t next=uint64_t(v)<<32|f.regs[reg(f.engine,0x85)];
            f.decode(next);
        }
        return f.writes!=f.failWrite; // ambiguous failed write can still land
    }
    static bool device(void* p) {
        auto& f=*static_cast<Fake*>(p);++f.syncDevice;f.events.push_back({'d',0,0});
        return f.syncDevice!=f.failSyncDevice;
    }
    static bool cpu(void* p) {
        auto& f=*static_cast<Fake*>(p);++f.syncCpu;f.events.push_back({'c',0,0});
        if(f.execute&&f.pendingBytes&&++f.completionPolls>=f.completionAfter) {
            if(!f.holdRptr)f.rptr=f.pendingBytes;
            if(!f.holdFence)f.fence=f.pendingFence;
        }
        return f.syncCpu!=f.failSyncCpu;
    }
    static void delay(void* p,uint32_t us) {static_cast<Fake*>(p)->events.push_back({'u',0,us});}
};
void started(Fake& f,State& s) {
    Result r;CHECK(start(layout(),0x80000,config(f.engine),f.memory(),f.io(),s,r)==Status::Ok);
    CHECK(s.phase()==Phase::Running&&s.needsRetention()&&!s.pending());
}
void goldenStartup() {
    for(unsigned e=0;e<2;++e) {
        Fake f(e);State s;Result r;
        CHECK(start(layout(),0x80000,config(e),f.memory(),f.io(),s,r)==Status::Ok);
        CHECK(r.stage==Stage::Complete&&r.retentionRequired&&r.hardwareMayHaveChanged);
        CHECK(f.rptr==0&&f.wptr==0&&f.fence==0&&f.syncDevice==1);
        for(unsigned i=0;i<256;++i)CHECK(f.ring[i]==0);
        CHECK(f.regs[reg(e,0x80)]==0x00841811);
        CHECK(f.regs[reg(e,0x8a)]==0x101&&f.regs[reg(e,0x589a)]==0x4400);
        CHECK(f.regs[reg(e,0x81)]==0x09000000&&f.regs[reg(e,0x82)]==0);
        CHECK(f.regs[reg(e,0x88)]==9&&f.regs[reg(e,0x89)]==0x10000000);
        CHECK(f.regs[reg(e,0xb2)]==9&&f.regs[reg(e,0xb3)]==0x10000008);
        CHECK(f.regs[reg(e,0x3f)]==0x010cec00&&f.regs[reg(e,0x3c)]==0x12340629);
        CHECK(f.regs[reg(e,0x2e)]==0xaa01&&f.regs[reg(e,0xb5)]==0);
        CHECK(!(f.regs[reg(e,0x92)]&0x10000000));
        std::vector<Event> w;size_t firstWrite=f.events.size(),firstSync=f.events.size();
        for(size_t i=0;i<f.events.size();++i) {
            if(f.events[i].kind=='w'){if(firstWrite==f.events.size())firstWrite=i;w.push_back(f.events[i]);}
            if(f.events[i].kind=='d')firstSync=i;
        }
        CHECK(firstSync<firstWrite&&w.size()==24);
        const uint32_t expected[]={0x22,0x80,0x83,0x84,0x85,0x86,0xb3,0xb2,0x88,0x89,
            0x81,0x82,0xb5,0x85,0x86,0x92,0xab,0xb5,0x2e,0x3c,0x3f,0x589a,0x80,0x8a};
        for(unsigned i=0;i<24;++i)CHECK(w[i].address==reg(e,expected[i]));
        CHECK(stop(f.io(),s,polls,r)==Status::Ok);
        CHECK(s.phase()==Phase::Stopped&&!s.needsRetention()&&!r.retentionRequired);
        CHECK(!(f.regs[reg(e,0x80)]&1)&&!(f.regs[reg(e,0x8a)]&1));
        CHECK(f.regs[reg(e,0x589a)]&1);
        CHECK(start(layout(),0x80000,config(e),f.memory(),f.io(),s,r)==Status::InvalidState);
        s.clearAfterVerifiedReset();CHECK(s.phase()==Phase::Empty&&!s.pending());
    }
}
void validation() {
    for(unsigned which=0;which<22;++which) {
        Fake f;State s;Result r;auto c=config();auto m=f.memory();
        if(which==0)c.engine=2;if(which==1)c.ringWords=255;if(which==2)c.ringWords=16385;
        if(which==3)c.ringWords=257;if(which==4)c.watchdog100ms=0;if(which==5)c.watchdog100ms=256;
        if(which==6)++c.ringGpuAddress;if(which==7)++c.rptrGpuAddress;
        if(which==8)++c.wptrGpuAddress;if(which==9)++c.fenceGpuAddress;
        if(which==10)c.ringGpuAddress=0xffffffffff00ULL;
        if(which==11)c.rptrGpuAddress=0x1000000000000ULL;
        if(which==12)c.fenceGpuAddress=c.ringGpuAddress;
        if(which==13)c.wptrGpuAddress=c.rptrGpuAddress;
        if(which==14)m.ring=nullptr;if(which==15)m.rptr=nullptr;
        if(which==16)m.wptr=nullptr;if(which==17)m.fence=nullptr;
        if(which==18)m.capacityWords=255;
        if(which==19)m.fence=reinterpret_cast<volatile uint32_t*>(m.rptr);
        if(which==20)m.rptr=reinterpret_cast<volatile uint64_t*>(m.ring);
        if(which==21)m.fence=reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(m.fence)+1);
        CHECK(start(layout(),0x80000,c,m,f.io(),s,r)==Status::InvalidArgument);
        CHECK(f.events.empty()&&f.ring[0]==0xcccccccc&&!s.needsRetention());
    }
    for(unsigned which=0;which<5;++which) {
        Fake f;State s;Result r;auto io=f.io();
        if(which==0)io.read32=nullptr;if(which==1)io.write32=nullptr;
        if(which==2)io.syncForDeviceAndHdp=nullptr;if(which==3)io.syncForCpu=nullptr;
        if(which==4)io.delayUs=nullptr;
        CHECK(start(layout(),0x80000,config(),f.memory(),io,s,r)==Status::InvalidArgument);
        CHECK(f.events.empty());
    }
    for(unsigned block=0;block<6;++block) {
        Fake f;State s;Result r;auto l=layout();++l.blocks[block].minor;
        CHECK(start(l,0x80000,config(),f.memory(),f.io(),s,r)==Status::UnsupportedLayout);
        CHECK(f.events.empty());
    }
    Fake f;State s;Result r;
    CHECK(start(layout(),0x100,config(),f.memory(),f.io(),s,r)==Status::OutsideBar);
    CHECK(f.events.empty());
    auto l=layout();l.blocks[0].basesDwords[1]=0x1260+0x80-0x589a;
    CHECK(start(l,UINT32_MAX,config(),f.memory(),f.io(),s,r)!=Status::Ok);
    CHECK(f.events.empty());
    l=layout();for(unsigned i=1;i<6;++i)l.blocks[i].baseCount=1;
    CHECK(start(l,0x80000,config(),f.memory(),f.io(),s,r)==Status::Ok);
}
void guards() {
    const Event bad[]={{'s',0x80,1},{'s',0x8a,1},{'s',0x589a,0},{'s',0x25,0},
        {'m',0xb4,1},{'m',0xb4,0x10000},{'m',0xb0,1},{'m',0x2b,0x10},{'m',0x589a,0x201}};
    for(const auto& b:bad) {
        Fake f;State s;Result r;f.regs[reg(0,b.address)]=b.value;
        CHECK(start(layout(),0x80000,config(),f.memory(),f.io(),s,r)==
            (b.kind=='s'?Status::EngineNotStopped:Status::UnsupportedQueueMode));
        CHECK(!f.writes&&!f.syncDevice&&!s.needsRetention());
    }
    for(unsigned e=0;e<2;++e)for(unsigned q=1;q<8;++q)for(unsigned ib=0;ib<2;++ib) {
        Fake f(e);State s;Result r;f.regs[reg(e,0x80+q*0x58+ib*0xa)]=1;
        CHECK(start(layout(),0x80000,config(e),f.memory(),f.io(),s,r)==Status::OtherQueueActive);
        CHECK(!f.writes&&!s.needsRetention());
    }
    for(unsigned e=0;e<2;++e)for(uint32_t bit:{1U,0x10U,0x20U,0x40U}) {
        Fake f(e);State s;Result r;f.regs[reg(e,0x2b)]=bit;
        CHECK(start(layout(),0x80000,config(e),f.memory(),f.io(),s,r)==Status::UnsupportedQueueMode);
        CHECK(!f.writes&&!f.syncDevice&&!s.needsRetention());
    }
}
void startFailures() {
    Fake g;State gs;started(g,gs);
    for(unsigned i=1;i<=g.reads;++i) {
        Fake f;State s;Result r;f.failRead=i;
        CHECK(start(layout(),0x80000,config(),f.memory(),f.io(),s,r)==Status::ReadFailed);
        CHECK(f.reads==i&&s.needsRetention()==bool(f.writes));
        CHECK(r.retentionRequired==s.needsRetention());
    }
    for(unsigned i=1;i<=g.reads;++i) {
        Fake f;State s;Result r;f.allOnesRead=i;
        CHECK(start(layout(),0x80000,config(),f.memory(),f.io(),s,r)==Status::DeviceUnavailable);
        CHECK(f.reads==i&&s.needsRetention()==bool(f.writes));
    }
    for(unsigned i=1;i<=g.writes;++i) {
        Fake f;State s;Result r;f.failWrite=i;
        CHECK(start(layout(),0x80000,config(),f.memory(),f.io(),s,r)==Status::WriteFailed);
        CHECK(f.writes==i&&s.needsRetention()&&s.phase()==Phase::Faulted);
        CHECK(stop(f.io(),s,polls,r)==Status::Ok);
        CHECK(!s.needsRetention());
    }
    const uint32_t controls[]={0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x88,0x89,0xb2,0xb3,
        0xb5,0x2e,0x3c,0x3f,0x589a,0x8a};
    for(auto a:controls) {
        Fake f;State s;Result r;f.corruptReadback=true;f.corruptAddress=reg(0,a);f.corruptMask=1;
        CHECK(start(layout(),0x80000,config(),f.memory(),f.io(),s,r)==Status::ReadbackMismatch);
        CHECK(s.needsRetention()&&s.phase()==Phase::Faulted);
    }
    Fake f;State s;Result r;f.failSyncDevice=1;
    CHECK(start(layout(),0x80000,config(),f.memory(),f.io(),s,r)==Status::SyncFailed);
    CHECK(!s.needsRetention()&&!f.writes);
}
void packetAndWrap() {
    for(unsigned engine=0;engine<2;++engine) {
        Fake f(engine);State s;Result r;started(f,s);f.execute=true;
        for(unsigned iteration=0;iteration<600;++iteration) {
            const uint64_t old=s.producerBytes()/4;const unsigned kind=iteration%3;
            const size_t mark=f.events.size();
            Status result=kind==0?submitCopy(f.io(),s,kSource,kDest,4096,r):
                kind==1?submitFill(f.io(),s,kDest,0x10203040,4096,r):submitIndirect(f.io(),s,kSource,8,r);
            CHECK(result==Status::Ok&&s.pending()&&r.submissionMayHaveOccurred);
            CHECK(s.producerBytes()%64==0&&s.fenceSequence()==iteration+1);
            CHECK(f.wptr==0&&!f.invalidPacket);
            size_t gcr=kind==0?7:kind==1?5:8;
            CHECK(f.word(old+gcr)==17&&f.word(old+gcr+1)==0&&f.word(old+gcr+2)==0xc3a10000);
            CHECK(f.word(old+gcr+3)==0&&f.word(old+gcr+4)==0);
            CHECK(f.word(old+gcr+5)==0x30005&&f.word(old+gcr+6)==0x10000010);
            CHECK(f.word(old+gcr+7)==9&&f.word(old+gcr+8)==iteration+1);
            if(kind==0) {
                const uint32_t expect[]={1,4095,0,0x20000000,9,0x30000000,9};
                for(unsigned i=0;i<7;++i)CHECK(f.word(old+i)==expect[i]);
            } else if(kind==1) {
                const uint32_t expect[]={11,0x30000000,9,0x10203040,4095};
                for(unsigned i=0;i<5;++i)CHECK(f.word(old+i)==expect[i]);
            } else {
                const uint32_t expect[]={0,0,4,0x20000000,9,8,0,0};
                for(unsigned i=0;i<8;++i)CHECK(f.word(old+i)==expect[i]);
            }
            CHECK(f.events[mark].kind=='c'&&f.events[mark+1].kind=='d');
            CHECK(f.events[mark+2].kind=='w'&&f.events[mark+2].address==reg(engine,0x85));
            CHECK(f.events[mark+3].kind=='w'&&f.events[mark+3].address==reg(engine,0x86));
            CHECK(submitFill(f.io(),s,kDest,0,4,r)==Status::PendingSubmission);
            CHECK(waitFence(f.io(),s,polls,r)==Status::Ok&&!s.pending());
            CHECK(r.observedRptrBytes==s.producerBytes()&&f.fence==iteration+1);
        }
        CHECK(stop(f.io(),s,polls,r)==Status::Ok&&!s.needsRetention());
    }
}
void badSubmissions() {
    Fake f;State s;Result r;started(f,s);
    const unsigned writes=f.writes,syncs=f.syncDevice;
    CHECK(submitCopy(f.io(),s,kSource,kDest,0,r)==Status::InvalidArgument);
    CHECK(submitCopy(f.io(),s,kSource,kSource+8,64,r)==Status::InvalidArgument);
    CHECK(submitCopy(f.io(),s,0xffffffffffffULL,kDest,8,r)==Status::InvalidArgument);
    CHECK(submitCopy(f.io(),s,kSource,kDest,0x40000001,r)==Status::InvalidArgument);
    for(uint64_t a:{kRing-4,kRing,kRing+1020,kWb-4,kWb,kWb+8,kWb+16}) {
        CHECK(submitFill(f.io(),s,a,0,8,r)==Status::InvalidArgument);
        CHECK(submitCopy(f.io(),s,a,kDest,8,r)==Status::InvalidArgument);
        CHECK(submitCopy(f.io(),s,kSource,a,8,r)==Status::InvalidArgument);
    }
    CHECK(submitFill(f.io(),s,kDest+1,0,4,r)==Status::InvalidArgument);
    CHECK(submitFill(f.io(),s,kDest,0,5,r)==Status::InvalidArgument);
    for(unsigned n:{0U,7U,9U,65537U,UINT32_MAX})
        CHECK(submitIndirect(f.io(),s,kSource,n,r)==Status::InvalidArgument);
    CHECK(submitIndirect(f.io(),s,kSource+4,8,r)==Status::InvalidArgument);
    CHECK(f.writes==writes&&f.syncDevice==syncs&&s.producerBytes()==0&&!s.pending());
}
void submitAndWaitFailures() {
    for(unsigned fail=0;fail<4;++fail) {
        Fake f;State s;Result r;started(f,s);
        if(fail<2)f.failWrite=f.writes+1+fail;
        if(fail==2)f.failSyncDevice=f.syncDevice+1;
        if(fail==3)f.failSyncCpu=f.syncCpu+1;
        CHECK(submitCopy(f.io(),s,kSource,kDest,64,r)==(fail<2?Status::WriteFailed:Status::SyncFailed));
        CHECK(s.phase()==Phase::Faulted&&s.needsRetention());
        CHECK(s.pending()==(fail<2));
        if(fail>=2)CHECK(!r.submissionMayHaveOccurred);
    }
    for(unsigned held=0;held<3;++held) {
        Fake f;State s;Result r;started(f,s);f.execute=held!=0;f.holdRptr=held==1;f.holdFence=held==2;
        CHECK(submitCopy(f.io(),s,kSource,kDest,64,r)==Status::Ok);
        CHECK(waitFence(f.io(),s,polls,r)==Status::FenceTimeout&&r.polls==4);
        CHECK(s.pending()&&s.needsRetention()&&s.phase()==Phase::Faulted);
        const unsigned writes=f.writes;
        CHECK(stop(f.io(),s,polls,r)==Status::FenceTimeout&&f.writes==writes);
        f.execute=true;f.holdRptr=f.holdFence=false;
        CHECK(stop(f.io(),s,polls,r)==Status::Ok&&!s.needsRetention());
    }
    for(unsigned kind=0;kind<4;++kind) {
        Fake f;State s;Result r;started(f,s);
        CHECK(submitCopy(f.io(),s,kSource,kDest,64,r)==Status::Ok);
        if(kind==0)f.rptr=1;if(kind==1)f.rptr=s.producerBytes()+4;
        if(kind==2)f.fence=s.fenceSequence()+1;if(kind==3)f.failSyncCpu=f.syncCpu+1;
        CHECK(waitFence(f.io(),s,polls,r)==(kind==3?Status::SyncFailed:Status::PointerCorrupt));
        CHECK(s.needsRetention()&&s.phase()==Phase::Faulted);
    }
    Fake f;State s;Result r;started(f,s);f.rptr=4;
    CHECK(submitCopy(f.io(),s,kSource,kDest,64,r)==Status::PointerCorrupt);
    CHECK(s.needsRetention()&&!s.pending());
}
void stopFailures() {
    Fake gold;State gs;Result gr;started(gold,gs);
    const unsigned beforeReads=gold.reads,beforeWrites=gold.writes;
    CHECK(stop(gold.io(),gs,polls,gr)==Status::Ok);
    for(unsigned i=1;i<=gold.reads-beforeReads;++i) {
        Fake f;State s;Result r;started(f,s);f.failRead=f.reads+i;
        CHECK(stop(f.io(),s,polls,r)==Status::ReadFailed);
        CHECK(s.needsRetention()&&s.phase()==Phase::Faulted);
    }
    for(unsigned i=1;i<=gold.writes-beforeWrites;++i) {
        Fake f;State s;Result r;started(f,s);f.failWrite=f.writes+i;
        CHECK(stop(f.io(),s,polls,r)==Status::WriteFailed);
        CHECK(s.needsRetention()&&s.phase()==Phase::Faulted);
    }
    Fake f;State s;Result r;started(f,s);f.regs[reg(0,0x25)]=0;
    CHECK(stop(f.io(),s,polls,r)==Status::IdleTimeout&&s.needsRetention());
    f.regs[reg(0,0x25)]=1;CHECK(stop(f.io(),s,polls,r)==Status::Ok);
    const PollOptions bad[]={{0,1},{1,0},{1000001,1},{1,11},{100001,10}};
    for(auto p:bad) {
        Fake x;State z;Result o;started(x,z);auto count=x.events.size();
        CHECK(stop(x.io(),z,p,o)==Status::InvalidArgument&&x.events.size()==count);
        CHECK(waitFence(x.io(),z,p,o)==Status::InvalidArgument&&x.events.size()==count);
    }
}
} // namespace
int main() {
    goldenStartup();validation();guards();startFailures();packetAndWrap();badSubmissions();
    submitAndWaitFailures();stopFailures();
    std::cout<<checks<<" assertions, "<<failures<<" failures\n";
    return failures?1:0;
}
