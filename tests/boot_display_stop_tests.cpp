// SPDX-License-Identifier: MIT
// Host-only MMIO model, never a hardware or display-execution test.
#include "hackynavi/boot_display_stop.hpp"
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
    bool applyFailure=false,neverDrain=false;unsigned drainAfter=0,remaining[4]={};bool written[4]={};
    Model(const bd::Layout& selected=layout(),unsigned off=0,unsigned blank=0):l(selected) {
        CHECK(deriveMemoryLayout({0x100,0x6ff,0x1000,24ULL<<30,0x8000000000ULL,256ULL<<20},memory)==MemoryLayoutStatus::Ok);
        registers[l.sos]=0;
        for(uint32_t reg:l.vga)registers[reg]=0;
        const uint32_t global[]={0x100,0x6ff,0x1000,0xffffff,0};
        for(size_t i=0;i<5;++i)registers[l.global[i]]=global[i];
        for(unsigned p=0;p<4;++p) {
            registers[l.power[p]]=(off&(1U<<p))?0x90000000:0;
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
    static bool read(void* c,uint32_t reg,uint32_t& value) {
        auto& m=*static_cast<Model*>(c);m.reads.push_back(reg);CHECK(m.registers.count(reg)==1);
        for(unsigned p=0;p<4;++p){
            if(reg==m.l.pipe[p][0] && m.written[p] && !m.neverDrain) {
                if(m.remaining[p])--m.remaining[p];else m.registers[reg]|=2;
            }
            bool window=reg==m.l.pipe[p][0]+4;for(uint32_t client:m.l.pipe[p])window=window || reg==client;
            if(window){CHECK(m.registers[m.l.power[p]]==0);CHECK(m.reads.size()>1 && m.reads[m.reads.size()-2]==m.l.power[p]);}
        }
        value=m.reads.size()==m.onesRead?UINT32_MAX:m.registers[reg];return m.reads.size()!=m.failRead;
    }
    static bool write(void* c,uint32_t reg,uint32_t value) {
        auto& m=*static_cast<Model*>(c);m.writes.push_back(reg);unsigned p=0;
        while(p<4 && reg!=m.l.pipe[p][0])++p;CHECK(p<4);if(p==4)return false;
        CHECK(m.registers[m.l.power[p]]==0 && (m.registers[reg+4]&1));
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
}
int main(int argc,char** argv){golden();faults();changes();
    if(argc>1){std::ifstream input(argv[1],std::ios::binary);std::vector<uint8_t>b((std::istreambuf_iterator<char>(input)),{});
        bd::Layout l={};CHECK(!b.empty() && bd::buildLayout(b.data(),b.size(),0x100000,l)==bd::Status::Ok);
        Model m(l,14,0);bs::Result r={};CHECK(m.capture());CHECK(m.stop(r)==bs::Status::Ok && m.writes.size()==1);}
    std::cout<<checks<<" boot display retirement checks; "<<failures<<" failures\n";return failures?1:0;}
