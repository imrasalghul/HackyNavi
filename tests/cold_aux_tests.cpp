// SPDX-License-Identifier: MIT
// Host-only register model. No real GPU access or execution is performed.
#include "hackynavi/cold_aux.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <type_traits>
#include <vector>
using namespace hackynavi;
namespace ca=hackynavi::coldaux;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<"line "<<__LINE__<<": " #x "\n";}}while(0)
void put16(std::vector<uint8_t>& b,size_t i,uint16_t v){b[i]=uint8_t(v);b[i+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t i,uint32_t v){put16(b,i,uint16_t(v));put16(b,i+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t i){uint16_t s=0;for(;i<b.size();++i)s=uint16_t(s+b[i]);return s;}
void fix(std::vector<uint8_t>& b){put16(b,14,sum(b,60));put16(b,8,sum(b,10));}
std::vector<uint8_t> fixture() {
    std::vector<uint8_t> b(208,0);
    put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,208);put16(b,12,60);put16(b,16,148);
    put32(b,60,0x53445049);put16(b,64,3);put16(b,66,148);put16(b,72,1);put16(b,76,140);put16(b,142,4);
    const uint16_t ids[]={12,12,91,255};const uint8_t major[]={4,4,6,13};
    const uint32_t base0[]={0x7800,0x7b00,0x11400,0x16000},base1[]={0x7e00,0x12000,0x2456800,0x16000};
    for(size_t i=0;i<4;++i){const size_t at=144+i*16;put16(b,at,ids[i]);b[at+3]=2;b[at+4]=major[i];
        if(i==1){b[at+2]=1;b[at+6]=1;b[at+7]=0x10;}put32(b,at+8,base0[i]);put32(b,at+12,base1[i]);}
    fix(b);return b;
}
ca::Layout layout(){auto b=fixture();ca::Layout l={};CHECK(ca::buildLayout(b.data(),b.size(),0x100000,l)==ca::Status::Ok);return l;}
struct Model {
    ca::Layout l=layout();std::map<uint32_t,uint32_t> regs;std::vector<uint32_t> reads;
    size_t failAt=0,unavailableAt=0,changeAt=0;uint32_t changeReg=0,changeValue=0;
    Model(){
        regs[l.sos]=0;regs[l.vcnPower[0]]=0;regs[l.vcnPower[1]]=0x02200800;
        regs[l.vcnPowerConfig[0]]=0;regs[l.vcnPowerConfig[1]]=0;
        const uint32_t vcn[]={0,0,0x10000000,0x2004,8,0x100,0x24f,0};
        const uint32_t jpeg[]={0,1,1,3,0x1bfff};
        const uint32_t lsdma[]={0,0,0,0,0,0,0,0x90000000,0x000e2001,0x27f,0x001001ff,0x501001ff};
        for(size_t v=0;v<2;++v)for(size_t i=0;i<ca::kVcnRegisters;++i)regs[l.vcn[v][i]]=vcn[i];
        for(size_t i=0;i<ca::kJpegRegisters;++i)regs[l.jpeg[i]]=jpeg[i];
        for(size_t i=0;i<ca::kLsdmaRegisters;++i)regs[l.lsdma[i]]=lsdma[i];
    }
    static bool read(void* c,uint32_t reg,uint32_t& value){auto& m=*static_cast<Model*>(c);m.reads.push_back(reg);
        CHECK(m.regs.count(reg)==1);if(m.reads.size()==m.changeAt)m.regs[m.changeReg]=m.changeValue;
        value=m.regs[reg];if(m.reads.size()==m.unavailableAt)value=UINT32_MAX;return m.reads.size()!=m.failAt;}
    ca::Status run(ca::Result& r){return ca::checkStopped(l,{this,read},r);}
};
void layouts(){
    auto b=fixture();ca::Layout l={};CHECK(ca::buildLayout(b.data(),b.size(),0x100000,l)==ca::Status::Ok);
    CHECK(l.vcnPower[0]==0x1f804 && l.vcnPower[1]==0x48004 && l.sos==0x58244);
    CHECK(l.jpeg[2]==(0x7e00+0x6d2)*4 && l.lsdma[7]==(0x11400+0x32)*4);
    for(size_t n=0;n<b.size();++n)CHECK(ca::buildLayout(b.data(),n,0x100000,l)!=ca::Status::Ok && !l.valid);
    const size_t invalidFields[]={146,147,148,149,150,151,162,164,166,167,178,180,182,194,196,198};
    for(size_t at:invalidFields){auto bad=b;bad[at]^=4;fix(bad);CHECK(ca::buildLayout(bad.data(),bad.size(),0x100000,l)!=ca::Status::Ok && !l.valid);}
    auto alias=b;put32(alias,160+12,0x7e00);fix(alias);
    CHECK(ca::buildLayout(alias.data(),alias.size(),0x100000,l)==ca::Status::OutsideBar);
    CHECK(ca::buildLayout(b.data(),b.size(),0x40000,l)==ca::Status::OutsideBar && !l.valid);
}
void protocol(){
    ca::Result r={};Model baseline;CHECK(baseline.run(r)==ca::Status::Ok && r.reads==180);
    CHECK(baseline.reads.front()==baseline.l.sos && baseline.reads.back()==baseline.l.sos);
    for(size_t i=1;i<=180;++i){Model m;m.failAt=i;CHECK(m.run(r)==ca::Status::ReadFailed && m.reads.size()==i);
        Model n;n.unavailableAt=i;CHECK(n.run(r)==ca::Status::DeviceUnavailable && n.reads.size()==i);}
    const uint32_t vcnMasks[]={0x80000107,0x1ff,0x10000200,0x2004,8,0x100,0x2cf,7};
    const uint32_t jpegMasks[]={0x80000011,1,1,0x1fff,0x1ffff};
    const uint32_t lsdmaMasks[]={0x1001,1,1,0x1001,1,1,0x60000,0xb0078fff,0x002e3201,0x1be7f,0x003fffff,0xf03fffff};
    for(size_t v=0;v<2;++v)for(size_t f=0;f<ca::kVcnRegisters;++f)for(unsigned bit=0;bit<32;++bit)
        if(vcnMasks[f]&(1U<<bit)){Model m;m.regs[m.l.vcn[v][f]]^=1U<<bit;CHECK(m.run(r)==ca::Status::VcnNotStopped && r.instance==v);}
    for(size_t f=0;f<ca::kJpegRegisters;++f)for(unsigned bit=0;bit<32;++bit)
        if(jpegMasks[f]&(1U<<bit)){Model m;m.regs[m.l.jpeg[f]]^=1U<<bit;CHECK(m.run(r)==ca::Status::JpegNotStopped);}
    for(size_t f=0;f<ca::kLsdmaRegisters;++f)for(unsigned bit=0;bit<32;++bit)
        if(lsdmaMasks[f]&(1U<<bit)){Model m;m.regs[m.l.lsdma[f]]^=1U<<bit;CHECK(m.run(r)==ca::Status::LsdmaNotIdle);}
    for(unsigned pass=0;pass<2;++pass)for(size_t v=0;v<2;++v){Model m;
        m.changeAt=pass*85+2+v*27;m.changeReg=m.l.vcnPower[v];m.changeValue=1;
        CHECK(m.run(r)==ca::Status::PowerUnavailable && m.reads.back()==m.l.vcnPower[v]);}
    for(uint32_t power:{1U,3U,UINT32_MAX}){Model m;m.regs[m.l.vcnPower[0]]=power;
        CHECK(m.run(r)!=(ca::Status::Ok) && m.reads.size()==2);}
    {Model m;m.regs[m.l.vcnPower[0]]=0x2a2a8aa0;m.regs[m.l.vcnPower[1]]=0x2a2a8aa0;
        CHECK(m.run(r)==ca::Status::Ok);}
    {Model m;m.regs[m.l.vcnPower[0]]=0x400000;CHECK(m.run(r)==ca::Status::PowerUnavailable && r.lastRegister==m.l.vcnPower[0]);}
    for(size_t at:{size_t(1),size_t(86),size_t(180)}){Model m;m.changeAt=at;m.changeReg=m.l.sos;m.changeValue=1;
        CHECK(m.run(r)==ca::Status::PriorDriverRequiresReset && m.reads.size()==at);}
    {Model m;m.l.jpeg[0]+=4;CHECK(m.run(r)==ca::Status::InvalidArgument && m.reads.empty());}
    {Model m;m.l.lsdma[0]^=1;CHECK(m.run(r)==ca::Status::InvalidArgument && m.reads.empty());}
}
void setOff(Model& m,unsigned mask) {
    for(unsigned v=0;v<2;++v)if(mask&(1U<<v)) {
        m.regs[m.l.vcnPower[v]]=(m.regs[m.l.vcnPower[v]]&0xc00000)|0x2a2aaaaa;
        for(size_t i=0;i<ca::kVcnRegisters;++i)m.regs[m.l.vcn[v][i]]=UINT32_MAX;
    }
    if(mask&4){m.regs[m.l.vcnPower[0]]|=0x800000;
        for(size_t i=0;i<ca::kJpegRegisters;++i)m.regs[m.l.jpeg[i]]=UINT32_MAX;}
}
void offPower(){
    ca::Result r={};
    for(unsigned mask=0;mask<8;++mask){Model m;setOff(m,mask);
        CHECK(m.run(r)==ca::Status::Ok && r.poweredOffMask==mask);
        for(uint32_t reg:m.reads){
            for(unsigned v=0;v<2;++v)if(mask&(1U<<v))for(uint32_t client:m.l.vcn[v])CHECK(reg!=client);
            if(mask&4)for(uint32_t client:m.l.jpeg)CHECK(reg!=client);
        }
    }
    Model allOff;setOff(allOff,7);CHECK(allOff.run(r)==ca::Status::Ok && r.reads==54);
    for(size_t at=1;at<=allOff.reads.size();++at)for(bool unavailable:{false,true}){
        Model m;setOff(m,7);if(unavailable)m.unavailableAt=at;else m.failAt=at;
        CHECK(m.run(r)==(unavailable?ca::Status::DeviceUnavailable:ca::Status::ReadFailed) && m.reads.size()==at);
    }
    // Every tile wake/invalid request prevents OFF admission. A no-op or OFF
    // request is allowed; unrelated JPEG/media CONFIG fields are independent.
    for(unsigned engine=0;engine<3;++engine){const unsigned v=engine==1?1:0;
        const uint32_t mask=engine==2?0xc00000:0x3f3fffff;
        for(unsigned bit=0;bit<30;bit+=2)if(mask&(3U<<bit))for(uint32_t value:{1U,3U}){
            Model m;setOff(m,7);m.regs[m.l.vcnPowerConfig[v]]=value<<bit;
            CHECK(m.run(r)==ca::Status::PowerUnavailable && !r.poweredOffMask);
        }
        Model m;setOff(m,7);m.regs[m.l.vcnPowerConfig[v]]=mask&0xaaaaaaaa;
        CHECK(m.run(r)==ca::Status::Ok);
    }
    // CONFIG cannot announce an impending gate while a powered window is read.
    for(unsigned v=0;v<2;++v){Model m;m.regs[m.l.vcnPowerConfig[v]]=2;
        CHECK(m.run(r)==ca::Status::PowerUnavailable);}
    // Linux all-ON request on VCN1 retains its physically absent tile status.
    {Model m;m.regs[m.l.vcnPowerConfig[1]]=0x15155555;CHECK(m.run(r)==ca::Status::Ok);}
    for(size_t at=1;at<=allOff.reads.size();++at)for(unsigned v=0;v<2;++v)
        if(allOff.reads[at-1]==allOff.l.vcnPower[v]){Model m;setOff(m,7);m.changeAt=at;
            m.changeReg=m.l.vcnPower[v];m.changeValue=1;
            CHECK(m.run(r)!=ca::Status::Ok && m.reads.size()>=at);}
    {Model m;m.l.vcnPowerConfig[0]+=4;CHECK(m.run(r)==ca::Status::InvalidArgument && m.reads.empty());}
}
void liveCaptureRegression(){
    // Actual 7900 XTX/macOS capture live-6a3a8173c6e2-20261011-000226-708:
    // zero MP0 sOS, followed by UINT32_MAX at VCN0 UVD_PGFSM_STATUS. Discovery
    // reported VCN0 base[1]=0x7e00, so 0x1f804 is the documented register;
    // treating this as completed PGFSM OFF would incorrectly admit memory use.
    Model m;ca::Result r={};m.regs[m.l.vcnPower[0]]=UINT32_MAX;
    CHECK(m.run(r)==ca::Status::DeviceUnavailable);
    CHECK(r.reads==2 && r.lastRegister==0x1f804 && r.lastValue==UINT32_MAX);
    CHECK(r.instance==0 && r.poweredOffMask==0);
    CHECK(m.reads==std::vector<uint32_t>({0x58244,0x1f804}));
}
void vcnPowerInventory(){
    using P=ca::PowerInventoryStatus;
    static_assert(!std::is_convertible<ca::PowerInventory,ca::Result>::value,
        "Register access observation must not become a DMA stop certificate");
    static_assert(!std::is_convertible<P,ca::Status>::value,
        "Access prerequisite status must not implicitly admit a stopped engine");
    ca::PowerInventory r={};Model baseline;
    const std::vector<uint32_t> expected={0x1f808,0x1f808,0x48008,0x48008};
    CHECK(ca::inventoryVcnPower(baseline.l,{&baseline,Model::read},r)==P::ReadAccessObserved);
    CHECK(baseline.reads==expected && r.reads==4 && r.failedInstance==0xff && r.lastReadSucceeded);
    CHECK(r.lastRegister==0x48008 && !r.lastValue);
    for(unsigned v=0;v<2;++v) {
        const auto& p=r.probes[v];CHECK(p.instance==v && p.reg==expected[v*2]);
        CHECK(p.attemptedMask==3 && p.readSucceededMask==3 && p.validMask==3 && !p.raw[0] && !p.raw[1]);
    }
    // Every unimplemented/reserved bit is diagnostic data, not decoded here.
    // The existing full checker remains responsible for its stricter static
    // power/reset/ring/DMA predicates after this access-only observation.
    for(unsigned v=0;v<2;++v)for(unsigned bit=1;bit<32;++bit) {
        Model m;m.regs[m.l.vcn[v][0]]=1U<<bit;
        CHECK(ca::inventoryVcnPower(m.l,{&m,Model::read},r)==P::ReadAccessObserved);
        CHECK(r.probes[v].raw[0]==(1U<<bit) && r.probes[v].raw[1]==(1U<<bit) && m.reads==expected);
    }
    // Both always-readable pairs are inventoried even when TILES_OFF is set;
    // the status never grants access to PGFSM, JPEG, LSDMA, or any client.
    for(unsigned mask=1;mask<4;++mask) {
        Model m;
        for(unsigned v=0;v<2;++v) {
            m.regs[m.l.vcnPower[v]]=UINT32_MAX;
            if(mask&(1U<<v))m.regs[m.l.vcn[v][0]]=1;
        }
        CHECK(ca::inventoryVcnPower(m.l,{&m,Model::read},r)==P::TilesOff);
        CHECK(m.reads==expected && r.reads==4 && r.failedInstance==(mask&1?0:1));
        for(unsigned v=0;v<2;++v)CHECK(r.probes[v].validMask==3 && r.probes[v].raw[0]==((mask>>v)&1U));
    }
    // On each transport failure, the callback may have written an output:
    // do not publish that untrusted value or carry the preceding raw value.
    for(size_t at=1;at<=4;++at)for(bool unavailable:{false,true}) {
        Model m;
        m.regs[m.l.vcn[0][0]]=0x100;m.regs[m.l.vcn[1][0]]=0x200;
        if(unavailable)m.unavailableAt=at;else m.failAt=at;
        CHECK(ca::inventoryVcnPower(m.l,{&m,Model::read},r)==(unavailable?P::DeviceUnavailable:P::ReadFailed));
        CHECK(m.reads==std::vector<uint32_t>(expected.begin(),expected.begin()+at) && r.reads==at);
        CHECK(r.lastRegister==expected[at-1] && r.failedInstance==(at-1)/2 && r.lastReadSucceeded==unavailable);
        CHECK(r.lastValue==(unavailable?UINT32_MAX:0));
        for(unsigned v=0;v<2;++v) {
            const unsigned attempted=at>=v*2+2?3U:(at==v*2+1?1U:0U);
            const unsigned completed=(at-1)>=v*2+2?3U:((at-1)==v*2+1?1U:0U);
            const auto& p=r.probes[v];
            CHECK(p.attemptedMask==attempted && p.validMask==completed);
            CHECK(p.readSucceededMask==(unavailable?attempted:completed));
            CHECK(p.instance==v && p.reg==(attempted?expected[v*2]:0));
        }
        CHECK(r.probes[(at-1)/2].raw[(at-1)%2]==(unavailable?UINT32_MAX:0));
    }
    for(unsigned v=0;v<2;++v)for(unsigned bit=0;bit<32;++bit) {
        Model m;m.changeAt=v*2+2;m.changeReg=m.l.vcn[v][0];m.changeValue=1U<<bit;
        CHECK(ca::inventoryVcnPower(m.l,{&m,Model::read},r)==P::Changing);
        CHECK(r.reads==v*2+2 && m.reads.size()==r.reads && r.failedInstance==v);
        CHECK(r.probes[v].validMask==3 && r.probes[v].raw[1]==(1U<<bit));
    }
    // A tile-off first sample changing to awake also fails, not Ready or OFF.
    for(unsigned v=0;v<2;++v) {
        Model m;m.regs[m.l.vcn[v][0]]=1;m.changeAt=v*2+2;m.changeReg=m.l.vcn[v][0];m.changeValue=0;
        CHECK(ca::inventoryVcnPower(m.l,{&m,Model::read},r)==P::Changing && r.failedInstance==v);
    }
    {Model m;m.l.vcn[0][0]+=4;
        CHECK(ca::inventoryVcnPower(m.l,{&m,Model::read},r)==P::InvalidArgument && m.reads.empty());}
    {Model m;m.l.barBytes=0x40000;
        CHECK(ca::inventoryVcnPower(m.l,{&m,Model::read},r)==P::InvalidArgument && m.reads.empty());}
    {Model m;m.l.valid=false;
        CHECK(ca::inventoryVcnPower(m.l,{&m,Model::read},r)==P::InvalidArgument && m.reads.empty());}
    {Model m;
        CHECK(ca::inventoryVcnPower(m.l,{&m,nullptr},r)==P::InvalidArgument && m.reads.empty());
        CHECK(!r.reads && r.failedInstance==0xff && !r.lastReadSucceeded && !r.lastRegister && !r.lastValue);
        for(unsigned v=0;v<2;++v)CHECK(r.probes[v].instance==v && !r.probes[v].attemptedMask && !r.probes[v].reg);}
    // The missing POWER_STATUS observation changes no existing admission.
    // A readable pair plus the captured all-ones PGFSM still fails unchanged.
    {Model m;m.regs[m.l.vcnPower[0]]=UINT32_MAX;
        CHECK(ca::inventoryVcnPower(m.l,{&m,Model::read},r)==P::ReadAccessObserved && m.reads==expected);
        ca::Result stopped={};CHECK(m.run(stopped)==ca::Status::DeviceUnavailable && !stopped.poweredOffMask);
        CHECK(m.reads.size()==6 && m.reads[4]==m.l.sos && m.reads[5]==0x1f804);}
}
void lsdmaOnly(){
    static_assert(!std::is_convertible<ca::LsdmaResult,ca::Result>::value,
        "LSDMA observation must not become a full auxiliary stop result");
    static_assert(!std::is_convertible<ca::LsdmaStatus,ca::Status>::value,
        "LSDMA status must remain explicitly scoped");
    const uint32_t masks[]={0x1001,1,1,0x1001,1,1,0x60000,0xb0078fff,
        0x002e3201,0x1be7f,0x003fffff,0xf03fffff};
    Model baseline;ca::LsdmaResult r={};
    // Reproduce unavailable media windows without claiming their state. Only
    // the independently readable LSDMA fixture may contribute to this result.
    for(unsigned v=0;v<2;++v) {
        baseline.regs[baseline.l.vcnPower[v]]=UINT32_MAX;
        baseline.regs[baseline.l.vcnPowerConfig[v]]=UINT32_MAX;
        for(uint32_t reg:baseline.l.vcn[v])baseline.regs[reg]=UINT32_MAX;
    }
    for(uint32_t reg:baseline.l.jpeg)baseline.regs[reg]=UINT32_MAX;
    CHECK(ca::checkLsdmaStopped(baseline.l,{&baseline,Model::read},r)==ca::LsdmaStatus::Ok);
    std::vector<uint32_t> expected;
    for(unsigned pass=0;pass<2;++pass){expected.push_back(baseline.l.sos);
        for(uint32_t reg:baseline.l.lsdma)expected.push_back(reg);}
    expected.push_back(baseline.l.sos);
    CHECK(baseline.reads==expected && r.reads==27 && r.pass==2);
    CHECK(r.lastRegister==baseline.l.sos && !r.lastValue && !r.expected && r.mask==UINT32_MAX);
    // The full diagnostic still fails at exactly the real target PGFSM read.
    baseline.reads.clear();ca::Result full={};
    CHECK(baseline.run(full)==ca::Status::DeviceUnavailable && full.lastRegister==0x1f804);
    CHECK(baseline.reads==std::vector<uint32_t>({0x58244,0x1f804}) && !full.poweredOffMask);
    // Each access, including all three SOS guards, must fail without further
    // reads when the provider disappears or the MMIO transport fails.
    for(size_t at=1;at<=expected.size();++at) {
        Model failed;failed.failAt=at;
        CHECK(ca::checkLsdmaStopped(failed.l,{&failed,Model::read},r)==ca::LsdmaStatus::ReadFailed);
        CHECK(r.reads==at && failed.reads.size()==at && r.lastRegister==expected[at-1] && !r.lastValue);
        Model absent;absent.unavailableAt=at;
        CHECK(ca::checkLsdmaStopped(absent.l,{&absent,Model::read},r)==ca::LsdmaStatus::DeviceUnavailable);
        CHECK(r.reads==at && absent.reads.size()==at && r.lastRegister==expected[at-1] && r.lastValue==UINT32_MAX);
    }
    for(size_t at:{size_t(1),size_t(14),size_t(27)}) {
        Model changed;changed.changeAt=at;changed.changeReg=changed.l.sos;changed.changeValue=1;
        CHECK(ca::checkLsdmaStopped(changed.l,{&changed,Model::read},r)==ca::LsdmaStatus::PriorDriverRequiresReset);
        CHECK(r.reads==at && changed.reads.size()==at && r.lastRegister==changed.l.sos && r.lastValue==1);
    }
    // Exercise both complete observations, not just the first successful pass.
    for(unsigned pass=0;pass<2;++pass)for(size_t i=0;i<ca::kLsdmaRegisters;++i) {
        Model busy;busy.changeAt=pass*13+i+2;busy.changeReg=busy.l.lsdma[i];
        busy.changeValue=busy.regs[busy.changeReg]^(masks[i]&(~masks[i]+1U));
        CHECK(ca::checkLsdmaStopped(busy.l,{&busy,Model::read},r)==ca::LsdmaStatus::LsdmaNotIdle);
        CHECK(r.reads==busy.changeAt && r.pass==pass && r.lastRegister==busy.changeReg &&
            r.lastValue==busy.changeValue && r.mask==masks[i]);
    }
    {Model bad;bad.l.lsdma[0]^=1;
        CHECK(ca::checkLsdmaStopped(bad.l,{&bad,Model::read},r)==ca::LsdmaStatus::InvalidArgument &&
            bad.reads.empty() && !r.reads && r.pass==0xff);}
    {Model bad;bad.l.vcnPower[0]+=4; // Layout is still discovery-bound, even without media I/O.
        CHECK(ca::checkLsdmaStopped(bad.l,{&bad,Model::read},r)==ca::LsdmaStatus::InvalidArgument && bad.reads.empty());}
    {Model bad;
        CHECK(ca::checkLsdmaStopped(bad.l,{&bad,nullptr},r)==ca::LsdmaStatus::InvalidArgument && bad.reads.empty());}
}
}
int main(int argc,char** argv){
    if(argc>1 && std::string(argv[1])=="--lsdma-only") {
        lsdmaOnly();std::cout<<checks<<" focused LSDMA checks; "<<failures<<" failures\n";return failures?1:0;
    }
    layouts();protocol();offPower();liveCaptureRegression();vcnPowerInventory();lsdmaOnly();
    if(argc>1){std::ifstream input(argv[1],std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
        ca::Layout l={};CHECK(!bytes.empty() && ca::buildLayout(bytes.data(),bytes.size(),0x100000,l)==ca::Status::Ok);
        CHECK(l.vcnPower[0]==0x1f804 && l.vcnPower[1]==0x48004 && l.sos==0x58244);}
    std::cout<<checks<<" cold auxiliary engine checks; "<<failures<<" failures\n";return failures?1:0;}
