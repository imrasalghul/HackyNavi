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
    uint32_t gpuHead=0,target=0,ibWords=0; uint64_t ibAddress=0;
    size_t syncs=0,acquires=0,delays=0;
    bool stall=false,badFence=false,badRptr=false,rejectSync=false,rejectAcquire=false,protocolError=false;
    bool deferExecution=false;
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
                CHECK(count==4 && !(word(gpuHead+3)&0xfff00000));
                ibAddress=uint64_t(word(gpuHead+1))|uint64_t(word(gpuHead+2))<<32; ibWords=word(gpuHead+3);
            } else if(op!=0x10 && op!=0x4a && op!=0x28 && op!=0x69 && op!=0x12)protocolError=true;
            gpuHead+=count;
        }
        if(!stall)wb.rptr=producer+(badRptr?1:0);
    }
    static bool read(void* context,uint32_t reg,uint32_t& value) {
        return gfxinit_test::Device::read(static_cast<Device*>(context),reg,value);
    }
    static bool write(void* context,uint32_t reg,uint32_t value) {
        auto& s=*static_cast<Device*>(context);
        const bool good=gfxinit_test::Device::write(&s,reg,value);
        if(reg==wpReg && value && !s.deferExecution && (s.hw.registers[activeReg]&1) && !(s.hw.registers[meReg]&0x14000000))s.execute(value);
        return good;
    }
    static bool sync(void* context) { auto& s=*static_cast<Device*>(context); ++s.syncs; return !s.rejectSync; }
    static bool acquire(void* context) { auto& s=*static_cast<Device*>(context); ++s.acquires; return !s.rejectAcquire; }
    static void delay(void* context,uint32_t us) { auto& s=*static_cast<Device*>(context); ++s.delays; CHECK(us==1000 || us==1); }
    q::Io io() { return {this,read,write,sync,acquire,delay}; }
    q::Memory memory() { return {words.data(),words.size(),&wb.rptr,&wb.wptr,&wb.fence}; }
    q::Status start(q::Result& r) { return q::start(ips(),0x100000,prepared,power,config,memory(),io(),{3,1},queue,r); }
};
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
} // namespace
int main() { successTests(); failureTests(); startWriteFailures(); publishWriteFailures(); stopWriteFailures();
    std::cout<<checks<<" graphics queue checks; "<<failures<<" failures\n"; return failures?1:0; }
