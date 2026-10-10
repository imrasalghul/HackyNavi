// SPDX-License-Identifier: MIT
// Offline register-callback tests only. No hardware state or ownership is simulated in production.
#include "hackynavi/boot_hdmi_stream.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
namespace s=hackynavi::boothdmistream;
namespace b=hackynavi::bootbackend;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v) { b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8); }
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v) { put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16)); }
uint16_t sum(const std::vector<uint8_t>& b,size_t p) { uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s; }
void fix(std::vector<uint8_t>& b) {put16(b,14,sum(b,60));put16(b,8,sum(b,10));}
std::vector<uint8_t> discovery() {
    std::vector<uint8_t> b(168,0);
    put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,168);
    put16(b,12,60);put16(b,16,108);put32(b,60,0x53445049);put16(b,64,3);
    put16(b,66,108);put16(b,72,1);put16(b,76,140);put16(b,142,1);
    put16(b,144,271);b[147]=4;b[148]=3;b[149]=2;put32(b,160,0x34c0);put32(b,164,0x8000);fix(b);return b;
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
const uint16_t extra[]={0x20b0,0x2093,0x2096,0x2099,0x209a,0x2097,0x209b,0x209c,0x209d,
    0x209f,0x20a0,0x20a1,0x20a2,0x20a3,0x20a4,0x20a5,0x20a6,0x20a7,
    0x2082,0x2075,0x2083,0x2085,0x2074,0x2087,0x206a,0x206b,0x206c,0x206d,0x2081,0x2095};
struct Device {
    std::vector<uint8_t> disc=discovery(),bios=rom();
    std::map<uint32_t,uint32_t> regs;
    std::vector<uint32_t> accessed,writes,statuses{2,1,2,1,2,1,2};
    std::vector<size_t> writeReads;
    size_t reads=0,guards=0,permissions=0,failAt=0,onesAt=0,denyGuard=0,denyPermission=0,statusReads=0;
    size_t failWrite=0,dropWrite=0,denyWrite=0,afterWriteDeny=0;
    uint32_t mutateAddress=0,mutateMask=1;size_t mutateAt=0;
    uint8_t selectedFront=2,selectedTiming=1;
    bool missing=false,applyFailed=false,activeMuteStuck=false,denyDelay=false;
    unsigned delays=0;
    Device(uint8_t front=2,uint8_t timing=1,uint8_t output=3,uint8_t combiner=0,uint8_t dpp=2) {
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
        selectedFront=front;selectedTiming=timing;initializeStream();
    }
    uint32_t reg(size_t q)const{return uint32_t((0x34c0+extra[q]+selectedFront*0x100)*4);}
    uint32_t status()const{return uint32_t((0x34c0+0x1b49+selectedTiming*0x80)*4);}
    uint32_t general()const{return address(b::Bank::Frontend,selectedFront,5);}
    void initializeStream(){
        for(size_t q=0;q<s::kRegisters;++q)regs[reg(q)]=0;
        regs[reg(0)]=0x101;regs[reg(2)]=0x10;regs[reg(3)]=0x10;regs[reg(4)]=0x200;
        regs[reg(5)]=0x1000;regs[reg(6)]=0x33333333;regs[reg(7)]=0x3333333;
        for(unsigned q=9;q<16;++q)regs[reg(q)]=0x20002;
        regs[reg(16)]=2;regs[reg(17)]=0x1000;regs[reg(18)]=0x801;
        regs[reg(19)]=0x300;regs[reg(20)]=0x40;regs[reg(21)]=4;
        regs[reg(22)]=0x10000;regs[reg(28)]=0x10;regs[status()]=2;
    }
    void set(b::Bank bank,size_t n,size_t q,uint32_t v){regs[address(bank,n,q)]=v;}
    static bool valid(void* p){auto& d=*static_cast<Device*>(p);return ++d.guards!=d.denyGuard;}
    static bool canRead(void* p,b::Bank bank,uint8_t n,uint32_t a){auto& d=*static_cast<Device*>(p);
        if(unsigned(bank)>3||n>=(unsigned(bank)<2?5:4)||!d.regs.count(a)){d.missing=true;return false;}
        return ++d.permissions!=d.denyPermission;
    }
    static bool permit(void* p,s::Domain domain,uint8_t n,uint32_t a,bool write){auto& d=*static_cast<Device*>(p);
        if(n>=5||!d.regs.count(a)){d.missing=true;return false;}
        if(domain==s::Domain::Afmt||domain==s::Domain::Vpg){if(d.regs[d.reg(0)]!=0x101)return false;}
        if(write&&d.writes.size()+1==d.denyWrite)return false;
        if(d.afterWriteDeny&&d.writes.size()>=d.afterWriteDeny)return false;
        return ++d.permissions!=d.denyPermission;
    }
    static bool read(void* p,uint32_t a,uint32_t& v){auto& d=*static_cast<Device*>(p);
        ++d.reads;d.accessed.push_back(a);const auto it=d.regs.find(a);if(it==d.regs.end()){d.missing=true;return false;}
        if(d.mutateAt==d.reads)d.regs[d.mutateAddress]^=d.mutateMask;
        v=d.regs[a];
        if(a==d.status()){
            v=d.statuses[d.statusReads<d.statuses.size()?d.statusReads:d.statuses.size()-1];++d.statusReads;
            if(d.statusReads>=7&&!d.activeMuteStuck)d.regs[d.reg(29)]=1;
        }
        if(d.reads==d.onesAt)v=UINT32_MAX;
        return d.reads!=d.failAt;
    }
    static bool write(void* p,uint32_t a,uint32_t v){auto& d=*static_cast<Device*>(p);d.writes.push_back(a);d.writeReads.push_back(d.reads);
        const bool failed=d.writes.size()==d.failWrite;
        if((!failed||d.applyFailed)&&d.writes.size()!=d.dropWrite)d.regs[a]=v;
        return !failed;
    }
    static bool delay(void* p,uint32_t){auto& d=*static_cast<Device*>(p);++d.delays;return !d.denyDelay;}
    s::Io io(){return {{this,valid,canRead,read},permit,write,delay};}
    s::Status capture(s::Controller& c,s::Result& r){return c.capture(disc.data(),disc.size(),0x100000,bios.data(),bios.size(),0,io(),r);}
    void reset(){reads=guards=permissions=statusReads=delays=0;accessed.clear();writes.clear();writeReads.clear();}
};
void terminal(Device& d,s::Controller& c,s::Result& r){
    CHECK(c.phase()==s::Phase::Faulted&&r.retentionRequired);
    const auto reads=d.reads,writes=d.writes.size();
    CHECK(c.mute({12,1},r)==s::Status::InvalidState);
    CHECK(d.capture(c,r)==s::Status::InvalidState&&d.reads==reads&&d.writes.size()==writes);
}
void positive(){
    // Independent selected FE, TG, OPP, MPCC and DPP; no identical-index assumption.
    for(uint8_t f=0;f<5;++f)for(uint8_t t=0;t<4;++t){Device d(f,t,3,0,1);s::Controller c;s::Result r{};
        const auto original=d.regs;
        CHECK(d.capture(c,r)==s::Status::Ok&&c.phase()==s::Phase::Armed&&d.writes.empty());
        CHECK(c.observation().frontendIndex==f&&c.observation().timingIndex==t&&c.observation().backend==3);
        CHECK(c.observed(s::Register::AudioSource)==4);
        d.reset();CHECK(c.mute({12,1},r)==s::Status::Ok);
        CHECK(c.phase()==s::Phase::MutedRetained&&r.attempted==3&&r.acknowledged==3&&r.cycles==3&&r.retentionRequired);
        CHECK(d.writes.size()==2&&d.writes[0]==d.general()&&d.writes[1]==d.reg(18)&&d.statusReads==7&&!d.missing);
        for(const auto& p:original){const auto actual=d.regs[p.first];
            CHECK(actual==(p.first==d.general()||p.first==d.reg(29)?1u:p.first==d.reg(18)?p.second&~1u:p.second));}
        const auto reads=d.reads;CHECK(c.mute({12,1},r)==s::Status::InvalidState&&d.reads==reads);
    }
    // Already muted is not a frame-progress certificate; all seven observations remain required.
    Device d;d.regs[d.general()]=d.regs[d.reg(29)]=1;d.regs[d.reg(18)]&=~1u;
    s::Controller c;s::Result r{};CHECK(d.capture(c,r)==s::Status::Ok);d.reset();
    CHECK(c.mute({12,1},r)==s::Status::Ok&&d.statusReads==7&&d.writes.size()==2);
}
void rejectedProfiles(){
    struct Bad {size_t q;uint32_t value;};
    const Bad bad[]={{0,1},{0,0},{1,1},{2,0x40},{3,0x100},{4,0x10000},{5,0x4000},
        {6,4},{7,0x10000000},{8,1},{16,0x10002},{17,0},{17,0x1001},{17,0x1100},
        {18,0x811},{18,0x1001},{18,0x4000001},{18,0x800001},{18,0x40000001},
        {19,1},{19,0x10000},{20,0x80},{21,5},{22,1},{23,0x100},{23,0x10},
        {24,1},{24,0x10000},{25,1},{26,1},{26,2},{27,0x100},{27,0x10},
        {28,0x100},{28,0x1000000},{29,0x10000},{29,1}};
    for(const auto& x:bad){Device d;d.regs[d.reg(x.q)]=x.value;s::Controller c;s::Result r{};
        CHECK(d.capture(c,r)!=s::Status::Ok&&d.writes.empty());terminal(d,c,r);}
    Device d;d.set(b::Bank::Frontend,2,4,0x1031);s::Controller c;s::Result r{};
    CHECK(d.capture(c,r)==s::Status::UnsupportedState&&d.writes.empty());terminal(d,c,r);
}
void captureFaults(){
    Device reference;s::Controller good;s::Result r{};CHECK(reference.capture(good,r)==s::Status::Ok);
    // One read fault per distinct address, plus complete second-pass raw changes.
    std::map<uint32_t,size_t> first;
    for(size_t n=0;n<reference.accessed.size();++n)first.emplace(reference.accessed[n],n+1);
    for(const auto& p:first)for(unsigned kind=0;kind<2;++kind){Device d;s::Controller c;
        if(kind)d.onesAt=p.second;else d.failAt=p.second;
        CHECK(d.capture(c,r)!=s::Status::Ok&&d.writes.empty());terminal(d,c,r);}
    for(size_t q=0;q<s::kRegisters;++q){Device d;s::Controller c;
        size_t occurrences=0;
        for(size_t n=0;n<reference.accessed.size();++n)if(reference.accessed[n]==d.reg(q)&&++occurrences==(q?2u:3u)){d.mutateAt=n+1;break;}
        CHECK(d.mutateAt!=0);d.mutateAddress=d.reg(q);d.mutateMask=1;
        CHECK(d.capture(c,r)==(q?s::Status::Changed:s::Status::ClockUnavailable)&&d.writes.empty());terminal(d,c,r);
    }
}
void frameCases(){
    const std::vector<uint32_t> paths[]={ {2},{1},{0},{2,1,2,1,2}, {3}, {2,1,3}, {0x20}, {0x80000002} };
    for(const auto& path:paths){Device d;d.statuses=path;s::Controller c;s::Result r{};
        CHECK(d.capture(c,r)==s::Status::Ok);d.reset();const auto result=c.mute({12,1},r);
        CHECK(result==s::Status::Timeout||result==s::Status::ContradictoryStatus);
        CHECK(d.writes.size()==1&&r.attempted==1&&r.acknowledged==1&&r.cycles<3);terminal(d,c,r);
    }
    Device d;d.statuses={0,2,0,1,0,2,1,2,1,2};s::Controller c;s::Result r{};
    CHECK(d.capture(c,r)==s::Status::Ok);d.reset();CHECK(c.mute({12,1},r)==s::Status::Ok&&d.delays==3);
    Device stuck;stuck.activeMuteStuck=true;s::Controller cs;CHECK(stuck.capture(cs,r)==s::Status::Ok);stuck.reset();
    CHECK(cs.mute({12,1},r)==s::Status::Changed&&stuck.writes.size()==1&&r.cycles==3);terminal(stuck,cs,r);
    Device delay;delay.statuses={0,2};delay.denyDelay=true;s::Controller cd;CHECK(delay.capture(cd,r)==s::Status::Ok);delay.reset();
    CHECK(cd.mute({12,1},r)==s::Status::AccessDenied&&delay.delays==1);terminal(delay,cd,r);
}
void mutationFaults(){
    Device reference;s::Controller good;s::Result r{};CHECK(reference.capture(good,r)==s::Status::Ok);reference.reset();CHECK(good.mute({12,1},r)==s::Status::Ok);
    for(size_t step=1;step<=2;++step)for(unsigned kind=0;kind<5;++kind){Device d;s::Controller c;
        CHECK(d.capture(c,r)==s::Status::Ok);d.reset();
        if(kind<2){d.failWrite=step;d.applyFailed=kind==1;}else if(kind==2)d.dropWrite=step;else if(kind==3)d.denyWrite=step;else d.afterWriteDeny=step;
        CHECK(c.mute({12,1},r)!=s::Status::Ok);terminal(d,c,r);
    }
    // Faults at the meaningful write/readback and final completion boundaries.
    for(size_t at:{reference.writeReads[0]+1,reference.writeReads[1]+1,reference.reads})for(unsigned kind=0;kind<2;++kind){
        Device d;s::Controller c;CHECK(d.capture(c,r)==s::Status::Ok);d.reset();if(kind)d.onesAt=at;else d.failAt=at;
        CHECK(c.mute({12,1},r)!=s::Status::Ok);terminal(d,c,r);
    }
    // Every retained extra control plus route, enabled TG, current clock and packet7/14 state.
    std::vector<uint32_t> change;
    for(size_t q=0;q<s::kRegisters;++q)change.push_back(reference.reg(q));
    change.push_back(address(b::Bank::Frontend,2,1));change.push_back(address(b::Bank::Link,3,1));
    change.push_back(address(b::Bank::Timing,1,0));change.push_back(address(b::Bank::Timing,1,1));
    for(auto a:change){Device d;s::Controller c;CHECK(d.capture(c,r)==s::Status::Ok);d.reset();
        d.regs[a]^=(a==d.reg(29)?2u:1u);CHECK(c.mute({12,1},r)!=s::Status::Ok&&d.writes.empty());terminal(d,c,r);
    }
    // Reassert audio send or revoke the clock immediately after the last write's readback.
    for(auto a:{reference.reg(18),reference.reg(0)}){Device d;s::Controller c;CHECK(d.capture(c,r)==s::Status::Ok);d.reset();
        d.mutateAt=reference.writeReads[1]+2;d.mutateAddress=a;
        CHECK(c.mute({12,1},r)!=s::Status::Ok&&d.writes.size()==2);terminal(d,c,r);
    }
}
void arguments(){Device d;s::Controller c;s::Result r{};
    CHECK(c.mute({10,1},r)==s::Status::InvalidState&&!r.retentionRequired&&d.reads==0);
    CHECK(d.capture(c,r)==s::Status::Ok);d.reset();
    for(const auto p:{s::Poll{0,1},s::Poll{1,0},s::Poll{1,101},s::Poll{700001,1}})
        CHECK(c.mute(p,r)==s::Status::InvalidArgument&&c.phase()==s::Phase::Armed&&d.reads==0);
    CHECK(c.mute({6,1},r)==s::Status::Timeout&&r.cycles==2);terminal(d,c,r);
}
}
int main(){positive();rejectedProfiles();captureFaults();frameCases();mutationFaults();arguments();
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
