// SPDX-License-Identifier: MIT
#include "hackynavi/gfx11_graphics_product.hpp"
#include <stdio.h>
#include <string.h>
#include <vector>
#include <array>
using namespace hackynavi::gfx11graphics;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("line %u: %s\n", unsigned(__LINE__), #x); } } while (0)
using Bytes = std::vector<uint8_t>;
// Actual immutable compiler products, not constructed execution results.
// Apple toolchain 32023 / MSL2.4 macOS15 provenance is in
// userspace/compiler/fixtures/apple-graphics/provenance.json. Remaining inputs
// are the authored SPIR-V products in the two frozen compiler test reports.
// build/apple-graphics-integration-1/air-vertex/product/shader.bin SHA256 ffa5d5142a6f8a5786296ba27f92e631c236bfd35161279b28919dd8997c9c61
const char* apple_vertex =
    "0300b5bf0388008403808295c10000930200febe1300a5bf080a0a4a010087bf810a847c010001d580e4a901020001d5"
    "f2e6a901030001d58000a901040001d5f2e4a901800a8a7c940187bff30252caf3040201800652caf2080403cf0800f8"
    "01020304f0ff89bf0000febe0200a5bf410900f8008080800000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf";
// build/apple-graphics-integration-1/air-fragment/product/shader.bin SHA256 15f6f976c7b77857915d8254f55d028c8413bcf74b96f8403767f1d90631f36c
const char* apple_fragment =
    "f00010caff0000010000803ef20010caff0002030000403f0f0800f8000102030000b0bf00009fbf00009fbf00009fbf"
    "00009fbf00009fbf";
// build/gfx11-ngg-tests-3/half/product/shader.bin SHA256 980461f5a3ec19648acf1fd2a71c36fed5f172e0b4370b5c4c4edfe5a6bcf2f0
const char* ngg_half =
    "0300b5bf0388008403808295c10000930200febe1400a5bf080a0a4a010087bf800a8a7c010042d4810a0202010001d5"
    "80e00500020001d5f0e20500030001d580000500040001d5f0e00500130187bff10252caf1040201800652caf0080403"
    "cf0800f801020304f0ff89bf0000febe0200a5bf410900f8008080800000b0bf00009fbf00009fbf00009fbf00009fbf"
    "00009fbf";
// build/gfx11-ngg-tests-3/literal/product/shader.bin SHA256 69619a05dddf65ae1d50c406cf0a395b32c28e92e02eafa2122a29c99ef02b05
const char* ngg_literal =
    "0300b5bf0388008403808295c10000930200febe1a00a5bf080a0a4aff02027e0000803e020087bf800a8a7c010042d4"
    "810a0202020001d580020600040001d580000500050001d5fffe05000000803e030001d5fffe05400000803e940187bf"
    "ff040402000080be010b52ca80080401ff060602000080becf0800f802030401f0ff89bf0000febe0200a5bf410900f8"
    "008080800000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf";
// build/gfx11-ngg-tests-3/reverse/product/shader.bin SHA256 c969de47af807b6c89ce36abb60514fc3b81bb4ef54ed36ebb4246d73eded247
const char* ngg_reverse =
    "0300b5bf0388008403808295c10000930200febe1400a5bf080a0a4a010087bf800a8a7c010042d4810a0202010001d5"
    "80e60500020001d5f3e40500030001d580000500040001d5f3e60500130187bff20252caf2040201800652caf3080403"
    "cf0800f801020304f0ff89bf0000febe0200a5bf410900f8008080800000b0bf00009fbf00009fbf00009fbf00009fbf"
    "00009fbf";
// build/gfx11-ngg-tests-3/triangle/product/shader.bin SHA256 4bc7b0dd82a377306241128a9651d041b8825eed9caca175f7ff86b40407d375
const char* ngg_triangle =
    "0300b5bf0388008403808295c10000930200febe1400a5bf080a0a4a010087bf800a8a7c010042d4810a0202010001d5"
    "80e40500020001d5f2e60500030001d580000500040001d5f2e40500130187bff30252caf3040201800652caf2080403"
    "cf0800f801020304f0ff89bf0000febe0200a5bf410900f8008080800000b0bf00009fbf00009fbf00009fbf00009fbf"
    "00009fbf";
// build/gfx11-fragment-tests-4/distinct/product/shader.bin SHA256 b6fe43ba2c760277b06bdcfccf29128446e175167574958ba070e9e7913f771c
const char* ps_distinct =
    "f00010caf2000001f60010caf40002030f0800f8000102030000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf";
// build/gfx11-fragment-tests-4/half/product/shader.bin SHA256 be6733d4607dd5517aa33c0403e07bd032025ae3a3b16f1819c55716de45bd91
const char* ps_half =
    "f002007e0f0800f8000000000000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf";
// build/gfx11-fragment-tests-4/literal/product/shader.bin SHA256 340aea59bb687732a3e16305fd6a61b169838114630137bdfc883f6054b0d9e8
const char* ps_literal =
    "ff02007e0000803e0f0800f8000000000000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf";
// build/gfx11-fragment-tests-4/mixed_literals/product/shader.bin SHA256 da7672137e3e1b8f5b571b92cb6d3f8850c40a76e31eef5009c6de6df150e1bf
const char* ps_mixed_literals =
    "ff02007e0000003eff02027e0000803ef20010caff0002030000403f0f0800f8000102030000b0bf00009fbf00009fbf"
    "00009fbf00009fbf00009fbf";
// build/gfx11-fragment-tests-4/signed/product/shader.bin SHA256 04d7664c2296b5ab7a81df731e084db81034ca3947c563d9b41b4c132c5ed595
const char* ps_signed =
    "f10010caf3000001f70010caf50002030f0800f8000102030000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf";
// build/gfx11-fragment-tests-4/white/product/shader.bin SHA256 438cb1ccc9b5dd01bd0b3792f6d358eb7e510e8fa0244e9908f56721c9e2e8da
const char* ps_white =
    "f202007e0f0800f8000000000000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf";
// build/gfx11-fragment-tests-4/zero/product/shader.bin SHA256 d6f23129e18360146f2a2dfcc45ea2a5ccbbbe8a23d659e982aa4dda17e918bf
const char* ps_zero =
    "8002007e0f0800f8000000000000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf";

unsigned nibble(char c) { return c <= '9' ? unsigned(c - '0') : unsigned(c - 'a' + 10); }
Bytes hex(const char* p) {
    Bytes bytes;
    while (*p) { bytes.push_back(uint8_t(nibble(p[0])*16 + nibble(p[1]))); p += 2; }
    return bytes;
}
void put(Bytes& bytes, size_t offset, uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) bytes[offset + n] = uint8_t(value >> (n*8));
}
uint32_t get(const Bytes& bytes, size_t offset) {
    return uint32_t(bytes[offset]) | uint32_t(bytes[offset+1]) << 8 |
        uint32_t(bytes[offset+2]) << 16 | uint32_t(bytes[offset+3]) << 24;
}
void append(Bytes& bytes, uint32_t value) { const size_t n = bytes.size(); bytes.resize(n+4); put(bytes,n,value); }
struct Product {
    Bytes bytes;
    size_t vb, fb, ps;
    Product(const Bytes& vertex, const Bytes& fragment) : vb(vertex.size()), fb(fragment.size()), ps(0) {
        Layout l{}; CHECK(layout(vb,fb,l) == Status::Ok); ps = l.fragmentOffset;
        bytes.resize(l.imageBytes); memcpy(bytes.data(),vertex.data(),vb); memcpy(bytes.data()+ps,fragment.data(),fb);
    }
    Request request() const { return {bytes.data(),bytes.size(),vb,fb}; }
    bool accepted(Report& report) const { return validate(request(),report) == Status::Ok; }
};
bool cleared(const Report& report) {
    Report expected{}; expected.status=report.status; expected.stage=report.stage;
    expected.byteOffset=report.byteOffset; expected.instructionWord=report.instructionWord;
    return !memcmp(&report.image,&expected.image,sizeof(Report)-offsetof(Report,image));
}
void reject(const Product& product, Status expected = Status::Ok) {
    Report report{}; memset(&report,0xa5,sizeof(report));
    const Status status=validate(product.request(),report);
    CHECK(status != Status::Ok); if (expected != Status::Ok) CHECK(status == expected);
    CHECK(report.status == status); CHECK(cleared(report));
}
Bytes vertexBody(const Bytes& body, uint32_t exportOperands=0x01010101) {
    Bytes bytes;
    for (uint32_t value : {0xbfb50003u,0x84008803u,0x95828003u,0x930000c1u,0xbefe0002u}) append(bytes,value);
    append(bytes,0xbfa50000u | uint32_t(body.size()/4+2));
    bytes.insert(bytes.end(),body.begin(),body.end());
    append(bytes,0xf80008cf); append(bytes,exportOperands);
    for (uint32_t value : {0xbf89fff0u,0xbefe0000u,0xbfa50002u,0xf8000941u,0x80808000u,0xbfb00000u}) append(bytes,value);
    for (unsigned i=0;i<5;++i) append(bytes,0xbf9f0000);
    return bytes;
}
Bytes psBody(const Bytes& body, uint32_t exportOperands=0) {
    Bytes bytes=body; append(bytes,0xf800080f);append(bytes,exportOperands);append(bytes,0xbfb00000);
    for(unsigned i=0;i<5;++i) append(bytes,0xbf9f0000);
    return bytes;
}
Bytes words(std::initializer_list<uint32_t> input) { Bytes bytes; for(auto w:input) append(bytes,w); return bytes; }
void actualProducts() {
    const char* vs[]={apple_vertex,ngg_triangle,ngg_half,ngg_literal,ngg_reverse};
    const char* fs[]={apple_fragment,ps_distinct,ps_half,ps_literal,ps_mixed_literals,ps_signed,ps_white,ps_zero};
    const uint32_t colors[][4]={
        {0x3e800000,0x3f000000,0x3f400000,0x3f800000},
        {0x3f800000,0x3f000000,0x40000000,0x40800000},
        {0x3f000000,0x3f000000,0x3f000000,0x3f000000},
        {0x3e800000,0x3e800000,0x3e800000,0x3e800000},
        {0x3e000000,0x3e800000,0x3f400000,0x3f800000},
        {0xbf800000,0xbf000000,0xc0000000,0xc0800000},
        {0x3f800000,0x3f800000,0x3f800000,0x3f800000}, {0,0,0,0}};
    for(auto vertex:vs) for(unsigned f=0;f<8;++f) {
        Product p(hex(vertex),hex(fs[f])); const Bytes before=p.bytes;
        Report r{}; CHECK(p.accepted(r)); CHECK(p.bytes==before);
        CHECK(r.vertex.rsrc1==2883586 && r.fragment.rsrc1==2883586);
        CHECK(r.vertex.rsrc2==2 && r.fragment.rsrc2==0);
        CHECK(r.vertex.allocatedVgprs==24 && r.fragment.allocatedVgprs==24);
        CHECK(r.vertex.hardwareSgprs==108 && r.fragment.hardwareSgprs==108);
        CHECK(r.vertex.waveSize==32 && r.fragment.waveSize==32);
        CHECK(r.vertex.executableBytes==p.vb-20 && r.fragment.executableBytes==p.fb-20);
        CHECK(r.vgtShaderStagesEn==104931344 && r.spiShaderPosFormat==4);
        CHECK(r.spiPsInputEna==128 && r.spiPsInputAddr==128 && r.spiPsInControl==32768);
        CHECK(r.spiShaderColFormat==9 && r.cbShaderMask==15 && r.spiShaderZFormat==0);
        CHECK(!memcmp(r.fragmentColorBits,colors[f],sizeof(colors[f])));
    }
    Product apple(hex(apple_vertex),hex(apple_fragment));Report r{};
    CHECK(apple.accepted(r));CHECK(r.vertex.instructionCount==23 && r.fragment.instructionCount==4);
    CHECK(r.vertex.requiredVgprs==6 && r.fragment.requiredVgprs==4);
}
void controlAndLayout() {
    const Product original(hex(apple_vertex),hex(apple_fragment));
    // Every bit of the mandatory prefix/branch and post-POS control skeleton.
    for(size_t offset=0;offset<24;offset+=4) for(unsigned bit=0;bit<32;++bit) {
        Product p=original;put(p.bytes,offset,get(p.bytes,offset)^(1u<<bit));reject(p);
    }
    for(size_t offset=original.vb-44;offset<original.vb-20;offset+=4)
        for(unsigned bit=0;bit<32;++bit) {
            Product p=original;put(p.bytes,offset,get(p.bytes,offset)^(1u<<bit));reject(p);
        }
    for(size_t offset : {original.vb-52,original.ps+original.fb-32})
        for(unsigned bit=0;bit<32;++bit) {
            Product p=original;put(p.bytes,offset,get(p.bytes,offset)^(1u<<bit));reject(p);
        }
    // Every padding byte is covered, including all five code-end words.
    for(size_t i=0;i<original.bytes.size();++i) {
        const bool padding=(i>=original.vb-20 && i<original.ps) || i>=original.ps+original.fb-20;
        if(!padding) continue;
        Product p=original;p.bytes[i]^=1;reject(p,Status::InvalidPadding);
    }
    Layout output{1,2,3,4};const Layout before=output;
    for(size_t v : {size_t(0),size_t(75),size_t(77),size_t(1025),SIZE_MAX}) {
        CHECK(layout(v,56,output)==Status::InvalidLayout);CHECK(!memcmp(&output,&before,sizeof(output)));
    }
    for(size_t f : {size_t(0),size_t(31),size_t(33),size_t(1025),SIZE_MAX}) {
        CHECK(layout(144,f,output)==Status::InvalidLayout);CHECK(!memcmp(&output,&before,sizeof(output)));
    }
    CHECK(layout(1024,1024,output)==Status::Ok && output.fragmentOffset==1024 && output.imageBytes==4096);
    for(size_t n=0;n<4096;++n) {
        Request q=original.request();q.imageBytes=n;Report r{};
        CHECK(validate(q,r)!=Status::Ok);CHECK(cleared(r));
    }
    Request q=original.request();q.image=nullptr;Report r{};
    CHECK(validate(q,r)==Status::InvalidArgument);CHECK(cleared(r));
    q=original.request();q.image=reinterpret_cast<const uint8_t*>(UINTPTR_MAX-3);
    CHECK(validate(q,r)==Status::InvalidArgument);CHECK(cleared(r));
    // Output aliasing is refused without changing any code bytes.
    alignas(Report) std::array<uint8_t,4096> aligned{};
    memcpy(aligned.data(),original.bytes.data(),aligned.size());const auto saved=aligned;
    q=original.request();q.image=aligned.data();
    CHECK(validate(q,*reinterpret_cast<Report*>(aligned.data()+256))==Status::InvalidArgument);
    CHECK(aligned==saved);
}
void dataflowAndEncodings() {
    const Bytes appleV=hex(apple_vertex),appleF=hex(apple_fragment);
    // All source selectors in an otherwise valid single MOV PS. No unproven
    // system VGPR, SGPR, EXEC/VCC or extended DPP/SDWA selector is accepted.
    for(unsigned src=0;src<512;++src) {
        Product p(appleV,psBody(words({0x7e000200u|src})));
        Report r{};const bool expected=(src>=128 && src<=208)||(src>=240 && src<=248);
        CHECK(p.accepted(r)==expected);if(!expected) CHECK(cleared(r));
    }
    for(unsigned reg=0;reg<256;++reg) {
        const uint32_t operands=reg*0x01010101u;
        Product p(appleV,psBody(words({0x7e0002f2u|(reg<<17)}),operands));Report r{};
        CHECK(p.accepted(r)==(reg<24));
        if(reg<24) {CHECK(r.fragment.requiredVgprs==reg+1);CHECK(r.fragmentColorBits[0]==0x3f800000);}
    }
    // Exact constant bits, including NaNs/negative zero, are bit-preserved;
    // the admission layer is not a software shader evaluator or rasterizer.
    uint32_t random=0x84ee7b21;
    for(unsigned n=0;n<4096;++n) {
        random^=random<<13;random^=random>>17;random^=random<<5;
        Product p(appleV,psBody(words({0x7e0002ff,random})));Report r{};
        CHECK(p.accepted(r));for(auto c:r.fragmentColorBits) CHECK(c==random);
    }
    // Vertex-only initial values: v5 is available, v0 and other system VGPRs
    // are not. Destination v0 is protected for the later primitive region.
    for(unsigned reg=0;reg<24;++reg) {
        Product p(vertexBody(words({0x7e020300u|reg})),appleF);Report r{};
        CHECK(p.accepted(r)==(reg==5));
        Product d(vertexBody(words({0x7e0002f2u|(reg<<17)}),reg*0x01010101u),appleF);
        CHECK(d.accepted(r)==(reg!=0));
    }
    // Only two compare-generated mask destinations are allowed. Every mask
    // use must be dominated by its compare inside the actual vertex region.
    for(unsigned dst=0;dst<256;++dst) {
        const Bytes b=words({0xd4420000u|dst,0x02020a81u,0xd5010001u,0x0001e480u|(dst<<18)});
        Product p(vertexBody(b),appleF);Report r{};
        CHECK(p.accepted(r)==(dst==1 || dst==106));
    }
    for(unsigned mask : {1u,106u}) {
        Product p(vertexBody(words({0xd5010001u,0x0001e480u|(mask<<18)})),appleF);
        reject(p,Status::UninitializedRegister);
    }
    // Reads of mask registers as ordinary data are excluded even after a
    // compare; system mask/EXEC mutations and arbitrary branches are excluded.
    for(uint32_t bad : {0xbefe0001u,0xbe800008u,0xbfa50000u,0xbfa0ffffu,0xbfb00000u,
        0xf4000000u,0xe0000000u,0xd8000000u,0xdc000000u,0xf800080fu}) {
        Product p(vertexBody(words({bad,0x7e0202f2u})),appleF);reject(p);
    }
    Product scalar(vertexBody(words({0xd4420001,0x02020a81,0x7e020201})),appleF);reject(scalar);
    Product maskAlias(vertexBody(words({0xd4420000,0x02020a81,0x7e0202f2})),appleF);reject(maskAlias);
    // A MOV pair cannot initialize one half by publishing the other first.
    Product paired(appleV,psBody(words({0xca1000f2,0x01000101}),0x01000100));
    reject(paired,Status::UninitializedRegister);
    // VOPD source-port same-parity and use of a literal with implicit VCC.
    Product bank(vertexBody(words({0x7e0202f2,0x7e0602f2,0xca100101,0x02020103})),appleF);reject(bank,Status::InvalidOperand);
    Product bus(vertexBody(words({0x7c840a81,0xca5202ff,0x010204f2,0x3e800000})),appleF);reject(bus);
    // Literal words cannot extend across the fixed POS/PS-export boundary.
    Product truncated(vertexBody(words({0x7e0202ff})),appleF);reject(truncated,Status::Truncated);
    Product vop3(vertexBody(words({0xd5010001})),appleF);reject(vop3,Status::Truncated);
    Product frag(appleV,psBody(words({0xca1000ff,0x010000f2})));reject(frag,Status::Truncated);
    // Scalar compare side effects cannot corrupt the preserved primitive mask,
    // even if their result appears unused on the currently visible path.
    for(unsigned target : {0u,2u,3u,8u,126u,127u}) {
        Product p(vertexBody(words({0xd4420000u|target,0x02020a81,0x7e0202f2})),appleF);reject(p);
    }
    for(unsigned bit=8;bit<16;++bit) {
        Product p(vertexBody(words({0xd5010001u|(1u<<bit),0x01a9e480})),appleF);reject(p);
    }
    // Accepted source programs can have arbitrary additional straight-line
    // MOVs up to the byte bound; acceptance is not keyed to fixture hashes.
    for(unsigned count=1;count<=237;++count) {
        Bytes body;for(unsigned n=0;n<count;++n) append(body,0x7e0202f2);
        Product p(vertexBody(body),appleF);Report r{};
        CHECK(p.accepted(r));CHECK(r.vertex.instructionCount==count+12);
    }
    Bytes maximumPs;for(unsigned i=0;i<248;++i) append(maximumPs,0x7e0002f2);
    Product max(appleV,psBody(maximumPs));Report r{};
    CHECK(max.fb==kMaxStageBytes);CHECK(max.accepted(r));CHECK(r.fragment.instructionCount==250);
    // Complete selector domains for the unused VOP3 comparison source and
    // unsupported VOP1 opcode field, rather than one handcrafted rejection.
    for(unsigned unused=0;unused<512;++unused) {
        Product p(vertexBody(words({0xd4420001,0x00020a81u|(unused<<18),0x7e0202f2})),appleF);
        CHECK(p.accepted(r)==(unused==128));
    }
    for(unsigned op=0;op<256;++op) {
        Product p(appleV,psBody(words({0x7e0000f2u|(op<<9)})));
        CHECK(p.accepted(r)==(op==1));
    }
    // Arbitrary byte changes may change harmless constants or valid operands;
    // accepted mutations still have the same bounded/no-resource derived
    // profile. Every rejected mutation clears any stale successful report.
    Product base(appleV,appleF);uint32_t seed=0xc37ee129;
    CHECK(base.accepted(r));
    for(unsigned i=0;i<20000;++i) {
        seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
        Product p=base;const size_t at=seed%(base.ps+base.fb);
        p.bytes[at]^=uint8_t(1u<<((seed>>18)&7));
        const Status result=validate(p.request(),r);
        if(result==Status::Ok) {
            CHECK(r.vertex.rsrc1==2883586 && r.fragment.rsrc1==2883586);
            CHECK(r.vertex.requiredVgprs<=24 && r.fragment.requiredVgprs<=24);
            CHECK(r.image.imageBytes==4096 && r.image.fragmentOffset==256);
        } else {CHECK(cleared(r));CHECK(r.status==result);}
    }
}
} // namespace
int main() {
    actualProducts();controlAndLayout();dataflowAndEncodings();
    printf("gfx11_graphics_product: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
