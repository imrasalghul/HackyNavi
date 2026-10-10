// SPDX-License-Identifier: MIT
// Offline protocol peer only. It does not execute firmware, emulate encryption,
// or establish inherited display ownership. Actual pinned ABI is the oracle.
#include "psp_ring_fixture.hpp"
#include "hackynavi/psp_display_ta.hpp"
#include "hdcp_psp.h"
#include <cstddef>
#include <cstring>
#include <iostream>
#include <new>
#include <type_traits>
using namespace pspring_test;
namespace dt=hackynavi::pspdisplayta;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<"FAIL "<<__LINE__<<": " #x "\n";}}while(0)
static_assert(sizeof(ta_dtm_shared_memory)==80,"actual DTM size");
static_assert(offsetof(ta_dtm_shared_memory,dtm_status)==8,"actual DTM status");
static_assert(offsetof(ta_dtm_shared_memory,dtm_in_message)==16,"actual DTM input");
static_assert(offsetof(ta_dtm_shared_memory,dtm_out_message)==76,"actual DTM output");
static_assert(sizeof(ta_hdcp_shared_memory)==10264,"actual HDCP size");
static_assert(offsetof(ta_hdcp_shared_memory,hdcp_status)==4,"actual HDCP status");
static_assert(offsetof(ta_hdcp_shared_memory,in_msg)==12,"actual HDCP input");
static_assert(offsetof(ta_hdcp_shared_memory,out_msg)==5136,"actual HDCP output");
static_assert(sizeof(ta_hdcp_cmd_hdcp1_create_session_output)==32,"actual HDCP1 challenge size");
static_assert(!std::is_copy_constructible<dt::State>::value,"owner noncopyable");
static_assert(!std::is_move_constructible<dt::State>::value,"owner immovable");
const dt::Topology topology={2,3,4,1,true};
struct Peer:Fixture {
    VramAllocation hdcp{},dtm{};
    dt::State owner;dt::Result outcome{};
    uint32_t loadId=20,authId=0,appStatus=0,protection=0,content=0;
    unsigned guards=0,failGuard=0,faults=0;
    bool respond=true,guard=true,loseAfterSharedRead=false,sharedReadFinished=false;
    unsigned finalReadGuards=0;
    size_t corrupt=0;
    std::vector<std::vector<uint8_t>> apps;
    Peer() {
        scratch.resize(16384);
        valid&=allocator.allocate(16384,4096,hdcp)==VramAllocatorStatus::Ok;
        valid&=allocator.allocate(16384,4096,dtm)==VramAllocatorStatus::Ok;
    }
    static bool live(void* p){auto& s=*static_cast<Peer*>(p);
        if(s.loseAfterSharedRead&&s.sharedReadFinished&&++s.finalReadGuards>1)return false;
        return s.guard&&++s.guards!=s.failGuard;}
    static void fault(void* p){++static_cast<Peer*>(p)->faults;}
    static bool readTa(void* p,uint32_t address,uint32_t& value) {
        auto& s=*static_cast<Peer*>(p);
        const bool last=address==4&&!s.apps.empty()&&
            s.selected()==size_t(s.dtm.range.offset-arenaOffset+16380);
        const bool ok=Fixture::read(p,address,value);
        if(last)s.sharedReadFinished=true;
        return ok;
    }
    static bool writeTa(void* p,uint32_t address,uint32_t value) {
        auto& s=*static_cast<Peer*>(p);const bool ok=Fixture::write(p,address,value);
        if(address!=s.layout.wptr||!s.autoComplete)return ok;
        uint8_t* cmd=s.bytes(s.command);const auto id=get(cmd+8);
        if(id==1)put(cmd+868,s.loadId);
        if(id!=3)return ok;
        const bool isDtm=get(cmd+28)==21;
        CHECK(isDtm||get(cmd+28)==20);
        uint8_t* shared=s.bytes(isDtm?s.dtm:s.hdcp);
        s.apps.emplace_back(shared,shared+16384);
        CHECK(get(cmd+32)==get(shared));
        if(!s.respond)return ok;
        if(isDtm){put(shared+8,s.appStatus);put(shared+4,0x12345678);}
        else {
            put(shared+4,s.appStatus);
            switch(get(shared)) {
            case TA_HDCP_COMMAND__HDCP1_CREATE_SESSION:
                for(unsigned i=0;i<28;++i)shared[5140+i]=uint8_t(31+i);
                // fall through
            case TA_HDCP_COMMAND__HDCP2_CREATE_SESSION_V2:put(shared+5136,s.authId);break;
            case TA_HDCP_COMMAND__HDCP1_GET_ENCRYPTION_STATUS:put(shared+5136,s.protection);break;
            case TA_HDCP_COMMAND__HDCP2_GET_ENCRYPTION_STATUS:
                put(shared+5136,s.content);put(shared+5140,s.protection);break;
            case TA_HDCP_COMMAND__HDCP1_DESTROY_SESSION:
            case TA_HDCP_COMMAND__HDCP2_DESTROY_SESSION:break;
            default:CHECK(false);
            }
        }
        if(s.corrupt)shared[s.corrupt]^=1;
        return ok;
    }
    ring::Io io(){auto v=Fixture::io();v.context=this;v.write32=writeTa;v.read32=readTa;return v;}
    dt::Io displayIo(){return {io(),live,fault};}
    ring::Status finish(){return ring::wait(io(),state,polls,result);}
    void start(const psp::State& sos,bool doBind=true,bool loadDtm=true) {
        CHECK(valid&&ring::create(layout,sos,allocator,buffers(),io(),polls,state,result)==ring::Status::Ok);
        CHECK(ring::submitLoadToc(io(),state,input.handle,0,64,result)==ring::Status::Ok&&finish()==ring::Status::Ok);
        CHECK(ring::submitSetupTmr(io(),state,tmr.handle,result)==ring::Status::Ok&&finish()==ring::Status::Ok);
        CHECK(ring::submitLoadTa(io(),state,pspta::Kind::Hdcp,input.handle,0,28928,hdcp.handle,result)==ring::Status::Ok&&finish()==ring::Status::Ok);
        if(loadDtm){loadId=21;CHECK(ring::submitLoadTa(io(),state,pspta::Kind::Dtm,input.handle,0,8448,dtm.handle,result)==ring::Status::Ok&&finish()==ring::Status::Ok);}
        if(doBind)CHECK(dt::bind(state,displayIo(),owner,outcome)==dt::Status::Ok);
    }
    void sealed() {
        CHECK(owner.phase()==dt::Phase::FaultedRetained&&state.phase()==ring::Phase::Faulted);
        CHECK(outcome.retentionRequired&&state.needsTaRetention()&&state.needsTmrRetention()&&faults==1);
        const auto before=dataWrites+writeCalls;
        CHECK(dt::retire(owner,outcome)==dt::Status::InvalidState);
        CHECK(ring::submitUnloadTa(io(),state,pspta::Kind::Hdcp,result)!=ring::Status::Ok);
        CHECK(ring::submitDestroyTmr(io(),state,result)!=ring::Status::Ok);
        CHECK(dataWrites+writeCalls==before);
    }
};
template<class T>void wire(const std::vector<uint8_t>& actual,const T& expected) {
    CHECK(actual.size()==16384);
    CHECK(std::memcmp(actual.data(),&expected,sizeof(expected))==0);
    bool zero=true;for(size_t i=sizeof(expected);i<actual.size();++i)zero&=actual[i]==0;CHECK(zero);
}
ta_dtm_shared_memory dtmOracle(unsigned slot,bool active,const dt::Topology& t=topology) {
    ta_dtm_shared_memory q={};q.cmd_id=TA_DTM_COMMAND__TOPOLOGY_UPDATE_V3;q.dtm_status=TA_DTM_STATUS__GENERIC_FAILURE;
    auto& v=q.dtm_in_message.topology_update_v3;v.display_handle=slot;v.is_active=active?1:0;
    if(active){v.controller=t.otg+1;v.ddc_line=t.ddc+1;v.link_enc=t.transmitter;v.stream_enc=t.streamEncoder;
        v.max_hdcp_supported_version=TA_DTM_HDCP_VERSION_MAX_SUPPORTED__2_3;v.encoder_type=TA_DTM_ENCODER_TYPE__DIG;
        v.phy_id=t.transmitter;v.link_hdcp_cap=t.hdcpCapable?1:0;v.dio_output_type=TA_DTM_DIO_OUTPUT_TYPE__DIRECT;v.dio_output_id=t.transmitter;}
    return q;
}
void lifecycle(const psp::State& sos,dt::Protocol protocol) {
    Peer p;p.start(sos);dt::Display display;dt::Authentication auth;
    CHECK(dt::addDisplay(p.owner,topology,polls,display,p.outcome)==dt::Status::Ok);
    CHECK(p.outcome.pspAcknowledged&&p.outcome.applicationAcknowledged&&p.outcome.responseId==0x12345678);
    wire(p.apps.back(),dtmOracle(0,true));
    CHECK(dt::retire(p.owner,p.outcome)==dt::Status::Busy);
    const auto writes=p.dataWrites;
    for(auto kind:{pspta::Kind::Hdcp,pspta::Kind::Dtm}) {
        CHECK(ring::submitInvokeTa(p.io(),p.state,kind,0,p.result)==ring::Status::InvalidState);
        CHECK(ring::submitUnloadTa(p.io(),p.state,kind,p.result)==ring::Status::InvalidState);
    }
    CHECK(p.dataWrites==writes);
    auto* badPoll=new(p.scratch.data()) ring::PollOptions{3,2};
    CHECK(dt::addDisplay(p.owner,topology,*badPoll,display,p.outcome)==dt::Status::AliasedStorage);
    auto* badResult=new(p.scratch.data()) dt::Result{};
    CHECK(dt::addDisplay(p.owner,topology,polls,display,*badResult)==dt::Status::AliasedStorage);
    CHECK(p.dataWrites==writes&&p.owner.phase()==dt::Phase::Bound);
    CHECK(dt::createAuthentication(p.owner,display,protocol,dt::Content::Type0,polls,auth,p.outcome)==dt::Status::Ok);
    CHECK(p.outcome.sessionValid&&p.outcome.sessionId==0); // Zero is a real valid returned auth handle.
    ta_hdcp_shared_memory expected={};expected.hdcp_status=TA_HDCP_STATUS__GENERIC_FAILURE;
    const bool one=protocol==dt::Protocol::Hdcp1;
    expected.cmd_id=one?TA_HDCP_COMMAND__HDCP1_CREATE_SESSION:TA_HDCP_COMMAND__HDCP2_CREATE_SESSION_V2;
    if(!one)expected.in_msg.hdcp2_create_session_v2.negotiate_content_type=TA_HDCP2_CONTENT_TYPE_NEGOTIATION_TYPE__FORCE_TYPE0;
    wire(p.apps.back(),expected);
    if(one){CHECK(p.outcome.an[0]==31&&p.outcome.an[7]==38&&p.outcome.aksv[0]==39&&p.outcome.aksv[4]==43&&p.outcome.ainfo==44);}
    CHECK(dt::removeDisplay(p.owner,display,polls,p.outcome)==dt::Status::Busy);
    CHECK(dt::queryAuthentication(p.owner,auth,polls,p.outcome)==dt::Status::Ok);
    CHECK(p.outcome.protectionValid&&p.outcome.protectionLevel==0);
    expected={};expected.hdcp_status=TA_HDCP_STATUS__GENERIC_FAILURE;
    expected.cmd_id=one?TA_HDCP_COMMAND__HDCP1_GET_ENCRYPTION_STATUS:TA_HDCP_COMMAND__HDCP2_GET_ENCRYPTION_STATUS;
    wire(p.apps.back(),expected);
    p.protection=1;p.content=TA_HDCP2_CONTENT_TYPE__TYPE1;
    CHECK(dt::queryAuthentication(p.owner,auth,polls,p.outcome)==dt::Status::Ok&&p.outcome.protectionLevel==1);
    CHECK(dt::destroyAuthentication(p.owner,auth,polls,p.outcome)==dt::Status::Ok&&!p.outcome.sessionValid);
    expected.cmd_id=one?TA_HDCP_COMMAND__HDCP1_DESTROY_SESSION:TA_HDCP_COMMAND__HDCP2_DESTROY_SESSION;
    wire(p.apps.back(),expected);
    CHECK(dt::queryAuthentication(p.owner,auth,polls,p.outcome)==dt::Status::StaleToken);
    CHECK(dt::destroyAuthentication(p.owner,auth,polls,p.outcome)==dt::Status::StaleToken);
    p.authId=90210;dt::Authentication newer;
    CHECK(dt::createAuthentication(p.owner,display,protocol,dt::Content::Type0,polls,newer,p.outcome)==dt::Status::Ok&&p.outcome.sessionId==90210);
    CHECK(dt::destroyAuthentication(p.owner,auth,polls,p.outcome)==dt::Status::StaleToken);
    CHECK(dt::destroyAuthentication(p.owner,newer,polls,p.outcome)==dt::Status::Ok&&get(p.apps.back().data()+12)==90210);
    CHECK(dt::removeDisplay(p.owner,display,polls,p.outcome)==dt::Status::Ok);wire(p.apps.back(),dtmOracle(0,false));
    CHECK(dt::removeDisplay(p.owner,display,polls,p.outcome)==dt::Status::StaleToken);
    dt::Display fresh;CHECK(dt::addDisplay(p.owner,topology,polls,fresh,p.outcome)==dt::Status::Ok);
    CHECK(dt::removeDisplay(p.owner,display,polls,p.outcome)==dt::Status::StaleToken);
    CHECK(dt::removeDisplay(p.owner,fresh,polls,p.outcome)==dt::Status::Ok);
    CHECK(dt::retire(p.owner,p.outcome)==dt::Status::Ok&&!p.outcome.retentionRequired);
    for(auto kind:{pspta::Kind::Hdcp,pspta::Kind::Dtm})
        CHECK(ring::submitUnloadTa(p.io(),p.state,kind,p.result)==ring::Status::Ok&&p.finish()==ring::Status::Ok);
    CHECK(ring::submitDestroyTmr(p.io(),p.state,p.result)==ring::Status::Ok&&p.finish()==ring::Status::Ok);
    CHECK(ring::stop(p.io(),p.state,polls,p.result)==ring::Status::Ok&&!p.forbidden);
}
void preflight(const psp::State& sos) {
    {Peer p;p.start(sos,false,false);CHECK(dt::bind(p.state,p.displayIo(),p.owner,p.outcome)==dt::Status::InvalidState);}
    {Peer p;p.scratch.resize(4096);p.start(sos,false);CHECK(dt::bind(p.state,p.displayIo(),p.owner,p.outcome)==dt::Status::InvalidArgument);}
    {Peer p;p.start(sos,false);p.guard=false;CHECK(dt::bind(p.state,p.displayIo(),p.owner,p.outcome)==dt::Status::GuardFailed&&p.owner.phase()==dt::Phase::Fresh);}
    for(auto kind:{pspta::Kind::Hdcp,pspta::Kind::Dtm}) {
        Peer p;p.start(sos,false);p.respond=false;
        // Genuine raw invocation on an owned TA predates the typed ledger. Even
        // PSP ACK supplies no authority to assume its application objects empty.
        std::memset(p.bytes(kind==pspta::Kind::Hdcp?p.hdcp:p.dtm),0,16384);
        CHECK(ring::submitInvokeTa(p.io(),p.state,kind,0,p.result)==ring::Status::Ok&&p.finish()==ring::Status::Ok);
        CHECK(dt::bind(p.state,p.displayIo(),p.owner,p.outcome)==dt::Status::InvalidState&&p.owner.phase()==dt::Phase::Fresh);
    }
    Peer p;p.start(sos);dt::State other;dt::Display display;dt::Authentication auth;
    CHECK(dt::bind(p.state,p.displayIo(),other,p.outcome)==dt::Status::InvalidState);
    const auto writes=p.dataWrites;
    dt::Topology bad=topology;bad.otg=4;CHECK(dt::addDisplay(p.owner,bad,polls,display,p.outcome)==dt::Status::InvalidArgument);
    bad=topology;bad.ddc=5;CHECK(dt::addDisplay(p.owner,bad,polls,display,p.outcome)==dt::Status::InvalidArgument);
    bad=topology;bad.streamEncoder=5;CHECK(dt::addDisplay(p.owner,bad,polls,display,p.outcome)==dt::Status::InvalidArgument);
    bad=topology;bad.transmitter=5;CHECK(dt::addDisplay(p.owner,bad,polls,display,p.outcome)==dt::Status::InvalidArgument);
    CHECK(dt::addDisplay(p.owner,topology,{0,1},display,p.outcome)==dt::Status::InvalidArgument);
    CHECK(dt::removeDisplay(p.owner,display,polls,p.outcome)==dt::Status::StaleToken);
    CHECK(dt::destroyAuthentication(p.owner,auth,polls,p.outcome)==dt::Status::StaleToken);
    CHECK(p.dataWrites==writes);
    CHECK(dt::addDisplay(p.owner,topology,polls,display,p.outcome)==dt::Status::Ok);
    CHECK(dt::addDisplay(p.owner,topology,polls,display,p.outcome)==dt::Status::Busy);
    CHECK(dt::createAuthentication(p.owner,display,dt::Protocol::Hdcp1,dt::Content::Type1,polls,auth,p.outcome)==dt::Status::InvalidArgument);
    CHECK(dt::createAuthentication(p.owner,display,static_cast<dt::Protocol>(2),dt::Content::Type0,polls,auth,p.outcome)==dt::Status::InvalidArgument);
    CHECK(dt::createAuthentication(p.owner,display,dt::Protocol::Hdcp2,static_cast<dt::Content>(0),polls,auth,p.outcome)==dt::Status::InvalidArgument);
    CHECK(dt::createAuthentication(p.owner,display,dt::Protocol::Hdcp2,dt::Content::Maximum,polls,auth,p.outcome)==dt::Status::Ok);
    CHECK(get(p.apps.back().data()+16)==TA_HDCP2_CONTENT_TYPE_NEGOTIATION_TYPE__MAX_SUPPORTED);
    CHECK(dt::createAuthentication(p.owner,display,dt::Protocol::Hdcp2,dt::Content::Type0,polls,auth,p.outcome)==dt::Status::Busy);
    Peer q;q.start(sos);CHECK(dt::queryAuthentication(q.owner,auth,polls,q.outcome)==dt::Status::StaleToken);
    CHECK(dt::removeDisplay(q.owner,display,polls,q.outcome)==dt::Status::StaleToken);
}
void capacity(const psp::State& sos) {
    Peer p;p.start(sos);dt::Display d[5];dt::Authentication a[4];
    for(unsigned i=0;i<4;++i){const dt::Topology t={uint8_t(i),uint8_t(i),uint8_t(i),uint8_t(i),true};
        CHECK(dt::addDisplay(p.owner,t,polls,d[i],p.outcome)==dt::Status::Ok);wire(p.apps.back(),dtmOracle(i,true,t));
        p.authId=101+i;
        CHECK(dt::createAuthentication(p.owner,d[i],dt::Protocol::Hdcp1,dt::Content::Type0,polls,a[i],p.outcome)==dt::Status::Ok);
        ta_hdcp_shared_memory expected={};expected.cmd_id=TA_HDCP_COMMAND__HDCP1_CREATE_SESSION;
        expected.hdcp_status=TA_HDCP_STATUS__GENERIC_FAILURE;expected.in_msg.hdcp1_create_session.display_handle=uint8_t(i);
        wire(p.apps.back(),expected);
    }
    const auto writes=p.dataWrites;
    CHECK(dt::addDisplay(p.owner,{0,4,4,4,true},polls,d[4],p.outcome)==dt::Status::Busy);
    CHECK(p.dataWrites==writes);
    for(unsigned i=0;i<4;++i){CHECK(dt::destroyAuthentication(p.owner,a[i],polls,p.outcome)==dt::Status::Ok);
        CHECK(dt::removeDisplay(p.owner,d[i],polls,p.outcome)==dt::Status::Ok);wire(p.apps.back(),dtmOracle(i,false));}
    CHECK(dt::retire(p.owner,p.outcome)==dt::Status::Ok);
}
// These intentionally misbound references exercise rejection before any
// lvalue-to-rvalue access to the wrong type. Object bytes and all I/O counters
// must remain unchanged; no alias is used after an accepted operation.
void aliases(const psp::State& sos) {
    Peer p;p.start(sos,false);
    auto io=p.displayIo();
    // A union supplies enough correctly aligned storage for either reference.
    // Inactive members are named only for address-based preflight rejection.
    union Overlap {
        dt::Result result;dt::State owner;ring::State ring;dt::Io io;
        dt::Display display;dt::Authentication authentication;
        Overlap():result{} {} ~Overlap() {}
    } overlap;
    const auto writes=p.dataWrites+p.writeCalls,guards=p.guards;
    const auto checkBindResult=[&](void* storage,size_t count) {
        const auto* bytes=static_cast<const uint8_t*>(storage);
        const std::vector<uint8_t> before(bytes,bytes+count);
        CHECK(dt::bind(p.state,io,p.owner,*reinterpret_cast<dt::Result*>(storage))==dt::Status::AliasedStorage);
        CHECK(std::memcmp(bytes,before.data(),count)==0);
    };
    CHECK(dt::bind(p.state,io,overlap.owner,overlap.result)==dt::Status::AliasedStorage);
    CHECK(dt::bind(overlap.ring,io,p.owner,overlap.result)==dt::Status::AliasedStorage);
    checkBindResult(&p.allocator,sizeof(p.allocator));
    CHECK(dt::bind(p.state,overlap.io,p.owner,overlap.result)==dt::Status::AliasedStorage);
    checkBindResult(p.scratch.data(),p.scratch.size());
    CHECK(dt::bind(p.state,io,*reinterpret_cast<dt::State*>(&p.allocator),p.outcome)==dt::Status::AliasedStorage);
    CHECK(dt::bind(p.state,*reinterpret_cast<const dt::Io*>(&p.allocator),p.owner,p.outcome)==dt::Status::AliasedStorage);
    CHECK(dt::bind(p.state,*reinterpret_cast<const dt::Io*>(&p.owner),p.owner,p.outcome)==dt::Status::AliasedStorage);
    CHECK(dt::bind(p.state,*reinterpret_cast<const dt::Io*>(&p.state),p.owner,p.outcome)==dt::Status::AliasedStorage);
    auto* inScratch=new(p.scratch.data()) dt::State;
    CHECK(dt::bind(p.state,io,*inScratch,p.outcome)==dt::Status::AliasedStorage&&inScratch->phase()==dt::Phase::Fresh);
    CHECK(dt::bind(p.state,*reinterpret_cast<const dt::Io*>(p.scratch.data()),p.owner,p.outcome)==dt::Status::AliasedStorage);
    inScratch->~State();
    CHECK(p.owner.phase()==dt::Phase::Fresh&&p.guards==guards&&p.dataWrites+p.writeCalls==writes);
    CHECK(dt::bind(p.state,io,p.owner,p.outcome)==dt::Status::Ok);
    dt::Display d;dt::Authentication a;
    const auto guardBefore=p.guards;
    CHECK(dt::addDisplay(p.owner,topology,polls,*reinterpret_cast<dt::Display*>(&p.allocator),p.outcome)==dt::Status::AliasedStorage);
    CHECK(dt::addDisplay(p.owner,*reinterpret_cast<const dt::Topology*>(&p.outcome),polls,d,p.outcome)==dt::Status::AliasedStorage);
    CHECK(dt::addDisplay(p.owner,topology,*reinterpret_cast<const ring::PollOptions*>(&p.outcome),d,p.outcome)==dt::Status::AliasedStorage);
    CHECK(dt::addDisplay(p.owner,topology,polls,overlap.display,overlap.result)==dt::Status::AliasedStorage);
    CHECK(dt::retire(overlap.owner,overlap.result)==dt::Status::AliasedStorage);
    CHECK(p.guards==guardBefore&&p.dataWrites+p.writeCalls==writes);
    CHECK(dt::addDisplay(p.owner,topology,polls,d,p.outcome)==dt::Status::Ok);
    const auto activeWrites=p.dataWrites+p.writeCalls,activeGuards=p.guards;
    CHECK(dt::createAuthentication(p.owner,d,dt::Protocol::Hdcp1,dt::Content::Type0,polls,
        *reinterpret_cast<dt::Authentication*>(&d),p.outcome)==dt::Status::AliasedStorage);
    CHECK(dt::createAuthentication(p.owner,d,dt::Protocol::Hdcp1,dt::Content::Type0,polls,
        *reinterpret_cast<dt::Authentication*>(p.scratch.data()),p.outcome)==dt::Status::AliasedStorage);
    CHECK(dt::removeDisplay(p.owner,overlap.display,polls,overlap.result)==dt::Status::AliasedStorage);
    CHECK(dt::queryAuthentication(p.owner,overlap.authentication,polls,overlap.result)==dt::Status::AliasedStorage);
    CHECK(dt::destroyAuthentication(p.owner,a,*reinterpret_cast<const ring::PollOptions*>(p.scratch.data()),p.outcome)==dt::Status::AliasedStorage);
    CHECK(p.guards==activeGuards&&p.dataWrites+p.writeCalls==activeWrites);
    CHECK(dt::removeDisplay(p.owner,d,polls,p.outcome)==dt::Status::Ok);
    CHECK(dt::retire(p.owner,p.outcome)==dt::Status::Ok);
}
void failuresCase(const psp::State& sos) {
    for(unsigned mode=0;mode<16;++mode) {
        Peer p;p.start(sos);dt::Display display;
        switch(mode) {
        case 0:p.failDataWrite=p.dataWrites+1;break;
        case 1:p.failDataWrite=p.dataWrites+4096;break;
        case 2:p.failDataRead=p.dataReads+1;break;
        case 3:p.failFlush=p.flushes+1;break;
        case 4:p.failWrite=p.writeCalls+1;break;
        case 5:p.autoComplete=false;break;
        case 6:p.responseStatus=5;break;
        case 7:p.respond=false;break;
        case 8:p.appStatus=2;break;
        case 9:p.corrupt=12;break;
        case 10:p.corrupt=16383;break;
        case 11:p.corrupt=16;break;
        case 12:p.failAcquire=p.acquires+3;break;
        case 13:p.failGuard=p.guards+120;break;
        case 14:p.guard=false;break;
        case 15:p.allocator.release(p.dtm.handle);break;
        }
        CHECK(dt::addDisplay(p.owner,topology,polls,display,p.outcome)!=dt::Status::Ok);
        if(mode==7||mode==8)CHECK(p.outcome.pspAcknowledged&&!p.outcome.applicationAcknowledged&&p.outcome.applicationStatus!=0);
        p.sealed();CHECK(!p.forbidden);
        if(mode==5){p.complete();CHECK(p.finish()==ring::Status::Ok);CHECK(p.owner.phase()==dt::Phase::FaultedRetained);
            CHECK(dt::addDisplay(p.owner,topology,polls,display,p.outcome)==dt::Status::InvalidState);p.sealed();}
    }
    for(unsigned mode=0;mode<9;++mode) {
        Peer p;p.start(sos);dt::Display display;dt::Authentication auth;
        CHECK(dt::addDisplay(p.owner,topology,polls,display,p.outcome)==dt::Status::Ok);
        if(mode==0)p.authId=TA_HDCP__INVALID_SESSION;
        if(mode==1)p.appStatus=TA_HDCP_STATUS__GENERIC_FAILURE;
        if(mode==2)p.corrupt=5132;
        const auto created=dt::createAuthentication(p.owner,display,dt::Protocol::Hdcp2,dt::Content::Type0,polls,auth,p.outcome);
        if(mode<3)CHECK(created!=dt::Status::Ok);
        else {
            CHECK(created==dt::Status::Ok);
            if(mode==3)p.appStatus=1;
            if(mode==4)p.protection=2;
            if(mode==5){p.protection=1;p.content=0;}
            if(mode==6)p.content=3;
            if(mode<=6)CHECK(dt::queryAuthentication(p.owner,auth,polls,p.outcome)!=dt::Status::Ok);
            else {p.appStatus=mode==7?1:0;p.corrupt=mode==8?5136:0;
                CHECK(dt::destroyAuthentication(p.owner,auth,polls,p.outcome)!=dt::Status::Ok);}
        }
        p.sealed();
    }
    {Peer p;p.start(sos);dt::Display a,b;dt::Authentication aa,ab;
     CHECK(dt::addDisplay(p.owner,topology,polls,a,p.outcome)==dt::Status::Ok);
     CHECK(dt::addDisplay(p.owner,{0,0,0,0,true},polls,b,p.outcome)==dt::Status::Ok);
     CHECK(dt::createAuthentication(p.owner,a,dt::Protocol::Hdcp1,dt::Content::Type0,polls,aa,p.outcome)==dt::Status::Ok);
     CHECK(dt::createAuthentication(p.owner,b,dt::Protocol::Hdcp2,dt::Content::Type0,polls,ab,p.outcome)==dt::Status::ResponseCorrupt);p.sealed();}
    {Peer p;p.start(sos);dt::Display d;CHECK(dt::addDisplay(p.owner,topology,polls,d,p.outcome)==dt::Status::Ok);
     p.appStatus=TA_DTM_STATUS__INVALID_PARAMETER;CHECK(dt::removeDisplay(p.owner,d,polls,p.outcome)==dt::Status::ApplicationRejected);p.sealed();}
    {Peer p;p.start(sos);dt::Display d;p.loseAfterSharedRead=true;
     CHECK(dt::addDisplay(p.owner,topology,polls,d,p.outcome)==dt::Status::GuardFailed);
     CHECK(p.outcome.pspAcknowledged&&!p.outcome.applicationAcknowledged&&p.finalReadGuards==2);
     p.sealed();}
}
} // namespace
int main() {
    psp::State sos;CHECK(loadTestSos(sos));
    lifecycle(sos,dt::Protocol::Hdcp1);lifecycle(sos,dt::Protocol::Hdcp2);preflight(sos);capacity(sos);aliases(sos);failuresCase(sos);
    std::cout<<"Actual pinned-header ABI oracle and offline display TA protocol: "<<checks<<" checks, "<<failures<<" failures; GPU execution: NOT RUN\n";
    return failures?1:0;
}
