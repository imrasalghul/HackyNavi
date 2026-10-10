// SPDX-License-Identifier: MIT
// Offline register/VRAM emulator tests; no actual GPU execution is asserted.
#include "psp_ring_fixture.hpp"
#include <iostream>
#include <type_traits>
using namespace pspring_test;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; std::cerr<<"FAIL "<<__LINE__<<": " #x "\n"; } } while(0)
static_assert(!std::is_copy_constructible<ring::State>::value,"state cannot copy");
static_assert(!std::is_move_constructible<ring::State>::value,"state cannot move");
static_assert(!std::is_copy_assignable<ring::State>::value,"state cannot assign");
void setupTmr(Fixture& s,const psp::State& sos) {
    CHECK(s.valid && s.create(sos)==ring::Status::Ok);
    CHECK(ring::submitLoadToc(s.io(),s.state,s.input.handle,0,64,s.result)==ring::Status::Ok);
    CHECK(s.state.pending());
    CHECK(ring::wait(s.io(),s.state,polls,s.result)==ring::Status::Ok);
    CHECK(s.result.tmrBytes==0x400000 && s.state.requiredTmrBytes()==0x400000);
    CHECK(ring::submitSetupTmr(s.io(),s.state,s.tmr.handle,s.result)==ring::Status::Ok);
    CHECK(!s.state.tmrReady() && s.state.needsTmrRetention());
    CHECK(ring::wait(s.io(),s.state,polls,s.result)==ring::Status::Ok);
    CHECK(s.state.tmrReady());
}
void layouts() {
    ring::Layout l={};
    CHECK(ring::buildLayout(ips(),0x80000,l)==ring::Status::Ok);
    CHECK(l.control==reg(0x80) && l.wptr==reg(0x83) && l.rptr==reg(0x84));
    CHECK(l.ringLo==reg(0x85) && l.ringHi==reg(0x86) && l.ringSize==reg(0x87));
    CHECK(l.heartbeat==reg(0x91));
    CHECK(ring::buildLayout(ips(),reg(0x91)+3,l)==ring::Status::OutsideBar && !l.valid);
    CHECK(ring::buildLayout(ips(),reg(0x91)+4,l)==ring::Status::Ok);
    for(unsigned block=0;block<6;++block) for(unsigned field=0;field<8;++field) {
        auto ip=ips(); auto& b=ip.blocks[block];
        if(field==0) ++b.hardwareId; if(field==1) ++b.major; if(field==2) ++b.minor;
        if(field==3) ++b.revision; if(field==4) ++b.instance; if(field==5) ++b.dieId;
        if(field==6) b.baseCount=0; if(field==7) b.baseCount=9;
        CHECK(ring::buildLayout(ip,0x80000,l)==ring::Status::UnsupportedLayout && !l.valid);
    }
}
void protocol(const psp::State& sos) {
    Fixture s; setupTmr(s,sos);
    CHECK(!s.forbidden && s.delays==1 && s.flushes==3);
    const auto& toc=s.commands[0]; const auto& tmr=s.commands[1];
    CHECK(get(toc.data()+8)==0x20 && get(toc.data()+36)==64);
    CHECK(get(toc.data()+28)==uint32_t(s.mcBase+s.input.range.offset));
    CHECK(get(tmr.data()+8)==5 && get(tmr.data()+36)==0x400000 && get(tmr.data()+40)==2);
    CHECK(get(tmr.data()+28)==uint32_t(s.mcBase+s.tmr.range.offset));
    CHECK(get(tmr.data()+32)==uint32_t((s.mcBase+s.tmr.range.offset)>>32));
    CHECK(get(tmr.data()+44)==uint32_t(s.physicalBase+s.tmr.range.offset));
    CHECK(get(tmr.data()+48)==uint32_t((s.physicalBase+s.tmr.range.offset)>>32));
    CHECK(ring::submitBootConfig(s.io(),s.state,s.result)==ring::Status::Ok);
    CHECK(ring::wait(s.io(),s.state,polls,s.result)==ring::Status::Ok && s.result.bootConfig==0x1234);
    CHECK(get(s.commands.back().data()+28)==2);
    for(unsigned i=0;i<68;++i) {
        const uint32_t type=i%2 ? 71 : 87;
        CHECK(ring::validateInputAllocation(s.state,s.input.handle,4096,128)==ring::Status::Ok);
        CHECK(ring::submitLoadIp(s.io(),s.state,s.input.handle,4096,128,type,s.result)==ring::Status::Ok);
        CHECK(s.result.submissionMayHaveOccurred && s.state.pending());
        CHECK(ring::submitBootConfig(s.io(),s.state,s.result)==ring::Status::PendingCommand);
        CHECK(ring::stop(s.io(),s.state,polls,s.result)==ring::Status::PendingCommand);
        CHECK(ring::wait(s.io(),s.state,polls,s.result)==ring::Status::Ok);
        CHECK(s.result.firmwareAddress==s.firmwareAddress && !s.state.pending());
        const auto& cmd=s.commands.back(); const auto& frame=s.frames.back();
        CHECK(get(cmd.data()+8)==6 && get(cmd.data()+36)==128 && get(cmd.data()+40)==type);
        CHECK(get(cmd.data()+28)==uint32_t(s.mcBase+s.input.range.offset+4096));
        for(size_t j=0;j<cmd.size();++j) if(j<8 || (j>=12 && j<28) || j>=44) CHECK(cmd[j]==0);
        CHECK(get(frame.data())==uint32_t(s.mcBase+s.command.range.offset));
        CHECK(get(frame.data()+4)==uint32_t((s.mcBase+s.command.range.offset)>>32));
        CHECK(get(frame.data()+12)==uint32_t(s.mcBase+s.fence.range.offset));
        CHECK(get(frame.data()+16)==uint32_t((s.mcBase+s.fence.range.offset)>>32));
        CHECK(get(frame.data()+20)==i+4);
        for(size_t j=0;j<frame.size();++j) if((j>=8 && j<12) || j>=24) CHECK(frame[j]==0);
    }
    CHECK(s.wp==s.rp && s.wp==(s.submitCount*16)%1024 && !s.forbidden);
    CHECK(ring::submitAutoloadRlc(s.io(),s.state,s.result)==ring::Status::Ok);
    CHECK(ring::wait(s.io(),s.state,polls,s.result)==ring::Status::Ok);
    CHECK(get(s.commands.back().data()+8)==0x21);
    CHECK(ring::submitDestroyTmr(s.io(),s.state,s.result)==ring::Status::Ok);
    CHECK(s.state.needsTmrRetention());
    CHECK(ring::wait(s.io(),s.state,polls,s.result)==ring::Status::Ok);
    CHECK(!s.state.tmrReady() && !s.state.needsTmrRetention());
    CHECK(ring::stop(s.io(),s.state,polls,s.result)==ring::Status::Ok);
    CHECK(!s.state.needsRingRetention() && s.state.phase()==ring::Phase::Stopped && s.delays==2);
    CHECK(s.create(sos)==ring::Status::InvalidState);
}
void preflight(const psp::State& sos) {
    for(unsigned mode=0;mode<17;++mode) {
        Fixture s; auto b=s.buffers(); auto l=s.layout; auto io=s.io(); auto p=polls;
        if(mode==0) b.scratch=nullptr; if(mode==1) b.scratchBytes=4095;
        if(mode==2) b.command=b.ring; if(mode==3) b.fence={};
        if(mode==4) io.beforeReadback=nullptr; if(mode==5) io.flushHdp=nullptr;
        if(mode==6) io.delayMs=nullptr; if(mode==7) io.read32=nullptr;
        if(mode==8) p.maxPolls=0; if(mode==9) p.delayUs=0;
        if(mode==10) p.maxPolls=1000001; if(mode==11) p.delayUs=1001;
        if(mode==12) { p.maxPolls=1000000; p.delayUs=11; }
        if(mode==13) ++l.rptr; if(mode==14) l.valid=false;
        if(mode==15) b.scratch=reinterpret_cast<uint8_t*>(&s.state);
        if(mode==16) b.scratch=reinterpret_cast<uint8_t*>(UINTPTR_MAX-16);
        const auto status=ring::create(l,sos,s.allocator,b,io,p,s.state,s.result);
        CHECK(status!=ring::Status::Ok && s.state.phase()==ring::Phase::Fresh);
        CHECK(s.writeCalls==0 && s.readCalls==0 && s.dataWrites==0 && s.selectors==0);
    }
    Fixture s; psp::State absent;
    CHECK(s.create(absent)==ring::Status::SosUnavailable && s.readCalls==0);
    CHECK(s.create(sos)==ring::Status::Ok);
    CHECK(ring::submitAutoloadRlc(s.io(),s.state,s.result)==ring::Status::InvalidState);
    CHECK(ring::submitLoadIp(s.io(),s.state,s.input.handle,0,64,71,s.result)==ring::Status::InvalidState);
    CHECK(ring::submitSetupTmr(s.io(),s.state,s.tmr.handle,s.result)==ring::Status::InvalidState);
    CHECK(ring::validateInputAllocation(s.state,s.ringAllocation.handle,0,64)==ring::Status::AliasedAllocation);
    CHECK(ring::validateInputAllocation(s.state,s.command.handle,0,64)==ring::Status::AliasedAllocation);
    CHECK(ring::validateInputAllocation(s.state,s.fence.handle,0,64)==ring::Status::AliasedAllocation);
    CHECK(ring::validateInputAllocation(s.state,s.input.handle,1,64)==ring::Status::InvalidArgument);
    CHECK(ring::validateInputAllocation(s.state,s.input.handle,0,0)==ring::Status::InvalidArgument);
    CHECK(ring::validateInputAllocation(s.state,s.input.handle,0x100000,4)==ring::Status::InvalidAllocation);
    CHECK(ring::validateInputAllocation(s.state,s.input.handle,UINT64_MAX-4095,4)==ring::Status::InvalidAllocation);
    const auto dataWrites=s.dataWrites;
    CHECK(ring::submitLoadToc(s.io(),s.state,s.command.handle,0,64,s.result)==ring::Status::AliasedAllocation);
    CHECK(s.dataWrites==dataWrites);
}
void createFailures(const psp::State& sos) {
    for(unsigned mode=0;mode<11;++mode) for(unsigned n=1;n<=(mode<3?5U:1U);++n) {
        Fixture s;
        if(mode==0) s.failRead=n;
        if(mode==1) s.onesRead=n;
        if(mode==2) { if(n==5) continue; s.failWrite=n; }
        if(mode==3) s.heartbeat=0;
        if(mode==4) s.control=0x80000001;
        if(mode==5) s.control=0;
        if(mode==6) s.mailboxStatus=2;
        if(mode==7) s.failFlush=1;
        if(mode==8) s.failDataWrite=2050;
        if(mode==9) s.failDataRead=2050;
        if(mode==10) s.failSelector=5;
        CHECK(s.create(sos)!=ring::Status::Ok);
        CHECK(s.state.phase()==ring::Phase::Faulted);
        CHECK(s.create(sos)==ring::Status::InvalidState);
        if(mode==2 && n==4) CHECK(s.delays==1);
        CHECK(!s.forbidden);
    }
}
void asynchronousFailures(const psp::State& sos) {
    for(unsigned mode=0;mode<14;++mode) {
        Fixture s; setupTmr(s,sos); s.autoComplete=false;
        if(mode==0) s.failWrite=s.writeCalls+1;
        CHECK(ring::submitLoadIp(s.io(),s.state,s.input.handle,0,64,71,s.result)==
            (mode==0 ? ring::Status::WriteFailed : ring::Status::Ok));
        if(mode==0) CHECK(s.state.needsRingRetention() && s.state.pending());
        if(mode==1) s.failAcquire=s.acquires+1;
        if(mode==2) s.failDataRead=s.dataReads+1;
        if(mode==3) s.failRead=s.readCalls+1;
        if(mode==4) s.onesRead=s.readCalls+1;
        if(mode==5) put(s.bytes(s.fence),s.state.fenceSequence()+1);
        if(mode==6) s.wp^=16;
        if(mode==7) s.rp=1;
        if(mode==8) s.responseStatus=0xffff000a;
        if(mode==9) s.corruptResponseId=0xbad;
        if(mode==10) s.firmwareAddress=s.mcBase+s.tmr.range.offset-1;
        if(mode==11) s.firmwareAddress=s.mcBase+s.tmr.range.offset+s.tmr.range.bytes;
        if(mode==12) s.firmwareAddress=0;
        if(mode>=8) s.complete();
        const auto status=ring::wait(s.io(),s.state,polls,s.result);
        CHECK(status==((mode==12 || mode==13)?ring::Status::Ok:
            mode==0?ring::Status::FenceTimeout: mode==1?ring::Status::VisibilityFailed:
            mode==2?ring::Status::TransferFailed: mode==3?ring::Status::ReadFailed:
            mode==4?ring::Status::DeviceUnavailable: mode==5?ring::Status::FenceCorrupt:
            (mode==6 || mode==7)?ring::Status::PointerCorrupt:mode==8?ring::Status::CommandRejected:
            ring::Status::ResponseCorrupt));
        CHECK(s.state.needsRingRetention() && s.state.needsTmrRetention());
        if(mode==0) {
            s.complete();
            CHECK(ring::wait(s.io(),s.state,polls,s.result)==ring::Status::Ok);
            CHECK(!s.state.pending() && s.state.phase()==ring::Phase::Faulted);
            CHECK(ring::submitBootConfig(s.io(),s.state,s.result)==ring::Status::InvalidState);
            CHECK(ring::stop(s.io(),s.state,polls,s.result)==ring::Status::Ok);
            CHECK(s.state.needsTmrRetention() && !s.state.needsRingRetention());
        }
        if(mode==8) {
            CHECK(!s.state.pending() && s.state.phase()==ring::Phase::Faulted);
            CHECK(ring::stop(s.io(),s.state,polls,s.result)==ring::Status::Ok);
            CHECK(s.state.needsTmrRetention());
        }
        CHECK(!s.forbidden);
    }
}
void tmrAndStop(const psp::State& sos) {
    const uint32_t invalidSizes[]={0,1,4095,4097,0x4001000,UINT32_MAX};
    for(auto size:invalidSizes) {
        Fixture s; CHECK(s.create(sos)==ring::Status::Ok); s.tmrSize=size;
        CHECK(ring::submitLoadToc(s.io(),s.state,s.input.handle,0,64,s.result)==ring::Status::Ok);
        CHECK(ring::wait(s.io(),s.state,polls,s.result)==ring::Status::InvalidTmrSize);
        CHECK(!s.state.tmrReady() && s.state.phase()==ring::Phase::Faulted);
    }
    for(unsigned mode=0;mode<6;++mode) {
        Fixture s; setupTmr(s,sos);
        CHECK(ring::validateInputAllocation(s.state,s.tmr.handle,0,64)==ring::Status::AliasedAllocation);
        CHECK(ring::submitSetupTmr(s.io(),s.state,s.tmr.handle,s.result)==ring::Status::InvalidState);
        CHECK(ring::submitLoadToc(s.io(),s.state,s.input.handle,0,64,s.result)==ring::Status::InvalidState);
        if(mode==0) s.failWrite=s.writeCalls+1;
        if(mode==1) s.mailboxStatus=1;
        if(mode==2) s.busyReads=100;
        if(mode==3) s.failRead=s.readCalls+1;
        if(mode==4) CHECK(s.allocator.release(s.fence.handle)==VramAllocatorStatus::Ok);
        const auto status=ring::stop(s.io(),s.state,polls,s.result);
        CHECK(status==(mode==5?ring::Status::Ok:mode==0?ring::Status::WriteFailed:
            mode==1?ring::Status::MailboxRejected:mode==2?ring::Status::MailboxTimeout:
            mode==3?ring::Status::ReadFailed:ring::Status::InvalidAllocation));
        CHECK(s.state.needsTmrRetention());
        CHECK(s.state.needsRingRetention()==(mode!=5));
        if(mode==0) CHECK(s.delays==2);
    }
}
} // namespace
int main() {
    psp::State sos; CHECK(loadTestSos(sos));
    layouts(); protocol(sos); preflight(sos); createFailures(sos); asynchronousFailures(sos); tmrAndStop(sos);
    std::cout<<checks<<" checks, "<<failures<<" failures (offline PSP KM emulator)\n";
    return failures?1:0;
}
