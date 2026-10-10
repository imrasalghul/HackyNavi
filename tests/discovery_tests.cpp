// SPDX-License-Identifier: MIT
// Synthetic fixtures only; these tests do not establish live GPU compatibility.
#include "hackynavi/discovery.hpp"
#include <cstring>
#include <iostream>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
using namespace hackynavi;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(e) do { ++checks; if (!(e)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " #e "\n"; } } while (0)
void put16(std::vector<uint8_t>& b, size_t p, uint16_t v) { b[p] = uint8_t(v); b[p+1] = uint8_t(v >> 8); }
void put32(std::vector<uint8_t>& b, size_t p, uint32_t v) { put16(b,p,uint16_t(v)); put16(b,p+2,uint16_t(v >> 16)); }
uint16_t get16(const std::vector<uint8_t>& b, size_t p) { return uint16_t(b[p] | (uint16_t(b[p+1]) << 8)); }
uint16_t sum(const std::vector<uint8_t>& b, size_t p, size_t n) {
    uint16_t s = 0; for (size_t i=0;i<n;++i) s = uint16_t(s+b[p+i]); return s;
}
void fix(std::vector<uint8_t>& b) {
    const size_t directory = get16(b,4)==2 ? 16 : 12;
    const size_t count = directory==16 ? get16(b,12) : 6;
    for (size_t i=0;i<count && directory+i*8+8<=b.size();++i) {
        const size_t entry=directory+i*8, off=get16(b,entry), size=get16(b,entry+4);
        if (off && off<=b.size() && size<=b.size()-off) put16(b,entry+2,sum(b,off,size));
    }
    const size_t size=get16(b,10);
    if (size>=12 && size<=b.size()) put16(b,8,sum(b,10,size-10));
}
void ip(std::vector<uint8_t>& b,size_t p,bool wide,uint8_t bases,uint16_t hw) {
    put16(b,p,hw); b[p+2]=2; b[p+3]=bases;
    b[p+4]=11; b[p+5]=0; b[p+6]=1; b[p+7]=0xa3;
    for(size_t i=0;i<bases;++i) {
        const size_t at=p+8+i*(wide?8:4);
        put32(b,at,0xf2345678U+uint32_t(i));
        if(wide) put32(b,at+4,0xabcdef01U);
    }
}
std::vector<uint8_t> fixture(uint16_t version=4,bool wide=true,uint16_t binaryVersion=1) {
    std::vector<uint8_t> b(512,0);
    put32(b,0,0x28211407); put16(b,4,binaryVersion); put16(b,10,512);
    const size_t dir=binaryVersion==2?16:12;
    if(binaryVersion==2) put16(b,12,8);
    put16(b,dir,80); put16(b,dir+4,256);
    put16(b,dir+8,400); put16(b,dir+12,16); b[400]=0xa5;
    put32(b,80,0x53445049); put16(b,84,version); put16(b,86,256);
    put32(b,88,0x12345678); put16(b,92,2);
    put16(b,94,0); put16(b,96,160); put16(b,98,3); put16(b,100,224);
    b[158]=wide?1:0;
    put16(b,160,0); put16(b,162,2); put16(b,224,3); put16(b,226,1);
    const bool encodedWide=version==4&&wide;
    ip(b,164,encodedWide,2,11); ip(b,164+8+2*(encodedWide?8:4),encodedWide,1,12);
    ip(b,228,encodedWide,1,13);
    fix(b); return b;
}
struct Capture { unsigned calls=0; uint64_t first=0; uint8_t harvest=0,sub=0,variant=0; };
void visit(const DiscoveryIp& p,void* ptr) {
    Capture& c=*static_cast<Capture*>(ptr);
    if(!c.calls) { c.first=p.baseAddresses[0]; c.harvest=p.harvest;c.sub=p.subRevision;c.variant=p.variant; }
    ++c.calls;
}
void versions() {
    for(uint16_t binary=0;binary<=2;++binary) for(uint16_t v=1;v<=4;++v) for(unsigned width=0;width<2;++width) {
        auto b=fixture(v,width!=0,binary); DiscoveryInfo out={}; Capture c;
        CHECK(parseDiscovery(b.data(),b.size(),out,visit,&c)==DiscoveryStatus::Ok);
        CHECK(c.calls==3 && out.ipCount==3 && out.dieCount==2 && out.dies[1].id==3);
        CHECK(out.tableCount==(binary==2?8:6));
        CHECK(out.baseAddresses64Bit==(v==4&&width!=0));
        CHECK(c.first==(v==4&&width?0xabcdef01f2345678ULL:0xf2345678ULL));
        CHECK(v<=2 ? c.harvest==3&&c.sub==0&&c.variant==0 : c.harvest==0&&c.sub==3&&c.variant==10);
    }
}
void rejected(std::vector<uint8_t> b,DiscoveryStatus expected,bool repair=true) {
    if(repair) fix(b);
    DiscoveryInfo out={}; Capture c;
    CHECK(parseDiscovery(b.data(),b.size(),out,visit,&c)==expected);
    CHECK(c.calls==0);
    if(expected!=DiscoveryStatus::UnsupportedVersion) CHECK(out.binarySize==0 && out.ipCount==0);
}
void adversarial() {
    auto b=fixture(); b[200]^=1; rejected(b,DiscoveryStatus::ChecksumMismatch,false);
    b=fixture(); b[14]^=1; put16(b,8,sum(b,10,502)); rejected(b,DiscoveryStatus::ChecksumMismatch,false);
    b=fixture(); b[0]=0; rejected(b,DiscoveryStatus::BadSignature);
    b=fixture(); b[80]=0; rejected(b,DiscoveryStatus::BadSignature);
    b=fixture(); put16(b,4,9); rejected(b,DiscoveryStatus::UnsupportedVersion);
    b=fixture(); put16(b,84,5); rejected(b,DiscoveryStatus::UnsupportedVersion);
    b=fixture(); put16(b,10,9); rejected(b,DiscoveryStatus::Malformed);
    b=fixture(); put16(b,10,513); rejected(b,DiscoveryStatus::Truncated);
    b=fixture(4,true,2); put16(b,12,33);
    // Refresh binary checksum only: the oversized directory must never be read.
    put16(b,8,sum(b,10,502)); rejected(b,DiscoveryStatus::CapacityExceeded,false);
    b=fixture(); put16(b,12,40); rejected(b,DiscoveryStatus::Malformed);
    b=fixture(); put16(b,20,80); rejected(b,DiscoveryStatus::Malformed);
    b=fixture(); put16(b,16,255); rejected(b,DiscoveryStatus::Malformed);
    b=fixture(); put16(b,92,17); rejected(b,DiscoveryStatus::CapacityExceeded);
    b=fixture(); put16(b,96,159); rejected(b,DiscoveryStatus::Malformed);
    b=fixture(); put16(b,96,335); rejected(b,DiscoveryStatus::Truncated);
    b=fixture(); put16(b,100,160); rejected(b,DiscoveryStatus::Malformed);
    b=fixture(); put16(b,98,0); rejected(b,DiscoveryStatus::Malformed);
    b=fixture(); put16(b,224,4); rejected(b,DiscoveryStatus::Malformed);
    b=fixture(); put16(b,162,100); rejected(b,DiscoveryStatus::Truncated);
    b=fixture(); b[231]=255; rejected(b,DiscoveryStatus::Truncated); // late die failure: no partial visitor output
    b=fixture(); b[167]=255; rejected(b,DiscoveryStatus::Truncated); // cannot consume next die
    b=fixture(); put16(b,28,500); put16(b,32,100); rejected(b,DiscoveryStatus::Truncated);
    b=fixture(); put16(b,32,1); rejected(b,DiscoveryStatus::Malformed); // absent offset with size
    b=fixture(); put16(b,94,3);put16(b,96,224);put16(b,98,0);put16(b,100,160);fix(b);
    DiscoveryInfo out={}; CHECK(parseDiscovery(b.data(),b.size(),out)==DiscoveryStatus::Ok); // unsorted directory
    CHECK(parseDiscovery(nullptr,512,out)==DiscoveryStatus::InvalidArgument);
    // Full uint8 address count fits the API without truncating raw high bits.
    b=fixture(); b.resize(4096);put16(b,10,4096);put16(b,16,4016);put16(b,86,4016);
    put16(b,20,0);put16(b,22,0);put16(b,24,0);put16(b,92,1);put16(b,162,1);
    ip(b,164,true,255,11);fix(b); Capture c;
    CHECK(parseDiscovery(b.data(),b.size(),out,visit,&c)==DiscoveryStatus::Ok && c.calls==1);
}
class Guard {
public:
    uint8_t* memory=nullptr;size_t page=0;
    Guard() {
#ifdef _WIN32
        SYSTEM_INFO s={};GetSystemInfo(&s);page=s.dwPageSize;
        memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,page*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        DWORD old=0;if(memory&&!VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);memory=nullptr;}
#else
        const long p=sysconf(_SC_PAGESIZE);if(p<=0)return;page=size_t(p);
        void* ptr=mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
        if(ptr==MAP_FAILED)return;memory=static_cast<uint8_t*>(ptr);
        if(mprotect(memory+page,page,PROT_NONE)){munmap(memory,page*2);memory=nullptr;}
#endif
    }
    ~Guard(){if(!memory)return;
#ifdef _WIN32
        VirtualFree(memory,0,MEM_RELEASE);
#else
        munmap(memory,page*2);
#endif
    }
};
void guarded() {
    Guard g;CHECK(g.memory&&g.page>=512);if(!g.memory||g.page<512)return;
    auto source=fixture();DiscoveryInfo info={};
    for(size_t n=0;n<=source.size();++n) {
        uint8_t* ptr=g.memory+g.page-n;
        if(n)std::memcpy(ptr,source.data(),n);
        Capture c;const auto s=parseDiscovery(ptr,n,info,visit,&c);
        CHECK(n==512?s==DiscoveryStatus::Ok:s==DiscoveryStatus::Truncated);
        if(n<512)CHECK(c.calls==0);
    }
    uint32_t random=0x31aabbcc;
    for(unsigned trial=0;trial<5000;++trial) {
        auto b=source;
        for(unsigned i=0;i<6;++i){random=random*1664525U+1013904223U;const size_t at=(random>>8)%b.size();random=random*1664525U+1013904223U;b[at]^=uint8_t(random>>24);}
        if(trial%2==0)fix(b); // exercise nested offsets as well as checksum rejection
        uint8_t* ptr=g.memory+g.page-b.size();std::memcpy(ptr,b.data(),b.size());
        Capture c;const auto s=parseDiscovery(ptr,b.size(),info,visit,&c);
        CHECK(s==DiscoveryStatus::Ok?c.calls==info.ipCount:c.calls==0);
    }
}
}
int main(){versions();adversarial();guarded();std::cout<<checks<<" assertions, "<<failures<<" failures; 513 guarded truncations and 5000 mutations.\n";return failures?1:0;}
