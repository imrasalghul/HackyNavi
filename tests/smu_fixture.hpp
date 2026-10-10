// SPDX-License-Identifier: MIT
// Test-only protocol emulation. Production never links these callbacks.
#ifndef HACKYNAVI_TEST_SMU_FIXTURE_HPP
#define HACKYNAVI_TEST_SMU_FIXTURE_HPP
#include "psp_ring_fixture.hpp"
#include "hackynavi/smu.hpp"
namespace smu_test {
using namespace hackynavi;
using pspring_test::put;
inline Navi31IpLayout ips() {
    auto l=pspring_test::ips(); l.blocks[0].basesDwords[0]=0x1260; return l;
}
struct FirmwareFixture {
    pspring_test::Fixture transport;
    HdpState hdp;
    uint32_t remapMemory=0,remapRegisters=0;
    bool valid=true;
    std::vector<uint8_t> files[7];
    FirmwareFixture() {
        const uint32_t total[]={272,288,288,288,288,288,384};
        const uint32_t header[]={44,64,60,60,60,48,204};
        for(unsigned i=0;i<7;++i) {
            files[i].assign(total[i],0); auto* d=files[i].data();
            put(d,total[i]); put(d+4,header[i]); d[8]=i==5?1:2; d[10]=i==0?1:i==6?3:0;
            d[12]=i==0?13:i==1?6:11; put(d+16,7); put(d+20,(i==1 || i==5)?32:16); put(d+24,256);
            for(unsigned j=256;j<total[i];++j) d[j]=uint8_t(j*53);
            if(i==1) { put(d+36,16); put(d+48,272); put(d+52,16); }
            if(i>=2 && i<=4) { put(d+36,16); put(d+40,256); put(d+44,16); put(d+48,272); put(d+52,0x1000); }
            if(i==5) { put(d+32,16); put(d+40,16); }
            if(i==6) {
                const unsigned offsets[]={116,132,148,156,164,180,196};
                for(unsigned j=0;j<7;++j) { put(d+offsets[j],16); put(d+offsets[j]+4,272+j*16); }
            }
        }
    }
    static bool read(void* context,uint32_t reg,uint32_t& value) {
        auto& s=*static_cast<FirmwareFixture*>(context);
        if(reg==0x44b4) value=s.remapMemory;
        else if(reg==0x44b8) value=s.remapRegisters;
        else if(reg==0x430c) value=24576;
        else if(reg==(0xa000+0x2807)*4) value=0x40000000;
        else if(reg==(0x1260+0xf40)*4) value=0;
        else if(reg==(0xa000+0x4e82)*4) value=0x80000000;
        else return pspring_test::Fixture::read(&s.transport,reg,value);
        return true;
    }
    static bool write(void* context,uint32_t reg,uint32_t value) {
        auto& s=*static_cast<FirmwareFixture*>(context);
        if(reg==0x44b4) s.remapMemory=value;
        else if(reg==0x44b8) s.remapRegisters=value;
        else if(reg==0x7f000) return pspring_test::Fixture::flush(&s.transport);
        else return pspring_test::Fixture::write(&s.transport,reg,value);
        return true;
    }
    static bool flush(void* context) {
        auto& s=*static_cast<FirmwareFixture*>(context); HdpResult r={};
        return flushHdp({context,read,write},s.hdp,r)==HdpStatus::Ok;
    }
    static bool acquire(void* context) {
        return pspring_test::Fixture::acquire(&static_cast<FirmwareFixture*>(context)->transport);
    }
    static void delayUs(void* context,uint32_t us) {
        pspring_test::Fixture::delayUs(&static_cast<FirmwareFixture*>(context)->transport,us);
    }
    static void delayMs(void* context,uint32_t ms) {
        pspring_test::Fixture::delayMs(&static_cast<FirmwareFixture*>(context)->transport,ms);
    }
    bool ready(gfxfw::State& firmware) {
        psp::State sos;
        if(!pspring_test::loadTestSos(sos) || transport.create(sos)!=pspring::Status::Ok) return false;
        auto& t=transport;
        if(pspring::submitLoadToc(t.io(),t.state,t.input.handle,0,16,t.result)!=pspring::Status::Ok ||
            pspring::wait(t.io(),t.state,pspring_test::polls,t.result)!=pspring::Status::Ok ||
            pspring::submitSetupTmr(t.io(),t.state,t.tmr.handle,t.result)!=pspring::Status::Ok ||
            pspring::wait(t.io(),t.state,pspring_test::polls,t.result)!=pspring::Status::Ok) return false;
        HdpResult hr={};
        if(configureHdp({true,0x44b4,0x44b8,0x430c,0x7f000,0x7f004,0x80000},
            {this,read,write},hdp,hr)!=HdpStatus::Ok) return false;
        const gfxfw::Files f={{files[0].data(),files[0].size()},{files[1].data(),files[1].size()},
            {files[2].data(),files[2].size()},{files[3].data(),files[3].size()},
            {files[4].data(),files[4].size()},{files[5].data(),files[5].size()},
            {files[6].data(),files[6].size()}};
        gfxfw::Result r={};
        return gfxfw::load(f,ips(),0xc8,t.allocator,t.input.handle,{this,0x80000,read,write},
            {this,read,write},hdp,{this,read,write,flush,acquire,delayUs,delayMs},t.state,
            pspring_test::polls,firmware,r)==gfxfw::Status::Ok && !t.forbidden;
    }
};
struct Event { char type; uint32_t reg,value; };
struct Simulation {
    smu::Layout layout{};
    smu::State state;
    smu::Result result{};
    std::vector<Event> events;
    uint32_t response=0,argument=0,message=0,readCalls=0,writeCalls=0,delays=0;
    uint32_t failRead=0,failWrite=0,onesRead=0,responseCode=1,busyReads=0,busyAfter=0;
    uint32_t badCommandNumber=0,badResponse=0,commandNumber=0;
    uint32_t smuVersion=0x4e9900,interfaceVersion=0x40;
    uint32_t reset=0x1f,cp=0,boot=0x80000000,resetReads=0,releaseAfter=0,loseResetAt=0;
    bool forbidden=false,valid=false;
    Simulation() { valid=smu::buildLayout(ips(),0x80000,layout)==smu::Status::Ok; }
    smu::Io io() { return {this,read,write,delay}; }
    static bool read(void* context,uint32_t reg,uint32_t& value) {
        auto& s=*static_cast<Simulation*>(context); ++s.readCalls;
        if(reg==s.layout.response) value=s.busyReads ? (--s.busyReads,0) : s.response;
        else if(reg==s.layout.argument) value=s.argument;
        else if(reg==s.layout.imuReset) {
            ++s.resetReads;
            value=s.releaseAfter && s.resetReads<s.releaseAfter ? 0 : s.reset;
            if(s.loseResetAt==s.resetReads) value=0;
        } else if(reg==s.layout.cpStatus) value=s.cp;
        else if(reg==s.layout.bootloadStatus) value=s.boot;
        else { s.forbidden=true; value=0; }
        if(s.readCalls==s.onesRead) value=UINT32_MAX;
        s.events.push_back({'r',reg,value}); return s.readCalls!=s.failRead;
    }
    static bool write(void* context,uint32_t reg,uint32_t value) {
        auto& s=*static_cast<Simulation*>(context); ++s.writeCalls;
        s.events.push_back({'w',reg,value});
        if(reg==s.layout.response) { if(value) s.forbidden=true; s.response=value; }
        else if(reg==s.layout.argument) s.argument=value;
        else if(reg==s.layout.message) {
            if(s.response || s.argument) s.forbidden=true;
            ++s.commandNumber; s.message=value;
            if(value==2) s.argument=s.smuVersion;
            else if(value==3) s.argument=s.interfaceVersion;
            else if(value!=0x28 && value!=0x29) s.forbidden=true;
            s.response=s.commandNumber==s.badCommandNumber ? s.badResponse : s.responseCode;
            s.busyReads=s.busyAfter;
        } else s.forbidden=true;
        return s.writeCalls!=s.failWrite;
    }
    static void delay(void* context,uint32_t us) {
        auto& s=*static_cast<Simulation*>(context); ++s.delays; s.events.push_back({'u',0,us});
    }
    smu::Status initialize(const gfxfw::State& fw) {
        return smu::initialize(layout,fw,io(),{3,2},state,result);
    }
    smu::Status awake(const gfxfw::State& fw) {
        const auto status=initialize(fw); if(status!=smu::Status::Ok) return status;
        return smu::disableGfxOff(io(),{3,2},state,result);
    }
};
} // namespace smu_test
#endif
