// SPDX-License-Identifier: MIT
// Offline register-callback tests only. No hardware state or ownership is simulated in production.
#include "hackynavi/boot_backend_stop.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>
namespace b=hackynavi::bootbackend;
namespace s=hackynavi::bootbackendstop;
namespace bs=hackynavi::bootdisplaystop;
namespace bd=hackynavi::bootdisplay;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v) { b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8); }
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v) { put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16)); }
uint16_t sum(const std::vector<uint8_t>& b,size_t p) { uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s; }
void fix(std::vector<uint8_t>& b) {put16(b,14,sum(b,60));put16(b,8,sum(b,10));}
std::vector<uint8_t> discovery() {
    std::vector<uint8_t> b(188,0);
    put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,188);
    put16(b,12,60);put16(b,16,128);put32(b,60,0x53445049);put16(b,64,3);
    put16(b,66,128);put16(b,72,1);put16(b,76,140);put16(b,142,2);
    put16(b,144,271);b[147]=4;b[148]=3;b[149]=2;put32(b,156,0x1000);put32(b,160,0x34c0);put32(b,164,0x8000);
    put16(b,168,255);b[171]=2;b[172]=13;put32(b,176,0x16000);put32(b,180,0x16000);fix(b);return b;
}
void table(std::vector<uint8_t>& b,size_t p,uint16_t size,uint8_t major,uint8_t minor) {
    put16(b,p,size);b[p+2]=major;b[p+3]=minor;
}
std::vector<uint8_t> rom() {
    std::vector<uint8_t> b(4096,0);b[0]=0x55;b[1]=0xaa;b[2]=8;
    put16(b,0x18,0x80);std::memcpy(b.data()+0x80,"PCIR",4);
    put16(b,0x84,0x1002);put16(b,0x86,0x744c);put16(b,0x8a,24);put16(b,0x90,8);b[0x95]=0x80;
    put16(b,0x48,0x100);table(b,0x100,44,2,3);std::memcpy(b.data()+0x104,"ATOM",4);
    put16(b,0x11e,0x190);put16(b,0x120,0x140);table(b,0x190,8,2,1);table(b,0x140,74,2,1);
    put16(b,0x144+12*2,0x200);put16(b,0x144+22*2,0x300);
    table(b,0x200,84,2,1);
    const uint8_t shifts[]={0,8,16,24,26};
    for(size_t i=0;i<5;++i) {
        const size_t h=0x204+i*8,c=0x22c+i*8;
        put32(b,h,0x5db5);b[h+4]=shifts[i];b[h+6]=uint8_t(i+1);
        put32(b,c,0x5d91+uint32_t(i)*4);b[c+4]=8;b[c+6]=uint8_t(0x90+i);
    }
    table(b,0x300,112,1,5);b[0x306]=4;
    const uint16_t ids[]={0x3113,0x3213,0x3313,0x340c}, enc[]={0x211e,0x2121,0x221e,0x2220};
    const uint8_t mux[]={3,4,2,0};
    for(size_t i=0;i<4;++i) {
        const size_t p=0x308+i*16,r=0x348+i*10;
        put16(b,p,ids[i]);put16(b,p+2,uint16_t(72+i*10));put16(b,p+4,enc[i]);put16(b,p+12,uint16_t(1U<<i));
        b[r]=1;b[r+1]=4;b[r+2]=uint8_t(0x90+mux[i]);b[r+4]=2;b[r+5]=4;b[r+6]=mux[i]+1;b[r+7]=1;b[r+8]=0xff;
    }
    b.back()=uint8_t(0-sum(b,0));return b;
}

// Test-side addresses independently named from actual generated DCN3.2 tables.
const uint16_t lo[]={0x1f04,0x20b1,0x20b2};
const uint16_t fe[]={0x208b,0x2091,0x210c,0x2094,0x2098,0x209e};
const uint16_t tg[]={0x1b41,0x1b85,0x1b8a,0x1b91,0x1b8b,0x1b54,0x1b82,0x1b44,0x1aca,0x1acb,0x1acc,0x1ad0};
const uint16_t mp[]={0,1,2,5,0xe,0x3d8};
uint32_t address(b::Bank bank,size_t n,size_t r) {
    if(bank==b::Bank::Link)return uint32_t((0x34c0+lo[r]+n*(r?0x100:1))*4);
    if(bank==b::Bank::Frontend)return uint32_t((0x34c0+fe[r]+n*0x100)*4);
    if(bank==b::Bank::Timing)return uint32_t((0x34c0+tg[r]+n*(r<8?0x80:0x10))*4);
    return uint32_t((0x8000+mp[r]+n*(r==5?4:0x15))*4);
}
struct Device {
    std::vector<uint8_t> disc=discovery(),bios=rom();
    std::map<uint32_t,uint32_t> regs;
    std::vector<uint32_t> accessed;
    size_t reads=0,guards=0,permissions=0,failAt=0,onesAt=0,changedAt=0,denyGuard=0,denyPermission=0;
    size_t imageOffset=0;uint32_t changeMask=0x80000000u;
    bd::Layout hubp{};hackynavi::MemoryLayout memory{};bs::State drained;
    std::vector<uint32_t> writes;std::vector<size_t> writeReads;size_t failWrite=0,dropWrite=0,denyWrite=0;
    bool applyFailedWrite=false,neverMpc=false,neverTiming=false,denyAfterWrite=false;
    uint8_t selectedFront=2,selectedTiming=1,selectedOutput=3,selectedCombiner=0;
    unsigned delays=0;
    bool missing=false;
    Device(uint8_t front=2,uint8_t timing=1,uint8_t output=3,uint8_t combiner=0,uint8_t dpp=2,uint8_t offMask=0) {
        for(unsigned bank=0;bank<4;++bank) {
            const size_t count=bank<2?5:4, nr=bank==0?3:bank==1?6:bank==2?12:6;
            for(size_t n=0;n<count;++n)for(size_t r=0;r<nr;++r)regs[address(b::Bank(bank),n,r)]=0;
        }
        for(size_t n=0;n<4;++n) {
            set(b::Bank::Timing,n,9,0xffff0000);
            for(size_t r=0;r<4;++r)set(b::Bank::Combiner,n,r,15);
            set(b::Bank::Combiner,n,4,5);set(b::Bank::Combiner,n,5,15);
        }
        // Actual synthetic Atom HDMI path maps PHY3, DDC0, HPD0 independently.
        set(b::Bank::Link,3,1,0x30000u|(0x100u<<front));set(b::Bank::Link,3,2,0x101);
        set(b::Bank::Frontend,front,0,0x01000000u|timing);set(b::Bank::Frontend,front,1,0x1d);
        set(b::Bank::Frontend,front,3,0x19);set(b::Bank::Frontend,front,4,0x31);
        set(b::Bank::Timing,timing,0,0x10201);set(b::Bank::Timing,timing,1,0x10101);
        set(b::Bank::Timing,timing,9,0xfff00000u|(uint32_t(output)<<16));
        set(b::Bank::Combiner,output,5,combiner);set(b::Bank::Combiner,combiner,0,dpp);
        set(b::Bank::Combiner,combiner,2,output);set(b::Bank::Combiner,combiner,3,output);
        set(b::Bank::Combiner,combiner,4,2);
        selectedFront=front;selectedTiming=timing;selectedOutput=output;selectedCombiner=combiner;
        CHECK(bd::buildLayout(disc.data(),disc.size(),0x100000,hubp)==bd::Status::Ok);
        CHECK(hackynavi::deriveMemoryLayout({0x100,0x6ff,0x1000,24ULL<<30,0x8000000000ULL,256ULL<<20},memory)==hackynavi::MemoryLayoutStatus::Ok);
        regs[hubp.sos]=0;for(auto a:hubp.vga)regs[a]=0;
        const uint32_t values[]={0x100,0x6ff,0x1000,0xffffff,0};
        for(unsigned i=0;i<5;++i)regs[hubp.global[i]]=values[i];
        for(unsigned p=0;p<4;++p){regs[hubp.power[p]]=0;regs[hubp.powerConfig[p]]=0;
            for(auto a:hubp.pipe[p])regs[a]=0;
            regs[hubp.pipe[p][0]]=3;regs[hubp.pipe[p][0]+4]=1;
            if(offMask&(1u<<p)){regs[hubp.power[p]]=0x80000000;regs[hubp.powerConfig[p]]=0x100;
                for(auto a:hubp.pipe[p])regs[a]=UINT32_MAX;regs[hubp.pipe[p][0]+4]=UINT32_MAX;}
        }
        regs[vtg()]=0x81000100;regs[dpg()]=1;regs[dpg()+7*4]=0;
        bs::Result br{};CHECK(bs::capture(hubp,memory,{this,read,nullptr,nullptr},drained,br)==bs::Status::Ok);
        CHECK(bs::stop({this,read,rawWrite,rawDelay},{4,1},drained,br)==bs::Status::Ok);
        resetCounts();
    }
    void set(b::Bank bank,size_t n,size_t r,uint32_t value){regs[address(bank,n,r)]=value;}
    b::Io bio(){return {this,valid,canRead,read};}
    static bool valid(void* p){auto& s=*static_cast<Device*>(p);return ++s.guards!=s.denyGuard;}
    static bool canRead(void* p,b::Bank bank,uint8_t n,uint32_t a) {
        auto& s=*static_cast<Device*>(p);++s.permissions;
        CHECK(unsigned(bank)<4&&n<(unsigned(bank)<2?5:4));CHECK(s.regs.count(a)==1);
        return s.permissions!=s.denyPermission;
    }
    static bool read(void* p,uint32_t a,uint32_t& value) {
        auto& s=*static_cast<Device*>(p);++s.reads;s.accessed.push_back(a);
        const auto it=s.regs.find(a);if(it==s.regs.end()){s.missing=true;return false;}
        value=it->second;
        if(a==address(b::Bank::Combiner,s.selectedCombiner,4)&&s.regs[address(b::Bank::Combiner,s.selectedCombiner,3)]==15&&!s.neverMpc)value=s.regs[a]=5;
        if(a==address(b::Bank::Timing,s.selectedTiming,0)&&!(value&1)&&!s.neverTiming)value=s.regs[a]=value&~0x10000u;
        if(a==address(b::Bank::Timing,s.selectedTiming,1)&&!(s.regs[address(b::Bank::Timing,s.selectedTiming,0)]&1)&&!s.neverTiming)value=s.regs[a]=value&~0x10000u;if(s.reads==s.onesAt)value=UINT32_MAX;if(s.reads==s.changedAt)value^=s.changeMask;
        return s.reads!=s.failAt;
    }
    b::Status capture(b::Capture& c,b::Result& r) {return b::capture(disc.data(),disc.size(),0x100000,bios.data(),bios.size(),imageOffset,bio(),c,r);}
    uint32_t vtg() const{return (0x34c0+0x52e +selectedTiming)*4;}
    uint32_t dpg() const{return (0x34c0+0x1854+selectedOutput*0x5a)*4;}
    void resetCounts(){reads=guards=permissions=0;accessed.clear();writes.clear();writeReads.clear();}
    static bool rawWrite(void* p,uint32_t a,uint32_t v){static_cast<Device*>(p)->regs[a]=v;return true;}
    static void rawDelay(void*,uint32_t){}
    static bool permit(void* p,s::Domain d,uint8_t n,uint32_t a,bool write){
        auto& x=*static_cast<Device*>(p);CHECK(n<5&&x.regs.count(a));
        if(d==s::Domain::Hubp)CHECK(n<4&&(x.regs[x.hubp.power[n]]==0||x.regs[x.hubp.power[n]]==0x10000000)&&x.regs[x.hubp.powerConfig[n]]<=1);
        if(write&&x.denyAfterWrite&&!x.writes.empty())return false;
        return !write||!x.denyWrite||x.writes.size()+1!=x.denyWrite;
    }
    static bool write(void* p,uint32_t a,uint32_t v){auto& x=*static_cast<Device*>(p);x.writes.push_back(a);x.writeReads.push_back(x.reads);
        const bool failed=x.writes.size()==x.failWrite;
        if((!failed||x.applyFailedWrite)&&x.writes.size()!=x.dropWrite)x.regs[a]=v;
        return !failed;
    }
    static bool delay(void* p,uint32_t us){CHECK(us==1);++static_cast<Device*>(p)->delays;return true;}
    s::Io io(){return {bio(),permit,write,delay};}
    s::Status arm(s::Controller& c,s::Result& r){return c.capture(disc.data(),disc.size(),0x100000,bios.data(),bios.size(),0,io(),drained,r);}
    void stopLink(){set(b::Bank::Link,3,1,regs[address(b::Bank::Link,3,1)]&~0x7f00u);set(b::Bank::Link,3,2,0);
        // Actual DCN3.2 reset does not disable its latched FIFO_ENABLE bit.
        set(b::Bank::Frontend,selectedFront,3,(regs[address(b::Bank::Frontend,selectedFront,3)]&~0x01000006u)|0x19u);
        set(b::Bank::Frontend,selectedFront,5,1);
    }
};

void positive(){
    for(uint8_t tg=0;tg<4;++tg)for(uint8_t mp=0;mp<4;++mp){Device d(2,tg,3,mp,1);s::Controller c;s::Result r{};
        CHECK(d.arm(c,r)==s::Status::Ok&&c.phase()==s::Phase::Armed);CHECK(d.writes.empty());
        CHECK(c.disconnect({4,1},r)==s::Status::Ok&&c.phase()==s::Phase::MpcDetached&&r.attempted==31&&r.acknowledged==31);
        CHECK(d.writes.size()==5&&d.writes[0]==address(b::Bank::Combiner,3,5));
        CHECK((d.regs[address(b::Bank::Timing,tg,0)]&0x10001)==0x10001);
        d.stopLink();CHECK(c.stopTiming({4,1},r)==s::Status::Ok&&c.phase()==s::Phase::TimingStopped);
        CHECK(r.attempted==511&&r.acknowledged==511&&r.retentionRequired&&d.writes.size()==9);
        CHECK(d.writes[7]==address(b::Bank::Timing,tg,0)&&d.writes[8]==d.vtg());
        CHECK(d.regs[address(b::Bank::Timing,tg,1)]==0x101&&d.regs[d.dpg()]==1);
        CHECK(c.stopTiming({4,1},r)==s::Status::InvalidState&&c.disconnect({4,1},r)==s::Status::InvalidState&&d.writes.size()==9);
    }
}
void negatives(){
    {Device d;s::Controller c;s::Result r{};d.failAt=5;CHECK(d.arm(c,r)==s::Status::CaptureRejected&&r.reads==5&&r.backend.reads==5&&!r.lastReadSucceeded&&d.writes.empty());}
    {Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok);d.denyAfterWrite=true;
        CHECK(c.disconnect({4,1},r)==s::Status::AccessDenied&&d.writes.size()==1&&r.attempted==1&&r.acknowledged==0&&c.phase()==s::Phase::Faulted);}
    {Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok&&c.disconnect({4,1},r)==s::Status::Ok);
        d.stopLink();d.regs[address(b::Bank::Frontend,2,0)]&=~0x01000000u;
        CHECK(c.stopTiming({4,1},r)==s::Status::Ok&&d.regs[address(b::Bank::Frontend,2,1)]==0x1d);}
    {Device d(2,1,3,0,2,10);s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok&&c.disconnect({4,1},r)==s::Status::Ok);
        d.stopLink();CHECK(c.stopTiming({4,1},r)==s::Status::Ok);}
    {Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok);d.regs[d.dpg()]^=0x10;CHECK(c.disconnect({4,1},r)==s::Status::Changed&&d.writes.empty());}
    {Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok);d.regs[address(b::Bank::Timing,1,1)]^=2;CHECK(c.disconnect({4,1},r)==s::Status::Changed&&d.writes.empty());}
    {Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok);d.denyWrite=1;CHECK(c.disconnect({4,1},r)==s::Status::AccessDenied&&d.writes.empty()&&c.phase()==s::Phase::Faulted);}
    {Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok);d.denyGuard=d.guards+1;CHECK(c.disconnect({4,1},r)==s::Status::AccessDenied&&d.writes.empty());}
    {Device d;s::Controller c;s::Result r{};d.regs[address(b::Bank::Combiner,3,5)]|=0x10000;
        CHECK(d.arm(c,r)==s::Status::Ok&&c.disconnect({4,1},r)==s::Status::Ok);
        CHECK(d.regs[address(b::Bank::Combiner,3,5)]==0x1000f);}
    {Device d;s::Controller c;s::Result r{};d.regs[d.dpg()]=0;CHECK(d.arm(c,r)==s::Status::OPPNotBlank&&d.writes.empty());CHECK(d.arm(c,r)==s::Status::InvalidState);}
    {Device d;s::Controller c;s::Result r{};d.regs[d.dpg()+28]=1;CHECK(d.arm(c,r)==s::Status::OPPNotBlank);}
    {Device d;s::Controller c;s::Result r{};d.regs[d.vtg()]&=~0x80000000u;CHECK(d.arm(c,r)==s::Status::TimingNotRunning);}
    {Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok);CHECK(c.stopTiming({4,1},r)==s::Status::InvalidState&&d.writes.empty());
        CHECK(c.disconnect({0,1},r)==s::Status::InvalidArgument&&c.phase()==s::Phase::Armed);}
    {Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok);CHECK(c.disconnect({4,1},r)==s::Status::Ok);CHECK(c.stopTiming({4,1},r)==s::Status::LinkNotStopped&&d.writes.size()==5&&c.phase()==s::Phase::Faulted);}
    for(unsigned p=0;p<4;++p)for(unsigned reg:{0u,19u,20u}){Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok);d.regs[d.hubp.pipe[p][reg]]^=reg?1u:2u;
        CHECK(c.disconnect({4,1},r)==s::Status::DrainRequired&&d.writes.empty()&&c.phase()==s::Phase::Faulted);}
    for(unsigned bit:{0u,28u,31u}){Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok);d.regs[d.hubp.power[3]]^=1u<<bit;
        CHECK(c.disconnect({4,1},r)==s::Status::Changed&&d.writes.empty());}
    {Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok);d.neverMpc=true;CHECK(c.disconnect({3,1},r)==s::Status::Timeout&&r.polls==3&&d.delays==2);}
    {Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok);CHECK(c.disconnect({4,1},r)==s::Status::Ok);d.stopLink();d.neverTiming=true;
        CHECK(c.stopTiming({3,1},r)==s::Status::Timeout&&r.polls==3&&d.writes.size()==9);}
}
void finalReassertions(){
    Device baseline;s::Controller bc;s::Result r{};CHECK(baseline.arm(bc,r)==s::Status::Ok&&bc.disconnect({4,1},r)==s::Status::Ok);
    baseline.stopLink();baseline.resetCounts();CHECK(bc.stopTiming({4,1},r)==s::Status::Ok);
    for(unsigned reg:{0u,1u}){const auto a=address(b::Bank::Timing,1,reg);size_t final=0;
        for(size_t i=0;i<baseline.accessed.size();++i)if(baseline.accessed[i]==a)final=i+1;
        Device d;s::Controller c;CHECK(d.arm(c,r)==s::Status::Ok&&c.disconnect({4,1},r)==s::Status::Ok);
        d.stopLink();d.resetCounts();d.changedAt=final;d.changeMask=0x10000;
        CHECK(c.stopTiming({4,1},r)==s::Status::Changed&&c.phase()==s::Phase::Faulted&&r.retentionRequired);
    }
}
void writes(){
    for(unsigned step=1;step<=9;++step)for(bool applied:{false,true}){Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok);d.failWrite=step;d.applyFailedWrite=applied;
        s::Status result=c.disconnect({4,1},r);if(step>5){CHECK(result==s::Status::Ok);d.stopLink();result=c.stopTiming({4,1},r);}
        CHECK(result==s::Status::WriteFailed&&d.writes.size()==step&&r.attempted==((1u<<step)-1)&&r.acknowledged==((1u<<(step-1))-1));
        CHECK(c.phase()==s::Phase::Faulted&&r.retentionRequired);CHECK(c.disconnect({4,1},r)==s::Status::InvalidState&&c.stopTiming({4,1},r)==s::Status::InvalidState&&d.writes.size()==step);
    }
    for(unsigned step:{1u,2u,4u,5u,6u,8u,9u}){Device d;s::Controller c;s::Result r{};CHECK(d.arm(c,r)==s::Status::Ok);d.dropWrite=step;
        auto result=c.disconnect({4,1},r);if(step>5){CHECK(result==s::Status::Ok);d.stopLink();result=c.stopTiming({4,1},r);}
        CHECK(result==s::Status::Changed&&c.phase()==s::Phase::Faulted&&d.writes.size()==step);
    }
}
void readFaults(){
    for(bool second:{false,true}){Device baseline;s::Controller bc;s::Result r{};CHECK(baseline.arm(bc,r)==s::Status::Ok);
        if(second){CHECK(bc.disconnect({4,1},r)==s::Status::Ok);baseline.stopLink();}baseline.resetCounts();
        CHECK((second?bc.stopTiming({4,1},r):bc.disconnect({4,1},r))==s::Status::Ok);const size_t reads=baseline.reads;
        // One fault per distinct observed register plus the transitions around
        // every actual write and the final read. Avoid repeating equivalent
        // failures across every full unchanged guard sweep.
        std::set<size_t> positions{1,reads};std::set<uint32_t> addresses;
        for(size_t i=0;i<baseline.accessed.size();++i)if(addresses.insert(baseline.accessed[i]).second)positions.insert(i+1);
        for(size_t i:baseline.writeReads){if(i)positions.insert(i);if(i<reads)positions.insert(i+1);}
        for(size_t at:positions)for(bool ones:{false,true}){Device d;s::Controller c;CHECK(d.arm(c,r)==s::Status::Ok);
            if(second){CHECK(c.disconnect({4,1},r)==s::Status::Ok);d.stopLink();}d.resetCounts();if(ones)d.onesAt=at;else d.failAt=at;
            const auto result=second?c.stopTiming({4,1},r):c.disconnect({4,1},r);
            CHECK(result==(ones?s::Status::DeviceUnavailable:s::Status::ReadFailed));CHECK(d.reads==at&&c.phase()==s::Phase::Faulted&&r.retentionRequired);
            const auto nw=d.writes.size();CHECK(c.disconnect({4,1},r)==s::Status::InvalidState&&c.stopTiming({4,1},r)==s::Status::InvalidState&&d.writes.size()==nw);
        }
    }
}
}
int main(){positive();negatives();writes();readFaults();finalReassertions();std::cout<<checks<<" checks, "<<failures<<" failures (offline protocol only)\n";return failures?1:0;}

