// SPDX-License-Identifier: MIT
// Numeric fixture derived from boot_display_backend_tests.cpp; no hardware observations.
#ifndef HACKYNAVI_TEST_BOOT_BACKEND_FIXTURE_HPP
#define HACKYNAVI_TEST_BOOT_BACKEND_FIXTURE_HPP
#include "hackynavi/boot_display_backend.hpp"
#include <cstring>
#include <map>
#include <vector>
namespace backend_fixture {
namespace b=hackynavi::bootbackend;
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
        if(unsigned(bank)>=4||n>=(unsigned(bank)<2?5:4)||s.regs.count(a)!=1)s.missing=true;
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
}
#endif
