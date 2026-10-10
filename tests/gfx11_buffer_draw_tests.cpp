// SPDX-License-Identifier: MIT
#include "hackynavi/gfx11_graphics_draw.hpp"
#include "fixtures/gfx11_buffer_shader.hpp"
#include <array>
#include <stdio.h>
#include <string.h>
using namespace hackynavi;
using namespace hackynavi::gfx11draw;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("line %u: %s\n",unsigned(__LINE__),#x);}}while(0)
using Words=std::array<uint32_t,kMaxCommandDwords>;
gfxisolated::Binding binding(uint32_t id,uint64_t va,uint64_t bytes,uint32_t access) {
    return {11,17,id,19,va,bytes,0,bytes,access};
}
struct Fixture {
    alignas(BufferedWorkspace) std::array<uint8_t,4096> image{};BufferedRequest request{};
    Fixture() {
        memcpy(image.data(),buffer_shader_fixture::vertex,sizeof(buffer_shader_fixture::vertex));
        memcpy(image.data()+256,buffer_shader_fixture::fragment,sizeof(buffer_shader_fixture::fragment));
        Request& r=request.draw;r.owner=11;r.epoch=17;r.vmid=3;
        r.topology={{0x545,0},6,24,16};r.image={image.data(),image.size(),144,72};
        r.targetLayout={64,64,64,256,16384,16384,256,0,0x2800a,0,0,(63u<<14)|63,0x1000000,0x428};
        r.code=binding(1,0x100001000,4096,5);r.indirect=binding(2,0x100002000,8192,5);
        r.attributes=binding(3,0x101000000,uint64_t(kAttributeBytesPerSe)*6,3);
        r.target=binding(4,0x400000000000,16384,3);
        request.fragment=binding(5,0x1234567000,4096,1);request.offset=0x890;
    }
};
void reject(const BufferedRequest& r,Status expected) {
    BufferedWorkspace w{};Words output;output.fill(0xa5a5a5a5);const Words old=output;size_t words=77;
    CHECK(encode(r,w,output.data(),output.size(),words)==expected);CHECK(words==77&&output==old);
}
size_t findDescriptor(const Words& s,size_t words) {
    for(size_t at=0;at<words;) {
        if(s[at]==0xffff1000){++at;continue;}
        const size_t count=((s[at]>>16)&0x3fff)+2;
        if(count>words-at)return words;
        if(s[at]==0xc0047600&&s[at+1]==12)return at+2;
        at+=count;
    }
    return words;
}
void positives(FILE* descriptors) {
    for(uint64_t address:{UINT64_C(0x1000),UINT64_C(0xfffffff0),UINT64_C(0x100000000),
            UINT64_C(0x1234567890),UINT64_C(0x7ffffffffff0)}) {
        Fixture f;f.request.fragment.gpu=address&~UINT64_C(4095);f.request.offset=address&4095;
        BufferedWorkspace w{};Words output{};size_t words=0;
        CHECK(encode(f.request,w,output.data(),output.size(),words)==Status::Ok);
        CHECK(words==720&&w.draw.program.fragment.rsrc2==8);
        const size_t start=findDescriptor(output,words);CHECK(start<words);
        if(start>=words)continue;
        CHECK(output[start]==uint32_t(address)&&output[start+1]==uint32_t(address>>32));
        CHECK(output[start+2]==16&&output[start+3]==0x30016fac);
        CHECK(validate(f.request,w,output.data(),words)==Status::Ok);
        if(descriptors)fprintf(descriptors,"%llu %u %u %u %u\n",(unsigned long long)address,
            output[start],output[start+1],output[start+2],output[start+3]);
        // Re-prepare ignores forged diagnostic descriptor words.
        w.descriptor[2]=4096;CHECK(validate(f.request,w,output.data(),words)==Status::Ok);
        for(unsigned i=0;i<4;++i)for(unsigned bit:{0u,15u,31u}) {
            Words altered=output;altered[start+i]^=1u<<bit;
            CHECK(validate(f.request,w,altered.data(),words)==Status::InvalidStream);
        }
        Words altered=output;altered[start-1]^=1;
        CHECK(validate(f.request,w,altered.data(),words)==Status::InvalidStream);
        altered=output;altered[words-1]^=1;
        CHECK(validate(f.request,w,altered.data(),words)==Status::InvalidStream);
        CHECK(validate(f.request,w,output.data(),words-1)==Status::InvalidStream);
        Workspace old{};CHECK(prepare(f.request.draw,old)==Status::InvalidShader);
    }
    Fixture f;f.request.fragment.bufferOffset=4096;f.request.fragment.allocationBytes=8192;
    BufferedWorkspace w{};CHECK(prepare(f.request,w)==Status::Ok);
}
void negatives() {
    for(uint64_t offset:{UINT64_C(1),UINT64_C(4081),UINT64_C(4096),UINT64_MAX}) {
        Fixture f;f.request.offset=offset;reject(f.request,Status::InvalidBinding);
    }
    for(unsigned field=0;field<9;++field) {
        Fixture f;auto& b=f.request.fragment;
        switch(field){case 0:b.id=0;break;case 1:b.generation=0;break;case 2:b.gpu=0;break;
        case 3:b.gpu=UINT64_C(1)<<47;break;case 4:b.gpu|=1;break;case 5:b.bytes=16;break;
        case 6:b.bufferOffset=4096;break;case 7:b.allocationBytes=16;break;case 8:b.bytes=UINT64_MAX;break;}
        reject(f.request,Status::InvalidBinding);
    }
    for(unsigned mode:{0u,2u,3u,5u,7u,UINT32_MAX}) {
        Fixture f;f.request.fragment.access=mode;reject(f.request,Status::Permission);
    }
    {Fixture f;f.request.fragment.owner++;reject(f.request,Status::ForeignContext);}
    {Fixture f;f.request.fragment.epoch++;reject(f.request,Status::ForeignContext);}
    for(unsigned n=0;n<4;++n) {
        Fixture f;const auto& r=f.request.draw;
        const auto& b=n==0?r.code:n==1?r.target:n==2?r.attributes:r.indirect;
        f.request.fragment.id=b.id;reject(f.request,Status::AliasedStorage);
        f.request.fragment.id=5;f.request.fragment.gpu=b.gpu;reject(f.request,Status::Overlap);
    }
    Fixture f;BufferedWorkspace w{};Words stream{};size_t count=0;
    CHECK(encode(f.request,w,stream.data(),stream.size(),count)==Status::Ok);
    const Words before=stream;size_t unchanged=12;
    CHECK(encode(f.request,w,stream.data(),count-1,unchanged)==Status::BufferTooSmall);
    CHECK(stream==before&&unchanged==12);
    // Whole extended request/workspace aliases are covered, including suffix.
    const BufferedRequest prior=f.request;
    CHECK(encode(f.request,w,stream.data(),stream.size(),reinterpret_cast<size_t&>(f.request.offset))==Status::InvalidArgument);
    CHECK(!memcmp(&f.request,&prior,sizeof(prior)));
    CHECK(encode(f.request,w,stream.data(),stream.size(),*reinterpret_cast<size_t*>(w.descriptor))==Status::InvalidArgument);
    CHECK(prepare(f.request,*reinterpret_cast<BufferedWorkspace*>(&f.request))==Status::InvalidArgument);
    CHECK(prepare(f.request,*reinterpret_cast<BufferedWorkspace*>(f.image.data()))==Status::InvalidArgument);
}
}
int main(int argc,char** argv) {
    FILE* output=argc==2?fopen(argv[1],"wb"):nullptr;if(argc==2&&!output)return 2;
    positives(output);negatives();if(output&&fclose(output))return 2;
    printf("buffer draw: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
