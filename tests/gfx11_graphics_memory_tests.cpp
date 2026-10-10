// SPDX-License-Identifier: MIT
// Pure geometry tests. No DMA allocator, simulated GPU or completion backend.
#include "hackynavi/gfx11_graphics_memory.hpp"
#include <stdio.h>
#include <string.h>
#include <vector>
#include <algorithm>
using namespace hackynavi::graphicsmemory;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("line %u: %s\n",unsigned(__LINE__),#x);}}while(0)
void verify(const std::vector<Mapping>& m,const Plan& p,uint64_t attr,uint64_t ib) {
    CHECK(p.attributeBytes==attr && p.indirectBytes==ib);
    CHECK(!(p.attributes&65535) && !(p.code&4095) && !(p.indirect&4095));
    CHECK(p.attributes>=kVaBegin && p.indirect+ib<=kVaEnd);
    CHECK(p.code==p.attributes+attr && p.indirect==p.code+4096);
    CHECK(p.attributes>>32==(p.indirect+ib-1)>>32);
    for(const auto& r:m)CHECK(p.indirect+ib<=r.va || p.attributes>=r.va+r.bytes);
}
void rejected(const Mapping* m,size_t n,uint64_t attr,uint64_t ib,Status expected) {
    Plan p{1,2,3,4,5};const Plan saved=p;CHECK(place(m,n,attr,ib,p)==expected);CHECK(!memcmp(&p,&saved,sizeof(p)));
}
void boundaries() {
    for(uint64_t attr=65536;attr<=16u*1024u*1024u;attr+=65536)for(uint64_t ib:{4096u,8192u}) {
        Plan p{};CHECK(place(nullptr,0,attr,ib,p)==Status::Ok);verify({},p,attr,ib);
    }
    for(uint64_t size:{0u,4096u,65535u,65537u,16777217u,UINT32_MAX})rejected(nullptr,0,size,4096,Status::InvalidArgument);
    for(uint64_t size:{0u,4095u,4097u,8191u,8193u,UINT32_MAX})rejected(nullptr,0,65536,size,Status::InvalidArgument);
    rejected(nullptr,1,65536,4096,Status::InvalidArgument);
    Mapping map{4096,4096};rejected(&map,129,65536,4096,Status::InvalidArgument);
    for(Mapping bad: {Mapping{0,4096},{4096,0},{4097,4096},{4096,4097},{kVaEnd,4096},{kVaEnd-4096,8192},{UINT64_MAX-4095,4096}})
        rejected(&bad,1,65536,4096,Status::InvalidMapping);
    Mapping cover{kVaBegin,kVaEnd-kVaBegin};rejected(&cover,1,65536,4096,Status::NoSpace);
    Mapping duplicate[]={{4096,8192},{8192,8192}};rejected(duplicate,2,65536,4096,Status::InvalidMapping);
    Mapping unordered[]={{12288,4096},{4096,4096},{8192,4096}};Plan p{};
    CHECK(place(unordered,3,65536,8192,p)==Status::Ok);
    // A gap crossing 4GiB is not allowed to carry code in another address32
    // window. Test exact fit, alignment loss and one-boundary descent.
    for(uint64_t boundary: {kWindowBytes,kVaEnd-kWindowBytes})for(unsigned page=0;page<40;++page) {
        const uint64_t lower=boundary-131072,upper=boundary+uint64_t(page)*4096;
        std::vector<Mapping> maps={{kVaBegin,lower-kVaBegin},{upper,kVaEnd-upper}};
        CHECK(place(maps.data(),maps.size(),65536,8192,p)==Status::Ok);verify(maps,p,65536,8192);
        CHECK(p.attributes==boundary-131072 || p.attributes>=boundary);
    }
}
void oracle() {
    // Exhaustively enumerate a small real VA subrange at64KiB granularity,
    // with independently brute-forced candidate starts. Outer ranges close
    // the rest of the VM. The production algorithm uses sorted free gaps.
    const uint64_t base=3*kWindowBytes-8*65536,end=base+16*65536;
    for(unsigned mask=0;mask<65536;mask+=17)for(uint64_t attr:{65536u,131072u,262144u}) {
        std::vector<Mapping> maps={{kVaBegin,base-kVaBegin},{end,kVaEnd-end}};
        for(unsigned slot=0;slot<16;++slot)if(mask&(1u<<slot))maps.push_back({base+slot*65536,65536});
        uint64_t expected=0;
        for(uint64_t at=base;at+attr+12288<=end;at+=65536) {
            const uint64_t last=at+attr+12288;
            if((at>>32)!=((last-1)>>32))continue;
            bool free=true;for(const auto& m:maps)if(at<m.va+m.bytes && m.va<last)free=false;
            if(free)expected=at;
        }
        std::reverse(maps.begin(),maps.end());Plan p{1,2,3,4,5};const Plan before=p;
        const Status result=place(maps.data(),maps.size(),attr,8192,p);
        CHECK((result==Status::Ok)==bool(expected));
        if(expected){CHECK(p.attributes==expected);verify(maps,p,attr,8192);}
        else {CHECK(result==Status::NoSpace);CHECK(!memcmp(&p,&before,sizeof(p)));}
    }
}
}
int main(){boundaries();oracle();printf("gfx11_graphics_memory: %u checks, %u failures\n",checks,failures);return failures?1:0;}
