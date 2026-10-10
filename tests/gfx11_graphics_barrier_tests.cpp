// SPDX-License-Identifier: MIT
// Pure command-byte validation. No queue, hardware or simulated GPU exists here.
#include "hackynavi/gfx11_graphics_barrier.hpp"
#include <cstdio>
#include <cstring>
namespace b=hackynavi::gfx11graphicsbarrier;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::fprintf(stderr,"%u: %s\n",__LINE__,#x);}}while(0)
// Golden sequence separately confirmed by the actual pinned Mesa ac_emit_barrier
// host oracle. Keep its report distinct from these portable boundary tests.
const uint32_t golden[]={0xc0064900,0x80703514,0,0,0,0,0,0,
    0xc0065800,0x22000,0xffffffff,0x01ffffff,0,0,0x80000000,0};
}
int main(int argc,char** argv) {
    CHECK(argc==1||argc==2);
    if(argc==2) {
        FILE* file=std::fopen(argv[1],"rb");CHECK(file!=nullptr);
        if(!file)return 1;
        uint32_t oracle[b::kDwords]={};
        CHECK(std::fread(oracle,1,sizeof(oracle),file)==sizeof(oracle));
        CHECK(std::fgetc(file)==EOF);CHECK(std::fclose(file)==0);
        CHECK(std::memcmp(oracle,golden,sizeof(golden))==0);
        CHECK(b::validate(oracle,b::kDwords)==b::Status::Ok);
    }
    uint32_t storage[b::kDwords+2];
    for(auto& word:storage)word=0xdeadbeef;
    CHECK(b::encode(storage+1,b::kDwords)==b::Status::Ok);
    CHECK(storage[0]==0xdeadbeef && storage[b::kDwords+1]==0xdeadbeef);
    CHECK(std::memcmp(storage+1,golden,sizeof(golden))==0);
    CHECK(b::validate(storage+1,b::kDwords)==b::Status::Ok);
    for(size_t word=0;word<b::kDwords;++word)for(unsigned bit=0;bit<32;++bit) {
        storage[word+1]^=uint32_t(1)<<bit;
        CHECK(b::validate(storage+1,b::kDwords)==b::Status::InvalidStream);
        storage[word+1]^=uint32_t(1)<<bit;
    }
    for(size_t length=0;length<b::kDwords;++length) {
        CHECK(b::validate(storage+1,length)==b::Status::InvalidStream);
        uint32_t before[b::kDwords+2];std::memcpy(before,storage,sizeof(storage));
        CHECK(b::encode(storage+1,length)==b::Status::BufferTooSmall);
        CHECK(std::memcmp(before,storage,sizeof(storage))==0);
    }
    CHECK(b::validate(storage+1,b::kDwords+1)==b::Status::InvalidStream);
    CHECK(b::validate(storage+1,SIZE_MAX)==b::Status::InvalidStream);
    CHECK(b::encode(storage+1,SIZE_MAX)==b::Status::Ok);
    CHECK(storage[0]==0xdeadbeef && storage[b::kDwords+1]==0xdeadbeef);
    CHECK(b::encode(nullptr,b::kDwords)==b::Status::InvalidArgument);
    CHECK(b::validate(nullptr,b::kDwords)==b::Status::InvalidArgument);
    unsigned char* bytes=reinterpret_cast<unsigned char*>(storage);
    for(size_t offset=1;offset<alignof(uint32_t);++offset) {
        CHECK(b::encode(reinterpret_cast<uint32_t*>(bytes+offset),b::kDwords)==b::Status::InvalidArgument);
        CHECK(b::validate(reinterpret_cast<uint32_t*>(bytes+offset),b::kDwords)==b::Status::InvalidArgument);
    }
    // An aligned span crossing the address-space end is rejected without access.
    auto* wrapping=reinterpret_cast<uint32_t*>(UINTPTR_MAX-sizeof(uint32_t)+1);
    CHECK(b::encode(wrapping,b::kDwords)==b::Status::InvalidArgument);
    CHECK(b::validate(wrapping,b::kDwords)==b::Status::InvalidArgument);
    const b::Status statuses[]={b::Status::Ok,b::Status::InvalidArgument,b::Status::BufferTooSmall,b::Status::InvalidStream};
    for(auto status:statuses)
        CHECK(std::strcmp(b::statusString(status),"Unknown")!=0);
    CHECK(std::strcmp(b::statusString(static_cast<b::Status>(99)),"Unknown")==0);
    std::printf("%u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
