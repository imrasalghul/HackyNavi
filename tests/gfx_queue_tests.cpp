// SPDX-License-Identifier: MIT
// Offline PM4/MMIO model only; no real rendering or GPU completion is claimed.
#include "gfx_init_fixture.hpp"
#include "hackynavi/gfx_queue.hpp"
using namespace hackynavi;
using namespace gfxfw_test;
namespace q=hackynavi::gfxqueue;
namespace {
constexpr uint32_t wpReg=(0x1260+0x1df4)*4;
constexpr uint32_t activeReg=(0x1260+0x1f40)*4,meReg=(0xa000+0x803)*4;
struct Device : gfxinit_test::Device {
    gfxinit::State prepared; q::State queue;
    std::vector<uint32_t> words;
    struct alignas(8) Wb { uint32_t rptr,pad; uint64_t wptr; uint32_t fence; } wb={};
    q::Config config={2048,0x1230000,0x1240000,0x1240008,0x1240010};
    uint32_t gpuHead=0,target=0,ibWords=0,ibVmid=0; uint64_t ibAddress=0;
    size_t syncs=0,acquires=0,delays=0;
    bool stall=false,badFence=false,badRptr=false,rejectSync=false,rejectAcquire=false,protocolError=false;
    bool deferExecution=false,checkStartLatch=false;
    size_t latchObservations=0;
    Device():words(2048,0) {
        gfxinit::Result r={}; CHECK(gfxinit_test::Device::run(prepared,r)==gfxinit::Status::Ok);
        events.clear(); calls=0; hw.accesses=hw.writes=0;
    }
    uint32_t word(uint32_t at) { return words[at&2047]; }
    void execute(uint32_t producer) {
        if(!syncs || (producer&255))protocolError=true;
        while(gpuHead<producer) {
            const uint32_t h=word(gpuHead);
            if(h==0xffff1000) { ++gpuHead; continue; }
            if((h&0xc0000000)!=0xc0000000) { protocolError=true; break; }
            const uint32_t count=((h>>16)&0x3fff)+2,op=(h>>8)&255;
            if(count>producer-gpuHead) { protocolError=true; break; }
            if(op==0x49) {
                CHECK(count==8 && word(gpuHead+1)==0x06603514 && word(gpuHead+2)==0x20000000);
                const uint64_t address=uint64_t(word(gpuHead+3))|uint64_t(word(gpuHead+4))<<32;
                CHECK(address==config.fenceGpu && !word(gpuHead+6) && !word(gpuHead+7));
                if(!stall)wb.fence=word(gpuHead+5)+(badFence?1:0);
            } else if(op==0x37) {
                CHECK(count==5 && word(gpuHead+1)==0x00100500);
                const uint64_t address=uint64_t(word(gpuHead+2))|uint64_t(word(gpuHead+3))<<32;
                CHECK(address==0x1300000); target=word(gpuHead+4);
            } else if(op==0x3f) {
                CHECK(count==4 && !(word(gpuHead+3)&0xf8f00000));
                ibAddress=uint64_t(word(gpuHead+1))|uint64_t(word(gpuHead+2))<<32;
                ibWords=word(gpuHead+3)&0xfffff; ibVmid=(word(gpuHead+3)>>24)&15;
            } else if(op==0x46) {
                CHECK(count==2 && word(gpuHead+1)==0x407);
            } else if(op==0x42) {
                CHECK(count==2 && word(gpuHead+1)==0);
            } else if(op!=0x10 && op!=0x4a && op!=0x28 && op!=0x69 && op!=0x12)protocolError=true;
            gpuHead+=count;
        }
        if(!stall)wb.rptr=producer+(badRptr?1:0);
    }
    static bool read(void* context,uint32_t reg,uint32_t& value) {
        auto& s=*static_cast<Device*>(context); s.observeStartLatch();
        return gfxinit_test::Device::read(&s,reg,value);
    }
    static bool write(void* context,uint32_t reg,uint32_t value) {
        auto& s=*static_cast<Device*>(context);
        s.observeStartLatch();
        const bool good=gfxinit_test::Device::write(&s,reg,value);
        if(reg==wpReg && value && !s.deferExecution && (s.hw.registers[activeReg]&1) && !(s.hw.registers[meReg]&0x14000000))s.execute(value);
        return good;
    }
    void observeStartLatch() { if(checkStartLatch) { ++latchObservations; CHECK(!queue.neverPublished()); } }
    static bool sync(void* context) { auto& s=*static_cast<Device*>(context); s.observeStartLatch(); ++s.syncs; return !s.rejectSync; }
    static bool acquire(void* context) { auto& s=*static_cast<Device*>(context); s.observeStartLatch(); ++s.acquires; return !s.rejectAcquire; }
    static void delay(void* context,uint32_t us) { auto& s=*static_cast<Device*>(context); s.observeStartLatch(); ++s.delays; CHECK(us==1000 || us==1); }
    q::Io io() { return {this,read,write,sync,acquire,delay}; }
    q::Memory memory() { return {words.data(),words.size(),&wb.rptr,&wb.wptr,&wb.fence}; }
    q::Status start(q::Result& r) { return q::start(ips(),0x100000,prepared,power,config,memory(),io(),{3,1},queue,r); }
};
void neverPublishedTests() {
    Device baseline; q::Result r{};
    CHECK(baseline.queue.neverPublished() && baseline.prepared.prepared() &&
        baseline.prepared.needsClearStateRetention());
    const auto initial=baseline.calls;
    CHECK(q::checkIdleCompletion(baseline.io(),baseline.queue,{3,1},r)==q::Status::InvalidState);
    CHECK(q::stop(baseline.io(),baseline.queue,{3,1},r)==q::Status::InvalidState);
    CHECK(baseline.calls==initial && baseline.queue.neverPublished());
    baseline.checkStartLatch=true;
    CHECK(baseline.start(r)==q::Status::Ok && !baseline.queue.neverPublished());
    CHECK(baseline.latchObservations>baseline.calls);
    const auto startCalls=baseline.calls;
    CHECK(q::submitWrite(baseline.io(),baseline.queue,0x1300000,7,r)==q::Status::Ok);
    CHECK(!baseline.queue.neverPublished() && baseline.queue.pending());
    CHECK(q::waitFence(baseline.io(),baseline.queue,{3,1},r)==q::Status::Ok);
    CHECK(q::stop(baseline.io(),baseline.queue,{3,1},r)==q::Status::Ok);
    CHECK(!baseline.queue.neverPublished() && !baseline.queue.needsRetention());
    // No attempted read/write, even before the first queue publication,
    // leaves this state eligible for the never-started native cleanup path.
    for(size_t at=1;at<=startCalls;++at) {
        Device d; d.checkStartLatch=true; d.failAt=at;
        CHECK(d.queue.neverPublished());
        CHECK(d.start(r)!=q::Status::Ok && d.calls==at);
        CHECK(!d.queue.neverPublished() && d.queue.phase()==q::Phase::Faulted);
        const auto calls=d.calls,observations=d.latchObservations;
        CHECK(d.start(r)==q::Status::InvalidState);
        CHECK(d.calls==calls && d.latchObservations==observations && !d.queue.neverPublished());
    }
    for(unsigned mode=0;mode<5;++mode) {
        Device d; auto c=d.config; auto m=d.memory(); auto p=q::PollOptions{3,1};
        uint64_t bytes=0x100000;
        if(mode==0)c.ringWords=0; if(mode==1)m.capacityWords=0;
        if(mode==2)p.maxPolls=0; if(mode==3)bytes=4; if(mode==4)c.fenceGpu=c.rptrGpu;
        d.checkStartLatch=true;
        CHECK(q::start(ips(),bytes,d.prepared,d.power,c,m,d.io(),p,d.queue,r)!=q::Status::Ok);
        CHECK(d.queue.neverPublished() && !d.calls && !d.latchObservations && !d.syncs);
    }
    for(unsigned mode=0;mode<2;++mode) {
        Device d; d.checkStartLatch=true;
        if(mode)d.rejectAcquire=true;else d.rejectSync=true;
        CHECK(d.start(r)==q::Status::SyncFailed && !d.queue.neverPublished());
    }
}
// The actual portable CSIB protocol can retire an initialized RLC owner
// without creating or submitting a queue. This is an offline MMIO model;
// native empty-VM/ownership gates still require separate source review.
struct UnstartedRetirementDevice : Device {
    static constexpr uint32_t rlc=(0xa000+0x4c00)*4,safe=(0xa000+0x980)*4,
        fifo=(0xa000+0x4c88)*4,length=(0xa000+0x989)*4;
    unsigned safeCommands=0;
    UnstartedRetirementDevice() { hw.registers[rlc]=1;hw.registers[fifo]=1; }
    static bool retireRead(void* context,uint32_t reg,uint32_t& value) {
        auto& d=*static_cast<UnstartedRetirementDevice*>(context);
        CHECK(d.queue.neverPublished());return gfxinit_test::Device::read(&d,reg,value);
    }
    static bool retireWrite(void* context,uint32_t reg,uint32_t value) {
        auto& d=*static_cast<UnstartedRetirementDevice*>(context);
        CHECK(d.queue.neverPublished());
        if(reg==length && !value)CHECK(!(d.hw.registers[rlc]&1) && !(d.hw.registers[(0xa000+0x4c80)*4]&3));
        const auto good=gfxinit_test::Device::write(&d,reg,value);
        if(reg==safe && value==3) { ++d.safeCommands;d.hw.registers[safe]=2; }
        return good;
    }
    gfxinit::Status retire(gfxinit::Result& r) {
        return gfxinit::retireClearState(ips(),0x100000,power,hw.allocator,
            {this,retireRead,retireWrite,delay},{3,1},prepared,r);
    }
};
constexpr uint32_t UnstartedRetirementDevice::rlc;
constexpr uint32_t UnstartedRetirementDevice::safe;
constexpr uint32_t UnstartedRetirementDevice::fifo;
constexpr uint32_t UnstartedRetirementDevice::length;
void unstartedRetirementTests() {
    UnstartedRetirementDevice baseline; gfxinit::Result r{};
    CHECK(baseline.retire(r)==gfxinit::Status::Ok && !baseline.prepared.needsClearStateRetention());
    CHECK(baseline.safeCommands==1 && !baseline.hw.registers[UnstartedRetirementDevice::length]);
    CHECK(baseline.queue.neverPublished() && baseline.queue.phase()==q::Phase::Fresh &&
        !baseline.queue.producerWords() && !baseline.queue.pending() && !baseline.syncs && !baseline.acquires);
    const auto calls=baseline.calls;
    CHECK(baseline.retire(r)==gfxinit::Status::InvalidState && baseline.calls==calls);
    for(size_t at=1;at<=calls;++at) {
        UnstartedRetirementDevice d;d.failAt=at;
        CHECK(d.retire(r)!=gfxinit::Status::Ok && d.calls==at);
        CHECK(d.prepared.needsClearStateRetention() && !d.prepared.prepared());
        CHECK(d.queue.neverPublished() && !d.queue.producerWords() && !d.syncs && !d.acquires);
        CHECK(d.retire(r)==gfxinit::Status::InvalidState && d.calls==at);
    }
}
void successTests() {
    Device d; q::Result r={};
    CHECK(d.start(r)==q::Status::Ok && d.queue.phase()==q::Phase::Running && d.queue.needsRetention());
    CHECK(d.wb.rptr==1024 && d.wb.fence==1 && d.queue.producerWords()==1024 && !d.queue.pending() && !d.protocolError);
    CHECK(d.wb.wptr==0 && d.syncs==2 && d.hw.registers[(0x1260+0x1de1)*4]==0x2000080a);
    CHECK(d.hw.registers[(0x1260+0x1de0)*4]==0x12300 && d.hw.registers[(0x1260+0x1e4e)*4]==7);
    CHECK((d.hw.registers[(0xa000+0x2904)*4]&0x40000000)!=0);
    // Multiple wraps of the actual CPU ring; GPU read pointer/fence must
    // advance, and the destination is written only by the packet decoder.
    for(unsigned i=0;i<40;++i) {
        CHECK(q::submitWrite(d.io(),d.queue,0x1300000,0xa5000000+i,r)==q::Status::Ok);
        CHECK(d.queue.pending() && d.target==0xa5000000+i);
        CHECK(q::submitIndirect(d.io(),d.queue,0x1400000,8,r)==q::Status::PendingSubmission);
        CHECK(q::waitFence(d.io(),d.queue,{3,1},r)==q::Status::Ok && r.fenceValue==i+2);
    }
    CHECK(q::submitIndirect(d.io(),d.queue,0x1400000,127,r)==q::Status::Ok);
    CHECK(d.ibAddress==0x1400000 && d.ibWords==127);
    CHECK(q::waitFence(d.io(),d.queue,{3,1},r)==q::Status::Ok && !d.protocolError);
    CHECK(q::stop(d.io(),d.queue,{3,1},r)==q::Status::Ok && !d.queue.needsRetention() && d.queue.phase()==q::Phase::Stopped);
    CHECK((d.hw.registers[meReg]&0x14000000)==0x14000000 && d.hw.registers[activeReg]==0);
    CHECK(d.prepared.needsClearStateRetention());
}
void capacityTests() {
    Device d; q::Result r={};
    CHECK(d.queue.submissionCapacity()==q::Status::InvalidState);
    CHECK(d.start(r)==q::Status::Ok);
    const auto words=d.words; const auto producer=d.queue.producerWords();
    const auto reads=d.calls, syncs=d.syncs, acquires=d.acquires;
    CHECK(d.queue.submissionCapacity()==q::Status::Ok);
    CHECK(d.words==words && d.queue.producerWords()==producer && d.calls==reads &&
        d.syncs==syncs && d.acquires==acquires && d.queue.needsRetention());
    CHECK(q::submitIsolatedIndirect(d.io(),d.queue,3,0x1400000,127,r)==q::Status::Ok);
    CHECK(d.queue.submissionCapacity()==q::Status::PendingSubmission);
    CHECK(q::waitFence(d.io(),d.queue,{3,1},r)==q::Status::Ok);
    CHECK(d.queue.submissionCapacity()==q::Status::Ok);
    // A completed fault still denies fresh publication until terminal stop.
    d.rejectAcquire=true;
    CHECK(q::checkIdleCompletion(d.io(),d.queue,{3,1},r)==q::Status::SyncFailed);
    CHECK(d.queue.submissionCapacity()==q::Status::InvalidState);
    d.rejectAcquire=false;
    CHECK(q::stop(d.io(),d.queue,{3,1},r)==q::Status::Ok);
    CHECK(d.queue.submissionCapacity()==q::Status::InvalidState);
    CHECK(!d.protocolError);
}
void failureTests() {
    for(unsigned mode=0;mode<7;++mode) {
        Device d; q::Result r={};
        if(mode==0)d.stall=true; if(mode==1)d.badFence=true; if(mode==2)d.badRptr=true;
        if(mode==3)d.rejectSync=true; if(mode==4)d.rejectAcquire=true;
        if(mode==5)d.hw.registers[(0x1260+0x1f41)*4]=1; if(mode==6)d.failAt=1;
        CHECK(d.start(r)!=q::Status::Ok && d.queue.phase()==q::Phase::Faulted && !d.protocolError);
        if(mode<=2 || mode==4)CHECK(d.queue.needsRetention() && d.queue.pending());
        if(mode>=5)CHECK(!d.queue.needsRetention());
    }
    {
        Device d; q::Result r={}; CHECK(d.start(r)==q::Status::Ok);
        const uint64_t addresses[]={d.config.ringGpu,d.config.rptrGpu,d.config.wptrGpu,d.config.fenceGpu,
            (UINT64_C(1)<<48)-2,UINT64_MAX,0x1300001};
        for(auto address:addresses)CHECK(q::submitWrite(d.io(),d.queue,address,7,r)==q::Status::InvalidArgument);
        CHECK(q::submitIndirect(d.io(),d.queue,0x1400000,0,r)==q::Status::InvalidArgument);
        CHECK(q::submitIndirect(d.io(),d.queue,0x1400000,0x100000,r)==q::Status::InvalidArgument);
        d.stall=true; CHECK(q::submitWrite(d.io(),d.queue,0x1300000,5,r)==q::Status::Ok);
        CHECK(q::waitFence(d.io(),d.queue,{3,1},r)==q::Status::FenceTimeout);
        CHECK(q::submitWrite(d.io(),d.queue,0x1300000,6,r)==q::Status::PendingSubmission);
        d.wb.rptr=d.queue.producerWords(); d.wb.fence=2;
        CHECK(q::waitFence(d.io(),d.queue,{3,1},r)==q::Status::Ok && d.queue.phase()==q::Phase::Faulted);
        CHECK(q::submitWrite(d.io(),d.queue,0x1300000,6,r)==q::Status::InvalidState);
        CHECK(q::stop(d.io(),d.queue,{3,1},r)==q::Status::Ok && !d.queue.needsRetention());
    }
    for(unsigned mode=0;mode<5;++mode) {
        Device d; q::Result r={}; auto c=d.config; auto m=d.memory();
        if(mode==0)c.ringWords=1024; if(mode==1)c.ringGpu|=4; if(mode==2)c.fenceGpu=c.ringGpu;
        if(mode==3)m.fence=m.rptr; if(mode==4)m.capacityWords=10;
        CHECK(q::start(ips(),0x100000,d.prepared,d.power,c,m,d.io(),{3,1},d.queue,r)==q::Status::InvalidArgument);
        CHECK(!d.calls && !d.syncs && d.queue.phase()==q::Phase::Fresh);
    }
}
// The inherited MMIO fixture applies writes before returning false at failAt.
// Exercise every write boundary, including ambiguous publication and halt, so
// an I/O error cannot free a live binding or silently reopen a faulted queue.
std::vector<size_t> writeCalls(const Device& d) {
    std::vector<size_t> result;
    for(size_t i=0;i<d.events.size();++i)if(d.events[i].write)result.push_back(i+1);
    return result;
}
void startWriteFailures() {
    Device baseline; q::Result r={}; CHECK(baseline.start(r)==q::Status::Ok);
    const auto writes=writeCalls(baseline); CHECK(writes.size()>=20);
    for(auto at:writes) {
        Device d; d.failAt=at;
        CHECK(d.start(r)==q::Status::WriteFailed);
        CHECK(d.queue.phase()==q::Phase::Faulted && d.queue.needsRetention() && r.retentionRequired);
        CHECK(d.calls==at && d.events.size()==at && d.events.back().write && !d.protocolError);
        const auto failed=d.events.back(); CHECK(d.hw.registers[failed.reg]==failed.value);
        d.failAt=0;
        const size_t calls=d.calls;
        const bool pending=d.queue.pending();
        CHECK(q::submitWrite(d.io(),d.queue,0x1300000,7,r)==
            (pending?q::Status::PendingSubmission:q::Status::InvalidState));
        CHECK(d.calls==calls && d.queue.needsRetention());
        if(pending) {
            CHECK(q::waitFence(d.io(),d.queue,{3,1},r)==q::Status::Ok);
            CHECK(d.queue.phase()==q::Phase::Faulted && !d.queue.pending() && d.queue.needsRetention());
            CHECK(q::submitWrite(d.io(),d.queue,0x1300000,7,r)==q::Status::InvalidState);
        }
        CHECK(q::stop(d.io(),d.queue,{3,1},r)==q::Status::Ok);
        CHECK(!d.queue.needsRetention() && d.queue.phase()==q::Phase::Stopped && !d.protocolError);
    }
}
q::Status submitTest(Device& d,bool ib,q::Result& r) {
    return ib?q::submitIndirect(d.io(),d.queue,0x1400000,127,r):
        q::submitWrite(d.io(),d.queue,0x1300000,0x7139a54c,r);
}
void isolatedTests() {
    Device d; q::Result r={}; CHECK(d.start(r)==q::Status::Ok);
    const auto noChange=[&](uint8_t vmid,uint64_t address,uint32_t count) {
        const size_t calls=d.calls,syncs=d.syncs,acquires=d.acquires;
        const uint32_t producer=d.queue.producerWords(); const auto words=d.words;
        CHECK(q::submitIsolatedIndirect(d.io(),d.queue,vmid,address,count,r)==q::Status::InvalidArgument);
        CHECK(d.calls==calls && d.syncs==syncs && d.acquires==acquires && d.words==words);
        CHECK(d.queue.producerWords()==producer && !d.queue.pending() && r.retentionRequired);
    };
    for(unsigned vmid=0;vmid<256;++vmid)if(vmid<1 || vmid>7)noChange(uint8_t(vmid),0x1400000,8);
    for(uint64_t address:{UINT64_C(0),UINT64_C(4092),UINT64_C(4097),(UINT64_C(1)<<47)-4,
                         UINT64_C(1)<<47,UINT64_MAX})noChange(1,address,8);
    noChange(1,0x1400000,0); noChange(1,0x1400000,0x100000);
    for(uint8_t vmid=1;vmid<=7;++vmid) {
        const uint32_t at=d.queue.producerWords();
        // Equal numbers in separate VM address spaces do NOT alias physical
        // ring storage. Native ownership/mapping validation occurs upstream.
        const uint64_t va=vmid==1?d.config.ringGpu:((UINT64_C(1)<<47)-4096);
        CHECK(q::submitIsolatedIndirect(d.io(),d.queue,vmid,va,127,r)==q::Status::Ok);
        CHECK(d.ibAddress==va && d.ibWords==127 && d.ibVmid==vmid && d.queue.pending());
        CHECK(d.word(at)==0xc0023f00 && d.word(at+1)==uint32_t(va) && d.word(at+2)==uint32_t(va>>32));
        CHECK(d.word(at+3)==(uint32_t(vmid)<<24|127));
        CHECK(d.word(at+4)==0xc0004600 && d.word(at+5)==0x407);
        CHECK(d.word(at+6)==0xc0004200 && d.word(at+7)==0);
        CHECK(d.word(at+8)==0xc0064900 && d.word(at+9)==0x06603514);
        CHECK(q::waitFence(d.io(),d.queue,{3,1},r)==q::Status::Ok && !d.protocolError);
    }
    // An ambiguous WPTR write can already have dispatched the context's IB.
    // Fault retention and the actual fence+RPTR wait remain mandatory.
    for(unsigned deferred=0;deferred<2;++deferred) {
        Device f; CHECK(f.start(r)==q::Status::Ok); f.calls=0; f.events.clear(); f.deferExecution=deferred!=0;
        Device baseline; CHECK(baseline.start(r)==q::Status::Ok); baseline.calls=0; baseline.events.clear();
        CHECK(q::submitIsolatedIndirect(baseline.io(),baseline.queue,7,0x1400000,127,r)==q::Status::Ok);
        const auto writes=writeCalls(baseline); CHECK(writes.size()==2); f.failAt=writes[0];
        CHECK(q::submitIsolatedIndirect(f.io(),f.queue,7,0x1400000,127,r)==q::Status::WriteFailed);
        CHECK(f.queue.pending() && f.queue.needsRetention() && f.queue.phase()==q::Phase::Faulted);
        CHECK(r.submissionMayHaveOccurred && r.retentionRequired);
        f.failAt=0;
        if(deferred) {
            CHECK(q::waitFence(f.io(),f.queue,{3,1},r)==q::Status::FenceTimeout);
            CHECK(f.queue.pending() && f.wb.fence==1);
            f.execute(f.hw.registers[wpReg]); // fixture decode, never production completion
        }
        CHECK(q::waitFence(f.io(),f.queue,{3,1},r)==q::Status::Ok && r.fenceValue==2);
        CHECK(f.queue.phase()==q::Phase::Faulted && f.queue.needsRetention() && !f.protocolError);
    }
}
void publishWriteFailures() {
    for(unsigned ib=0;ib<2;++ib) {
        Device baseline; q::Result r={}; CHECK(baseline.start(r)==q::Status::Ok);
        baseline.calls=0; baseline.events.clear();
        CHECK(submitTest(baseline,ib!=0,r)==q::Status::Ok);
        const auto writes=writeCalls(baseline); CHECK(writes.size()==2);
        for(auto at:writes)for(unsigned deferred=0;deferred<2;++deferred) {
            Device d; CHECK(d.start(r)==q::Status::Ok);
            d.calls=0; d.events.clear(); d.failAt=at; d.deferExecution=deferred!=0;
            CHECK(submitTest(d,ib!=0,r)==q::Status::WriteFailed);
            CHECK(d.queue.phase()==q::Phase::Faulted && d.queue.pending() && d.queue.needsRetention());
            CHECK(r.submissionMayHaveOccurred && r.retentionRequired && d.calls==at);
            CHECK(d.hw.registers[wpReg]==d.queue.producerWords() && !d.protocolError);
            d.failAt=0;
            const size_t calls=d.calls;
            CHECK(submitTest(d,ib!=0,r)==q::Status::PendingSubmission && d.calls==calls);
            if(deferred) {
                CHECK(d.wb.fence==1 && d.wb.rptr==1024);
                CHECK(q::waitFence(d.io(),d.queue,{3,1},r)==q::Status::FenceTimeout);
                CHECK(d.queue.pending() && d.queue.needsRetention());
                // Only the PM4 decoder supplies the late GPU completion.
                d.deferExecution=false; d.execute(d.hw.registers[wpReg]);
            }
            CHECK(q::waitFence(d.io(),d.queue,{3,1},r)==q::Status::Ok && r.fenceValue==2);
            CHECK(d.queue.phase()==q::Phase::Faulted && !d.queue.pending() && d.queue.needsRetention());
            if(ib)CHECK(d.ibAddress==0x1400000 && d.ibWords==127);
            else CHECK(d.target==0x7139a54c);
            CHECK(submitTest(d,ib!=0,r)==q::Status::InvalidState);
            CHECK(q::stop(d.io(),d.queue,{3,1},r)==q::Status::Ok && !d.queue.needsRetention());
            CHECK(!d.protocolError);
        }
    }
}
void stopWriteFailures() {
    Device baseline; q::Result r={}; CHECK(baseline.start(r)==q::Status::Ok);
    baseline.calls=0; baseline.events.clear();
    CHECK(q::stop(baseline.io(),baseline.queue,{3,1},r)==q::Status::Ok);
    const auto writes=writeCalls(baseline); CHECK(writes.size()==4);
    for(auto at:writes) {
        Device d; CHECK(d.start(r)==q::Status::Ok);
        d.calls=0; d.events.clear(); d.failAt=at;
        CHECK(q::stop(d.io(),d.queue,{3,1},r)==q::Status::WriteFailed);
        CHECK(d.queue.phase()==q::Phase::Faulted && d.queue.needsRetention() && r.retentionRequired);
        CHECK(d.calls==at && d.events.back().write);
        const auto failed=d.events.back(); CHECK(d.hw.registers[failed.reg]==failed.value);
        const size_t calls=d.calls;
        CHECK(submitTest(d,false,r)==q::Status::InvalidState && d.calls==calls);
        d.failAt=0;
        CHECK(q::stop(d.io(),d.queue,{3,1},r)==q::Status::Ok);
        CHECK(d.queue.phase()==q::Phase::Stopped && !d.queue.needsRetention() && !d.protocolError);
        CHECK(d.prepared.needsClearStateRetention());
    }
}
void idleCompletionTests() {
    for(unsigned stopped=0;stopped<2;++stopped) {
        Device d; q::Result r={}; CHECK(d.start(r)==q::Status::Ok);
        CHECK(q::submitIsolatedIndirect(d.io(),d.queue,3,0x1400000,127,r)==q::Status::Ok);
        const size_t before=d.calls;
        CHECK(q::checkIdleCompletion(d.io(),d.queue,{3,1},r)==q::Status::InvalidState && d.calls==before);
        CHECK(q::waitFence(d.io(),d.queue,{3,1},r)==q::Status::Ok);
        if(stopped)CHECK(q::stop(d.io(),d.queue,{3,1},r)==q::Status::Ok);
        d.events.clear(); d.calls=0;
        const auto words=d.words; const auto producer=d.queue.producerWords();
        CHECK(q::checkIdleCompletion(d.io(),d.queue,{3,1},r)==q::Status::Ok);
        CHECK(r.fenceValue==2 && r.observedRptr==producer && r.producerWords==producer);
        CHECK(r.stage==q::Stage::Complete && r.polls==1 && !r.writes && !r.submissionMayHaveOccurred);
        CHECK(d.words==words && d.queue.producerWords()==producer &&
            d.queue.phase()==(stopped?q::Phase::Stopped:q::Phase::Running));
        const size_t reads=d.calls;
        CHECK(reads>=16 && !d.protocolError);
        // A failed read at ANY real control/idle observation denies the proof.
        for(size_t at=1;at<=reads;++at) {
            Device f; CHECK(f.start(r)==q::Status::Ok);
            if(stopped)CHECK(q::stop(f.io(),f.queue,{3,1},r)==q::Status::Ok);
            f.calls=0; f.events.clear(); f.failAt=at;
            CHECK(q::checkIdleCompletion(f.io(),f.queue,{3,1},r)==q::Status::ReadFailed);
            CHECK(f.queue.phase()==q::Phase::Faulted && f.calls==at && !r.writes);
        }
        // Counter equality is mandatory even if the CP is idle; neither a
        // previous fence nor an early/late ring pointer retires the final job.
        for(unsigned corrupt=0;corrupt<4;++corrupt) {
            Device f; CHECK(f.start(r)==q::Status::Ok);
            if(stopped)CHECK(q::stop(f.io(),f.queue,{3,1},r)==q::Status::Ok);
            if(corrupt==0)--f.wb.fence; if(corrupt==1)++f.wb.fence;
            if(corrupt==2)--f.wb.rptr; if(corrupt==3)++f.wb.rptr;
            CHECK(q::checkIdleCompletion(f.io(),f.queue,{3,1},r)==q::Status::PointerCorrupt);
            CHECK(f.queue.phase()==q::Phase::Faulted && !r.writes);
        }
    }
    // Hardware-control drift must be detected independently of a valid fence.
    const uint32_t registers[]={(0xa000+0x900)*4,(0xa000+0x803)*4,(0xa000+0x2904)*4,
        (0xa000+0x2807)*4,(0x1260+0x1f40)*4,(0x1260+0x1f41)*4,(0x1260+0x1df1)*4,
        (0x1260+0x1e8d)*4,wpReg,(0x1260+0x1df5)*4,(0x1260+0x1de0)*4,
        (0x1260+0x1e51)*4,(0x1260+0x1de3)*4,(0x1260+0x1de4)*4,
        (0x1260+0x1e8b)*4,(0x1260+0x1e8c)*4};
    const uint32_t flips[]={1,0x10000000,0x40000000,0x40000000,1,1,1,0x40000000,
        256,1,1,1,4,1,8,1};
    for(unsigned i=0;i<sizeof(registers)/sizeof(registers[0]);++i) {
        Device f; q::Result r={}; CHECK(f.start(r)==q::Status::Ok);
        f.hw.registers[registers[i]]^=flips[i];
        CHECK(q::checkIdleCompletion(f.io(),f.queue,{3,1},r)!=q::Status::Ok);
        CHECK(f.queue.phase()==q::Phase::Faulted && !r.writes);
    }
    {
        Device f; q::Result r={}; CHECK(f.start(r)==q::Status::Ok);
        f.hw.busyCp=true;
        CHECK(q::checkIdleCompletion(f.io(),f.queue,{3,1},r)==q::Status::IdleTimeout && r.polls==3);
        CHECK(f.queue.phase()==q::Phase::Faulted && !r.writes);
    }
    {
        Device f; q::Result r={}; CHECK(f.start(r)==q::Status::Ok); f.rejectAcquire=true;
        CHECK(q::checkIdleCompletion(f.io(),f.queue,{3,1},r)==q::Status::SyncFailed);
        CHECK(f.queue.phase()==q::Phase::Faulted && !r.writes);
    }
}
} // namespace
int main() { neverPublishedTests(); unstartedRetirementTests(); successTests(); capacityTests(); failureTests(); startWriteFailures(); isolatedTests(); publishWriteFailures(); stopWriteFailures(); idleCompletionTests();
    std::cout<<checks<<" graphics queue checks; "<<failures<<" failures\n"; return failures?1:0; }
