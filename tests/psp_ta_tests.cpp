// SPDX-License-Identifier: MIT
// Real firmware metadata + offline PSP protocol transcripts/fault injection.
// The transcript peer is test-only and is never evidence of GPU execution.
#include "psp_ring_fixture.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>
using namespace pspring_test;
namespace ta=hackynavi::pspta;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; std::cerr<<"FAIL "<<__LINE__<<": " #x "\n"; } } while(0)
bool clear(const ta::Image& i) {
    const uint8_t* p=reinterpret_cast<const uint8_t*>(&i);
    for(size_t n=0;n<sizeof(i);++n) if(p[n]) return false;
    return true;
}
void parser(const std::vector<uint8_t>& real) {
    ta::Image image={};
    CHECK(ta::parse(real.data(),real.size(),image)==ta::Status::Ok);
    CHECK(image.header.sizeBytes==271616 && image.header.payloadOffset==256);
    CHECK(image.hdcp.version==385876051 && image.hdcp.image.offset==209408 && image.hdcp.image.size==28928);
    CHECK(image.dtm.version==301989919 && image.dtm.image.offset==238336 && image.dtm.image.size==8448);
    const ta::Image expected=image;
    for(size_t n=0;n<105;++n) {
        image=expected; CHECK(ta::parse(real.data(),n,image)!=ta::Status::Ok && clear(image));
    }
    image=expected; CHECK(ta::parse(nullptr,real.size(),image)==ta::Status::InvalidArgument && clear(image));
    image=expected; CHECK(ta::parse(reinterpret_cast<const uint8_t*>(&image),sizeof(image),image)==ta::Status::InvalidArgument);
    CHECK(image.hdcp.version==expected.hdcp.version);
    image=expected; CHECK(ta::parse(reinterpret_cast<const uint8_t*>(UINTPTR_MAX-3),8,image)==ta::Status::InvalidArgument && clear(image));
    struct Mutation { size_t at; uint32_t value; };
    const Mutation bad[]={ {0,271615},{4,35},{8,0x00010002},{12,14},{24,252},{32,0},{32,33},
      {36,9},{40,0},{48,0},{52,1},{60,0},{60,209153},{64,28927},{68,4},{68,6},
      {76,UINT32_MAX},{80,UINT32_MAX},{96,24828} };
    for(const auto& m:bad) { auto b=real; put(b.data()+m.at,m.value); image=expected;
        CHECK(ta::parse(b.data(),b.size(),image)!=ta::Status::Ok && clear(image)); }
    // Actual descriptors may be reordered; range admission is independent of order.
    auto b=real; for(unsigned n=0;n<16;++n) std::swap(b[36+n],b[68+n]);
    CHECK(ta::parse(b.data(),b.size(),image)==ta::Status::Ok);
    CHECK(image.hdcp.image.offset==expected.hdcp.image.offset && image.dtm.image.offset==expected.dtm.image.offset);
    b=real; b.push_back(0); CHECK(ta::parse(b.data(),b.size(),image)!=ta::Status::Ok && clear(image));
    b=real; b[209408]^=1; // Parser is metadata admission, not cryptographic authentication.
    CHECK(ta::parse(b.data(),b.size(),image)==ta::Status::Ok);
}
struct Peer:Fixture {
    VramAllocation hdcp{},dtm{};
    uint32_t session=0;
    Peer() { valid&=allocator.allocate(ta::kSharedBytes,4096,hdcp)==VramAllocatorStatus::Ok;
        valid&=allocator.allocate(ta::kSharedBytes,4096,dtm)==VramAllocatorStatus::Ok; }
    static bool writeTa(void* p,uint32_t address,uint32_t value) {
        auto& self=*static_cast<Peer*>(p);
        const bool ok=Fixture::write(static_cast<Fixture*>(&self),address,value);
        if(address==self.layout.wptr && self.autoComplete && get(self.bytes(self.command)+8)==1)
            put(self.bytes(self.command)+868,self.session);
        return ok;
    }
    ring::Io io() { auto out=Fixture::io(); out.context=this; out.write32=writeTa; return out; }
    ring::Status finish() { return ring::wait(io(),state,polls,result); }
    void start(const psp::State& sos) {
        CHECK(valid && ring::create(layout,sos,allocator,buffers(),io(),polls,state,result)==ring::Status::Ok);
        CHECK(ring::submitLoadToc(io(),state,input.handle,0,64,result)==ring::Status::Ok && finish()==ring::Status::Ok);
        CHECK(ring::submitSetupTmr(io(),state,tmr.handle,result)==ring::Status::Ok && finish()==ring::Status::Ok);
    }
    ring::Status load(ta::Kind kind=ta::Kind::Hdcp) {
        return ring::submitLoadTa(io(),state,kind,input.handle,0,kind==ta::Kind::Hdcp?28928:8448,
            kind==ta::Kind::Hdcp?hdcp.handle:dtm.handle,result);
    }
    ring::TaInformation info(ta::Kind kind=ta::Kind::Hdcp) {
        ring::TaInformation out={}; CHECK(state.taInformation(kind,out)); return out;
    }
};
void lifecycle(const psp::State& sos) {
    Peer p; p.start(sos); CHECK(!p.state.needsTaRetention());
    CHECK(p.load()==ring::Status::Ok && p.state.pending() && p.result.taRetentionRequired);
    CHECK(p.info().loadAttempted && !p.info().loadAcknowledged && p.info().phase==ring::TaPhase::Loading);
    const auto& cmd=p.commands.back();
    const uint64_t code=p.mcBase+p.input.range.offset,shared=p.mcBase+p.hdcp.range.offset;
    CHECK(get(cmd.data()+8)==1 && get(cmd.data()+28)==uint32_t(code) && get(cmd.data()+32)==uint32_t(code>>32));
    CHECK(get(cmd.data()+36)==28928 && get(cmd.data()+40)==uint32_t(shared) && get(cmd.data()+44)==uint32_t(shared>>32));
    CHECK(get(cmd.data()+48)==ta::kSharedBytes);
    for(unsigned at=0;at<864;at+=4) if(at!=8 && (at<28 || at>48)) CHECK(get(cmd.data()+at)==0);
    CHECK(p.finish()==ring::Status::Ok);
    CHECK(p.info().sessionValid && p.info().sessionId==0 && p.info().loadAcknowledged);
    CHECK(ring::validateInputAllocation(p.state,p.hdcp.handle,0,16)==ring::Status::AliasedAllocation);
    CHECK(ring::validateInputAllocation(p.state,p.input.handle,0,16)==ring::Status::Ok);
    const auto before=p.writeCalls;
    CHECK(ring::submitDestroyTmr(p.io(),p.state,p.result)==ring::Status::InvalidState && p.writeCalls==before);
    CHECK(ring::stop(p.io(),p.state,polls,p.result)==ring::Status::InvalidState && p.writeCalls==before);
    CHECK(p.load()==ring::Status::InvalidState && p.writeCalls==before);
    CHECK(ring::submitInvokeTa(p.io(),p.state,ta::Kind::Hdcp,0x12345678,p.result)==ring::Status::Ok);
    CHECK(get(p.commands.back().data()+28)==0 && get(p.commands.back().data()+32)==0x12345678);
    for(unsigned at=36;at<864;at+=4) CHECK(get(p.commands.back().data()+at)==0);
    CHECK(p.finish()==ring::Status::Ok && p.info().invocations==1 && p.info().phase==ring::TaPhase::Loaded);
    p.session=73; CHECK(p.load(ta::Kind::Dtm)==ring::Status::Ok && p.finish()==ring::Status::Ok);
    CHECK(p.info(ta::Kind::Dtm).sessionId==73);
    CHECK(ring::submitUnloadTa(p.io(),p.state,ta::Kind::Hdcp,p.result)==ring::Status::Ok);
    CHECK(p.info().unloadAttempted && !p.info().unloadAcknowledged && p.state.needsTaRetention());
    CHECK(p.finish()==ring::Status::Ok && p.info().unloadAcknowledged && !p.info().retained);
    CHECK(p.state.needsTaRetention() && p.load()==ring::Status::InvalidState);
    CHECK(p.allocator.release(p.hdcp.handle)==VramAllocatorStatus::Ok);
    CHECK(ring::submitUnloadTa(p.io(),p.state,ta::Kind::Dtm,p.result)==ring::Status::Ok &&
        get(p.commands.back().data()+28)==73 && p.finish()==ring::Status::Ok);
    CHECK(!p.state.needsTaRetention());
    CHECK(ring::submitDestroyTmr(p.io(),p.state,p.result)==ring::Status::Ok && p.finish()==ring::Status::Ok);
    CHECK(ring::stop(p.io(),p.state,polls,p.result)==ring::Status::Ok && !p.forbidden);
}
void preflight(const psp::State& sos) {
    Peer p; p.start(sos); const auto before=p.writeCalls;
    const VramAllocationHandle aliases[]={p.ringAllocation.handle,p.command.handle,p.fence.handle,p.tmr.handle,p.input.handle};
    for(auto h:aliases) CHECK(ring::submitLoadTa(p.io(),p.state,ta::Kind::Hdcp,p.input.handle,0,28928,h,p.result)!=ring::Status::Ok);
    CHECK(ring::submitLoadTa(p.io(),p.state,ta::Kind::Hdcp,p.hdcp.handle,0,16,p.hdcp.handle,p.result)==ring::Status::AliasedAllocation);
    CHECK(ring::submitLoadTa(p.io(),p.state,ta::Kind::Hdcp,p.input.handle,1,16,p.hdcp.handle,p.result)==ring::Status::InvalidArgument);
    for(uint32_t n:{0u,3u,0xfffffffcu}) CHECK(ring::submitLoadTa(p.io(),p.state,ta::Kind::Hdcp,p.input.handle,0,n,p.hdcp.handle,p.result)!=ring::Status::Ok);
    CHECK(p.load(static_cast<ta::Kind>(2))==ring::Status::InvalidArgument);
    CHECK(ring::submitInvokeTa(p.io(),p.state,ta::Kind::Hdcp,1,p.result)==ring::Status::InvalidState);
    CHECK(ring::submitUnloadTa(p.io(),p.state,ta::Kind::Dtm,p.result)==ring::Status::InvalidState);
    CHECK(p.writeCalls==before && !p.state.needsTaRetention() && p.info().phase==ring::TaPhase::Fresh);
    ring::TaInformation out={}; out.sessionId=444;
    CHECK(!p.state.taInformation(static_cast<ta::Kind>(2),out) && out.sessionId==444);
}
void failuresCase(const psp::State& sos) {
    // Guard, command/frame transfer, publication, delivered-but-failed pointer,
    // timeout, response transport, request corruption and explicit rejection.
    for(unsigned kind=0;kind<12;++kind) {
        Peer p; p.start(sos);
        if(kind==0) p.failRead=p.readCalls+1;
        if(kind==1) p.failDataWrite=p.dataWrites+1;
        if(kind==2) p.failDataWrite=p.dataWrites+257;
        if(kind==3) p.failFlush=p.flushes+1;
        if(kind==4) p.failWrite=p.writeCalls+1;
        if(kind==5) p.autoComplete=false;
        if(kind==6) p.responseStatus=5;
        const auto submitted=p.load();
        if(kind<=4) CHECK(submitted!=ring::Status::Ok);
        else {
            CHECK(submitted==ring::Status::Ok);
            if(kind==7) p.failAcquire=p.acquires+2;
            if(kind==8) put(p.bytes(p.command)+40,0);
            if(kind==9) put(p.bytes(p.command)+860,1);
            if(kind==10) put(p.bytes(p.fence),99);
            if(kind==11) p.failDataRead=p.dataReads+2;
            CHECK(p.finish()!=ring::Status::Ok);
        }
        CHECK(p.state.phase()==ring::Phase::Faulted && p.state.needsTaRetention() && p.state.needsTmrRetention());
        CHECK(p.info().phase==ring::TaPhase::FaultedRetained && p.info().loadAttempted && !p.info().loadAcknowledged);
        const auto count=p.writeCalls;
        CHECK(p.load()==(p.state.pending()?ring::Status::PendingCommand:ring::Status::InvalidState));
        CHECK(ring::stop(p.io(),p.state,polls,p.result)!=ring::Status::Ok && p.writeCalls==count);
        if(kind==4 || kind==5) {
            if(kind==5) { p.complete(); put(p.bytes(p.command)+868,29); }
            CHECK(p.finish()==ring::Status::Ok);
            CHECK(p.info().loadAcknowledged && p.info().phase==ring::TaPhase::FaultedRetained && p.state.phase()==ring::Phase::Faulted);
            CHECK(ring::submitUnloadTa(p.io(),p.state,ta::Kind::Hdcp,p.result)==ring::Status::InvalidState);
        }
        CHECK(!p.forbidden);
    }
    { Peer p; p.start(sos); CHECK(p.load()==ring::Status::Ok && p.finish()==ring::Status::Ok);
      CHECK(p.load(ta::Kind::Dtm)==ring::Status::Ok); CHECK(p.finish()==ring::Status::ResponseCorrupt);
      CHECK(p.info(ta::Kind::Dtm).retained && !p.info(ta::Kind::Dtm).sessionValid); }
    for(bool releaseFirmware:{false,true}) {
        Peer p; p.start(sos); CHECK(p.load()==ring::Status::Ok);
        CHECK(p.allocator.release(releaseFirmware?p.input.handle:p.hdcp.handle)==VramAllocatorStatus::Ok);
        CHECK(p.finish()==ring::Status::InvalidAllocation && p.state.needsTaRetention());
    }
    // A completed load no longer retains firmware, but retains its shared BO.
    { Peer p; p.start(sos); CHECK(p.load()==ring::Status::Ok && p.finish()==ring::Status::Ok);
      CHECK(p.allocator.release(p.input.handle)==VramAllocatorStatus::Ok);
      CHECK(ring::submitUnloadTa(p.io(),p.state,ta::Kind::Hdcp,p.result)==ring::Status::Ok && p.finish()==ring::Status::Ok); }
    for(unsigned mode=0;mode<4;++mode) {
        Peer p; p.start(sos); CHECK(p.load()==ring::Status::Ok && p.finish()==ring::Status::Ok);
        if(mode==0) p.responseStatus=7;
        if(mode==1) p.failWrite=p.writeCalls+1;
        const auto st=ring::submitUnloadTa(p.io(),p.state,ta::Kind::Hdcp,p.result);
        if(mode==1) CHECK(st==ring::Status::WriteFailed);
        else { CHECK(st==ring::Status::Ok);
            if(mode==2) put(p.bytes(p.command)+28,42);
            if(mode==3) put(p.bytes(p.command)+32,1);
            CHECK(p.finish()!=ring::Status::Ok); }
        CHECK(p.info().retained && p.info().unloadAttempted && !p.info().unloadAcknowledged);
        if(mode==1) { CHECK(p.finish()==ring::Status::Ok && !p.info().retained);
            CHECK(p.state.phase()==ring::Phase::Faulted && p.info().phase==ring::TaPhase::Retired); }
    }
    // Invoke uses only the privately returned session and immutable persistent
    // shared allocation. Reject altered command/list and stale shared ownership.
    for(unsigned mode=0;mode<5;++mode) {
        Peer p; p.start(sos); p.session=0xfedcba98;
        CHECK(p.load()==ring::Status::Ok && p.finish()==ring::Status::Ok);
        if(mode==0) p.responseStatus=3;
        CHECK(ring::submitInvokeTa(p.io(),p.state,ta::Kind::Hdcp,7,p.result)==ring::Status::Ok);
        CHECK(get(p.commands.back().data()+28)==0xfedcba98);
        if(mode==1) put(p.bytes(p.command)+32,8);
        if(mode==2) put(p.bytes(p.command)+36,1);
        if(mode==3) CHECK(p.allocator.release(p.hdcp.handle)==VramAllocatorStatus::Ok);
        if(mode==4) put(p.bytes(p.command)+860,1);
        CHECK(p.finish()!=ring::Status::Ok && p.info().retained && p.info().phase==ring::TaPhase::FaultedRetained);
    }
}
}
int main(int argc,char** argv) {
    if(argc>2) return 2;
    if(argc==2) {
        std::ifstream f(argv[1],std::ios::binary); std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});
        if(bytes.size()!=271616) return 3;
        parser(bytes);
    } else std::cout<<"No firmware argument: actual-image metadata checks not run\n";
    psp::State sos; CHECK(loadTestSos(sos));
    lifecycle(sos);preflight(sos);failuresCase(sos);
    std::cout<<checks<<" checks, "<<failures<<" failures (metadata and offline protocol only)\n";
    return failures?1:0;
}
