// SPDX-License-Identifier: MIT
// Host-only register model; these tests perform no real GPU access.
#include "hackynavi/cold_dma_capture.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
using namespace hackynavi;
namespace cd=hackynavi::colddma;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<"line "<<__LINE__<<": " #x "\n";}}while(0)
void put16(std::vector<uint8_t>& b,size_t i,uint16_t v){b[i]=uint8_t(v);b[i+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t i,uint32_t v){put16(b,i,uint16_t(v));put16(b,i+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t i){uint16_t s=0;for(;i<b.size();++i)s=uint16_t(s+b[i]);return s;}
void fix(std::vector<uint8_t>& b){put16(b,14,sum(b,60));put16(b,8,sum(b,10));}
std::vector<uint8_t> fixture(){
    std::vector<uint8_t> b(196,0);
    put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,196);put16(b,12,60);put16(b,16,136);
    put32(b,60,0x53445049);put16(b,64,3);put16(b,66,136);put16(b,72,1);put16(b,76,140);put16(b,142,3);
    put16(b,144,40);b[147]=2;b[148]=6;put32(b,152,0x10a0);put32(b,156,0x240a000);
    put16(b,160,271);b[163]=3;b[164]=3;b[165]=2;put32(b,168,0xc0);put32(b,172,0xc0);put32(b,176,0x34c0);
    put16(b,180,255);b[183]=2;b[184]=13;put32(b,188,0x16000);put32(b,192,0x16000);fix(b);return b;
}
cd::Layout layout(){auto b=fixture();cd::Layout l={};CHECK(cd::buildLayout(b.data(),b.size(),0x100000,l)==cd::Status::Ok);return l;}
struct Model {
    cd::Layout l=layout();std::map<uint32_t,uint32_t> regs;std::vector<uint32_t> reads;
    size_t failAt=0,unavailableAt=0,changeAt=0;uint32_t changeReg=0,changeValue=0;
    Model(){regs[l.sos]=0;regs[l.dmubControl]=0;regs[l.dmubReset]=1;regs[l.mall]=0;
        for(uint32_t reg:l.ih)regs[reg]=0;regs[l.ih[0]]=0xc0047;
        for(uint32_t reg:l.wb)regs[reg]=0;
        for(size_t p=0;p<cd::kPipes;++p){regs[l.power[p]]=0;regs[l.powerConfig[p]]=0;
            regs[l.mallConfig[p]]=0;regs[l.mallStatus[p]]=0x80000;regs[l.mallSubvp[p]]=0;regs[l.vmPageConfig[p]]=0;}}
    static bool read(void* context,uint32_t reg,uint32_t& value){auto& m=*static_cast<Model*>(context);
        m.reads.push_back(reg);CHECK(m.regs.count(reg)==1);
        if(m.reads.size()==m.changeAt)m.regs[m.changeReg]=m.changeValue;
        value=m.regs[reg];if(m.reads.size()==m.unavailableAt)value=UINT32_MAX;return m.reads.size()!=m.failAt;}
    cd::Status run(cd::Result& r){return cd::checkStopped(l,{this,read},r);}
    void off(unsigned mask){for(unsigned p=0;p<cd::kPipes;++p)if(mask&(1U<<p)){
        regs[l.power[p]]=0x90000000;regs[l.powerConfig[p]]=0x100;
        regs[l.mallConfig[p]]=UINT32_MAX;regs[l.mallStatus[p]]=UINT32_MAX;
        regs[l.mallSubvp[p]]=UINT32_MAX;regs[l.vmPageConfig[p]]=UINT32_MAX;}}
};
void layouts(){
    auto b=fixture();cd::Layout l={};CHECK(cd::buildLayout(b.data(),b.size(),0x100000,l)==cd::Status::Ok);
    CHECK(l.sos==0x58244 && l.ih[0]==(0x10a0+0xc2)*4 && l.wb[0]==(0x34c0+0x3229)*4);
    CHECK(l.mallConfig[3]==(0x34c0+0x88a)*4 && l.mallStatus[3]==(0x34c0+0x893)*4);
    CHECK(l.mallSubvp[3]==(0x34c0+0x88b)*4 && l.vmPageConfig[3]==(0x34c0+0x889)*4);
    for(size_t n=0;n<b.size();++n)CHECK(cd::buildLayout(b.data(),n,0x100000,l)!=cd::Status::Ok && !l.valid);
    for(size_t at:{146U,147U,148U,149U,150U,151U,162U,163U,164U,165U,166U,167U,182U,183U,184U,185U,186U,187U}){
        auto bad=b;bad[at]^=4;fix(bad);CHECK(cd::buildLayout(bad.data(),bad.size(),0x100000,l)!=cd::Status::Ok && !l.valid);}
    for(uint64_t bar:{0ULL,3ULL,0x58247ULL})CHECK(cd::buildLayout(b.data(),b.size(),bar,l)==cd::Status::OutsideBar && !l.valid);
    auto alias=b;put32(alias,152,0x34c0+0x523-0xc2);fix(alias);
    CHECK(cd::buildLayout(alias.data(),alias.size(),0x100000,l)==cd::Status::OutsideBar);
    auto overflow=b;put32(overflow,176,UINT32_MAX);fix(overflow);
    CHECK(cd::buildLayout(overflow.data(),overflow.size(),0x100000,l)==cd::Status::OutsideBar);
}
void protocol(){
    cd::Result r={};Model baseline;CHECK(baseline.run(r)==cd::Status::Ok && r.reads==239 && !r.poweredOffMask);
    CHECK(baseline.reads.front()==baseline.l.sos && baseline.reads.back()==baseline.l.dmubReset);
    for(size_t at=1;at<=baseline.reads.size();++at){Model m;m.failAt=at;
        CHECK(m.run(r)==cd::Status::ReadFailed && r.reads==at);
        Model n;n.unavailableAt=at;CHECK(n.run(r)==cd::Status::DeviceUnavailable && r.reads==at);}
    const uint32_t ihMasks[]={0xfc73ff,0x80020f01,0x80000e01,0x100,0x70000,0,0xc0001,0,0xc0001,0x77,0xee};
    const uint32_t wbMasks[]={0x333300,0xff,0xc,1,4,1,0x80000001,0x11,0x10001,0x01000f01,4,0xf,0xf,0xf,0xf,1,0x07ffffff,0x07ffffff};
    for(size_t f=0;f<cd::kIhRegisters;++f)for(unsigned bit=0;bit<32;++bit)if(ihMasks[f]&(1U<<bit)){
        Model m;m.regs[m.l.ih[f]]^=1U<<bit;CHECK(m.run(r)==cd::Status::IhNotStopped);}
    for(size_t f=0;f<cd::kWbRegisters;++f)for(unsigned bit=0;bit<32;++bit)if(wbMasks[f]&(1U<<bit)){
        Model m;m.regs[m.l.wb[f]]^=1U<<bit;
        CHECK(m.run(r)==(f<5?cd::Status::PowerUnavailable:cd::Status::WritebackNotStopped));}
    for(size_t p=0;p<cd::kPipes;++p){
        for(unsigned bit=0;bit<3;++bit){Model m;m.regs[m.l.mallConfig[p]]=1U<<bit;CHECK(m.run(r)==cd::Status::MallNotStopped);}
        {Model m;m.regs[m.l.mallSubvp[p]]=1;CHECK(m.run(r)==cd::Status::MallNotStopped);}
        {Model m;m.regs[m.l.vmPageConfig[p]]=0x80;CHECK(m.run(r)==cd::Status::MallNotStopped);}
        for(unsigned bit=0;bit<20;++bit)if(bit!=5){Model m;m.regs[m.l.mallStatus[p]]^=1U<<bit;
            CHECK(m.run(r)==cd::Status::MallNotStopped);}
    }
    for(uint32_t bad:{1U,0x10U}){Model m;m.regs[m.l.mall]=bad;CHECK(m.run(r)==cd::Status::MallNotStopped);}
    {Model m;m.regs[m.l.mall]=0x20;for(uint32_t reg:m.l.mallStatus)m.regs[reg]|=0x20;CHECK(m.run(r)==cd::Status::Ok);}
    for(size_t rp:{5U,7U}){
        {Model m;m.regs[m.l.ih[rp]]=4;CHECK(m.run(r)==cd::Status::IhNotStopped);}
        {Model m;m.regs[m.l.ih[rp+1]]=4;CHECK(m.run(r)==cd::Status::IhNotStopped);}
        {Model m;m.regs[m.l.ih[rp]]=0x3fffc;m.regs[m.l.ih[rp+1]]=0x3fffc;CHECK(m.run(r)==cd::Status::Ok);}
    }
    // Any callback failure, device loss, or cold guard change aborts immediately.
    for(size_t at=1;at<=baseline.reads.size();++at){const uint32_t reg=baseline.reads[at-1];
        if(reg!=baseline.l.sos && reg!=baseline.l.dmubControl && reg!=baseline.l.dmubReset)continue;
        Model m;m.changeAt=at;m.changeReg=reg;m.changeValue=reg==m.l.dmubControl?0x10000:reg==m.l.dmubReset?0:1;
        CHECK(m.run(r)==(reg==m.l.sos?cd::Status::PriorDriverRequiresReset:cd::Status::FirmwareNotStopped));CHECK(r.reads==at);
    }
    // Configurations and historic state not otherwise constrained must still
    // remain stable; a new owner changing them invalidates the observation.
    for(size_t f=0;f<cd::kIhRegisters;++f){Model m;m.changeAt=113;m.changeReg=m.l.ih[f];m.changeValue=m.regs[m.changeReg]^0x40000000;
        CHECK(m.run(r)==cd::Status::UnstableSnapshot);}
    for(size_t f=0;f<cd::kWbRegisters;++f){Model m;m.changeAt=113;m.changeReg=m.l.wb[f];m.changeValue=0x40000000;
        CHECK(m.run(r)==cd::Status::UnstableSnapshot);}
    {Model m;m.l.wb[6]+=4;CHECK(m.run(r)==cd::Status::InvalidArgument && m.reads.empty());}
    {Model m;m.l.ih[0]^=1;CHECK(m.run(r)==cd::Status::InvalidArgument && m.reads.empty());}
    {Model m;m.l.mallStatus[3]+=4;CHECK(m.run(r)==cd::Status::InvalidArgument && m.reads.empty());}
    {Model m;m.l.mallSubvp[2]+=4;CHECK(m.run(r)==cd::Status::InvalidArgument && m.reads.empty());}
    {Model m;m.l.vmPageConfig[1]+=4;CHECK(m.run(r)==cd::Status::InvalidArgument && m.reads.empty());}
    CHECK(cd::checkStopped(baseline.l,{nullptr,nullptr},r)==cd::Status::InvalidArgument && !r.reads);
}
void power(){
    cd::Result r={};
    for(unsigned mask=0;mask<16;++mask){Model m;m.off(mask);CHECK(m.run(r)==cd::Status::Ok && r.poweredOffMask==mask);
        for(uint32_t reg:m.reads)for(size_t p=0;p<cd::kPipes;++p)if(mask&(1U<<p))
            CHECK(reg!=m.l.mallConfig[p] && reg!=m.l.mallStatus[p] && reg!=m.l.mallSubvp[p] && reg!=m.l.vmPageConfig[p]);}
    for(size_t p=0;p<cd::kPipes;++p){
        for(uint32_t bad:{0x40000000U,0x80000000U,0xd0000000U}){
            Model m;m.regs[m.l.power[p]]=bad;CHECK(m.run(r)==cd::Status::PowerUnavailable);}
        for(uint32_t bad:{0U,1U,0x101U}){Model m;m.off(1U<<p);m.regs[m.l.powerConfig[p]]=bad;
            CHECK(m.run(r)==cd::Status::PowerUnavailable);}
        {Model m;m.regs[m.l.powerConfig[p]]=0x100;CHECK(m.run(r)==cd::Status::PowerUnavailable);}
        {Model m;m.regs[m.l.powerConfig[p]]=1;CHECK(m.run(r)==cd::Status::Ok);}
    }
    Model baseline;CHECK(baseline.run(r)==cd::Status::Ok);
    // Every power observation can detect a transition before further window access.
    for(size_t at=1;at<=baseline.reads.size();++at)for(size_t p=0;p<cd::kPipes;++p)if(baseline.reads[at-1]==baseline.l.power[p]){
        Model m;m.changeAt=at;m.changeReg=m.l.power[p];m.changeValue=0x40000000;
        CHECK(m.run(r)==cd::Status::PowerUnavailable);CHECK(r.reads<=at+2);
    }
    Model off;off.off(15);CHECK(off.run(r)==cd::Status::Ok && r.reads==111);
    for(size_t at=1;at<=off.reads.size();++at){Model m;m.off(15);m.failAt=at;CHECK(m.run(r)==cd::Status::ReadFailed && r.reads==at);}
}
void currentPowerStates(){
    cd::Result r={};
    for(uint32_t on:{0u,0x10000000u})for(uint32_t config:{0u,1u})for(uint32_t off:{0x80000000u,0x90000000u}) {
        auto prepare=[&](Model& m,unsigned offMask){
            for(unsigned p=0;p<cd::kPipes;++p){m.regs[m.l.power[p]]=on;m.regs[m.l.powerConfig[p]]=config;}
            m.off(offMask);
            for(unsigned p=0;p<cd::kPipes;++p)if(offMask&(1U<<p))m.regs[m.l.power[p]]=off;
        };
        for(unsigned mask=0;mask<16;++mask){Model m;prepare(m,mask);
            CHECK(m.run(r)==cd::Status::Ok && r.poweredOffMask==mask);
            for(uint32_t reg:m.reads)for(unsigned p=0;p<cd::kPipes;++p)if(mask&(1U<<p))
                CHECK(reg!=m.l.mallConfig[p] && reg!=m.l.mallStatus[p] && reg!=m.l.mallSubvp[p] && reg!=m.l.vmPageConfig[p]);
        }
        // Every callback failure/all-ones aborts in each accepted ON/OFF
        // encoding; no reserved bits or request conflicts are masked away.
        for(unsigned mask:{0u,15u}){
            Model base;prepare(base,mask);CHECK(base.run(r)==cd::Status::Ok);
            for(size_t at=1;at<=base.reads.size();++at)for(bool ones:{false,true}){
                Model m;prepare(m,mask);if(ones)m.unavailableAt=at;else m.failAt=at;
                CHECK(m.run(r)==(ones?cd::Status::DeviceUnavailable:cd::Status::ReadFailed) && m.reads.size()==at);
            }
            for(unsigned p=0;p<cd::kPipes;++p)for(unsigned bit=0;bit<32;++bit){
                Model m;prepare(m,mask);m.regs[m.l.power[p]]^=1U<<bit;
                CHECK(m.run(r)==(bit==28?cd::Status::Ok:cd::Status::PowerUnavailable));
                Model c;prepare(c,mask);c.regs[c.l.powerConfig[p]]^=1U<<bit;
                CHECK(c.run(r)==(!mask&&bit==0?cd::Status::Ok:cd::Status::PowerUnavailable));
            }
            for(unsigned p=0;p<cd::kPipes;++p){bool seenStatus=false,seenConfig=false;
                for(size_t at=1;at<=base.reads.size();++at){const auto reg=base.reads[at-1];
                    const bool state=reg==base.l.power[p],cfg=reg==base.l.powerConfig[p];
                    if(!state&&!cfg)continue;
                    bool& seen=state?seenStatus:seenConfig;if(!seen){seen=true;continue;}
                    Model m;prepare(m,mask);m.changeAt=at;m.changeReg=reg;
                    m.changeValue=m.regs[reg]^(state?0x10000000U:1U);
                    const auto status=m.run(r);
                    CHECK(status==cd::Status::PowerUnavailable || status==cd::Status::UnstableSnapshot);
                    CHECK(m.reads.size()<=at+2 && !r.poweredOffMask);
                }
            }
        }
        // With the newly accepted desired-bit ON state, every actual MALL
        // drain/client predicate still runs and prevents cold DMA admission.
        for(unsigned p=0;p<cd::kPipes;++p){Model m;prepare(m,0);m.regs[m.l.mallStatus[p]]=0;
            CHECK(m.run(r)==cd::Status::MallNotStopped);}
    }
}
void readJournalEvidence(){
    const auto verify=[](const cd::ReadCapture& j,const Model& m,size_t fail,size_t ones) {
        CHECK(j.attempted==m.reads.size()&&j.recorded==j.attempted&&!j.truncated);
        for(size_t i=0;i<j.recorded;++i){const uint32_t bit=1U<<(i%32);
            CHECK(j.entries[i].byteOffset==m.reads[i]);
            CHECK(j.entries[i].returnedValue==(i+1==ones?UINT32_MAX:m.regs.at(m.reads[i])));
            CHECK(bool(j.callbackSucceeded[i/32]&bit)==(i+1!=fail));
            CHECK(bool(j.nonAllOnes[i/32]&bit)==(i+1!=fail&&i+1!=ones));}
        for(size_t i=j.recorded;i<cd::ReadCapture::capacity;++i)
            CHECK(!j.entries[i].byteOffset&&!j.entries[i].returnedValue&&
                !(j.callbackSucceeded[i/32]&(1U<<(i%32)))&&!(j.nonAllOnes[i/32]&(1U<<(i%32))));
    };
    for(unsigned mask:{0u,5u,15u}) {
        Model base,plain;base.off(mask);plain.off(mask);cd::Result a{},b{};cd::ReadCapture j{};
        CHECK(plain.run(a)==cd::Status::Ok);
        CHECK(cd::checkStoppedRecorded(base.l,{&base,Model::read},b,&j)==a.status);
        CHECK(base.reads==plain.reads&&a.poweredOffMask==b.poweredOffMask);verify(j,base,0,0);
        if(!mask)CHECK(j.recorded==239);
        for(size_t at=1;at<=base.reads.size();++at)for(bool ones:{false,true}){
            Model original,traced;original.off(mask);traced.off(mask);
            if(ones)original.unavailableAt=traced.unavailableAt=at;else original.failAt=traced.failAt=at;
            const auto status=original.run(a);
            CHECK(cd::checkStoppedRecorded(traced.l,{&traced,Model::read},b,&j)==status);
            CHECK(original.reads==traced.reads&&traced.reads.size()==at);
            CHECK(a.reads==b.reads&&a.lastRegister==b.lastRegister&&a.lastValue==b.lastValue&&
                a.mask==b.mask&&a.expected==b.expected&&a.pass==b.pass&&a.pipe==b.pipe);
            verify(j,traced,ones?0:at,ones?at:0);
        }
    }
    {Model m;cd::Result r{};cd::ReadCapture j{};m.regs[m.l.wb[15]]=1;
        CHECK(cd::checkStoppedRecorded(m.l,{&m,Model::read},r,&j)==cd::Status::WritebackNotStopped);
        CHECK(j.entries[j.recorded-1].byteOffset==m.l.wb[15]&&j.entries[j.recorded-1].returnedValue==1);
        verify(j,m,0,0);}
    {Model m;cd::Result r{};cd::ReadCapture j{};j.attempted=99;
        CHECK(cd::checkStoppedRecorded(m.l,{nullptr,nullptr},r,&j)==cd::Status::InvalidArgument&&!j.attempted);
        CHECK(cd::checkStoppedRecorded(m.l,{&m,Model::read},r,nullptr)==cd::Status::Ok);}
}

}
int main(int argc,char** argv){layouts();protocol();power();currentPowerStates();readJournalEvidence();
    if(argc>1){std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),{});
        cd::Layout l={};CHECK(!b.empty() && cd::buildLayout(b.data(),b.size(),0x100000,l)==cd::Status::Ok);
        CHECK(l.ih[0]==0x4588 && l.sos==0x58244 && l.mall==(0x34c0+0x523)*4);}
    std::cout<<checks<<" cold DMA checks; "<<failures<<" failures\n";return failures?1:0;}
