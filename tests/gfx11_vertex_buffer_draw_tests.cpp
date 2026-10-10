// SPDX-License-Identifier: MIT
#include "hackynavi/gfx11_graphics_draw.hpp"
#include "fixtures/gfx11_vertex_buffer_shader.hpp"
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
    alignas(VertexBufferedWorkspace) std::array<uint8_t,4096> image{};
    VertexFragmentBufferedRequest request{};
    explicit Fixture(bool both=true) {
        memcpy(image.data(),vertex_buffer_fixture::vertex,sizeof(vertex_buffer_fixture::vertex));
        const size_t bytes=both?sizeof(buffer_shader_fixture::fragment):sizeof(vertex_buffer_fixture::constantFragment);
        memcpy(image.data()+256,both?buffer_shader_fixture::fragment:vertex_buffer_fixture::constantFragment,bytes);
        auto& r=request.draw;r.owner=11;r.epoch=17;r.vmid=3;
        r.topology={{0x545,0},6,24,16};r.image={image.data(),4096,sizeof(vertex_buffer_fixture::vertex),bytes};
        r.targetLayout={64,64,64,256,16384,16384,256,0,0x2800a,0,0,(63u<<14)|63,0x1000000,0x428};
        r.code=binding(1,0x100001000,4096,5);r.indirect=binding(2,0x100002000,8192,5);
        r.attributes=binding(3,0x101000000,uint64_t(kAttributeBytesPerSe)*6,3);
        r.target=binding(4,0x400000000000,16384,3);
        request.vertex=binding(5,0x1234567000,4096,1);request.vertexOffset=0x890;
        request.fragment=binding(6,0x1234568000,4096,1);request.fragmentOffset=0xff0;
    }
    VertexBufferedRequest vertexOnly()const{return {request.draw,request.vertex,request.vertexOffset};}
};
template<class R> void reject(const R& r,Status expected) {
    VertexBufferedWorkspace w{};Words output;output.fill(0xa5a5a5a5);const Words before=output;size_t words=77;
    CHECK(encode(r,w,output.data(),output.size(),words)==expected);CHECK(words==77&&output==before);
}
size_t findSequence(const Words& s,size_t words,uint32_t index,unsigned count) {
    for(size_t at=0;at<words;) {
        if(s[at]==0xffff1000){++at;continue;}
        const size_t bytes=((s[at]>>16)&0x3fff)+2;
        if(bytes>words-at)return words;
        if(s[at]==(0xc0007600u|(count<<16))&&s[at+1]==index)return at+2;
        at+=bytes;
    }
    return words;
}
template<class R> void accepted(const R& r,bool both,FILE* descriptors) {
    VertexBufferedWorkspace w{};Words output{};size_t words=0;
    CHECK(encode(r,w,output.data(),output.size(),words)==Status::Ok);
    CHECK(words>0&&words<=output.size()&&!(words&7));CHECK(w.draw.program.vertex.rsrc2==10);
    const size_t v=findSequence(output,words,0x8c,5),f=findSequence(output,words,12,4);
    CHECK(v<words);CHECK(both?f<words:f==words);if(v>=words)return;
    CHECK(output[v]==w.vertexDescriptor[0]&&output[v+1]==w.vertexDescriptor[1]);
    CHECK(output[v+2]==48&&output[v+3]==0x30016fac&&output[v+4]==0);
    if(both)CHECK(output[f+2]==16&&output[f+3]==0x30016fac);
    CHECK(validate(r,w,output.data(),words)==Status::Ok);
    if(descriptors)fprintf(descriptors,"%llu %u %u %u %u\n",
        (unsigned long long)(uint64_t(output[v])|(uint64_t(output[v+1])<<32)),output[v],output[v+1],output[v+2],output[v+3]);
    w.vertexDescriptor[2]=4096;w.fragmentDescriptor[2]=4096;w.draw.program.vertex.rsrc2=0;
    CHECK(validate(r,w,output.data(),words)==Status::Ok);
    // Every canonical command word is authenticated, including baseVertex,
    // draw count, resource descriptors and completion barriers.
    for(size_t i=0;i<words;++i) {
        Words changed=output;changed[i]^=1;
        CHECK(validate(r,w,changed.data(),words)==Status::InvalidStream);
    }
    const Words before=output;size_t unchanged=12;
    CHECK(encode(r,w,output.data(),words-1,unchanged)==Status::BufferTooSmall);
    CHECK(output==before&&unchanged==12);
    CHECK(validate(r,w,output.data(),words-1)==Status::InvalidStream);
    Workspace old{};CHECK(prepare(r.draw,old)==Status::InvalidShader);
}
void positives(FILE* descriptors) {
    for(uint64_t address:{UINT64_C(0x1000),UINT64_C(0xffffffd0),UINT64_C(0x100000000),
                         UINT64_C(0x1234567890),UINT64_C(0x7fffffffffd0)}) {
        Fixture f;f.request.vertex.gpu=address&~UINT64_C(4095);f.request.vertexOffset=address&4095;
        accepted(f.request,true,descriptors);
        Fixture single(false);single.request.vertex=f.request.vertex;single.request.vertexOffset=f.request.vertexOffset;
        accepted(single.vertexOnly(),false,nullptr);
    }
    Fixture f;f.request.vertex.bufferOffset=4096;f.request.vertex.allocationBytes=8192;
    VertexBufferedWorkspace w{};CHECK(prepare(f.request,w)==Status::Ok);
    // Internal profile selection cannot silently admit an unbound fragment shader.
    reject(f.vertexOnly(),Status::InvalidShader);
    Fixture single(false);reject(single.request,Status::InvalidShader);
}
void negatives() {
    for(bool fragment:{false,true}) {
        for(uint64_t offset:{UINT64_C(1),UINT64_C(4081),UINT64_C(4096),UINT64_MAX}) {
            Fixture f;(fragment?f.request.fragmentOffset:f.request.vertexOffset)=offset;
            reject(f.request,Status::InvalidBinding);
        }
        for(unsigned field=0;field<9;++field) {
            Fixture f;auto& b=fragment?f.request.fragment:f.request.vertex;
            switch(field){case 0:b.id=0;break;case 1:b.generation=0;break;case 2:b.gpu=0;break;
            case 3:b.gpu=UINT64_C(1)<<47;break;case 4:b.gpu|=1;break;case 5:b.bytes=16;break;
            case 6:b.bufferOffset=4096;break;case 7:b.allocationBytes=16;break;case 8:b.bytes=UINT64_MAX;break;}
            reject(f.request,Status::InvalidBinding);
        }
        for(unsigned mode:{0u,2u,3u,5u,7u,UINT32_MAX}) {
            Fixture f;(fragment?f.request.fragment:f.request.vertex).access=mode;reject(f.request,Status::Permission);
        }
        {Fixture f;(fragment?f.request.fragment:f.request.vertex).owner++;reject(f.request,Status::ForeignContext);}
        {Fixture f;(fragment?f.request.fragment:f.request.vertex).epoch++;reject(f.request,Status::ForeignContext);}
        for(unsigned n=0;n<5;++n) {
            Fixture f;const auto& r=f.request.draw;
            const auto& b=n==0?r.code:n==1?r.target:n==2?r.attributes:n==3?r.indirect:fragment?f.request.vertex:f.request.fragment;
            auto& bad=fragment?f.request.fragment:f.request.vertex;const auto id=bad.id;
            bad.id=b.id;reject(f.request,Status::AliasedStorage);
            bad.id=id;bad.gpu=b.gpu;reject(f.request,Status::Overlap);
        }
    }
    // Last aligned offset with only 32 bytes left cannot hold three float4s.
    {Fixture f;f.request.vertexOffset=4064;reject(f.request,Status::InvalidBinding);}
    Fixture f;VertexBufferedWorkspace w{};Words stream{};size_t count=0;
    CHECK(encode(f.request,w,stream.data(),stream.size(),count)==Status::Ok);
    const auto prior=f.request;
    CHECK(encode(f.request,w,stream.data(),stream.size(),reinterpret_cast<size_t&>(f.request.fragmentOffset))==Status::InvalidArgument);
    CHECK(!memcmp(&f.request,&prior,sizeof(prior)));
    CHECK(encode(f.request,w,stream.data(),stream.size(),*reinterpret_cast<size_t*>(w.fragmentDescriptor))==Status::InvalidArgument);
    CHECK(prepare(f.request,*reinterpret_cast<VertexBufferedWorkspace*>(&f.request))==Status::InvalidArgument);
    CHECK(prepare(f.request,*reinterpret_cast<VertexBufferedWorkspace*>(f.image.data()))==Status::InvalidArgument);
    auto single=f.vertexOnly();
    CHECK(prepare(single,*reinterpret_cast<VertexBufferedWorkspace*>(&single))==Status::InvalidArgument);
}
}
int main(int argc,char** argv) {
    FILE* output=argc==2?fopen(argv[1],"wb"):nullptr;if(argc==2&&!output)return 2;
    positives(output);negatives();if(output&&fclose(output))return 2;
    printf("vertex buffer draw: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
