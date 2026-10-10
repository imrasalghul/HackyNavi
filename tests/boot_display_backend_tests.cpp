// SPDX-License-Identifier: MIT
// Offline register-callback tests only. No hardware state or ownership is simulated in production.
#include "hackynavi/boot_display_backend.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
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
struct Device {
    std::vector<uint8_t> disc=discovery(),bios=rom();
    std::map<uint32_t,uint32_t> regs;
    std::vector<uint32_t> accessed;
    size_t reads=0,guards=0,permissions=0,failAt=0,onesAt=0,changedAt=0,denyGuard=0,denyPermission=0;
    size_t imageOffset=0;
    bool missing=false;
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
    }
    void set(b::Bank bank,size_t n,size_t r,uint32_t value){regs[address(bank,n,r)]=value;}
    b::Io io(){return {this,valid,canRead,read};}
    static bool valid(void* p){auto& s=*static_cast<Device*>(p);return ++s.guards!=s.denyGuard;}
    static bool canRead(void* p,b::Bank bank,uint8_t n,uint32_t a) {
        auto& s=*static_cast<Device*>(p);++s.permissions;
        CHECK(unsigned(bank)<4&&n<(unsigned(bank)<2?5:4));CHECK(s.regs.count(a)==1);
        return s.permissions!=s.denyPermission;
    }
    static bool read(void* p,uint32_t a,uint32_t& value) {
        auto& s=*static_cast<Device*>(p);++s.reads;s.accessed.push_back(a);
        const auto it=s.regs.find(a);if(it==s.regs.end()){s.missing=true;return false;}
        value=it->second;if(s.reads==s.onesAt)value=UINT32_MAX;if(s.reads==s.changedAt)value^=0x80000000u;
        return s.reads!=s.failAt;
    }
    b::Status capture(b::Capture& c,b::Result& r) {return b::capture(disc.data(),disc.size(),0x100000,bios.data(),bios.size(),imageOffset,io(),c,r);}
};
void positives() {
    for(uint8_t f=0;f<5;++f)for(uint8_t t=0;t<4;++t)for(uint8_t o=0;o<4;++o)for(uint8_t m=0;m<4;++m)for(uint8_t d=0;d<4;++d) {
        Device x(f,t,o,m,d);b::Capture c{};b::Result r{};
        CHECK(x.capture(c,r)==b::Status::Ok&&c.valid);CHECK(!x.missing&&x.reads==2*b::kSamplesPerPass);
        CHECK(c.backend==3&&c.frontendIndex==f&&c.timingIndex==t&&c.opp==o&&c.mpcc==m&&c.dpp==d);
        CHECK(c.route.phy==3&&c.route.ddc==0&&c.route.hpd==0&&c.path==3&&c.route.connectorObject==0x340c&&c.route.encoderObject==0x2220);
        CHECK(!c.scrambled&&!c.quarterRate&&c.bitsPerComponent==8);
        for(size_t n=0;n<5;++n)CHECK(c.linkReadMask[n]==7&&c.frontendReadMask[n]==63);
        for(size_t n=0;n<4;++n)CHECK(c.timingReadMask[n]==4095&&c.combinerReadMask[n]==63);
    }
    for(uint32_t depth=0;depth<4;++depth)for(uint32_t flags:{0u,2u,6u}) {
        Device x;const auto value=0x19u|flags|(depth<<28)|(depth?0x01000000u:0u);
        x.set(b::Bank::Frontend,2,3,value);b::Capture c{};b::Result r{};
        CHECK(x.capture(c,r)==b::Status::Ok&&c.scrambled==bool(flags&2)&&c.quarterRate==bool(flags&4));
        CHECK(c.bitsPerComponent==(depth==0?8:depth==1?10:depth==2?12:16));
    }
}
void failureCases() {
    Device reference;b::Capture c{};b::Result r{};CHECK(reference.capture(c,r)==b::Status::Ok);
    for(size_t at=1;at<=reference.reads;++at) {
        for(unsigned kind=0;kind<2;++kind) {
            Device x;if(kind)x.onesAt=at;else x.failAt=at;
            CHECK(x.capture(c,r)==(kind?b::Status::DeviceUnavailable:b::Status::ReadFailed));
            CHECK(!c.valid&&x.reads==at&&r.lastReadSucceeded==bool(kind));
        }
        if(at>b::kSamplesPerPass) {
            Device x;x.changedAt=at;CHECK(x.capture(c,r)==b::Status::Changed);CHECK(!c.valid&&x.reads==at);
        }
    }
    for(size_t at=1;at<=reference.guards;++at) {
        Device x;x.denyGuard=at;CHECK(x.capture(c,r)==b::Status::AccessDenied);CHECK(!c.valid&&x.guards==at);
    }
    for(size_t at=1;at<=reference.permissions;++at) {
        Device x;x.denyPermission=at;CHECK(x.capture(c,r)==b::Status::AccessDenied);
        CHECK(!c.valid&&x.permissions==at&&x.reads==(at/2));
    }
    for(unsigned bit=0;bit<32;++bit) {
        Device x;x.set(b::Bank::Link,4,0,1u<<bit);CHECK(x.capture(c,r)==b::Status::UnsupportedMux);
        CHECK(x.reads==5&&!c.valid); // no FE/window reads after unknown mux
    }
    struct Mutation {b::Bank bank;uint8_t n,r;uint32_t value;};
    const Mutation negatives[]={
        {b::Bank::Link,3,1,0x30500},{b::Bank::Link,3,1,0x31000},{b::Bank::Link,3,1,0x30401},
        {b::Bank::Link,3,1,0x20400},{b::Bank::Link,3,1,0x10030400},{b::Bank::Link,3,2,1},
        {b::Bank::Link,3,2,0},{b::Bank::Link,1,2,0x101},{b::Bank::Link,1,1,0x400},
        {b::Bank::Frontend,1,1,1},{b::Bank::Frontend,1,2,0x10000},{b::Bank::Frontend,1,0,0x1000000},
        {b::Bank::Frontend,2,0,0x11000001},{b::Bank::Frontend,2,0,0x1000101},
        {b::Bank::Frontend,2,0,0x1000007},{b::Bank::Frontend,2,1,0x1f},{b::Bank::Frontend,2,1,0x1000001d},
        {b::Bank::Frontend,2,2,1},{b::Bank::Frontend,2,3,0x1d},{b::Bank::Frontend,2,3,0x1000019},
        {b::Bank::Frontend,2,3,0x10000019},{b::Bank::Frontend,2,4,0x32},{b::Bank::Frontend,2,5,2},
        {b::Bank::Timing,1,0,0x100001},{b::Bank::Timing,1,0,1},{b::Bank::Timing,1,0,0x13201},
        {b::Bank::Timing,1,1,0x10111},{b::Bank::Timing,1,1,1},{b::Bank::Timing,1,2,0x100},
        {b::Bank::Timing,1,3,0x400},{b::Bank::Timing,1,4,1},{b::Bank::Timing,1,5,0x1000000},
        {b::Bank::Timing,1,6,1},{b::Bank::Timing,1,7,1},{b::Bank::Timing,1,8,0x100},
        {b::Bank::Timing,1,9,0xfff30001},{b::Bank::Timing,1,9,0xff230000},{b::Bank::Timing,1,10,0x10},
        {b::Bank::Timing,1,11,1},{b::Bank::Timing,0,0,0x10001},{b::Bank::Timing,0,9,0xfff30000},
        {b::Bank::Combiner,3,5,4},{b::Bank::Combiner,1,5,0},{b::Bank::Combiner,0,0,15},
        {b::Bank::Combiner,0,1,1},{b::Bank::Combiner,0,2,2},{b::Bank::Combiner,0,3,0x13},
        {b::Bank::Combiner,0,4,4},{b::Bank::Combiner,2,0,2},{b::Bank::Combiner,2,4,2}};
    for(const auto& bad:negatives) {Device x;x.set(bad.bank,bad.n,bad.r,bad.value);CHECK(x.capture(c,r)!=b::Status::Ok);CHECK(!c.valid);}
    for(size_t n=0;n<reference.disc.size();++n) {Device x;x.disc.resize(n);CHECK(x.capture(c,r)!=b::Status::Ok);CHECK(!x.reads&&!c.valid);}
    for(size_t n=0;n<reference.bios.size();n+=13) {Device x;x.bios.resize(n);CHECK(x.capture(c,r)!=b::Status::Ok);CHECK(!x.reads&&!c.valid);}
    Device x;auto io=x.io();io.canRead=nullptr;
    CHECK(b::capture(x.disc.data(),x.disc.size(),0x100000,x.bios.data(),x.bios.size(),0,io,c,r)==b::Status::InvalidArgument);CHECK(!x.reads);
    CHECK(b::capture(x.disc.data(),x.disc.size(),16,x.bios.data(),x.bios.size(),0,x.io(),c,r)==b::Status::OutsideBar);CHECK(!x.reads);
    x.bios[0x110]^=1;CHECK(x.capture(c,r)==b::Status::RomRejected&&!x.reads);
    Device dup;put16(dup.bios,0x308+4,0x2220);put16(dup.bios,0x308,0x310c);dup.bios.back()=0;dup.bios.back()=uint8_t(0-sum(dup.bios,0));
    CHECK(dup.capture(c,r)==b::Status::UnsupportedRoute&&!c.valid);
}
void supplied(const char* path) {
    std::ifstream input(path,std::ios::binary);std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    CHECK(!bytes.empty());if(bytes.empty())return;
    Device x;x.bios=bytes;hackynavi::ImageInfo image{};hackynavi::DisplayInfo display{};hackynavi::dmcub::Layout layout{};
    CHECK(hackynavi::findNextImage(bytes.data(),bytes.size(),0,image)==hackynavi::Status::Ok&&image.checksumValid);
    CHECK(hackynavi::parseDisplay(bytes.data(),bytes.size(),image,display)==hackynavi::Status::Ok);
    CHECK(hackynavi::dmcub::buildLayout(x.disc.data(),x.disc.size(),0x100000,layout)==hackynavi::dmcub::Status::Ok);
    unsigned hdmi=0;
    for(uint8_t p=0;p<display.pathCount;++p) {
        hackynavi::dmcub::Route route{};
        CHECK(hackynavi::dmcub::parseRoute(bytes.data(),bytes.size(),image,layout,p,route)==hackynavi::dmcub::Status::Ok);
        if((route.connectorObject&255)!=0x0c)continue;
        ++hdmi;Device actual;actual.bios=bytes;actual.imageOffset=image.offset;actual.set(b::Bank::Link,3,1,0);actual.set(b::Bank::Link,3,2,0);
        actual.set(b::Bank::Link,route.phy,1,(uint32_t(route.hpd)<<28)|0x30400);actual.set(b::Bank::Link,route.phy,2,0x101);
        b::Capture c{};b::Result r{};CHECK(actual.capture(c,r)==b::Status::Ok&&c.valid);
        CHECK(c.path==p&&c.backend==route.phy&&c.route.ddc==route.ddc&&c.route.hpd==route.hpd&&c.route.encoderObject==route.encoderObject);
        std::cout<<"Actual ROM HDMI path="<<unsigned(p)<<" PHY="<<unsigned(route.phy)<<" DDC="<<unsigned(route.ddc)<<" HPD="<<unsigned(route.hpd)<<" (numeric register fixture, no target capture)\n";
    }
    CHECK(hdmi>0);
}
}
int main(int argc,char** argv) {
    positives();failureCases();if(argc>1)supplied(argv[1]);
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
