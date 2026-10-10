// SPDX-License-Identifier: MIT
// Exercises the actual production CPU geometry/copy helper. Numeric layouts
// here test bounds only; they are not an addrlib result or GPU observation.
#include "../texture_region.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace hackynavi::texture;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){if(failures<12)std::printf("line %u: %s\n",unsigned(__LINE__),#x);++failures;}}while(0)
HNColorLayout layout(uint32_t width,uint32_t height) {
    HNColorLayout result{};result.width=width;result.height=height;
    result.pitch_pixels=(width+63)&~63u;result.pitch_bytes=result.pitch_pixels*4;
    result.slice_bytes=result.total_bytes=uint64_t(result.pitch_bytes)*height;return result;
}
bool equal(const Transfer& a,const Transfer& b) {
    return a.textureOffset()==b.textureOffset()&&a.rowBytes()==b.rowBytes()&&a.rows()==b.rows()&&
        a.texturePitch()==b.texturePitch()&&a.hostPitch()==b.hostPitch()&&a.hostSpan()==b.hostSpan()&&
        a.packedBytes()==b.packedBytes()&&a.textureBytes()==b.textureBytes();
}
void pixels(uint32_t width,uint32_t height,uint32_t x,uint32_t y,uint32_t rw,uint32_t rh,size_t padding) {
    const auto l=layout(width,height);Transfer t;
    const size_t pitch=rw*4+padding;
    CHECK(plan(l,{x,y,0,rw,rh,1},pitch,t));
    CHECK(t.textureOffset()==uint64_t(y)*l.pitch_bytes+x*4);
    CHECK(t.rowBytes()==rw*4&&t.rows()==rh&&t.packedBytes()==rw*rh*4);
    std::vector<uint8_t> host(t.hostSpan()+16,0xa5),packed(t.packedBytes()+16,0x6e);
    std::vector<uint8_t> storage(size_t(l.total_bytes)+16,0xcc),readback(t.hostSpan()+16,0xb6);
    for(size_t row=0;row<rh;++row)for(size_t col=0;col<rw*4;++col)
        host[row*pitch+col]=uint8_t(row*37+col*3);
    const auto original=host;
    CHECK(pack(t,host.data(),t.hostSpan(),packed.data(),t.packedBytes()));
    CHECK(commit(t,packed.data(),t.packedBytes(),storage.data(),size_t(l.total_bytes)));
    CHECK(read(t,storage.data(),size_t(l.total_bytes),readback.data(),t.hostSpan()));
    CHECK(host==original);
    // Independently inspect every byte, including pixel columns outside the
    // rectangle, row padding, trailing guards and caller row padding.
    for(size_t i=0;i<storage.size();++i) {
        const size_t row=i/l.pitch_bytes,col=i%l.pitch_bytes;
        const bool in=row>=y&&row<y+rh&&col>=x*4&&col<(x+rw)*4;
        CHECK(storage[i]==(in?uint8_t((row-y)*37+(col-x*4)*3):0xcc));
    }
    for(size_t i=0;i<readback.size();++i) {
        const size_t row=i/pitch,col=i%pitch;const bool in=row<rh&&col<rw*4;
        CHECK(readback[i]==(in?uint8_t(row*37+col*3):0xb6));
    }
    for(size_t i=t.packedBytes();i<packed.size();++i)CHECK(packed[i]==0x6e);
    const auto before=readback;
    CHECK(!read(t,storage.data(),size_t(l.total_bytes)-1,readback.data(),t.hostSpan()));
    CHECK(readback==before);
    CHECK(!read(t,storage.data(),size_t(l.total_bytes),readback.data(),t.hostSpan()-1));CHECK(readback==before);
    CHECK(!pack(t,host.data(),t.hostSpan()-1,packed.data(),t.packedBytes()));
    CHECK(!commit(t,packed.data(),t.packedBytes()-1,storage.data(),size_t(l.total_bytes)));
}
void rejected() {
    const auto l=layout(65,19);Transfer good;CHECK(plan(l,{2,3,0,60,13,1},256,good));
    for(Region r: {Region{0,0,0,0,1,1},Region{0,0,0,1,0,1},Region{0,0,1,1,1,1},
        Region{0,0,0,1,1,0},Region{0,0,0,1,1,2},Region{65,0,0,1,1,1},
        Region{0,19,0,1,1,1},Region{64,0,0,2,1,1},Region{0,18,0,1,2,1},
        Region{UINT64_MAX,0,0,1,1,1},Region{0,UINT64_MAX,0,1,1,1},
        Region{0,0,0,UINT64_MAX,1,1},Region{0,0,0,1,UINT64_MAX,1}}) {
        auto out=good;CHECK(!plan(l,r,256,out));CHECK(equal(good,out));
    }
    for(uint64_t pitch:{uint64_t(0),uint64_t(3),uint64_t(239),uint64_t(241),UINT64_MAX,UINT64_MAX-3}) {
        auto out=good;CHECK(!plan(l,{2,3,0,60,13,1},pitch,out));CHECK(equal(good,out));
    }
    for(unsigned n=0;n<7;++n) {
        auto bad=l;
        switch(n){case 0:bad.width=0;break;case 1:bad.height=16385;break;case 2:++bad.pitch_pixels;break;
        case 3:++bad.pitch_bytes;break;case 4:++bad.total_bytes;break;case 5:++bad.slice_bytes;break;case 6:bad.width=UINT32_MAX;break;}
        auto out=good;CHECK(!plan(bad,{0,0,0,1,1,1},4,out));CHECK(equal(good,out));
    }
    std::vector<uint8_t> bytes(size_t(l.total_bytes),0x37);const auto before=bytes;
    CHECK(!read(good,bytes.data(),bytes.size(),bytes.data()+3,bytes.size()-3));CHECK(bytes==before);
    CHECK(!pack(good,bytes.data(),bytes.size(),bytes.data(),bytes.size()));CHECK(bytes==before);
    CHECK(!commit(good,bytes.data()+7,bytes.size()-7,bytes.data(),bytes.size()));CHECK(bytes==before);
    CHECK(!read(good,nullptr,bytes.size(),bytes.data(),bytes.size()));CHECK(bytes==before);
    CHECK(!read(good,reinterpret_cast<void*>(UINTPTR_MAX-3),bytes.size(),bytes.data(),bytes.size()));CHECK(bytes==before);
    CHECK(!pack(good,bytes.data(),bytes.size(),reinterpret_cast<void*>(UINTPTR_MAX-3),bytes.size()));
    Transfer empty;CHECK(!pack(empty,bytes.data(),bytes.size(),bytes.data()+512,bytes.size()-512));CHECK(bytes==before);
    auto self=good;CHECK(!read(good,bytes.data(),bytes.size(),&good,sizeof(good)));CHECK(equal(good,self));
    // Largest admitted dimension geometry and exact last pixel; no 1GiB CPU
    // allocation is needed to prove multiplication/last-row bounds.
    const auto large=layout(16384,16384);Transfer t;
    CHECK(plan(large,{16383,16383,0,1,1,1},4,t));CHECK(t.textureOffset()+4==large.total_bytes);
    CHECK(plan(large,{16383,16383,0,1,1,1},0,t));CHECK(t.hostPitch()==4&&t.hostSpan()==4);
    CHECK(plan(large,{0,0,0,16384,16384,1},65536,t));CHECK(t.packedBytes()==1073741824);
    CHECK(!plan(large,{0,0,0,1,16384,1},UINT64_MAX-3,t));
    CHECK(planWrite(l,{0,0,0,1,1,1},0,0,0,0,t));CHECK(t.hostSpan()==4);
    CHECK(!planRead(l,{0,0,0,1,1,1},0,0,0,0,t));
    CHECK(!planWrite(l,{0,0,0,1,2,1},0,0,0,0,t));
    CHECK(planRead(l,{0,0,0,1,1,1},uint64_t(INT32_MAX)*4-4,0,0,0,t));
    CHECK(!planRead(l,{0,0,0,1,1,1},uint64_t(INT32_MAX)*4,0,0,0,t));
    CHECK(planWrite(l,{0,0,0,1,1,1},32767u*4-4,0,0,0,t));
    CHECK(!planWrite(l,{0,0,0,1,1,1},32767u*4,0,0,0,t));
    for(unsigned mode=0;mode<2;++mode)for(unsigned field=0;field<3;++field) {
        const auto before=t;
        const bool result=mode?planRead(l,{0,0,0,1,1,1},4,field==0,field==1,field==2,t):
            planWrite(l,{0,0,0,1,1,1},4,field==0,field==1,field==2,t);
        CHECK(!result);CHECK(equal(before,t));
    }
}
}
int main() {
    for(uint32_t width:{1u,2u,63u,64u,65u,129u})for(uint32_t height:{1u,2u,17u}) {
        pixels(width,height,0,0,width,height,0);
        pixels(width,height,width-1,height-1,1,1,20);
        if(width>2&&height>2)pixels(width,height,1,1,width-2,height-2,12);
    }
    rejected();std::printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
