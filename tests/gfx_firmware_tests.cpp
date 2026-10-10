// SPDX-License-Identifier: MIT
// Host simulations and offline authentic-file parsing; no hardware execution.
#include "gfx_firmware_fixture.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <vector>
using namespace hackynavi;
namespace g=hackynavi::gfxfw;
using namespace gfxfw_test;
namespace {
void parserTests() {
    Fixtures f; g::ImuFirmware imu={}; g::RlcFirmware rlc={};
    CHECK(g::parseImu(f.b[5].data(),f.b[5].size(),imu)==Status::Ok);
    CHECK(imu.instruction.offset==256 && imu.instruction.size==16 && imu.data.offset==272 && imu.data.size==16);
    CHECK(g::parseRlc(f.b[6].data(),f.b[6].size(),rlc)==Status::Ok);
    for(unsigned i=0;i<7;++i) CHECK(rlc.segments[i].offset==272+16*i && rlc.segments[i].size==16);
    CHECK(rlc.segments[7].offset==256 && rlc.segments[7].size==16);
    for(unsigned which=5;which<7;++which) for(size_t n=0;n<f.b[which].size();++n) {
        if(which==5) {
            CHECK(g::parseImu(f.b[which].data(),n,imu)!=Status::Ok); CHECK(!imu.header.sizeBytes && !imu.instruction.size);
        } else {
            CHECK(g::parseRlc(f.b[which].data(),n,rlc)!=Status::Ok); CHECK(!rlc.header.sizeBytes && !rlc.segments[7].size);
        }
    }
    for(unsigned mode=0;mode<8;++mode) {
        auto b=f.b[5];
        if(mode==0) put16(b,8,2); if(mode==1) put16(b,10,1); if(mode==2) put32(b,4,44);
        if(mode==3) put32(b,32,UINT32_MAX); if(mode==4) put32(b,40,UINT32_MAX);
        if(mode==5) put32(b,32,0); if(mode==6) put32(b,40,0); if(mode==7) put32(b,32,15);
        CHECK(g::parseImu(b.data(),b.size(),imu)!=Status::Ok); CHECK(!imu.header.sizeBytes);
    }
    // Explicit IMU fields have different semantics; PSP must not treat them as
    // absolute file addresses, even when they resemble one or overflow one.
    auto b=f.b[5]; put32(b,36,UINT32_MAX); put32(b,44,UINT32_MAX);
    CHECK(g::parseImu(b.data(),b.size(),imu)==Status::Ok && imu.data.offset==272);
    const unsigned off[]={116,132,148,156,164,180,196};
    for(unsigned i=0;i<7;++i) {
        auto a=f.b[6]; put32(a,off[i],0); put32(a,off[i]+4,0);
        CHECK(g::parseRlc(a.data(),a.size(),rlc)==Status::Ok && !rlc.segments[i].size);
        for(unsigned mode=0;mode<5;++mode) {
            a=f.b[6];
            if(mode==0) put32(a,off[i]+4,UINT32_MAX); if(mode==1) put32(a,off[i],UINT32_MAX);
            if(mode==2) put32(a,off[i]+4,256); if(mode==3) put32(a,off[i]+4,273+i*16);
            if(mode==4) put32(a,off[i],15);
            CHECK(g::parseRlc(a.data(),a.size(),rlc)==Status::Malformed && !rlc.header.sizeBytes);
        }
    }
    for(unsigned mode=0;mode<3;++mode) {
        auto a=f.b[6]; if(mode==0) put16(a,8,3); if(mode==1) put16(a,10,4); if(mode==2) put32(a,4,200);
        CHECK(g::parseRlc(a.data(),a.size(),rlc)!=Status::Ok && !rlc.header.sizeBytes);
    }
}
void planTests() {
    Fixtures f; g::Plan p={}; g::Result r={};
    CHECK(g::plan(f.files(),ips(),0xc8,p,r)==g::Status::Ok && p.count==24 && p.stagingBytes==4096);
    const uint32_t types[]={18,71,72,87,88,89,90,91,92,93,94,95,96,97,68,69,22,20,21,26,48,25,7,8};
    for(unsigned i=0;i<24;++i) CHECK(p.segments[i].pspType==types[i]);
    CHECK(p.segments[3].file==g::File::Pfp && p.segments[4].file==g::File::Me && p.segments[5].file==g::File::Mec);
    for(unsigned i=6;i<=13;++i) CHECK(p.segments[i].range.offset==272 && p.segments[i].range.size==16);
    auto a=f; put32(a.b[2],40,272); // Linux PSP code uses COMMON offset despite explicit offset.
    CHECK(g::plan(a.files(),ips(),0xc8,p,r)==g::Status::Ok && p.segments[3].range.offset==256);
    a=f; put32(a.b[6],116,0); put32(a.b[6],120,0);
    CHECK(g::plan(a.files(),ips(),0xc8,p,r)==g::Status::Ok && p.count==23 && p.segments[16].pspType==20);
    for(unsigned which=0;which<7;++which) for(unsigned mode=0;mode<5;++mode) {
        a=f;
        if(mode==0) a.b[which].clear(); if(mode==1) a.b[which].pop_back();
        if(mode==2) put16(a.b[which],12,99); if(mode==3) put16(a.b[which],14,1); if(mode==4) put16(a.b[which],8,8);
        CHECK(g::plan(a.files(),ips(),0xc8,p,r)!=g::Status::Ok && !p.count && !p.stagingBytes);
        CHECK(static_cast<unsigned>(r.file)==which);
    }
    for(unsigned revision=0;revision<256;++revision) if(revision!=0xc8)
        CHECK(g::plan(f.files(),ips(),uint8_t(revision),p,r)==g::Status::UnsupportedDevice && !p.count);
    for(unsigned block=0;block<6;++block) for(unsigned field=0;field<8;++field) {
        auto l=ips(); auto& x=l.blocks[block];
        if(field==0) ++x.hardwareId; if(field==1) ++x.major; if(field==2) ++x.minor; if(field==3) ++x.revision;
        if(field==4) ++x.instance; if(field==5) ++x.dieId; if(field==6) x.baseCount=0; if(field==7) x.baseCount=9;
        CHECK(g::plan(f.files(),l,0xc8,p,r)==g::Status::UnsupportedLayout && !p.count);
    }
    a=f; put32(a.b[2],48,256);
    CHECK(g::plan(a.files(),ips(),0xc8,p,r)==g::Status::InvalidSegment);
    a=f; put32(a.b[1],48,256);
    CHECK(g::plan(a.files(),ips(),0xc8,p,r)==g::Status::InvalidSegment);
}
void verifyOfficialTransport(const Fixtures&);
void official(const char* directory) {
    const char* names[]={"smu_13_0_0.bin","sdma_6_0_0.bin","gc_11_0_0_pfp.bin","gc_11_0_0_me.bin",
        "gc_11_0_0_mec.bin","gc_11_0_0_imu.bin","gc_11_0_0_rlc.bin"};
    Fixtures f;
    for(unsigned i=0;i<7;++i) {
        std::ifstream in(std::string(directory)+"/"+names[i],std::ios::binary);
        CHECK(bool(in)); f.b[i]=std::vector<uint8_t>(std::istreambuf_iterator<char>(in),{});
    }
    g::Plan p={}; g::Result r={};
    CHECK(g::plan(f.files(),ips(),0xc8,p,r)==g::Status::Ok);
    CHECK(p.count==23 && p.stagingBytes==270336); // MEC267008 exceeds SMU261632.
    CHECK(p.segments[0].pspType==18 && p.segments[0].range.offset==256 && p.segments[0].range.size==261632);
    CHECK(p.segments[14].range.offset==256 && p.segments[14].range.size==66048);
    CHECK(p.segments[15].range.offset==66304 && p.segments[15].range.size==66048);
    const uint32_t type[]={20,21,26,48,25,7,8};
    const uint32_t offset[]={25344,27904,64544,130592,163872,172576,256};
    const uint32_t bytes[]={2560,36640,66048,33280,8704,12800,25088};
    for(unsigned i=0;i<7;++i) CHECK(p.segments[16+i].pspType==type[i] && p.segments[16+i].range.offset==offset[i] && p.segments[16+i].range.size==bytes[i]);
    verifyOfficialTransport(f);
}
void transportTests() {
    Fixtures f; g::Plan plan={}; g::Result r={};
    CHECK(g::plan(f.files(),ips(),0xc8,plan,r)==g::Status::Ok);
    {
        Simulation s; g::State state;
        CHECK(s.run(f.files(),state,r)==g::Status::Ok);
        CHECK(state.autoloadComplete() && state.loadedCount()==24 && !state.needsStagingRetention());
        CHECK(r.acceptedSegments==24 && r.verifiedSegments==24 && r.autoloadSubmitted && r.firmwareMayBeLoaded);
        CHECK(s.received.size()==25 && s.autoloads==1 && s.loadCount==24 && !s.failed);
        for(unsigned i=0;i<24;++i) {
            const auto& segment=plan.segments[i]; const auto& expected=f.b[static_cast<unsigned>(segment.file)];
            CHECK(s.received[i].id==6 && s.received[i].type==segment.pspType);
            CHECK(s.received[i].bytes==std::vector<uint8_t>(expected.begin()+segment.range.offset,expected.begin()+segment.range.offset+segment.range.size));
            CHECK(state.loaded(i)->pspType==segment.pspType && state.loaded(i)->tmrMcAddress==s.tmrMc+uint64_t(segment.pspType)*4096);
        }
        CHECK(!state.loaded(24) && s.received.back().id==0x21);
        const size_t accesses=s.accesses; CHECK(s.run(f.files(),state,r)==g::Status::InvalidState && s.accesses==accesses);
    }
    for(unsigned mode=0;mode<10;++mode) {
        Simulation s; g::State state;
        if(mode==0)s.registers[mesControl]=0; if(mode==1)s.badBoot=true; if(mode==2)s.busyCp=true;
        if(mode==3)s.onesStatus=true; if(mode==4)s.mismatch=true; if(mode==5)s.failPublish=true;
        if(mode==6)s.rejectLoad=4; if(mode==7)s.stallLoad=4; if(mode==8)s.badMesAfter=true; if(mode==9)s.failAccess=1;
        const auto status=s.run(f.files(),state,r);
        const g::Status wanted[]={g::Status::ActiveMes,g::Status::AutoloadTimeout,g::Status::AutoloadTimeout,
            g::Status::DeviceUnavailable,g::Status::TransferFailed,g::Status::VisibilityFailed,
            g::Status::PspFailed,g::Status::PspFailed,g::Status::ActiveMes,g::Status::ReadFailed};
        CHECK(status==wanted[mode] && state.phase()==g::Phase::Faulted && !state.autoloadComplete() && !s.failed);
        if(mode==0 || mode==9)CHECK(s.writes==0 && s.received.empty());
        if(mode==4 || mode==5)CHECK(s.received.empty());
        if(mode==6 || mode==7)CHECK(s.loadCount==4 && r.acceptedSegments==3 && !s.autoloads && state.needsStagingRetention());
        if(mode==1 || mode==2)CHECK(r.polls==3 && s.autoloads==1);
        const size_t accesses=s.accesses; const bool retain=state.needsStagingRetention();
        CHECK(s.run(f.files(),state,r)==g::Status::InvalidState && s.accesses==accesses && r.stagingRetentionRequired==retain);
    }
    {
        Simulation s; g::State state; const VramAllocationHandle aliases[]={s.ringBuffer.handle,s.command.handle,s.fence.handle,s.tmr.handle};
        for(auto handle:aliases) {
            CHECK(s.run(f.files(),state,r,handle)!=g::Status::Ok && !s.accesses && !s.writes);
            CHECK(state.phase()==g::Phase::Fresh);
        }
        auto bad=f; bad.b[6].pop_back();
        CHECK(s.run(bad.files(),state,r)==g::Status::FirmwareRejected && !s.accesses && !s.writes);
    }
    // Each PSP boundary can reject or stop responding. Never overwrite the
    // pending input or send a later component/autoload after that boundary.
    for(unsigned load=1;load<=24;++load) for(bool stall:{false,true}) {
        Simulation s; g::State state;
        if(stall)s.stallLoad=load; else s.rejectLoad=load;
        CHECK(s.run(f.files(),state,r)==g::Status::PspFailed);
        CHECK(s.loadCount==load && r.acceptedSegments==load-1 && s.received.size()==load && !s.autoloads);
        CHECK(state.loadedCount()==load-1 && state.phase()==g::Phase::Faulted && state.needsStagingRetention());
        const auto& last=s.received.back().bytes;
        const size_t staging=size_t(s.staging.range.offset-Simulation::vramStart);
        CHECK(last==std::vector<uint8_t>(s.memory.begin()+staging,s.memory.begin()+staging+last.size()));
    }
}
void verifyOfficialTransport(const Fixtures& f) {
    Simulation s; g::State state; g::Plan plan={}; g::Result r={};
    CHECK(g::plan(f.files(),ips(),0xc8,plan,r)==g::Status::Ok);
    CHECK(s.run(f.files(),state,r)==g::Status::Ok && state.autoloadComplete());
    CHECK(s.received.size()==24 && state.loadedCount()==23 && !s.failed);
    for(size_t i=0;i<plan.count;++i) {
        const auto& segment=plan.segments[i]; const auto& b=f.b[static_cast<unsigned>(segment.file)];
        CHECK(s.received[i].type==segment.pspType);
        CHECK(s.received[i].bytes==std::vector<uint8_t>(b.begin()+segment.range.offset,b.begin()+segment.range.offset+segment.range.size));
    }
    CHECK(s.received.back().id==0x21 && s.autoloads==1);
}
} // namespace
int main(int argc,char** argv) {
    parserTests(); planTests(); transportTests(); if(argc>1) official(argv[1]);
    std::cout<<checks<<" graphics firmware checks; "<<failures<<" failures\n";
    return failures?1:0;
}
