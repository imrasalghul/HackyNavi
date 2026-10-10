// SPDX-License-Identifier: MIT
// Immutable compiler-product/launch admission checks; no GPU execution model.
#include "hackynavi/gfx_aco_fixture.hpp"
#include "../userspace/compiler/fixtures/apple-add-one/shader_data.hpp"
#include <cstdio>
#include <cstring>
using namespace hackynavi;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);}}while(false)
using gfxaco::Status;
gfxisolated::Binding binding(uint32_t id,uint64_t gpu,uint32_t access,uint64_t bytes=4096) {
    return {0x12345678,0xfedcba98,id,9,gpu,bytes,0,bytes,access};
}
gfxaco::Request request() {
    gfxaco::Request r={};r.owner=0x12345678;r.epoch=0xfedcba98;r.vmid=1;
    r.shader=binding(1,0x10000010000ULL,5);
    r.source=binding(2,0x20000020000ULL,1,20480);
    r.destination=binding(3,0x30000030000ULL,3,20480);
    r.indirect=binding(4,0x40000040000ULL,5);
    r.parameters=binding(5,0x50000050000ULL,1);return r;
}
struct Capture {
    uint8_t shader[4096],parameters[4096];uint32_t commands[128];size_t words;
    bool build(const gfxaco::Request& r) {
        words=0;return gfxaco::shaderImage(shader,sizeof(shader))==Status::Ok&&
            gfxaco::parameterImage(r,parameters,sizeof(parameters))==Status::Ok&&
            gfxaco::encode(r,commands,128,words)==Status::Ok;
    }
    Status validate(const gfxaco::Request& r) const {
        return gfxaco::validate(r,shader,sizeof(shader),parameters,sizeof(parameters),commands,words);
    }
};
uint32_t le32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
// Independently decode the packet stream to assert the hardware launch state,
// rather than only comparing an encoder's output with its canonical validator.
void inspectPackets(const Capture& c,const gfxaco::Request& r) {
    uint32_t sh[0x300]={};bool written[0x300]={};unsigned dispatches=0,partial=0,sync=0,acquire=0;
    size_t n=0;
    while(n<c.words) {
        const uint32_t h=c.commands[n++];if(h==0xffff1000)continue;
        CHECK((h>>30)==3);const size_t payload=((h>>16)&0x3fff)+1;
        CHECK(payload<=c.words-n);if(payload>c.words-n)return;
        const uint32_t op=(h>>8)&0xff;
        if(op==0x76) {
            CHECK((h&3)==2);const uint32_t first=c.commands[n];CHECK(first+payload-1<=0x300);
            for(size_t j=1;j<payload&&first+j-1<0x300;++j){sh[first+j-1]=c.commands[n+j];written[first+j-1]=true;}
        } else if(op==0x15) {
            ++dispatches;CHECK(payload==4);CHECK(c.commands[n]==64&&c.commands[n+1]==1&&c.commands[n+2]==1);
            CHECK(c.commands[n+3]==0x8001); // COMPUTE_SHADER_EN + CS_W32_EN; no predication/indirect.
        } else if(op==0x46) {++partial;CHECK(payload==1&&c.commands[n]==0x407);}
        else if(op==0x42) {++sync;CHECK(payload==1&&c.commands[n]==0);}
        else if(op==0x58) {++acquire;CHECK(payload==7&&c.commands[n+6]==0x43a1);}
        else CHECK(op==0x28||op==0x79||op==0x9b);
        n+=payload;
    }
    CHECK(dispatches==1&&partial==1&&sync==1&&acquire==1);
    CHECK(written[0x212]&&sh[0x212]==0x202c0002&&written[0x213]&&sh[0x213]==0x788);
    CHECK(written[0x228]&&sh[0x228]==0);
    CHECK(sh[0x207]==64&&sh[0x208]==1&&sh[0x209]==1);
    CHECK(written[0x204]&&written[0x205]&&written[0x206]&&sh[0x204]==0&&sh[0x205]==0&&sh[0x206]==0);
    CHECK(sh[0x20c]==uint32_t(r.shader.gpu>>8)&&sh[0x20d]==uint32_t(r.shader.gpu>>40));
    CHECK(sh[0x240]==uint32_t(r.parameters.gpu)&&sh[0x241]==uint32_t(r.parameters.gpu>>32));
    CHECK(sh[0x242]==uint32_t(r.parameters.gpu+32)&&sh[0x243]==uint32_t((r.parameters.gpu+32)>>32));
    CHECK(sh[0x215]==0&&written[0x215]);CHECK(!(c.words&7)&&c.words<=gfxaco::kMaxCommandDwords);
}
void productAndLaunch() {
    auto r=request();r.sourceOffset=12;r.destinationOffset=3840;Capture c;CHECK(c.build(r));
    CHECK(c.validate(r)==Status::Ok);
    CHECK(std::memcmp(c.shader,gfxaco::fixture::kCode,104)==0);
    CHECK(gfxaco::kCodeBytes==104&&gfxaco::kExecutableBytes==84&&gfxaco::kDataBytes==16640);
    for(size_t n=104;n<4096;++n)CHECK(c.shader[n]==0);
    // Mesa ac_build_raw_buffer_descriptor(GFX11,false,address,16640) uses
    // byte NUM_RECORDS, stride0, full48-bit address and RAW OOB selection.
    const uint64_t addresses[]={r.source.gpu+12,r.destination.gpu+3840};
    for(size_t n=0;n<2;++n) {
        CHECK(le32(c.parameters+n*16)==uint32_t(addresses[n]));
        CHECK(le32(c.parameters+n*16+4)==uint32_t(addresses[n]>>32));
        CHECK(le32(c.parameters+n*16+8)==16640);
        CHECK(le32(c.parameters+n*16+12)==0x30016fac);
    }
    const uint32_t push[]={4096,1,1,0,0,0,0,0,0,64,1,1};
    for(size_t n=0;n<12;++n)CHECK(le32(c.parameters+32+n*4)==push[n]);
    for(size_t n=80;n<4096;++n)CHECK(c.parameters[n]==0);
    inspectPackets(c,r);
    for(uint8_t vmid=1;vmid<=7;++vmid){r.vmid=vmid;CHECK(c.validate(r)==Status::Ok);}
    // Data offsets are relative to the retained mapping, including submapping
    // BO offsets; descriptors must never add the backing bufferOffset again.
    r.source.bufferOffset=4096;r.source.allocationBytes+=4096;
    r.destination.bufferOffset=8192;r.destination.allocationBytes+=8192;
    CHECK(c.build(r));CHECK(c.validate(r)==Status::Ok);
    CHECK(le32(c.parameters)==uint32_t(r.source.gpu+12));
    // Top-of-VA legal mappings remain representable in descriptors/PGM.
    r.source.gpu=(UINT64_C(1)<<47)-r.source.bytes;CHECK(c.build(r));CHECK(c.validate(r)==Status::Ok);
    CHECK(le32(c.parameters+4)==0x7fff);
}
void mutations() {
    auto r=request();Capture c;CHECK(c.build(r));
    // Every single bit of the exact code, instruction-prefetch tail and unused
    // private code page is checked. No caller-provided instruction is admitted.
    for(size_t n=0;n<4096;++n)for(unsigned bit=0;bit<8;++bit) {
        c.shader[n]^=uint8_t(1U<<bit);CHECK(c.validate(r)==Status::InvalidShader);c.shader[n]^=uint8_t(1U<<bit);
    }
    for(size_t n=0;n<4096;++n)for(unsigned bit=0;bit<8;++bit) {
        c.parameters[n]^=uint8_t(1U<<bit);CHECK(c.validate(r)==Status::InvalidParameters);c.parameters[n]^=uint8_t(1U<<bit);
    }
    for(size_t n=0;n<c.words;++n)for(unsigned bit=0;bit<32;++bit) {
        c.commands[n]^=1U<<bit;CHECK(c.validate(r)==Status::InvalidStream);c.commands[n]^=1U<<bit;
    }
    for(size_t words=1;words<c.words;++words)
        CHECK(gfxaco::validate(r,c.shader,4096,c.parameters,4096,c.commands,words)==Status::InvalidStream);
    c.commands[c.words]=0xffff1000;
    CHECK(gfxaco::validate(r,c.shader,4096,c.parameters,4096,c.commands,c.words+1)==Status::InvalidStream);
}
void ownershipAndBounds() {
    const auto good=request();Capture c;CHECK(c.build(good));
    for(unsigned selected=0;selected<5;++selected)for(unsigned mutation=0;mutation<15;++mutation) {
        auto r=good;gfxisolated::Binding* all[]={&r.shader,&r.source,&r.destination,&r.indirect,&r.parameters};auto& b=*all[selected];
        if(mutation==0)b.owner++;
        if(mutation==1)b.epoch++;
        if(mutation==2)b.id=0;
        if(mutation==3)b.generation=0;
        if(mutation==4)b.bytes=0;
        if(mutation==5)b.gpu=UINT64_C(1)<<47;
        if(mutation==6)b.gpu=0;
        if(mutation==7)b.gpu++;
        if(mutation==8)b.bytes++;
        if(mutation==9)b.bufferOffset++;
        if(mutation==10)b.allocationBytes--;
        if(mutation==11)b.bytes=UINT64_MAX-4095;
        if(mutation==12)b.bufferOffset=b.allocationBytes;
        if(mutation==13)b.access^=2;
        if(mutation==14)b.access|=8;
        CHECK(c.validate(r)!=Status::Ok);
    }
    for(unsigned a=0;a<5;++a)for(unsigned b=a+1;b<5;++b) {
        auto r=good;gfxisolated::Binding* all[]={&r.shader,&r.source,&r.destination,&r.indirect,&r.parameters};
        all[b]->id=all[a]->id;CHECK(c.validate(r)==Status::AliasedStorage);
        ++all[b]->generation;CHECK(c.validate(r)==Status::AliasedStorage);
        r=good;all[b]->gpu=all[a]->gpu;CHECK(c.validate(r)==Status::Overlap);
    }
    for(unsigned selected=0;selected<3;++selected) {
        auto r=good;gfxisolated::Binding* all[]={&r.shader,&r.indirect,&r.parameters};
        all[selected]->bytes=all[selected]->allocationBytes=8192;CHECK(c.validate(r)==Status::InvalidBinding);
        r=good;all[selected]->allocationBytes=8192;all[selected]->bufferOffset=4096;
        CHECK(c.validate(r)==Status::InvalidBinding);
    }
    for(unsigned n=0;n<2;++n) {
        auto r=good;uint64_t& offset=n?r.destinationOffset:r.sourceOffset;
        offset=3840;CHECK(c.build(r));CHECK(c.validate(r)==Status::Ok);
        offset=3844;CHECK(c.validate(r)==Status::InvalidBinding);
        offset=1;CHECK(c.validate(r)==Status::InvalidBinding);
        offset=UINT64_MAX-3;CHECK(c.validate(r)==Status::InvalidBinding);
    }
    auto r=good;r.owner=0;CHECK(c.validate(r)==Status::InvalidArgument);
    r=good;r.epoch=0;CHECK(c.validate(r)==Status::InvalidArgument);
    r=good;r.vmid=0;CHECK(c.validate(r)==Status::InvalidArgument);
    r.vmid=8;CHECK(c.validate(r)==Status::InvalidArgument);
}
void malformedAndTransactional() {
    auto r=request();Capture c;CHECK(c.build(r));
    uint8_t bytes[4096];std::memset(bytes,0xa5,sizeof(bytes));
    CHECK(gfxaco::shaderImage(bytes,4095)==Status::BufferTooSmall);
    CHECK(gfxaco::parameterImage(r,bytes,4095)==Status::BufferTooSmall);
    auto bad=r;bad.sourceOffset=1;CHECK(gfxaco::parameterImage(bad,bytes,4096)==Status::InvalidBinding);
    for(auto v:bytes)CHECK(v==0xa5);
    CHECK(gfxaco::shaderImage(nullptr,4096)==Status::InvalidArgument);
    CHECK(gfxaco::shaderImage(reinterpret_cast<uint8_t*>(UINTPTR_MAX-4094),4096)==Status::InvalidArgument);
    uint32_t commands[128];for(auto& word:commands)word=0xa5a5a5a5;size_t words=0x9876;
    CHECK(gfxaco::encode(r,commands,c.words-1,words)==Status::BufferTooSmall);
    CHECK(gfxaco::encode(bad,commands,128,words)==Status::InvalidBinding);
    CHECK(words==0x9876);for(auto word:commands)CHECK(word==0xa5a5a5a5);
    CHECK(gfxaco::encode(r,nullptr,128,words)==Status::InvalidArgument);
    CHECK(gfxaco::encode(r,reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(commands)+1),128,words)==Status::InvalidArgument);
    CHECK(gfxaco::encode(r,reinterpret_cast<uint32_t*>(UINTPTR_MAX-3),128,words)==Status::InvalidArgument);
    CHECK(gfxaco::validate(r,nullptr,4096,c.parameters,4096,c.commands,c.words)==Status::InvalidArgument);
    CHECK(gfxaco::validate(r,c.shader,104,c.parameters,4096,c.commands,c.words)==Status::InvalidArgument);
    CHECK(gfxaco::validate(r,c.shader,4096,c.parameters,80,c.commands,c.words)==Status::InvalidArgument);
    CHECK(gfxaco::validate(r,c.shader,4096,c.parameters,4096,c.commands,0)==Status::InvalidArgument);
    CHECK(gfxaco::validate(r,c.shader,4096,c.parameters,4096,c.commands,SIZE_MAX)==Status::InvalidArgument);
    CHECK(gfxaco::validate(r,c.shader,4096,c.parameters,4096,reinterpret_cast<uint32_t*>(UINTPTR_MAX-3),c.words)==Status::InvalidArgument);
}
}
int main(){productAndLaunch();mutations();ownershipAndBounds();malformedAndTransactional();
    std::printf("gfx-aco-fixture: %zu checks, %zu failures\n",checks,failures);return failures?1:0;}
