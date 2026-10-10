// SPDX-License-Identifier: MIT
#include "hackynavi/gfx11_graphics_draw.hpp"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <array>
using namespace hackynavi;
using namespace hackynavi::gfx11draw;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){if(failures<12)printf("line %u: %s\n",unsigned(__LINE__),#x);++failures;}}while(0)
using Bytes=std::vector<uint8_t>;
using Words=std::array<uint32_t,kMaxCommandDwords>;
// Actual Apple32023 AIR -> real translation -> pinned ACO products; provenance
// and hashes in build/apple-graphics-integration-1. No GPU outcome is modeled.
const char* vertex="0300b5bf0388008403808295c10000930200febe1300a5bf080a0a4a010087bf810a847c010001d580e4a901020001d5"
 "f2e6a901030001d58000a901040001d5f2e4a901800a8a7c940187bff30252caf3040201800652caf2080403cf0800f8"
 "01020304f0ff89bf0000febe0200a5bf410900f8008080800000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf";
const char* fragment="f00010caff0000010000803ef20010caff0002030000403f0f0800f8000102030000b0bf00009fbf00009fbf00009fbf"
 "00009fbf00009fbf";
unsigned nibble(char c){return c<='9'?unsigned(c-'0'):unsigned(c-'a'+10);}
Bytes hex(const char* p){Bytes b;while(*p){b.push_back(uint8_t(nibble(p[0])*16+nibble(p[1])));p+=2;}return b;}
Bytes read(const char* path) {
    FILE* f=fopen(path,"rb");if(!f)return {};
    Bytes b;for(int c=fgetc(f);c!=EOF;c=fgetc(f)){if(b.size()>=1024){fclose(f);return {};}b.push_back(uint8_t(c));}
    if(fclose(f))return {};return b;
}
HNColorLayout color(uint32_t width,uint32_t height) {
    const uint32_t pitch=(width+63)&~63u;const uint64_t bytes=uint64_t(pitch)*4*height;
    return {width,height,pitch,pitch*4,bytes,bytes,256,0,0x2800a,0,0,(width-1)<<14|(height-1),0x1000000,0x428};
}
gfxisolated::Binding binding(uint32_t id,uint64_t va,uint64_t bytes,uint32_t access) {
    return {11,17,id,19,va,bytes,0,bytes,access};
}
struct Fixture {
    Bytes image;Request request{};
    Fixture(const Bytes& vs=hex(vertex),const Bytes& ps=hex(fragment),uint32_t width=3840,uint32_t height=2160,
            uint32_t se=6,uint32_t cu=16,uint32_t gb=0x545,uint32_t hi=1) {
        gfx11graphics::Layout l{};
        if(gfx11graphics::layout(vs.size(),ps.size(),l)!=gfx11graphics::Status::Ok)return;
        image.resize(l.imageBytes);memcpy(image.data(),vs.data(),vs.size());memcpy(image.data()+l.fragmentOffset,ps.data(),ps.size());
        request.owner=11;request.epoch=17;request.vmid=3;
        request.topology={{gb,0},se,se*4,cu};request.image={image.data(),image.size(),vs.size(),ps.size()};
        request.targetLayout=color(width,height);
        const uint64_t base=uint64_t(hi)<<32;
        request.code=binding(1,base+0x1000,4096,5);
        request.indirect=binding(2,base+0x2000,8192,5);
        request.attributes=binding(3,base+0x1000000,uint64_t(kAttributeBytesPerSe)*se,3);
        const uint64_t targetBytes=(request.targetLayout.total_bytes+4095)&~UINT64_C(4095);
        // Target deliberately need not share code's 4GiB address window.
        request.target=binding(4,0x400000000000ULL,targetBytes,3);
    }
};
gfxisolated::Binding& bound(Request& r,unsigned n){return n==0?r.code:n==1?r.target:n==2?r.attributes:r.indirect;}
void reject(const Request& r,Status expected) {
    Workspace w{};Words stream;stream.fill(0xa7a7a7a7);const Words prior=stream;size_t size=12345;
    CHECK(encode(r,w,stream.data(),stream.size(),size)==expected);
    CHECK(size==12345);CHECK(stream==prior);
}
uint32_t registerValue(const Words& stream,size_t words,uint32_t address) {
    uint32_t result=0xdeadbeef;
    for(size_t at=0;at<words;) {
        if(stream[at]==0xffff1000){++at;continue;}
        const uint32_t op=(stream[at]>>8)&255;const size_t count=((stream[at]>>16)&0x3fff)+1;
        if(at+count>=words)return 0xdeadbeef;
        if(op==0x69||op==0x76||op==0x79) {
            const uint32_t reg=(op==0x69?0x28000:op==0x76?0xb000:0x30000)+stream[at+1]*4;
            if(address>=reg&&(address-reg)/4<count-1)result=stream[at+2+(address-reg)/4];
        }
        at+=count+1;
    }
    return result;
}
void positives() {
    for(uint32_t dim:{1u,2u,3u,63u,64u,65u,127u,128u,129u,3840u,4099u,16383u,16384u}) {
        Fixture f(hex(vertex),hex(fragment),dim,16385-dim);
        Workspace w{};Words data;data.fill(0xcacacaca);size_t n=0;
        CHECK(encode(f.request,w,data.data(),data.size(),n)==Status::Ok);
        CHECK(n&&!(n&7)&&n<=kMaxCommandDwords);
        CHECK(validate(f.request,w,data.data(),n)==Status::Ok);
        float half=float(dim)*0.5f;uint32_t bits=0;memcpy(&bits,&half,4);
        CHECK(registerValue(data,n,0x2843c)==bits);
        CHECK(registerValue(data,n,0x28440)==bits);
        float halfHeight=float(16385-dim)*0.5f;uint32_t heightBits=0;memcpy(&heightBits,&halfHeight,4);
        CHECK(registerValue(data,n,0x28444)==(heightBits|0x80000000u));
        CHECK(registerValue(data,n,0x28448)==heightBits);
        CHECK(registerValue(data,n,0x2844c)==0x3f800000&&registerValue(data,n,0x28450)==0);
        // The actual Apple triangle's top vertex Y=+1 maps to row zero;
        // its two Y=-1 vertices map to the bottom. No shader flip is applied.
        float sy=0,ty=0;uint32_t syBits=registerValue(data,n,0x28444),tyBits=registerValue(data,n,0x28448);
        memcpy(&sy,&syBits,4);memcpy(&ty,&tyBits,4);
        CHECK(sy+ty==0&&-sy+ty==float(16385-dim));
        CHECK(registerValue(data,n,0x28254)==(dim|((16385-dim)<<16)));
        CHECK(registerValue(data,n,0xb230)==0);CHECK(registerValue(data,n,0x28c70)==0x2800a);
        CHECK(registerValue(data,n,0x3096c)==0x1f810080);
        CHECK(registerValue(data,n,0x3111c)==0x20015);
        CHECK(registerValue(data,n,0x28c60)==uint32_t(f.request.target.gpu>>8));
        CHECK(registerValue(data,n,0x28e40)==uint32_t(f.request.target.gpu>>40));
        CHECK(registerValue(data,n,0x28c6c)==f.request.targetLayout.cb_color_view);
        CHECK(registerValue(data,n,0x28c74)==f.request.targetLayout.cb_color_attrib);
        CHECK(registerValue(data,n,0x28c78)==f.request.targetLayout.cb_dcc_control);
        CHECK(registerValue(data,n,0x28c94)==0&&registerValue(data,n,0x28ea0)==0);
        CHECK(registerValue(data,n,0x28ec0)==f.request.targetLayout.cb_color_attrib2);
        CHECK(registerValue(data,n,0x28ee0)==f.request.targetLayout.cb_color_attrib3);
        for(uint32_t absent:{0x28c64u,0x28c68u,0x28c7cu,0x28c80u,0x28c84u,0x28c88u,0x28e60u,0x28e80u})
            CHECK(registerValue(data,n,absent)==0xdeadbeef);
        CHECK(w.vertexPrefetch==3&&w.fragmentPrefetch==2);
        for(size_t i=n;i<data.size();++i)CHECK(data[i]==0xcacacaca);
    }
    // Exhaust both signed-Y and positive-X integer-derived float conversions.
    for(uint32_t d=1;d<=16384;++d) {
        Fixture f(hex(vertex),hex(fragment),d,d);Workspace w{};Words data{};size_t n=0;
        CHECK(encode(f.request,w,data.data(),data.size(),n)==Status::Ok);
        float half=float(d)*0.5f;uint32_t bits=0;memcpy(&bits,&half,4);
        CHECK(registerValue(data,n,0x2843c)==bits);
        CHECK(registerValue(data,n,0x28444)==(bits|0x80000000u));
        CHECK(registerValue(data,n,0x28448)==bits);
    }
    for(uint32_t se:{1u,2u,4u,6u})for(uint32_t cu:{2u,4u,8u,12u,16u})for(uint32_t hi:{0u,1u,256u,32767u}) {
        Fixture f(hex(vertex),hex(fragment),64,64,se,cu,0x141,hi);Workspace w{};Words data{};size_t n=0;
        CHECK(encode(f.request,w,data.data(),data.size(),n)==Status::Ok);
        CHECK(validate(f.request,w,data.data(),n)==Status::Ok);
        CHECK(w.address32Hi==hi&&w.psCuMask==(1u<<cu)-1);
        CHECK(registerValue(data,n,0xb324)==hi>>8);
        CHECK(registerValue(data,n,0xb21c)==(0x3f0000|(cu==2?0xffff:0xfffd)));
        CHECK(registerValue(data,n,0x30980)==(cu==2?0x7fe:0x1ff));
    }
}
void negatives() {
    Fixture f;Request r=f.request;
    r.owner=0;reject(r,Status::InvalidArgument);r=f.request;r.epoch=0;reject(r,Status::InvalidArgument);
    for(unsigned vm:{0u,8u,255u}){r=f.request;r.vmid=uint8_t(vm);reject(r,Status::InvalidArgument);}
    for(unsigned value:{0u,7u,UINT32_MAX}){r=f.request;r.topology.maxShaderEngines=value;reject(r,Status::UnsupportedTopology);}
    for(unsigned value:{0u,2u,23u,25u}){r=f.request;r.topology.maxRenderBackends=value;reject(r,Status::UnsupportedTopology);}
    for(unsigned value:{0u,1u,3u,17u,32u}){r=f.request;r.topology.minGoodCuPerSa=value;reject(r,Status::UnsupportedTopology);}
    for(unsigned i=0;i<4;++i) {
        r=f.request;bound(r,i).owner++;reject(r,Status::ForeignContext);
        r=f.request;bound(r,i).epoch++;reject(r,Status::ForeignContext);
        r=f.request;bound(r,i).generation=0;reject(r,Status::InvalidBinding);
        r=f.request;bound(r,i).id=0;reject(r,Status::InvalidBinding);
        r=f.request;bound(r,i).gpu++;reject(r,Status::InvalidBinding);
        r=f.request;bound(r,i).gpu=UINT64_C(1)<<47;reject(r,Status::InvalidBinding);
        r=f.request;bound(r,i).bytes--;reject(r,Status::InvalidBinding);
        r=f.request;bound(r,i).bufferOffset=4096;reject(r,Status::InvalidBinding);
        for(unsigned access=0;access<8;++access)if(access!=bound(f.request,i).access) {
            r=f.request;bound(r,i).access=access;reject(r,Status::Permission);
        }
        for(unsigned j=0;j<i;++j) {
            r=f.request;bound(r,i).id=bound(r,j).id;reject(r,Status::AliasedStorage);
            r=f.request;bound(r,i).gpu=bound(r,j).gpu;
            Workspace scratch{};CHECK(prepare(r,scratch)!=Status::Ok);
        }
    }
    r=f.request;r.attributes.gpu+=4096;reject(r,Status::InvalidBinding);
    r=f.request;r.attributes.gpu+=UINT64_C(1)<<32;reject(r,Status::InvalidBinding);
    r=f.request;r.attributes.gpu=0x1ffff0000;reject(r,Status::InvalidBinding);
    for(size_t bit=0;bit<sizeof(HNColorLayout)*8;++bit) {
        r=f.request;reinterpret_cast<uint8_t*>(&r.targetLayout)[bit/8]^=uint8_t(1u<<(bit%8));
        reject(r,Status::InvalidTarget);
    }
    r=f.request;r.target.bytes=r.target.allocationBytes=4096;reject(r,Status::InvalidTarget);
    r=f.request;r.image.vertexBytes-=4;reject(r,Status::InvalidShader);
    r=f.request;r.image.fragmentBytes-=4;reject(r,Status::InvalidShader);
    r=f.request;r.image.imageBytes-=1;reject(r,Status::InvalidShader);
    f.image[0]^=1;reject(f.request,Status::InvalidShader);f.image[0]^=1;
    Workspace w{};Words canonical{};size_t n=0;
    CHECK(encode(f.request,w,canonical.data(),canonical.size(),n)==Status::Ok);
    for(size_t i=0;i<n;++i)for(unsigned bit=0;bit<32;++bit) {
        Words changed=canonical;changed[i]^=1u<<bit;
        CHECK(validate(f.request,w,changed.data(),n)==Status::InvalidStream);
    }
    for(size_t len=1;len<n;++len)CHECK(validate(f.request,w,canonical.data(),len)==Status::InvalidStream);
    for(size_t cap=0;cap<n;++cap) {
        Words data;data.fill(0xabababab);const Words before=data;size_t count=765;
        CHECK(encode(f.request,w,data.data(),cap,count)==Status::BufferTooSmall);CHECK(data==before);CHECK(count==765);
    }
    // CPU publication aliases reject before changing code, request, count or workspace.
    const Bytes imageBefore=f.image;const Workspace workspaceBefore=w;
    CHECK(encode(f.request,w,reinterpret_cast<uint32_t*>(f.image.data()),kMaxCommandDwords,n)==Status::InvalidArgument);
    CHECK(f.image==imageBefore&&memcmp(&w,&workspaceBefore,sizeof(w))==0);
    CHECK(encode(f.request,w,canonical.data(),canonical.size(),w.commandWords)==Status::InvalidArgument);
    CHECK(memcmp(&w,&workspaceBefore,sizeof(w))==0);
    alignas(Workspace) uint8_t space[sizeof(Request)+sizeof(Workspace)]={};
    auto* alias=reinterpret_cast<Workspace*>(space);auto* req=reinterpret_cast<Request*>(space);*req=f.request;
    std::array<uint8_t,sizeof(space)> prior{};memcpy(prior.data(),space,sizeof(space));
    CHECK(prepare(*req,*alias)==Status::InvalidArgument);CHECK(memcmp(prior.data(),space,sizeof(space))==0);
}
} // namespace
int main(int argc,char** argv) {
    if(argc==12&&!strcmp(argv[1],"--emit")) {
        Fixture f(read(argv[10]),read(argv[11]),uint32_t(strtoul(argv[8],nullptr,0)),uint32_t(strtoul(argv[9],nullptr,0)),
            uint32_t(strtoul(argv[3],nullptr,0)),uint32_t(strtoul(argv[4],nullptr,0)),uint32_t(strtoul(argv[5],nullptr,0)),uint32_t(strtoul(argv[6],nullptr,0)));
        if(strcmp(argv[7],"actual"))return 2;
        Workspace w{};Words data{};size_t n=0;if(encode(f.request,w,data.data(),data.size(),n)!=Status::Ok)return 3;
        FILE* file=fopen(argv[2],"wb");if(!file)return 4;
        const bool written=fwrite(data.data(),4,n,file)==n;const int closed=fclose(file);if(!written||closed)return 5;
        printf("{\"words\":%zu,\"vertex_executable\":%u,\"fragment_executable\":%u}\n",n,w.program.vertex.executableBytes,w.program.fragment.executableBytes);
        return 0;
    }
    if(argc!=1)return 2;
    positives();negatives();printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
