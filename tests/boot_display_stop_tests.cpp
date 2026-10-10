// SPDX-License-Identifier: MIT
// Host-only MMIO model, never a hardware or display-execution test.
#include "hackynavi/boot_display_stop_capture.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
using namespace hackynavi;
namespace bd=hackynavi::bootdisplay;namespace bs=hackynavi::bootdisplaystop;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<"line "<<__LINE__<<": " #x "\n";}}while(0)
void put16(std::vector<uint8_t>& b,size_t at,uint16_t v){b[at]=uint8_t(v);b[at+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t at,uint32_t v){put16(b,at,uint16_t(v));put16(b,at+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t at){uint16_t v=0;for(;at<b.size();++at)v=uint16_t(v+b[at]);return v;}
bd::Layout layout() {
    std::vector<uint8_t>b(180,0);put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);
    put16(b,10,180);put16(b,12,60);put16(b,16,120);put32(b,60,0x53445049);
    put16(b,64,3);put16(b,66,120);put16(b,72,1);put16(b,76,140);put16(b,142,2);
    put16(b,144,271);b[147]=3;b[148]=3;b[149]=2;put32(b,156,0x1000);put32(b,160,0x10000);
    put16(b,164,255);b[167]=2;b[168]=13;put32(b,172,0x16000);put32(b,176,0x16000);
    put16(b,14,sum(b,60));put16(b,8,sum(b,10));bd::Layout l={};
    CHECK(bd::buildLayout(b.data(),b.size(),0x100000,l)==bd::Status::Ok);return l;
}
struct Model {
    bd::Layout l;MemoryLayout memory={};bs::State state;
    std::map<uint32_t,uint32_t> registers;std::vector<uint32_t> reads,writes;
    size_t failRead=0,onesRead=0,failWrite=0,dropWrite=0;
    size_t changeAt=0;uint32_t changeRegister=0,changeValue=0;
    bool applyFailure=false,neverDrain=false;unsigned drainAfter=0,remaining[4]={};bool written[4]={};
    Model(const bd::Layout& selected=layout(),unsigned off=0,unsigned blank=0,uint32_t offValue=0x90000000):l(selected) {
        CHECK(deriveMemoryLayout({0x100,0x6ff,0x1000,24ULL<<30,0x8000000000ULL,256ULL<<20},memory)==MemoryLayoutStatus::Ok);
        registers[l.sos]=0;
        for(uint32_t reg:l.vga)registers[reg]=0;
        const uint32_t global[]={0x100,0x6ff,0x1000,0xffffff,0};
        for(size_t i=0;i<5;++i)registers[l.global[i]]=global[i];
        for(unsigned p=0;p<4;++p) {
            registers[l.power[p]]=(off&(1U<<p))?offValue:0;
            registers[l.powerConfig[p]]=(off&(1U<<p))?0x100:0;
            for(uint32_t reg:l.pipe[p])registers[reg]=0;
            registers[l.pipe[p][0]+4]=1;
            set(p,bd::PipeRegister::Control,(blank&(1U<<p))?3:2);
            set(p,bd::PipeRegister::SurfaceConfig,8);set(p,bd::PipeRegister::Pitch,2047);
            set(p,bd::PipeRegister::ViewportDimension,(1080U<<16)|1920);
            set(p,bd::PipeRegister::TlbControl,0x19);set(p,bd::PipeRegister::ApertureLow,0x4000);
            set(p,bd::PipeRegister::ApertureHigh,0x1bfff);
            const uint64_t mc=memory.vramStart+0x02000000+uint64_t(p)*0x1000000;
            for(auto reg:{bd::PipeRegister::AddressLo,bd::PipeRegister::InUseLo,bd::PipeRegister::EarliestLo})set(p,reg,uint32_t(mc));
            for(auto reg:{bd::PipeRegister::AddressHi,bd::PipeRegister::InUseHi,bd::PipeRegister::EarliestHi})set(p,reg,uint32_t(mc>>32));
            if(off&(1U<<p)){for(uint32_t reg:l.pipe[p])registers[reg]=UINT32_MAX;registers[l.pipe[p][0]+4]=UINT32_MAX;}
        }
    }
    void set(unsigned p,bd::PipeRegister r,uint32_t v){registers[l.pipe[p][static_cast<size_t>(r)]]=v;}
    void desiredOn(unsigned mask=15,uint32_t config=1) {
        for(unsigned p=0;p<4;++p)if(mask&(1U<<p)) {
            registers[l.power[p]]=0x10000000;registers[l.powerConfig[p]]=config;
        }
    }
    bool readable(unsigned p) {
        return (registers[l.power[p]]==0 || registers[l.power[p]]==0x10000000) &&
            registers[l.powerConfig[p]]<=1;
    }
    static bool read(void* c,uint32_t reg,uint32_t& value) {
        auto& m=*static_cast<Model*>(c);m.reads.push_back(reg);CHECK(m.registers.count(reg)==1);
        if(m.reads.size()==m.changeAt)m.registers[m.changeRegister]=m.changeValue;
        for(unsigned p=0;p<4;++p){
            if(reg==m.l.pipe[p][0] && m.written[p] && !m.neverDrain) {
                if(m.remaining[p])--m.remaining[p];else m.registers[reg]|=2;
            }
            bool window=reg==m.l.pipe[p][0]+4;for(uint32_t client:m.l.pipe[p])window=window || reg==client;
            if(window){CHECK(m.readable(p));CHECK(m.reads.size()>1 && m.reads[m.reads.size()-2]==m.l.power[p]);}
        }
        value=m.reads.size()==m.onesRead?UINT32_MAX:m.registers[reg];return m.reads.size()!=m.failRead;
    }
    static bool write(void* c,uint32_t reg,uint32_t value) {
        auto& m=*static_cast<Model*>(c);m.writes.push_back(reg);unsigned p=0;
        while(p<4 && reg!=m.l.pipe[p][0])++p;CHECK(p<4);if(p==4)return false;
        CHECK(m.readable(p) && (m.registers[reg+4]&1));
        CHECK((value&0x1001)==1 && !(value&0x84000000));
        const bool fail=m.writes.size()==m.failWrite;
        if((!fail || m.applyFailure) && m.writes.size()!=m.dropWrite) {
            m.registers[reg]=value&~0x70ff000aU;m.written[p]=true;m.remaining[p]=m.drainAfter;
        }
        return !fail;
    }
    static void delay(void*,uint32_t us){CHECK(us==1);}
    bs::Io io(){return {this,read,write,delay};}
    bool capture(){bs::Result r={};const auto s=bs::capture(l,memory,io(),state,r);
        CHECK(s==bs::Status::Ok && state.phase()==bs::Phase::Armed && state.capture().complete && r.retentionRequired);
        reads.clear();writes.clear();return s==bs::Status::Ok;}
    bs::Status stop(bs::Result& r){return bs::stop(io(),{4,1},state,r);}
};
void golden() {
    for(unsigned off=0;off<16;++off)for(unsigned blank:{0U,15U}){
        Model m(layout(),off,blank);bs::Result r={};if(!m.capture())continue;
        // Loading PSP after arming is legitimate; the retained cold epoch
        // must not demand SOS==0 during its own ordinary terminal teardown.
        m.registers[m.l.sos]=1;m.drainAfter=2;
        CHECK(m.stop(r)==bs::Status::Ok && m.state.phase()==bs::Phase::Stopped && !r.retentionRequired);
        unsigned expected=0;for(unsigned p=0;p<4;++p)if(!((off|blank)&(1U<<p)))++expected;
        CHECK(m.writes.size()==expected && r.writes==expected && r.blankedMask==(unsigned(~(off|blank))&15));
        CHECK(m.state.capture().poweredOffMask==off);
        CHECK(m.stop(r)==bs::Status::InvalidState && !r.retentionRequired);
    }
    {Model m;m.set(0,bd::PipeRegister::Control,0);CHECK(m.capture());bs::Result r={};CHECK(m.stop(r)==bs::Status::Ok);}
    {Model m;m.set(0,bd::PipeRegister::Control,0x1002);CHECK(m.capture());bs::Result r={};CHECK(m.stop(r)==bs::Status::Ok);}
}
void faults() {
    Model baseline;bs::Result r={};CHECK(baseline.capture());CHECK(baseline.stop(r)==bs::Status::Ok);
    for(size_t at=1;at<=baseline.reads.size();++at)for(bool unavailable:{false,true}) {
        Model m;CHECK(m.capture());if(unavailable)m.onesRead=at;else m.failRead=at;
        CHECK(m.stop(r)==(unavailable?bs::Status::DeviceUnavailable:bs::Status::ReadFailed));
        CHECK(m.reads.size()==at && r.retentionRequired && m.state.phase()==bs::Phase::Faulted);
        const size_t writes=m.writes.size();CHECK(m.stop(r)==bs::Status::InvalidState && m.writes.size()==writes);
    }
    for(size_t at=1;at<=4;++at)for(bool applies:{false,true}) {
        Model m;CHECK(m.capture());m.failWrite=at;m.applyFailure=applies;
        CHECK(m.stop(r)==bs::Status::WriteFailed && r.writesMayHaveOccurred && r.retentionRequired && m.writes.size()==at);
        CHECK(m.stop(r)==bs::Status::InvalidState && m.writes.size()==at);
    }
    {Model m;CHECK(m.capture());m.dropWrite=1;CHECK(m.stop(r)==bs::Status::ReadbackMismatch && r.retentionRequired);}
    {Model m;CHECK(m.capture());m.neverDrain=true;CHECK(m.stop(r)==bs::Status::DrainTimeout && m.writes.size()==1 && r.retentionRequired);}
    {Model m;CHECK(m.capture());m.set(0,bd::PipeRegister::Control,0x10);
        // Changed OTG selector is rejected before any blank write.
        CHECK(m.stop(r)==bs::Status::SnapshotChanged && m.writes.empty());}
    {Model m;m.set(0,bd::PipeRegister::Control,0x10);CHECK(m.capture());
        CHECK(m.stop(r)==bs::Status::DrainTimeout && m.writes.empty() && r.retentionRequired);}
}
void changes() {
    bs::Result r={};
    for(unsigned p=0;p<4;++p)for(size_t field=0;field<bd::kPipeRegisterCount;++field) {
        Model m;CHECK(m.capture());m.registers[m.l.pipe[p][field]]^=field?1:4;
        CHECK(m.stop(r)==bs::Status::SnapshotChanged && m.writes.empty() && r.retentionRequired);
    }
    for(unsigned p=0;p<4;++p)for(bool config:{false,true}) {
        Model m;CHECK(m.capture());m.registers[config?m.l.powerConfig[p]:m.l.power[p]]^=1;
        CHECK(m.stop(r)==bs::Status::PowerChanged && m.writes.empty());
    }
    for(unsigned p=0;p<4;++p){Model m;CHECK(m.capture());m.registers[m.l.pipe[p][0]+4]=0;
        CHECK(m.stop(r)==bs::Status::ClockUnavailable && m.writes.empty());}
    {Model m;m.registers[m.l.sos]=1;CHECK(bs::capture(m.l,m.memory,m.io(),m.state,r)==bs::Status::CaptureRejected && !r.retentionRequired);}
    {Model m;m.registers[m.l.pipe[0][0]+4]=0;CHECK(bs::capture(m.l,m.memory,m.io(),m.state,r)==bs::Status::ClockUnavailable && m.writes.empty());}
    {Model m;CHECK(m.capture());CHECK(bs::stop(m.io(),{100001,1},m.state,r)==bs::Status::InvalidArgument && m.reads.empty());}
}
void captureEvidence() {
    // Reproduce all four target current0/desired1/CONFIG0 observations through
    // the real capture API. The private retained snapshot must remain exact.
    {Model m;bs::Result r={};m.desiredOn(15,0);CHECK(m.capture());
        CHECK(m.state.capture().complete && m.state.capture().snapshotReadComplete && m.state.capture().planeCount==4);
        for(unsigned p=0;p<4;++p)CHECK(m.state.capture().snapshot.power[p]==0x10000000 &&
                                    m.state.capture().snapshot.powerConfig[p]==0);
        CHECK(m.stop(r)==bs::Status::Ok && m.writes.size()==4 && !r.retentionRequired);}
    // An explicit gate request still stops before any HUBP access.
    {Model m;bs::Result r={};m.desiredOn(15,0);m.registers[m.l.powerConfig[0]]=0x100;
        CHECK(bs::capture(m.l,m.memory,m.io(),m.state,r)==bs::Status::CaptureRejected);
        CHECK(r.captureStatus==bd::Status::PowerUnavailable && r.pipe==0 && r.reads==3);
        CHECK(r.lastRegister==m.l.powerConfig[0] && r.lastValue==0x100 && r.lastReadSucceeded);
        CHECK(r.lastPowerProbe.pipe==0 && r.lastPowerProbe.readsSucceededMask==3 &&
            r.lastPowerProbe.statusBefore==0x10000000 && r.lastPowerProbe.config==0x100 && r.lastPowerProbe.statusAfter==0);
        CHECK(m.state.phase()==bs::Phase::Fresh && !r.retentionRequired && m.writes.empty());}
    {Model m;bs::Result r={};m.failRead=2;
        CHECK(bs::capture(m.l,m.memory,m.io(),m.state,r)==bs::Status::CaptureRejected);
        CHECK(r.captureStatus==bd::Status::TransportError && r.pipe==0 && !r.lastReadSucceeded && r.lastValue==0);
        CHECK(r.lastPowerProbe.readsSucceededMask==0 && m.writes.empty());}
    {Model m;bs::Result r={};m.onesRead=4;
        CHECK(bs::capture(m.l,m.memory,m.io(),m.state,r)==bs::Status::CaptureRejected);
        CHECK(r.captureStatus==bd::Status::DeviceUnavailable && r.pipe==0 && r.lastReadSucceeded && r.lastValue==UINT32_MAX);
        CHECK(r.lastPowerProbe.readsSucceededMask==7 && r.lastPowerProbe.statusAfter==UINT32_MAX && m.writes.empty());}
    {Model m;bs::Result r={};CHECK(m.capture());m.registers[m.l.powerConfig[0]]=1;
        CHECK(m.stop(r)==bs::Status::PowerChanged && r.lastReadSucceeded && r.lastValue==1);
        CHECK(r.lastPowerProbe.pipe==0 && r.lastPowerProbe.readsSucceededMask==3 &&
            r.lastPowerProbe.config==1 && r.lastPowerProbe.statusAfter==0 && m.writes.empty());}
    {Model m;bs::Result r={};CHECK(m.capture());m.failWrite=1;
        CHECK(m.stop(r)==bs::Status::WriteFailed && !r.lastReadSucceeded && r.writesMayHaveOccurred);
        CHECK(r.lastRegister==m.l.pipe[0][0] && (r.lastValue&1) && r.retentionRequired);}
}
void desiredOnStop(uint32_t config) {
    // Exercise the actual capture API and its retained private State, never a
    // fabricated complete flag. Mixtures of OFF, already blank, ordinary ON
    // and desired1 ON with either FORCEON value must produce exactly the
    // corresponding blank requests.
    for(unsigned off=0;off<16;++off)for(unsigned force:{1U,5U,15U}) {
        Model m(layout(),off,2);m.desiredOn(force&~off,config);bs::Result r={};CHECK(m.capture());
        CHECK(m.state.capture().snapshotReadComplete && m.state.capture().poweredOffMask==off);
        for(unsigned p=0;p<4;++p)if((force&~off)&(1U<<p)) {
            CHECK(m.state.capture().snapshot.power[p]==0x10000000 &&
                  m.state.capture().snapshot.powerConfig[p]==config);
        }
        CHECK(m.stop(r)==bs::Status::Ok && !r.retentionRequired && m.state.phase()==bs::Phase::Stopped);
        const unsigned expected=(~(off|2U))&15;
        CHECK(r.blankedMask==expected);
        for(unsigned p=0;p<4;++p) {
            CHECK(m.written[p]==bool(expected&(1U<<p)));
            if((force&~off)&(1U<<p))CHECK(m.registers[m.l.power[p]]==0x10000000 && m.registers[m.l.powerConfig[p]]==config);
            if(off&(1U<<p))for(uint32_t offset:m.reads) {
                CHECK(offset!=m.l.pipe[p][0]+4);
                for(uint32_t window:m.l.pipe[p])CHECK(offset!=window);
            }
        }
    }
    Model baseline;baseline.desiredOn(15,config);bs::Result r={};CHECK(baseline.capture());
    CHECK(baseline.stop(r)==bs::Status::Ok && baseline.writes.size()==4);
    // Callback failure represents lost native access authority as well as
    // transport failure. Every read phase must stop immediately, retain the
    // original capture, and seal against any blind retry after partial writes.
    for(size_t at=1;at<=baseline.reads.size();++at)for(bool unavailable:{false,true}) {
        Model m;m.desiredOn(15,config);CHECK(m.capture());
        if(unavailable)m.onesRead=at;else m.failRead=at;
        CHECK(m.stop(r)==(unavailable?bs::Status::DeviceUnavailable:bs::Status::ReadFailed));
        CHECK(m.reads.size()==at && r.retentionRequired && m.state.phase()==bs::Phase::Faulted);
        CHECK(r.writesMayHaveOccurred==!m.writes.empty());
        CHECK(m.state.capture().snapshotReadComplete && m.state.capture().snapshot.power[0]==0x10000000);
        const size_t writes=m.writes.size();
        CHECK(m.stop(r)==bs::Status::InvalidState && m.reads.size()==at && m.writes.size()==writes);
    }
    // Change FORCEON, request a gate, set an unknown bit or change status at
    // every actual power guard, including the last guard immediately before
    // a blank write. No window read or write may follow the changed evidence.
    for(size_t at=1;at<=baseline.reads.size();++at)for(unsigned p=0;p<4;++p) {
        const uint32_t reg=baseline.reads[at-1];
        if(reg!=baseline.l.power[p] && reg!=baseline.l.powerConfig[p])continue;
        const auto changed=reg==baseline.l.power[p]?std::vector<uint32_t>{0U,0x50000000U}:
                                                                std::vector<uint32_t>{config^1U,config|0x100U,config|2U};
        for(uint32_t value:changed) {
            Model m;m.desiredOn(15,config);CHECK(m.capture());m.changeAt=at;m.changeRegister=reg;m.changeValue=value;
            CHECK(m.stop(r)==bs::Status::PowerChanged && m.reads.size()==at);
            CHECK(r.retentionRequired && m.state.phase()==bs::Phase::Faulted && r.lastReadSucceeded && r.lastValue==value);
            CHECK(m.reads.back()==reg && r.lastRegister==reg);
            const size_t writes=m.writes.size();
            CHECK(m.stop(r)==bs::Status::InvalidState && m.reads.size()==at && m.writes.size()==writes);
        }
    }
    for(size_t at=1;at<=4;++at)for(bool applies:{false,true}) {
        Model m;m.desiredOn(15,config);CHECK(m.capture());m.failWrite=at;m.applyFailure=applies;
        CHECK(m.stop(r)==bs::Status::WriteFailed && r.retentionRequired && r.writesMayHaveOccurred);
        CHECK(m.writes.size()==at && m.state.phase()==bs::Phase::Faulted);
        CHECK(m.stop(r)==bs::Status::InvalidState && m.writes.size()==at);
    }
}
void offCurrentState() {
    const bd::Layout l=layout();bs::Result r={};
    for(unsigned off=0;off<16;++off)for(unsigned desired=0;desired<16;++desired) {
        Model m(l,off,2,0x80000000);
        for(unsigned p=0;p<4;++p) {
            m.registers[l.power[p]]|=(desired&(1U<<p))?0x10000000:0;
            if(!(off&(1U<<p)))m.registers[l.powerConfig[p]]=p&1;
        }
        CHECK(m.capture());CHECK(m.state.capture().poweredOffMask==off);
        CHECK(m.stop(r)==bs::Status::Ok && m.state.phase()==bs::Phase::Stopped && !r.retentionRequired);
        const unsigned expected=(~(off|2U))&15;CHECK(r.blankedMask==expected);
        for(unsigned p=0;p<4;++p) {
            CHECK(m.written[p]==bool(expected&(1U<<p)));
            CHECK(m.state.capture().snapshot.power[p]==m.registers[l.power[p]]);
            if(off&(1U<<p))for(uint32_t reg:m.reads) {
                CHECK(reg!=l.pipe[p][0]+4);for(uint32_t window:l.pipe[p])CHECK(reg!=window);
            }
        }
    }
    for(uint32_t offValue:{0x80000000U,0x90000000U})for(unsigned off:{5U,15U}) {
        Model baseline(l,off,0,offValue);
        CHECK(bs::capture(l,baseline.memory,baseline.io(),baseline.state,r)==bs::Status::Ok);
        const size_t captureReads=baseline.reads.size();baseline.reads.clear();
        CHECK(baseline.stop(r)==bs::Status::Ok);
        for(size_t at=1;at<=captureReads;++at)for(bool ones:{false,true}) {
            Model m(l,off,0,offValue);if(ones)m.onesRead=at;else m.failRead=at;
            const bs::Status status=bs::capture(l,m.memory,m.io(),m.state,r);
            CHECK(status==bs::Status::CaptureRejected || status==(ones?bs::Status::DeviceUnavailable:bs::Status::ReadFailed));
            if(status==bs::Status::CaptureRejected)CHECK(r.captureStatus==(ones?bd::Status::DeviceUnavailable:bd::Status::TransportError));
            CHECK(m.reads.size()==at && m.writes.empty() && m.state.phase()==bs::Phase::Fresh && !r.retentionRequired);
            CHECK(r.lastReadSucceeded==ones && r.lastValue==(ones?UINT32_MAX:0U));
        }
        for(size_t at=1;at<=baseline.reads.size();++at)for(bool ones:{false,true}) {
            Model m(l,off,0,offValue);CHECK(m.capture());if(ones)m.onesRead=at;else m.failRead=at;
            CHECK(m.stop(r)==(ones?bs::Status::DeviceUnavailable:bs::Status::ReadFailed));
            CHECK(m.reads.size()==at && m.state.phase()==bs::Phase::Faulted && r.retentionRequired);
            CHECK(r.writesMayHaveOccurred==!m.writes.empty() && r.lastReadSucceeded==ones);
            const size_t writes=m.writes.size();CHECK(m.stop(r)==bs::Status::InvalidState && m.reads.size()==at && m.writes.size()==writes);
        }
        // A recorded OFF state is retained as raw evidence, not a masked
        // permission. Desired, current, reserved and request changes all seal
        // the private State at the first mismatching always-on observation.
        for(size_t at=1;at<=baseline.reads.size();++at)for(unsigned p=0;p<4;++p) {
            if(!(off&(1U<<p)))continue;
            const uint32_t reg=baseline.reads[at-1];
            if(reg!=l.power[p] && reg!=l.powerConfig[p])continue;
            for(uint32_t delta:reg==l.power[p]?std::vector<uint32_t>{0x10000000U,0x80000000U,1U}:
                                              std::vector<uint32_t>{0x100U,1U,2U}) {
                Model m(l,off,0,offValue);CHECK(m.capture());m.changeAt=at;m.changeRegister=reg;
                m.changeValue=m.registers[reg]^delta;
                CHECK(m.stop(r)==bs::Status::PowerChanged && m.reads.size()==at && r.lastRegister==reg);
                CHECK(r.lastReadSucceeded && r.lastValue==m.changeValue && r.retentionRequired && m.state.phase()==bs::Phase::Faulted);
                const size_t writes=m.writes.size();CHECK(m.stop(r)==bs::Status::InvalidState && m.reads.size()==at && m.writes.size()==writes);
            }
        }
        // Ambiguous blank writes on the remaining ON pipes retain memory
        // whether the device accepted the write or the callback failed first.
        for(size_t at=1;at<=baseline.writes.size();++at)for(bool applies:{false,true}) {
            Model m(l,off,0,offValue);CHECK(m.capture());m.failWrite=at;m.applyFailure=applies;
            CHECK(m.stop(r)==bs::Status::WriteFailed && m.writes.size()==at && r.retentionRequired && r.writesMayHaveOccurred);
            CHECK(m.state.phase()==bs::Phase::Faulted && m.stop(r)==bs::Status::InvalidState && m.writes.size()==at);
        }
    }
}
void captureReadJournal(){
    for(unsigned off:{0u,5u,15u}) {
        Model base(layout(),off);bs::Result r{};bs::ReadCapture j{};
        CHECK(bs::captureRecorded(base.l,base.memory,base.io(),base.state,r,&j)==bs::Status::Ok);
        CHECK(j.recorded==base.reads.size()&&!j.truncated&&base.writes.empty());
        if(!off)CHECK(j.recorded==1205);
        for(size_t at=1;at<=base.reads.size();++at)for(bool ones:{false,true}) {
            Model plain(layout(),off),traced(layout(),off);bs::Result a{},b{};
            if(ones)plain.onesRead=traced.onesRead=at;else plain.failRead=traced.failRead=at;
            const auto status=bs::capture(plain.l,plain.memory,plain.io(),plain.state,a);
            CHECK(bs::captureRecorded(traced.l,traced.memory,traced.io(),traced.state,b,&j)==status);
            CHECK(traced.reads==plain.reads&&traced.reads.size()==at&&traced.writes.empty()&&plain.writes.empty());
            CHECK(traced.state.phase()==plain.state.phase()&&a.captureStatus==b.captureStatus&&
                a.reads==b.reads&&a.lastValue==b.lastValue&&a.retentionRequired==b.retentionRequired);
            CHECK(j.recorded==at&&j.attempted==at&&!j.truncated);
            for(size_t i=0;i<j.recorded;++i){const uint32_t bit=1U<<(i%32);
                CHECK(j.entries[i].byteOffset==traced.reads[i]);
                CHECK(j.entries[i].returnedValue==(ones&&i+1==at?UINT32_MAX:traced.registers.at(traced.reads[i])));
                CHECK(bool(j.callbackSucceeded[i/32]&bit)==(ones||i+1!=at));
                CHECK(bool(j.nonAllOnes[i/32]&bit)==(i+1!=at));}
        }
    }
}

}
int main(int argc,char** argv){golden();faults();changes();captureEvidence();captureReadJournal();desiredOnStop(0);desiredOnStop(1);offCurrentState();
    if(argc>1){std::ifstream input(argv[1],std::ios::binary);std::vector<uint8_t>b((std::istreambuf_iterator<char>(input)),{});
        bd::Layout l={};CHECK(!b.empty() && bd::buildLayout(b.data(),b.size(),0x100000,l)==bd::Status::Ok);
        Model m(l,14,0);bs::Result r={};CHECK(m.capture());CHECK(m.stop(r)==bs::Status::Ok && m.writes.size()==1);}
    std::cout<<checks<<" boot display retirement checks; "<<failures<<" failures\n";return failures?1:0;}
