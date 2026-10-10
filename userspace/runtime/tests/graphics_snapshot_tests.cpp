// SPDX-License-Identifier: MIT
// Actual frozen Apple/authored ACO stage products. No Context construction,
// transport substitution, native connection, draw execution or GPU claim.
#include "../graphics_snapshot.hpp"
#include "hackynavi/gfx11_graphics_product.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace hackynavi;
namespace {
unsigned checks=0, failures=0;
#define CHECK(x) do { ++checks; if(!(x)) {if(failures<12)std::printf("line %u: %s\n",unsigned(__LINE__),#x);++failures;} } while(0)
using Bytes=std::vector<uint8_t>;
using Pipeline=compute::GraphicsPipeline;
Bytes read(const char* path) {
    FILE* file=std::fopen(path,"rb");if(!file)return {};
    Bytes bytes;for(int c=std::fgetc(file);c!=EOF;c=std::fgetc(file)) {
        if(bytes.size()>=1024){std::fclose(file);return {};}
        bytes.push_back(uint8_t(c));
    }
    const bool error=std::ferror(file);const int closed=std::fclose(file);
    return error||closed?Bytes{}:bytes;
}
HNColorLayout layout(uint32_t width,uint32_t height) {
    const uint32_t pitch=(width+63)&~63U;
    const uint64_t bytes=uint64_t(pitch)*4*height;
    return {width,height,pitch,pitch*4,bytes,bytes,256,0,0x2800a,0,0,
            (width-1)<<14|(height-1),0x01000000,0x428};
}
void snapshot(const Bytes& sourceVertex,const Bytes& sourceFragment) {
    Bytes vertex=sourceVertex,fragment=sourceFragment;
    std::shared_ptr<const Pipeline> pipeline;
    CHECK(bool(Pipeline::capture(vertex.data(),vertex.size(),fragment.data(),fragment.size(),pipeline)));
    if(!pipeline)return;
    CHECK(pipeline->vertexBytes()==vertex.size());CHECK(pipeline->fragmentBytes()==fragment.size());
    gfx11graphics::Layout image{};
    CHECK(gfx11graphics::layout(vertex.size(),fragment.size(),image)==gfx11graphics::Status::Ok);
    Bytes expected(4096);std::memcpy(expected.data(),vertex.data(),vertex.size());
    std::memcpy(expected.data()+image.fragmentOffset,fragment.data(),fragment.size());
    std::fill(vertex.begin(),vertex.end(),0xee);std::fill(fragment.begin(),fragment.end(),0xdd);
    vertex.clear();vertex.shrink_to_fit();fragment.clear();fragment.shrink_to_fit();
    Bytes output(4096,0x77);
    CHECK(bool(pipeline->copyImage(output.data(),output.size())));CHECK(output==expected);
    gfx11graphics::Report report{};
    CHECK(gfx11graphics::validate({output.data(),output.size(),sourceVertex.size(),sourceFragment.size()},report)==gfx11graphics::Status::Ok);
    CHECK(report.vertex.rsrc1==0x2c0002&&report.fragment.rsrc1==0x2c0002);
    // A copied result can be modified/freed without changing the pipeline.
    std::fill(output.begin(),output.end(),0);CHECK(bool(pipeline->copyImage(output.data(),output.size())));CHECK(output==expected);
    const auto saved=pipeline;
    CHECK(Pipeline::capture(sourceVertex.data(),sourceVertex.size(),sourceFragment.data(),sourceFragment.size(),pipeline).status==transport::Status::Invalid);
    CHECK(pipeline==saved);
    for(size_t bytes:{size_t(0),size_t(1),size_t(4095),size_t(4097),SIZE_MAX}) {
        const Bytes before=output;
        CHECK(pipeline->copyImage(output.data(),bytes).status==transport::Status::Invalid);CHECK(output==before);
    }
    CHECK(pipeline->copyImage(nullptr,4096).status==transport::Status::Invalid);
    CHECK(pipeline->copyImage(reinterpret_cast<void*>(UINTPTR_MAX-2047),4096).status==transport::Status::Invalid);
}
void rejected(const Bytes& vs,const Bytes& ps) {
    std::shared_ptr<const Pipeline> pipeline;
    for(size_t n:{size_t(0),size_t(4),size_t(20),size_t(23),size_t(1025),SIZE_MAX}) {
        CHECK(!Pipeline::capture(vs.data(),n,ps.data(),ps.size(),pipeline));CHECK(!pipeline);
        CHECK(!Pipeline::capture(vs.data(),vs.size(),ps.data(),n,pipeline));CHECK(!pipeline);
    }
    CHECK(!Pipeline::capture(nullptr,vs.size(),ps.data(),ps.size(),pipeline));CHECK(!pipeline);
    CHECK(!Pipeline::capture(vs.data(),vs.size(),nullptr,ps.size(),pipeline));CHECK(!pipeline);
    CHECK(!Pipeline::capture(reinterpret_cast<const void*>(UINTPTR_MAX-7),vs.size(),ps.data(),ps.size(),pipeline));CHECK(!pipeline);
    CHECK(!Pipeline::capture(vs.data(),vs.size(),reinterpret_cast<const void*>(UINTPTR_MAX-7),ps.size(),pipeline));CHECK(!pipeline);
    Bytes broken=vs;broken[0]^=1;
    CHECK(Pipeline::capture(broken.data(),broken.size(),ps.data(),ps.size(),pipeline).status==transport::Status::Unsupported);CHECK(!pipeline);
    broken=ps;broken.back()^=1;
    CHECK(Pipeline::capture(vs.data(),vs.size(),broken.data(),broken.size(),pipeline).status==transport::Status::Unsupported);CHECK(!pipeline);
    CHECK(!Pipeline::capture(ps.data(),ps.size(),vs.data(),vs.size(),pipeline));CHECK(!pipeline);
}
void targets() {
    const HNColorTopology topology{0x545,0};
    for(uint32_t width:{1u,63u,64u,65u,4099u,16384u})for(uint32_t height:{1u,2u,63u,64u,65u})
        for(uint64_t offset:{UINT64_C(0),UINT64_C(4096),UINT64_C(65536)}) {
            const auto l=layout(width,height);const uint64_t rounded=(l.total_bytes+4095)&~UINT64_C(4095);
            uint64_t result=0xabcdef;
            CHECK(bool(compute::graphicsTargetFootprint(topology,l,offset+l.total_bytes,offset+rounded,offset,result)));
            CHECK(result==rounded);
            result=0xabcdef;
            CHECK(!compute::graphicsTargetFootprint(topology,l,offset+l.total_bytes-1,offset+rounded,offset,result));CHECK(result==0xabcdef);
            CHECK(!compute::graphicsTargetFootprint(topology,l,offset+rounded,offset+rounded-1,offset,result));CHECK(result==0xabcdef);
            CHECK(!compute::graphicsTargetFootprint(topology,l,offset+rounded,offset+rounded,offset+1,result));CHECK(result==0xabcdef);
        }
    const auto good=layout(128,65);const uint64_t rounded=(good.total_bytes+4095)&~UINT64_C(4095);
    for(size_t bit=0;bit<sizeof(HNColorLayout)*8;++bit) {
        auto changed=good;reinterpret_cast<uint8_t*>(&changed)[bit/8]^=uint8_t(1U<<(bit%8));
        uint64_t result=0xcafe;
        CHECK(!compute::graphicsTargetFootprint(topology,changed,good.total_bytes,rounded,0,result));CHECK(result==0xcafe);
    }
    for(HNColorTopology changed: {HNColorTopology{0,0},HNColorTopology{0xffffffff,0},HNColorTopology{0x545,0xffffffff}}) {
        uint64_t result=0xcafe;CHECK(!compute::graphicsTargetFootprint(changed,good,good.total_bytes,rounded,0,result));CHECK(result==0xcafe);
    }
    uint64_t result=0;
    auto largest=layout(16384,256);
    CHECK(bool(compute::graphicsTargetFootprint(topology,largest,16ULL<<20,16ULL<<20,0,result)));
    CHECK(result==16ULL<<20);largest=layout(16384,257);result=17;
    CHECK(!compute::graphicsTargetFootprint(topology,largest,largest.total_bytes,largest.total_bytes,0,result));CHECK(result==17);
    const auto small=layout(1,1);const uint64_t lastPage=UINT64_MAX-4095;
    CHECK(!compute::graphicsTargetFootprint(topology,small,UINT64_MAX,lastPage,lastPage,result));CHECK(result==17);
    CHECK(!compute::graphicsTargetFootprint(topology,small,0,4096,0,result));CHECK(result==17);
}
}
int main(int argc,char** argv) {
    if(argc!=7)return 2;
    Bytes stages[6];for(unsigned n=0;n<6;++n){stages[n]=read(argv[n+1]);if(stages[n].empty())return 3;}
    for(unsigned v=0;v<3;++v)for(unsigned p=0;p<3;++p)snapshot(stages[v*2],stages[p*2+1]);
    rejected(stages[0],stages[1]);targets();
    std::printf("graphics snapshot: %u checks, %u failures; no native/GPU operation\n",checks,failures);
    return failures?1:0;
}
