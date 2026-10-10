// SPDX-License-Identifier: MIT
// Target-only actual Apple AIR->SPIR-V->ACO execution; --run is mandatory.
// Builds and ordinary invocation do not open a GPU connection.
#include "iokit_client.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace t=hackynavi::transport;
namespace a=hackynavi::nativeabi;
namespace {
constexpr uint32_t kAllocationBytes=20480;
using Allocation=std::array<uint32_t,kAllocationBytes/4>;
using ResultWords=std::array<uint32_t,a::kCompilerResultBytes/4>;
bool checked(const char* operation,t::Result result) {
    if(result)return true;
    std::fprintf(stderr,"%s failed: status=%u IOReturn=0x%08x\n",operation,unsigned(result.status),unsigned(result.iokit));
    return false;
}
uint32_t inputWord(uint32_t n) {
    // Exercise uint overflow and both sides of the signed boundary along with
    // an index-dependent pattern. These values are inputs, never GPU results.
    switch(n%16) {
    case 0:return 0;
    case 1:return UINT32_MAX;
    case 2:return UINT32_MAX-1;
    case 3:return 0x7fffffff;
    case 4:return 0x80000000;
    default:return 0x137ad9bfU+n*0x9e3779b9U;
    }
}
uint32_t guardWord(uint32_t n){return 0xf17e0000U^(n*0x1020305U);}
bool zeroed(t::Client& client,const t::Buffer& buffer,const char* label) {
    Allocation data;data.fill(0xffffffff);
    if(!checked(label,client.read(buffer,0,data.data(),kAllocationBytes)))return false;
    for(size_t n=0;n<data.size();++n)if(data[n]) {
        std::fprintf(stderr,"%s: allocation word %zu was not zeroed (%08x)\n",label,n,data[n]);return false;
    }
    return true;
}
bool run(uint64_t registry) {
    t::Client client;
    if(!checked("open compiler v1.2 profile",client.openCompiler(registry)))return false;
    const bool passed=[&]() {
        a::HelloReply info{};if(!checked("information",client.info(info)))return false;
        std::printf("AppleAddOneGfx1100V1: registry=%llu epoch=%llu ABI=%u.%u capabilities=0x%llx\n",
            static_cast<unsigned long long>(info.registryId),static_cast<unsigned long long>(info.header.epoch),
            info.header.major,info.header.minor,static_cast<unsigned long long>(info.capabilities));
        if(info.pageBytes!=4096||info.vaBegin>UINT64_MAX-2*kAllocationBytes||
            info.vaEnd<=info.vaBegin||info.vaEnd-info.vaBegin<2*kAllocationBytes) {
            std::fprintf(stderr,"target cannot provide two isolated20KiB mappings\n");return false;
        }
        t::Buffer source{},destination{};
        if(!checked("allocate source",client.allocate(kAllocationBytes,4096,a::Domain::System,0,source))||
            !checked("allocate destination",client.allocate(kAllocationBytes,4096,a::Domain::System,0,destination))||
            !zeroed(client,source,"read zeroed source")||!zeroed(client,destination,"read zeroed destination")||
            !checked("map source read-only",client.mapVa(source,info.vaBegin,0,kAllocationBytes,a::Read))||
            !checked("map destination read/write",client.mapVa(destination,info.vaBegin+kAllocationBytes,0,
                kAllocationBytes,a::Read|a::Write)))return false;
        Allocation input{},poison{};
        for(uint32_t n=0;n<a::kCompilerElements;++n) {
            input[n]=inputWord(n);
            // Poison is guaranteed different from the expected result. Only
            // the subsequent immutable kernel GPU readback may pass the test.
            poison[n]=~(input[n]+1U);
        }
        for(uint32_t n=0;n<a::kCompilerGuardWords;++n) {
            input[a::kCompilerElements+n]=~guardWord(n);
            poison[a::kCompilerElements+n]=guardWord(n);
        }
        if(!checked("upload source",client.write(source,0,input.data(),kAllocationBytes))||
            !checked("upload destination poison and guards",client.write(destination,0,poison.data(),kAllocationBytes)))return false;
        t::Fence fence{};
        if(!checked("submit original ACO add_one",client.compilerAddOne(source,0,destination,0,fence))||
            !checked("GPU-written completion",client.wait(fence,a::kMaxWaitNs)))return false;
        ResultWords actual;actual.fill(0xacabdead);
        if(!checked("read immutable GPU result and guards",client.readResult(fence,0,actual.data(),a::kCompilerResultBytes)))return false;
        for(uint32_t n=0;n<a::kCompilerElements;++n) {
            const uint32_t expected=input[n]+1U;
            if(actual[n]!=expected) {
                std::fprintf(stderr,"add_one mismatch at word %u: input=%08x expected=%08x observed=%08x\n",
                    n,input[n],expected,actual[n]);return false;
            }
        }
        for(uint32_t n=0;n<a::kCompilerGuardWords;++n)if(actual[a::kCompilerElements+n]!=guardWord(n)) {
            std::fprintf(stderr,"guard %u modified: expected=%08x observed=%08x\n",n,guardWord(n),actual[a::kCompilerElements+n]);return false;
        }
        std::printf("Verified %u GPU add_one values and %u untouched guard words, readback=%u bytes, sequence=%llu\n",
            a::kCompilerElements,a::kCompilerGuardWords,a::kCompilerResultBytes,
            static_cast<unsigned long long>(fence.sequence));return true;
    }();
    // Explicit native close is checked on failures too. Never infer retirement
    // from IOServiceClose's ordinary success or from process exit.
    const bool closed=checked("explicit native close",client.close());
    return passed&&closed;
}
}
int main(int argc,char** argv) {
    if(argc<2||argc>3||std::strcmp(argv[1],"--run")) {
        std::fprintf(stderr,"usage: %s --run [registry-entry-id]\nRuns the pinned Apple add_one ACO shader on the Navi31 target.\n",argv[0]);return 2;
    }
    uint64_t registry=0;
    if(argc==3){char* end=nullptr;errno=0;if(argv[2][0]=='-')return 2;
        registry=std::strtoull(argv[2],&end,0);if(errno||!registry||end==argv[2]||*end)return 2;}
    if(!run(registry))return 1;
    std::puts("PASS: real native Apple add_one ACO execution, complete readback, guards and close verified.");return 0;
}
