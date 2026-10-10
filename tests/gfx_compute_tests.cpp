// SPDX-License-Identifier: MIT
#include "hackynavi/gfx_compute.hpp"
#include <cstdio>
#include <cstring>
#include <initializer_list>
using namespace hackynavi::gfxcompute;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::printf("FAIL line%u: %s\n",unsigned(__LINE__),#x);}} while(0)
Request request(Operation op=Operation::Clear) {
    return {op,{0x1234500000,4096},op==Operation::Copy?Buffer{0x2345600000,4096}:Buffer{},
        {0x3456700000,4096},4096,{0xa38ce145,0x529df083,0xce171936,0x294efa06}};
}
uint32_t word(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
void shader() {
    uint8_t image[kShaderImageBytes+2];std::memset(image,0xa7,sizeof(image));
    CHECK(shaderImage(image+1,kShaderImageBytes-1)==Status::BufferTooSmall);
    for(uint8_t c:image)CHECK(c==0xa7);
    CHECK(shaderImage(nullptr,kShaderImageBytes)==Status::InvalidArgument);
    CHECK(shaderImage(image+1,kShaderImageBytes)==Status::Ok);
    CHECK(image[0]==0xa7 && image[sizeof(image)-1]==0xa7);
    // Source-exact GFX11 shader instruction prefix, code end and independent
    // copy payload offset. No CPU execution of these GPU opcodes is tested.
    CHECK(word(image+1)==0xb0802006);CHECK(word(image+1+12)==0x3ff);
    CHECK(word(image+1+56)==0xbfb00000);
    CHECK(word(image+1+kCopyShaderOffset)==0xb0802006);
    CHECK(word(image+1+kCopyShaderOffset+28)==0xe00c0000);
    CHECK(word(image+1+kCopyShaderOffset+40)==0xe01c0000);
    CHECK(word(image+1+kCopyShaderOffset+52)==0xbfb00000);
    CHECK(word(image+1+kShaderImageBytes-4)==0xbf9f0000);
}
void decode(const Request& r) {
    uint32_t out[kMaxCommandDwords+2];for(auto& v:out)v=0x6af09713;
    size_t words=99;CHECK(encode(r,out+1,kMaxCommandDwords,words)==Status::Ok);
    CHECK(words==104 && (words&7)==0);CHECK(out[0]==0x6af09713);
    for(size_t i=words+1;i<kMaxCommandDwords+2;++i)CHECK(out[i]==0x6af09713);
    uint32_t sh[1024]={},uc[1024]={};unsigned dispatch=0,barrier=0,context=0,csFlush=0,pfp=0;
    for(size_t i=0;i<words;) {
        const uint32_t header=out[1+i++];
        if(header==0xffff1000)continue;
        CHECK((header>>30)==3);const uint32_t op=(header>>8)&255;
        const size_t n=((header>>16)&0x3fff)+1;CHECK(n<=words-i);
        if(n>words-i)break;const uint32_t* p=out+1+i;
        if(op==0x76 || op==0x9b || op==0x79) {
            const uint32_t offset=p[0]&0xffff;CHECK(offset+n-1<=1024);
            CHECK(bool(header&2)==(op!=0x79));
            if(op==0x9b)CHECK((p[0]>>28)==3);
            for(size_t j=1;j<n;++j)(op==0x79?uc:sh)[offset+j-1]=p[j];
        } else if(op==0x28) {++context;CHECK(n==2 && p[0]==0x80000000 && p[1]==0x80000000);}
        else if(op==0x58) {
            ++barrier;CHECK(n==7 && !dispatch);CHECK(p[0]==0 && p[1]==UINT32_MAX && p[2]==0xffffff);
            CHECK(p[3]==0 && p[4]==0 && p[5]==10);CHECK(p[6]==0x43a1);
        } else if(op==0x15) {
            ++dispatch;CHECK(n==4 && barrier && context && (header&2));
            CHECK(p[0]==(r.bytes/16+63)/64 && p[1]==1 && p[2]==1 && p[3]==1);
        } else if(op==0x46) {++csFlush;CHECK(dispatch==1 && n==1 && p[0]==0x407);}
        else if(op==0x42) {++pfp;CHECK(csFlush==1 && n==1 && p[0]==0);}
        else CHECK(false);
        i+=n;
    }
    CHECK(dispatch==1 && csFlush==1 && pfp==1 && barrier==1);
    const uint64_t code=r.shader.gpu+(r.operation==Operation::Copy?kCopyShaderOffset:0);
    CHECK(sh[0x20c]==uint32_t(code>>8) && sh[0x20d]==uint32_t(code>>40));
    CHECK(sh[0x207]==64 && sh[0x208]==1 && sh[0x209]==1);
    CHECK(sh[0x212]==0x400c0041 && sh[0x213]==0x90 && sh[0x228]==0x30);
    CHECK(code+uint64_t((sh[0x228]>>4)&63)*128<=r.shader.gpu+r.shader.bytes);
    for(unsigned x: {0x216U,0x217U,0x219U,0x21aU,0x22bU,0x22cU})CHECK(sh[x]==UINT32_MAX);
    CHECK(sh[0x22d]==0 && sh[0x22e]==0 && uc[0x22a]==0);
    const uint64_t first=r.operation==Operation::Copy?r.source.gpu:r.destination.gpu;
    CHECK(sh[0x240]==uint32_t(first) && sh[0x241]==(uint32_t(first>>32)|0x100000));
    CHECK(sh[0x242]==r.bytes/16 && sh[0x243]==0x1003dfac);
    if(r.operation==Operation::Clear) { for(unsigned i=0;i<4;++i)CHECK(sh[0x244+i]==r.pattern[i]); }
    else { CHECK(sh[0x244]==uint32_t(r.destination.gpu));
        CHECK(sh[0x245]==(uint32_t(r.destination.gpu>>32)|0x100000));
        CHECK(sh[0x246]==r.bytes/16 && sh[0x247]==0x1003dfac); }
}
void rejected(const Request& r,Status expected,size_t capacity=kMaxCommandDwords) {
    uint32_t out[kMaxCommandDwords];for(auto& v:out)v=0x146da195;
    size_t words=27;CHECK(encode(r,out,capacity,words)==expected);CHECK(words==27);
    for(uint32_t v:out)CHECK(v==0x146da195);
}
void invalid() {
    Request r=request();for(size_t n=0;n<104;++n)rejected(r,Status::BufferTooSmall,n);
    r.bytes=0;rejected(r,Status::InvalidArgument);r=request();r.bytes=17;rejected(r,Status::InvalidArgument);
    r=request();r.bytes=kMaxTransferBytes+16;rejected(r,Status::InvalidArgument);
    r=request();r.shader.gpu++;rejected(r,Status::InvalidRange);
    r=request();r.shader.bytes=4095;rejected(r,Status::InvalidRange);
    r=request();r.destination.gpu++;rejected(r,Status::InvalidRange);
    r=request();r.destination.bytes=4095;rejected(r,Status::InvalidRange);
    r=request();r.destination.gpu=(UINT64_C(1)<<47)-2048;rejected(r,Status::InvalidRange);
    r=request();r.destination.gpu=UINT64_MAX-15;rejected(r,Status::InvalidRange);
    r=request();r.shader.gpu=r.destination.gpu;rejected(r,Status::OverlappingMemory);
    r=request();r.source.gpu=1;rejected(r,Status::InvalidArgument);
    r=request(Operation::Copy);r.source.bytes=4095;rejected(r,Status::InvalidRange);
    r=request(Operation::Copy);r.source.gpu=r.destination.gpu+16;rejected(r,Status::OverlappingMemory);
    r=request(Operation::Copy);r.source.gpu=r.shader.gpu+16;rejected(r,Status::OverlappingMemory);
    r=request();r.operation=static_cast<Operation>(99);rejected(r,Status::InvalidArgument);
}
}
int main() {
    shader();invalid();decode(request());decode(request(Operation::Copy));
    for(uint32_t bytes: {16U,1008U,1024U,1040U,kMaxTransferBytes}) {
        Request r=request(Operation::Copy);r.bytes=bytes;r.source.bytes=r.destination.bytes=bytes;decode(r);
    }
    std::printf("GFX11 compute encoding: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
