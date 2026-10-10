// SPDX-License-Identifier: MIT
// Host-only callback tests. No MMIO, firmware calls, SSH or hardware execution.
#include "hackynavi/boot_display_context.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
using namespace hackynavi;
namespace bc=hackynavi::bootdisplaycontext;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void put16(std::vector<uint8_t>& b,size_t at,uint16_t v){b[at]=uint8_t(v);b[at+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t at,uint32_t v){put16(b,at,uint16_t(v));put16(b,at+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t at){uint16_t v=0;for(;at<b.size();++at)v=uint16_t(v+b[at]);return v;}
std::vector<uint8_t> discovery() {
    std::vector<uint8_t> b(164,0);
    put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,164);put16(b,12,60);put16(b,16,104);
    put32(b,60,0x53445049);put16(b,64,3);put16(b,66,104);put16(b,72,1);put16(b,76,140);put16(b,142,1);
    put16(b,144,271);b[147]=3;b[148]=3;b[149]=2;put32(b,156,0xc0);put32(b,160,0x34c0);
    put16(b,14,sum(b,60));put16(b,8,sum(b,10));return b;
}
bc::Layout layout() {
    const auto b=discovery();bc::Layout l={};
    CHECK(bc::buildLayout(b.data(),b.size(),0x100000,l)==bc::Status::Ok);return l;
}
struct Model {
    bc::Layout l=layout();std::map<uint32_t,uint32_t> values;
    std::vector<uint32_t> reads;size_t validCalls=0,denyAt=0,failAt=0,onesAt=0,changeAt=0;
    uint32_t changeRegister=0,changeValue=0;
    Model() {
        for(size_t p=0;p<4;++p){values[l.power[p]]=0x10000000;values[l.powerConfig[p]]=uint32_t(p&1);}
        for(size_t r=0;r<32;++r)values[l.registers[r]]=0x12340000U|uint32_t(r);
    }
    bc::Io io(){return {this,valid,read};}
    static bool valid(void* context){auto& m=*static_cast<Model*>(context);return ++m.validCalls!=m.denyAt;}
    static bool read(void* context,uint32_t offset,uint32_t& value) {
        auto& m=*static_cast<Model*>(context);m.reads.push_back(offset);CHECK(m.values.count(offset)==1);
        if(m.reads.size()==m.changeAt)m.values[m.changeRegister]=m.changeValue;
        for(size_t p=0;p<4;++p)for(size_t r=3;r<6;++r)if(offset==m.l.registers[p*6+r]) {
            CHECK(m.reads.size()>=4);
            const size_t n=m.reads.size();
            CHECK(m.reads[n-4]==m.l.power[p] && m.reads[n-3]==m.l.powerConfig[p] && m.reads[n-2]==m.l.power[p]);
            CHECK((m.values[m.l.power[p]]==0 || m.values[m.l.power[p]]==0x10000000) && m.values[m.l.powerConfig[p]]<=1);
        }
        value=m.reads.size()==m.onesAt?UINT32_MAX:m.values[offset];
        if(m.reads.size()==m.failAt){value=0xbadc0ffe;return false;}return true;
    }
};
void baseline() {
    Model m;bc::Capture c={};const auto before=m.values;
    CHECK(bc::capture(m.l,m.io(),c)==bc::Status::Ok);
    CHECK(c.readsComplete && c.completedPasses==2 && c.readsAttempted==208 && m.validCalls==416);
    CHECK(!c.changedMask && !c.powerChangedMask && c.failedPass==0xff && c.failedPipe==0xff && c.failedRegisterIndex==0xff);
    CHECK(m.values==before);
    for(size_t pass=0;pass<2;++pass) {
        CHECK(c.attemptedMask[pass]==UINT32_MAX && c.readSucceededMask[pass]==UINT32_MAX && c.validMask[pass]==UINT32_MAX);
        CHECK(c.powerValidMask[pass]==15);
        for(size_t r=0;r<32;++r)CHECK(c.values[pass][r]==m.values[m.l.registers[r]]);
        for(size_t p=0;p<4;++p)CHECK(c.powerStatus[pass][p]==0x10000000 && c.powerConfig[pass][p]==(p&1));
    }
    CHECK(m.l.registers[0]==0xe7b8 && m.l.registers[1]==0x14004 && m.l.registers[2]==0x14114);
    CHECK(m.l.registers[3]==0xead0 && m.l.registers[4]==0xead8 && m.l.registers[5]==0xeadc);
    CHECK(m.l.registers[18]==0xe7c4 && m.l.registers[19]==0x14604 && m.l.registers[20]==0x14714);
    CHECK(m.l.registers[24]==0xe4e8 && m.l.registers[25]==0xe864 && m.l.registers[31]==0xe87c);
    for(unsigned p=0;p<4;++p)for(uint32_t power:{0U,0x10000000U})for(uint32_t config:{0U,1U}) {
        Model x;x.values[x.l.power[p]]=power;x.values[x.l.powerConfig[p]]=config;
        CHECK(bc::capture(x.l,x.io(),c)==bc::Status::Ok && c.readsComplete);
    }
}
void partialReads() {
    Model initial;bc::Capture baseline={};CHECK(bc::capture(initial.l,initial.io(),baseline)==bc::Status::Ok);
    for(size_t at=1;at<=208;++at)for(bool ones:{false,true}) {
        Model m;bc::Capture c={};if(ones)m.onesAt=at;else m.failAt=at;
        CHECK(bc::capture(m.l,m.io(),c)==(ones?bc::Status::DeviceUnavailable:bc::Status::ReadFailed));
        CHECK(!c.readsComplete && c.completedPasses==(at>104?1:0) && c.readsAttempted==at && m.reads.size()==at);
        CHECK(c.lastRegister==initial.reads[at-1] && c.lastReadSucceeded==ones && c.lastValue==(ones?UINT32_MAX:0U));
        for(size_t r=0;r<32;++r)if(m.l.registers[r]==c.lastRegister) {
            const size_t pass=at>104?1:0;const uint32_t bit=uint32_t(1)<<r;
            CHECK(c.attemptedMask[pass]&bit);
            CHECK(bool(c.readSucceededMask[pass]&bit)==ones && !(c.validMask[pass]&bit));
            CHECK(c.values[pass][r]==(ones?UINT32_MAX:0U));
        }
        for(size_t pass=0;pass<2;++pass)for(size_t r=0;r<32;++r)
            if(!(c.readSucceededMask[pass]&(uint32_t(1)<<r)))CHECK(c.values[pass][r]==0);
    }
    for(size_t at=1;at<=416;++at) {
        Model m;bc::Capture c={};m.denyAt=at;
        CHECK(bc::capture(m.l,m.io(),c)==bc::Status::AccessDenied && !c.readsComplete);
        CHECK(c.readsAttempted==at/2 && m.reads.size()==at/2 && m.validCalls==at);
        CHECK(c.lastReadSucceeded==(at%2==0));
        for(size_t r=0;r<32;++r)if(m.l.registers[r]==c.lastRegister) {
            const size_t pass=at>208?1:0;const uint32_t bit=uint32_t(1)<<r;
            CHECK(bool(c.attemptedMask[pass]&bit)==(at%2==0));
            CHECK(bool(c.readSucceededMask[pass]&bit)==(at%2==0) && !(c.validMask[pass]&bit));
            CHECK(c.values[pass][r]==(at%2==0?m.values[c.lastRegister]:0));
        }
    }
}
void changes() {
    for(size_t r=0;r<32;++r) {
        Model m;bc::Capture c={};m.changeAt=105;m.changeRegister=m.l.registers[r];m.changeValue=0;
        CHECK(bc::capture(m.l,m.io(),c)==bc::Status::Changed);
        CHECK(c.readsComplete && c.completedPasses==2 && c.changedMask==(uint32_t(1)<<r));
        CHECK(c.validMask[0]==UINT32_MAX && c.validMask[1]==UINT32_MAX && !c.powerChangedMask);
    }
    Model first;bc::Capture observed={};CHECK(bc::capture(first.l,first.io(),observed)==bc::Status::Ok);
    for(size_t at=1;at<=208;++at)for(unsigned p=0;p<4;++p) {
        const auto address=first.reads[at-1];
        if(address!=first.l.power[p] && address!=first.l.powerConfig[p])continue;
        Model m;bc::Capture c={};m.changeAt=at;m.changeRegister=address;
        m.changeValue=address==m.l.power[p]?0x40000000:0x100;
        CHECK(bc::capture(m.l,m.io(),c)==bc::Status::PowerUnavailable && !c.readsComplete);
        CHECK(m.reads.size()==at && c.lastRegister==address && c.lastPowerProbe.pipe==p);
        CHECK(c.lastReadSucceeded && c.lastValue==m.changeValue);
    }
    // Valid individually but inconsistent with the established power baseline.
    for(unsigned p=0;p<4;++p) {
        Model m;bc::Capture c={};m.changeAt=105;m.changeRegister=m.l.power[p];m.changeValue=0;
        CHECK(bc::capture(m.l,m.io(),c)==bc::Status::Changed && !c.readsComplete && c.completedPasses==1);
        CHECK(c.powerChangedMask==(1U<<p) && c.failedPipe==p && !c.changedMask);
    }
}
void preflight() {
    for(size_t r=0;r<32;++r) {
        Model m;bc::Capture c={};m.l.registers[r]+=4;
        CHECK(bc::capture(m.l,m.io(),c)==bc::Status::InvalidArgument && m.reads.empty() && m.validCalls==0);
        CHECK(!c.readsComplete && !c.readsAttempted && !c.validMask[0] && c.failedPass==0xff);
    }
    for(unsigned p=0;p<4;++p)for(bool config:{false,true}) {
        Model m;bc::Capture c={};(config?m.l.powerConfig[p]:m.l.power[p])+=4;
        CHECK(bc::capture(m.l,m.io(),c)==bc::Status::InvalidArgument && m.reads.empty());
    }
    for(unsigned defect=0;defect<5;++defect) {
        Model m;bc::Capture c={};auto io=m.io();
        if(defect==0)io.valid=nullptr;if(defect==1)io.read32=nullptr;
        if(defect==2)m.l.valid=false;if(defect==3)m.l.barBytes=3;if(defect==4)m.l.registers[24]=0;
        CHECK(bc::capture(m.l,io,c)==bc::Status::InvalidArgument && m.reads.empty());
    }
    const auto d=discovery();bc::Layout l={};
    for(size_t bytes=0;bytes<d.size();++bytes) {
        CHECK(bc::buildLayout(d.data(),bytes,0x100000,l)==bc::Status::DiscoveryRejected);
        CHECK(!l.valid && !l.barBytes);
    }
    CHECK(bc::buildLayout(d.data(),d.size(),0x14000,l)==bc::Status::OutsideBar && !l.valid);
    for(unsigned p=0;p<4;++p)for(uint32_t power:{1U,0x40000000U,0x80000000U,0x90000000U,0xc0000000U}) {
        Model m;bc::Capture c={};m.values[m.l.power[p]]=power;
        CHECK(bc::capture(m.l,m.io(),c)==bc::Status::PowerUnavailable && m.reads.size()==p*3+1);
        CHECK(!c.attemptedMask[0] && !c.validMask[0] && !c.readsComplete);
    }
}
}
int main(int argc,char** argv) {
    baseline();partialReads();changes();preflight();
    if(argc>1) {
        std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t>d((std::istreambuf_iterator<char>(f)),{});
        CHECK(!d.empty());bc::Layout l={};CHECK(bc::buildLayout(d.data(),d.size(),0x100000,l)==bc::Status::Ok);
        CHECK(l.registers[0]==0xe7b8 && l.registers[31]==0xe87c);
    }
    std::cout<<checks<<" boot display context assertions, "<<failures<<" failures\n";return failures?1:0;
}
