// SPDX-License-Identifier: MIT
// Offline mailbox and power-status tests. No hardware power state is inferred.
#include "smu_fixture.hpp"
#include <iostream>
#include <type_traits>
using namespace smu_test;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; std::cerr<<"FAIL "<<__LINE__<<": " #x "\n"; } } while(0)
static_assert(!std::is_copy_constructible<smu::State>::value,"state cannot copy");
static_assert(!std::is_move_constructible<smu::State>::value,"state cannot move");
static_assert(!std::is_copy_assignable<smu::State>::value,"state cannot assign");
void layoutTests() {
    smu::Layout l={};
    CHECK(smu::buildLayout(ips(),0x80000,l)==smu::Status::Ok);
    CHECK(l.message==(0x16000+0x282)*4 && l.argument==(0x16000+0x292)*4 && l.response==(0x16000+0x29a)*4);
    CHECK(l.imuReset==(0xa000+0x40bc)*4 && l.cpStatus==(0x1260+0xf40)*4 && l.bootloadStatus==(0xa000+0x4e82)*4);
    CHECK(smu::buildLayout(ips(),(0x16000+0x29a)*4+3,l)==smu::Status::OutsideBar && !l.valid);
    CHECK(smu::buildLayout(ips(),(0x16000+0x29a)*4+4,l)==smu::Status::Ok);
    for(unsigned i=0;i<6;++i) for(unsigned field=0;field<8;++field) {
        auto ip=ips(); auto& b=ip.blocks[i];
        if(field==0) ++b.hardwareId; if(field==1) ++b.major; if(field==2) ++b.minor;
        if(field==3) ++b.revision; if(field==4) ++b.instance; if(field==5) ++b.dieId;
        if(field==6) b.baseCount=0; if(field==7) b.baseCount=9;
        CHECK(smu::buildLayout(ip,0x80000,l)==smu::Status::UnsupportedLayout && !l.valid);
    }
    auto ip=ips(); ip.blocks[0].baseCount=1;
    CHECK(smu::buildLayout(ip,0x80000,l)==smu::Status::OutsideBar && !l.valid);
    ip=ips(); ip.blocks[3].basesDwords[0]=UINT32_MAX;
    CHECK(smu::buildLayout(ip,UINT64_MAX,l)==smu::Status::OutsideBar && !l.valid);
}
void success(const gfxfw::State& fw) {
    Simulation s;
    CHECK(s.valid && s.response==0 && s.initialize(fw)==smu::Status::Ok);
    CHECK(s.state.phase()==smu::Phase::Ready && !s.state.awake() && !s.state.gfxOffDisabled());
    CHECK(s.state.smuVersion()==s.smuVersion && s.state.interfaceVersion()==s.interfaceVersion);
    CHECK(s.writeCalls==6 && s.readCalls==5 && s.events.size()==11 && !s.forbidden);
    CHECK(s.events[0].type=='w' && s.events[0].reg==s.layout.response && s.events[0].value==0);
    CHECK(s.events[1].type=='w' && s.events[1].reg==s.layout.argument && s.events[1].value==0);
    CHECK(s.events[2].type=='w' && s.events[2].reg==s.layout.message && s.events[2].value==2);
    CHECK(s.events[5].type=='r' && s.events[5].reg==s.layout.response);
    CHECK(s.events[8].type=='w' && s.events[8].value==3);
    CHECK(smu::disableGfxOff(s.io(),{3,2},s.state,s.result)==smu::Status::Ok);
    CHECK(s.state.awake() && s.state.gfxOffDisabled() && s.message==0x29 && s.result.gfxOffDisabled);
    CHECK(s.result.imuReset==0x1f && s.result.cpStatus==0 && s.result.bootloadStatus==0x80000000);
    CHECK(s.result.writes==3 && s.result.reads==6 && s.result.stage==smu::Stage::Complete);
    const uint32_t written=s.writeCalls;
    CHECK(smu::verifyAwake(s.io(),{3,2},s.state,s.result)==smu::Status::Ok && s.writeCalls==written);
    CHECK(smu::allowGfxOffAfterQuiescence(s.io(),{3,2},s.state,s.result)==smu::Status::Ok);
    CHECK(s.state.phase()==smu::Phase::Ready && !s.state.awake() && !s.state.gfxOffDisabled() && s.message==0x28);
    CHECK(smu::verifyAwake(s.io(),{3,2},s.state,s.result)==smu::Status::InvalidState);
    s.releaseAfter=s.resetReads+2;
    CHECK(smu::disableGfxOff(s.io(),{3,2},s.state,s.result)==smu::Status::Ok && s.delays==1);
    CHECK(!s.forbidden);
}
void preflight(const gfxfw::State& fw) {
    gfxfw::State absent; Simulation s;
    CHECK(s.initialize(absent)==smu::Status::InvalidState && s.writeCalls==0 && s.readCalls==0);
    CHECK(smu::disableGfxOff(s.io(),{3,2},s.state,s.result)==smu::Status::InvalidState);
    for(unsigned i=0;i<11;++i) {
        Simulation x; auto io=x.io(); auto l=x.layout; smu::PollOptions p={3,2};
        if(i==0) io.read32=nullptr; if(i==1) io.write32=nullptr; if(i==2) io.delayUs=nullptr;
        if(i==3) p.maxPolls=0; if(i==4) p.delayUs=0;
        if(i==5) p.maxPolls=1000001; if(i==6) p.delayUs=1001;
        if(i==7) { p.maxPolls=1000000; p.delayUs=11; }
        if(i==8) l.valid=false; if(i==9) ++l.response; if(i==10) l.imuReset=l.message;
        CHECK(smu::initialize(l,fw,io,p,x.state,x.result)!=smu::Status::Ok);
        CHECK(x.state.phase()==smu::Phase::Fresh && x.writeCalls==0 && x.readCalls==0);
    }
}
void initializeFailures(const gfxfw::State& fw) {
    for(unsigned mode=0;mode<3;++mode) for(unsigned n=1;n<=(mode==2?6U:5U);++n) {
        Simulation s;
        if(mode==0) s.failRead=n; if(mode==1) s.onesRead=n; if(mode==2) s.failWrite=n;
        CHECK(s.initialize(fw)==(mode==0?smu::Status::ReadFailed:mode==1?smu::Status::DeviceUnavailable:smu::Status::WriteFailed));
        CHECK(s.state.phase()==smu::Phase::Faulted && !s.state.awake());
        CHECK(s.initialize(fw)==smu::Status::InvalidState && !s.forbidden);
    }
    const uint32_t replies[]={0,0xff,0xfe,0xfd,0xfc,2,0x100};
    const smu::Status statuses[]={smu::Status::ResponseTimeout,smu::Status::CommandFailed,smu::Status::UnknownCommand,
        smu::Status::BadPrerequisite,smu::Status::Busy,smu::Status::UnknownResponse,smu::Status::UnknownResponse};
    for(unsigned command=1;command<=2;++command) for(unsigned i=0;i<7;++i) {
        Simulation s; s.badCommandNumber=command; s.badResponse=replies[i];
        CHECK(s.initialize(fw)==statuses[i]); CHECK(s.commandNumber==command && s.state.phase()==smu::Phase::Faulted);
        CHECK(!s.forbidden);
    }
    for(unsigned i=0;i<2;++i) {
        Simulation s; if(i==0) s.smuVersion=0; else s.interfaceVersion=0;
        CHECK(s.initialize(fw)==smu::Status::InvalidVersion && !s.state.awake());
    }
    Simulation s; s.busyAfter=2;
    CHECK(s.initialize(fw)==smu::Status::Ok && s.delays==4);
}
void disableFailures(const gfxfw::State& fw) {
    for(unsigned mode=0;mode<3;++mode) for(unsigned n=1;n<=(mode==2?3U:6U);++n) {
        Simulation s; CHECK(s.initialize(fw)==smu::Status::Ok);
        if(mode==0) s.failRead=s.readCalls+n; if(mode==1) s.onesRead=s.readCalls+n;
        if(mode==2) s.failWrite=s.writeCalls+n;
        CHECK(smu::disableGfxOff(s.io(),{3,2},s.state,s.result)==
            (mode==0?smu::Status::ReadFailed:mode==1?smu::Status::DeviceUnavailable:smu::Status::WriteFailed));
        CHECK(!s.state.awake() && s.state.phase()==smu::Phase::Faulted);
        CHECK(smu::disableGfxOff(s.io(),{3,2},s.state,s.result)==smu::Status::InvalidState);
        CHECK(!s.forbidden);
    }
    for(unsigned bit=0;bit<5;++bit) {
        Simulation s; CHECK(s.initialize(fw)==smu::Status::Ok); s.reset=0x1f^(1U<<bit);
        CHECK(smu::disableGfxOff(s.io(),{3,2},s.state,s.result)==smu::Status::AwakeTimeout);
        CHECK(!s.state.awake() && s.state.gfxOffDisabled() && s.resetReads==3 && s.result.reads==5);
        // No ordinary CP/BOOTLOAD register access until every RESETB releases.
        for(const auto& e:s.events) CHECK(e.reg!=s.layout.cpStatus && e.reg!=s.layout.bootloadStatus);
    }
    for(unsigned mode=0;mode<5;++mode) {
        Simulation s; CHECK(s.initialize(fw)==smu::Status::Ok);
        if(mode==0) s.cp=1; if(mode==1) s.boot=0;
        if(mode==2) s.response=0;
        if(mode==3) { s.badCommandNumber=3; s.badResponse=0xfc; }
        if(mode==4) { s.loseResetAt=2; s.releaseAfter=3; }
        const auto status=smu::disableGfxOff(s.io(),{3,2},s.state,s.result);
        CHECK(status==(mode<2?smu::Status::AwakeTimeout:mode==2?smu::Status::ResponseTimeout:
            mode==3?smu::Status::Busy:smu::Status::Ok));
        CHECK(s.state.awake()==(mode==4));
        if(mode==2) CHECK(s.commandNumber==2 && s.result.writes==0);
    }
    Simulation s; CHECK(s.awake(fw)==smu::Status::Ok); s.reset=0;
    CHECK(smu::verifyAwake(s.io(),{3,2},s.state,s.result)==smu::Status::AwakeTimeout);
    CHECK(!s.state.awake() && s.result.writes==0);
}
void allowFailure(const gfxfw::State& fw) {
    for(unsigned n=1;n<=3;++n) {
        Simulation s; CHECK(s.awake(fw)==smu::Status::Ok); s.failWrite=s.writeCalls+n;
        CHECK(smu::allowGfxOffAfterQuiescence(s.io(),{3,2},s.state,s.result)==smu::Status::WriteFailed);
        CHECK(!s.state.awake() && !s.state.gfxOffDisabled());
        CHECK(smu::verifyAwake(s.io(),{3,2},s.state,s.result)==smu::Status::InvalidState);
    }
}
} // namespace
int main() {
    FirmwareFixture f; gfxfw::State firmware; CHECK(f.ready(firmware));
    layoutTests(); success(firmware); preflight(firmware); initializeFailures(firmware);
    disableFailures(firmware); allowFailure(firmware);
    std::cout<<checks<<" checks, "<<failures<<" failures (offline SMU13 mailbox emulator)\n";
    return failures?1:0;
}
