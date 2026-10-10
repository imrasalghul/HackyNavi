// SPDX-License-Identifier: MIT
// REAL target-only test: --run submits sealed shaders through the native kext.
// Building this program never opens a GPU connection or performs the test.
#include "iokit_client.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace t=hackynavi::transport;
namespace a=hackynavi::nativeabi;
namespace {
bool checked(const char* operation,t::Result result) {
    if(result)return true;
    std::fprintf(stderr,"%s failed: status=%u IOReturn=0x%08x\n",operation,unsigned(result.status),unsigned(result.iokit));
    return false;
}
bool runCase(uint64_t registry,bool copy) {
    t::Client client;
    if(!checked("open built-in profile",client.openBuiltIn(registry)))return false;
    const bool passed=[&]() {
        a::HelloReply info{};if(!checked("information",client.info(info)))return false;
        std::printf("%s: registry=%llu epoch=%llu ABI=%u.%u capabilities=0x%llx\n",copy?"copy":"clear",
            static_cast<unsigned long long>(info.registryId),static_cast<unsigned long long>(info.header.epoch),
            info.header.major,info.header.minor,static_cast<unsigned long long>(info.capabilities));
        constexpr uint32_t bytes=4096;
        if(info.vaBegin>UINT64_MAX-2*bytes||info.vaEnd-info.vaBegin<2*bytes)return false;
        t::Buffer source{},destination{};
        if(!checked("allocate destination",client.allocate(bytes,4096,a::Domain::System,0,destination))||
            !checked("map destination",client.mapVa(destination,info.vaBegin,0,bytes,a::Read|a::Write)))return false;
        std::array<uint32_t,bytes/4> expected{},actual{};
        if(!checked("read zeroed allocation",client.read(destination,0,actual.data(),bytes)))return false;
        for(auto word:actual)if(word){std::fprintf(stderr,"allocation was not zeroed\n");return false;}
        const uint32_t pattern[4]={0x13579bdf,0x2468ace0,0xdeadbeef,0x01234567};
        for(size_t i=0;i<expected.size();++i)expected[i]=copy?uint32_t(0x53a719b1U+uint32_t(i)*0x10203U):pattern[i%4];
        actual.fill(0xa5a5a5a5);
        if(!checked("upload destination poison",client.write(destination,0,actual.data(),bytes)))return false;
        if(copy) {
            if(!checked("allocate source",client.allocate(bytes,4096,a::Domain::System,0,source))||
                !checked("map source",client.mapVa(source,info.vaBegin+bytes,0,bytes,a::Read))||
                !checked("upload source",client.write(source,0,expected.data(),bytes)))return false;
        }
        t::Fence fence{};
        if(!checked(copy?"GPU copy":"GPU clear",copy?client.copy(source,0,destination,0,bytes,fence):
            client.clear(destination,0,bytes,pattern,fence)))return false;
        if(!checked("GPU-written completion",client.wait(fence,a::kMaxWaitNs))||
            !checked("read captured GPU result",client.readResult(fence,0,actual.data(),bytes)))return false;
        for(size_t i=0;i<expected.size();++i)if(actual[i]!=expected[i]) {
            std::fprintf(stderr,"%s mismatch at word %zu: expected=%08x observed=%08x\n",copy?"copy":"clear",i,expected[i],actual[i]);
            return false;
        }
        std::printf("%s verified %zu GPU result words, sequence=%llu\n",copy?"copy":"clear",actual.size(),
            static_cast<unsigned long long>(fence.sequence));return true;
    }();
    const bool closed=checked("explicit native close",client.close());
    return passed&&closed;
}
}
int main(int argc,char** argv) {
    if(argc<2||argc>3||std::strcmp(argv[1],"--run")) {
        std::fprintf(stderr,"usage: %s --run [registry-entry-id]\nRuns real GPU copy and clear in separate sessions.\n",argv[0]);return 2;
    }
    uint64_t registry=0;
    if(argc==3){char* end=nullptr;errno=0;if(argv[2][0]=='-')return 2;
        registry=std::strtoull(argv[2],&end,0);if(errno||!registry||end==argv[2]||*end)return 2;}
    if(!runCase(registry,true)||!runCase(registry,false))return 1;
    std::puts("PASS: real native GPU copy and clear, full readback, completion and close verified.");return 0;
}
