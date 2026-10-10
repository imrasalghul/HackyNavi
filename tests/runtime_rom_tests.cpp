// SPDX-License-Identifier: MIT
// Offline SMUIO ROM and NBIO RSMU peripheral model; never touches a GPU.
#include "hackynavi/runtime_rom.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
namespace r=hackynavi::runtimerom;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; std::cerr<<__LINE__<<": " #x "\n"; } } while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v) { b[p]=uint8_t(v); b[p+1]=uint8_t(v>>8); }
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v) { put16(b,p,uint16_t(v)); put16(b,p+2,uint16_t(v>>16)); }
uint32_t u32(const std::vector<uint8_t>& b,size_t p) { return uint32_t(b[p])|uint32_t(b[p+1])<<8|uint32_t(b[p+2])<<16|uint32_t(b[p+3])<<24; }
uint16_t sum16(const std::vector<uint8_t>& b,size_t p) { uint16_t n=0; for(;p<b.size();++p)n=uint16_t(n+b[p]); return n; }
void fix(std::vector<uint8_t>& b) { put16(b,14,sum16(b,60)); put16(b,8,sum16(b,10)); }
std::vector<uint8_t> discovery(uint32_t smu=0x16800,uint32_t nbif=0x4040000) {
    std::vector<uint8_t> b(188,0);
    put32(b,0,0x28211407); put16(b,4,1); put16(b,6,4); put16(b,10,uint16_t(b.size()));
    put16(b,12,60); put16(b,16,uint16_t(b.size()-60)); put32(b,60,0x53445049); put16(b,64,3);
    put16(b,66,uint16_t(b.size()-60)); put16(b,72,1); put16(b,76,140); put16(b,142,2);
    put16(b,144,108); b[147]=6; b[148]=4; b[149]=3;
    const uint32_t bases[]={0,0x14,0xd20,0x10400,0x241b000,nbif};
    for(unsigned i=0;i<6;++i)put32(b,152+i*4,bases[i]);
    put16(b,176,4); b[179]=1; b[180]=13; b[182]=6; put32(b,184,smu); fix(b); return b;
}
r::Layout layout(bool direct=false) {
    const auto b=discovery(0x16800,direct?0:0x4040000); r::Layout l={};
    CHECK(r::buildLayout(b.data(),b.size(),0x80000,l)==r::Status::Ok); return l;
}
void checksum(std::vector<uint8_t>& b) {
    b[0x60]=0; uint8_t n=0; for(auto v:b)n=uint8_t(n+v); b[0x60]=uint8_t(0-n);
}
void header(std::vector<uint8_t>& b,size_t p,uint16_t size,uint8_t major,uint8_t minor) { put16(b,p,size); b[p+2]=major; b[p+3]=minor; }
std::vector<uint8_t> bios() {
    std::vector<uint8_t> b(512,0); b[0]=0x55; b[1]=0xaa; b[2]=1;
    std::memcpy(b.data()+0x30," 761295520",10);
    put16(b,0x18,0x80); std::memcpy(b.data()+0x80,"PCIR",4);
    put16(b,0x84,0x1002); put16(b,0x86,0x744c); put16(b,0x8a,24); put16(b,0x90,1); b[0x95]=0x80;
    put16(b,0x48,0x100); header(b,0x100,44,2,3); std::memcpy(b.data()+0x104,"ATOM",4);
    put16(b,0x11e,0x190); put16(b,0x120,0x140); header(b,0x190,8,2,1); header(b,0x140,74,2,1);
    // Erased flash/padding is legitimate; don't reject all-ones data DWORDs.
    put32(b,0x1fc,UINT32_MAX); checksum(b); return b;
}
struct Device {
    r::Layout l=layout(); uint32_t bank=2,high=0,selected=0,index=0;
    std::vector<uint8_t> image=bios(),flash,output=std::vector<uint8_t>(r::kRecommendedCapacity+8,0xcc);
    struct Op { bool write; uint32_t reg,value; }; std::vector<Op> ops;
    size_t failAt=0,onesAt=0,bankReads=0,dataReads=0; bool changeBank=false,ignoreIndex=false;
    r::Result result={};
    Device() { rebuild(); }
    void rebuild() { flash.assign(size_t(bank<<17)+image.size(),0xff); std::copy(image.begin(),image.end(),flash.begin()+(bank<<17)); }
    r::Io io() {return {this,read,write};}
    static bool read(void* c,uint32_t address,uint32_t& value) {
        auto& s=*static_cast<Device*>(c); value=0;
        if(address==s.l.rsmuIndexHigh)value=s.high;
        else if(address==s.l.rsmuIndex)value=s.selected;
        else if(address==s.l.romOffsetControl || address==s.l.rsmuData) {
            if(address==s.l.rsmuData)CHECK(s.selected==s.l.romOffsetControl);
            ++s.bankReads; value=0xa5a50000|uint32_t(s.bank+(s.changeBank && s.bankReads>1?1:0));
        } else if(address==s.l.romData) {
            ++s.dataReads; CHECK(s.index<=s.flash.size()-4); value=u32(s.flash,s.index); s.index+=4;
        } else CHECK(false);
        s.ops.push_back({false,address,value}); if(s.ops.size()==s.onesAt)value=UINT32_MAX;
        return s.ops.size()!=s.failAt;
    }
    static bool write(void* c,uint32_t address,uint32_t value) {
        auto& s=*static_cast<Device*>(c); s.ops.push_back({true,address,value});
        if(address==s.l.rsmuIndex) { CHECK(value==s.l.romOffsetControl); s.selected=value; }
        else if(address==s.l.romIndex) { CHECK(value==s.bank<<17); if(!s.ignoreIndex)s.index=value; }
        else CHECK(false); // no flash data, clock, HI or enable register writes
        return s.ops.size()!=s.failAt; // mutation deliberately lands first
    }
    r::Status run(size_t cap=r::kRecommendedCapacity) { return r::acquire(l,io(),output.data()+4,cap,result); }
    void canaries(size_t written) {
        CHECK(std::all_of(output.begin(),output.begin()+4,[](uint8_t b){return b==0xcc;}));
        CHECK(std::all_of(output.begin()+4+written,output.end(),[](uint8_t b){return b==0xcc;}));
    }
};
void layouts() {
    r::Layout l=layout(); CHECK(l.romIndex==0x5a390 && l.romData==0x5a394 && l.romOffsetControl==0x1013308c);
    CHECK(l.rsmuIndex==0x50 && l.rsmuData==0x54 && l.rsmuIndexHigh==0x58);
    const auto b=discovery();
    for(size_t n=0;n<b.size();++n) { CHECK(r::buildLayout(b.data(),n,0x80000,l)==r::Status::DiscoveryRejected); CHECK(!l.valid); }
    for(auto at:{176,178,179,180,181,182,183}) {
        auto bad=b; ++bad[size_t(at)]; fix(bad);
        CHECK(r::buildLayout(bad.data(),bad.size(),0x80000,l)!=r::Status::Ok && !l.valid);
    }
    auto huge=discovery(UINT32_MAX); CHECK(r::buildLayout(huge.data(),huge.size(),UINT64_MAX,l)==r::Status::AddressOverflow);
    CHECK(r::buildLayout(b.data(),b.size(),0x5a397,l)==r::Status::OutsideBar);
    CHECK(r::buildLayout(b.data(),b.size(),0x5a398,l)==r::Status::Ok);
    Device s; auto io=s.io();
    for(unsigned n=0;n<9;++n) {
        auto bad=s.l; auto callbacks=io; size_t capacity=s.output.size()-4; uint8_t* out=s.output.data()+4;
        if(n==0)bad.valid=false; if(n==1)bad.romData+=4; if(n==2)bad.rsmuIndexHigh+=4;
        if(n==3)bad.romOffsetControl=bad.romIndex; if(n==4)bad.barBytes=4;
        if(n==5)callbacks.read32=nullptr; if(n==6)callbacks.write32=nullptr; if(n==7)capacity=63; if(n==8)out=nullptr;
        CHECK(r::acquire(bad,callbacks,out,capacity,s.result)!=r::Status::Ok && s.ops.empty());
        CHECK(!s.result.image.size && !s.result.atom.header.size);
    }
}
void normal() {
    for(unsigned n=0;n<4;++n) {
        Device s; if(n==0)s.bank=0; if(n==1)s.bank=127; if(n==2)s.l=layout(true); s.rebuild();
        CHECK(s.run()==r::Status::Ok); CHECK(s.result.image.size==512 && s.result.image.checksumValid && s.result.atom.header.size==44);
        CHECK(s.result.bytesRead==512 && s.result.bytesRequired==512 && s.result.bankByteOffset==s.bank<<17);
        CHECK(s.result.selectorWritesMayHaveOccurred && s.bankReads==2 && s.dataReads==128);
        CHECK(s.result.writes==(n==2?1u:3u)); CHECK(std::equal(s.image.begin(),s.image.end(),s.output.begin()+4)); s.canaries(512);
        size_t romIndexWrites=0;
        for(const auto& op:s.ops)if(op.write && op.reg==s.l.romIndex)++romIndexWrites;
        CHECK(romIndexWrites==1);
    }
    Device small; CHECK(small.run(64)==r::Status::BufferTooSmall && small.result.bytesRequired==512);
    CHECK(small.dataReads==16 && small.result.bytesRead==64 && !small.result.image.size); small.canaries(64);
    // The same caller can retry a selector-only acquisition with a larger buffer.
    CHECK(small.run()==r::Status::Ok);
}
void failuresTest() {
    Device base; CHECK(base.run()==r::Status::Ok); const auto ops=base.ops;
    for(size_t n=1;n<=ops.size();++n) {
        Device s; s.failAt=n;
        CHECK(s.run()==(ops[n-1].write?r::Status::WriteFailed:r::Status::ReadFailed));
        CHECK(s.ops.size()==n && !s.result.image.size && !s.result.atom.header.size);
        CHECK(s.result.selectorWritesMayHaveOccurred==(n>=2)); s.canaries(s.result.bytesRead);
        // A landed selector write has no firmware or DMA ownership to retire.
        s.failAt=0; CHECK(s.run()==r::Status::Ok);
    }
    for(unsigned n=0;n<6;++n) {
        Device s;
        if(n==0)s.high=1; if(n==1)s.high=UINT32_MAX; if(n==2)s.onesAt=4;
        if(n==3)s.changeBank=true; if(n==4)s.ignoreIndex=true;
        if(n==5) { s.l=layout(true); s.onesAt=1; }
        const auto status=s.run(); CHECK(status!=r::Status::Ok && !s.result.image.size && !s.result.atom.header.size);
        if(n<2)CHECK(s.ops.size()==1 && !s.result.selectorWritesMayHaveOccurred);
        if(n==3)CHECK(status==r::Status::BankChanged);
        s.canaries(s.result.bytesRead);
    }
    for(unsigned n=0;n<11;++n) {
        Device s;
        if(n==0)s.image[0]=0; if(n==1)s.image[2]=0; if(n==2)s.image[0x30]^=1;
        if(n==3)s.image[0x80]^=1; if(n==4)s.image[0x84]^=1; if(n==5)s.image[0x86]^=1;
        if(n==6)s.image[0x94]=3; if(n==7)s.image[0x91]=1; if(n==8)s.image[0x104]^=1;
        if(n==9)put16(s.image,0x120,0x1ff); if(n==10)s.image[0x1e0]^=1;
        if(n!=10)checksum(s.image); s.rebuild();
        CHECK(s.run()==r::Status::RomRejected && !s.result.image.size && !s.result.atom.header.size);
        s.canaries(s.result.bytesRead);
    }
}
void actual(const char* rom,const char* discoveryPath) {
    std::ifstream input(rom,std::ios::binary); Device s;
    s.flash.assign(std::istreambuf_iterator<char>(input),{});
    CHECK(s.flash.size()==2097152);
    if(discoveryPath) {
        std::ifstream disc(discoveryPath,std::ios::binary); const std::vector<uint8_t> b((std::istreambuf_iterator<char>(disc)),{});
        CHECK(r::buildLayout(b.data(),b.size(),0x100000,s.l)==r::Status::Ok);
    }
    CHECK(s.run()==r::Status::Ok && s.result.bytesRead==57856 && s.result.image.checksumValid);
    CHECK(std::equal(s.flash.begin()+0x40000,s.flash.begin()+0x40000+s.result.bytesRead,s.output.begin()+4));
    CHECK(s.result.atom.subsystemVendorId==0x148c && s.result.atom.subsystemDeviceId==0x2422); s.canaries(57856);
}
}
int main(int argc,char** argv) {
    layouts(); normal(); failuresTest(); if(argc>1)actual(argv[1],argc>2?argv[2]:nullptr);
    std::cout<<checks<<" checks, "<<failures<<" failures\n"; return failures?1:0;
}
