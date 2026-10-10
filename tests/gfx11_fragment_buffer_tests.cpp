// SPDX-License-Identifier: MIT
#include "hackynavi/gfx11_graphics_product.hpp"
#include "fixtures/gfx11_buffer_shader.hpp"
#include <array>
#include <stdio.h>
#include <string.h>
using namespace hackynavi::gfx11graphics;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("line %u: %s\n",unsigned(__LINE__),#x);}}while(0)
struct Fixture {
    alignas(Report) std::array<uint8_t,4096> bytes{};
    Fixture() {
        memcpy(bytes.data(),buffer_shader_fixture::vertex,sizeof(buffer_shader_fixture::vertex));
        memcpy(bytes.data()+256,buffer_shader_fixture::fragment,sizeof(buffer_shader_fixture::fragment));
    }
    Request request() const {return {bytes.data(),bytes.size(),144,72};}
    void put(unsigned psWord,uint32_t value) {
        for(unsigned i=0;i<4;++i)bytes[256+psWord*4+i]=uint8_t(value>>(i*8));
    }
};
void reject(const Fixture& f) {
    Report report;memset(&report,0xa5,sizeof(report));
    const Status s=validateFragmentBuffer(f.request(),report);CHECK(s!=Status::Ok);CHECK(s==report.status);
    Report clean{};clean.status=report.status;clean.stage=report.stage;
    clean.byteOffset=report.byteOffset;clean.instructionWord=report.instructionWord;
    CHECK(!memcmp(&clean.image,&report.image,sizeof(report)-offsetof(Report,image)));
}
void positive() {
    Fixture f;Report r{};
    CHECK(validateFragmentBuffer(f.request(),r)==Status::Ok);
    CHECK(r.fragment.rsrc1==0x2c0002&&r.fragment.rsrc2==8);
    CHECK(r.fragment.executableBytes==52&&r.fragment.instructionCount==8);
    CHECK(r.fragment.requiredVgprs==4&&r.fragment.allocatedVgprs==24);
    CHECK(r.fragment.hardwareSgprs==108&&r.fragment.waveSize==32);
    CHECK(r.spiPsInputEna==128&&r.spiPsInputAddr==128&&r.spiPsInControl==32768);
    CHECK(r.spiShaderColFormat==9&&r.cbShaderMask==15);
    for(unsigned c:r.fragmentColorBits)CHECK(c==0);
    // The old public admission remains resource-free.
    CHECK(validate(f.request(),r)!=Status::Ok);
    // Different register assignment and load order preserve the actual ABI.
    const unsigned components[]={2,0,3,1},regs[]={23,12,0,7};uint32_t exp=0;
    for(unsigned n=0;n<4;++n) {
        f.put(1+n*2,0xe0500000|components[n]*4);f.put(2+n*2,0x80000080|regs[n]<<8);
        exp|=regs[n]<<(components[n]*8);
    }
    f.put(11,exp);CHECK(validateFragmentBuffer(f.request(),r)==Status::Ok);
    CHECK(r.fragment.requiredVgprs==24);
}
void negative() {
    // Real memory safety boundaries: address, descriptor, offset and modes.
    const uint32_t badOp[]={0xe0500010,0xe0500fff,0xe0500001,0xe0500002,
        0xe0501000,0xe0502000,0xe0510000,0xe0900000,0xe0700000,0xe0504000,0xe0508000};
    for(uint32_t value:badOp){Fixture f;f.put(1,value);reject(f);}
    const uint32_t badArgs[]={0x80010080,0x81000080,0x00000080,0x80000000,
        0x80400080,0x80800080,0x80001880,0x8000ff80};
    for(uint32_t value:badArgs){Fixture f;f.put(2,value);reject(f);}
    // No stale/duplicated data, skipped wait, partial export or extra code.
    for(const auto& mutation:std::array<std::array<uint32_t,2>,14>{{
        {{0,0xbf850002}},{{0,0xbe800080}},{{3,0xe0500000}},{{4,0x80000080}},
        {{9,0xbf8907f7}},{{9,0xbf89fff0}},{{9,0xbf800000}},{{10,0xf8000807}},
        {{10,0xf800080f^0x800}},{{11,0x00020103}},{{11,0x03020104}},
        {{12,0xbfa00000}},{{13,0xbfb00000}},{{17,0}}}}) {
        Fixture f;f.put(mutation[0],mutation[1]);reject(f);
    }
    {Fixture f;f.bytes[0]^=1;reject(f);} // vertex control remains independently admitted
    {Fixture f;f.bytes[255]=1;reject(f);}
    {Fixture f;f.bytes[4095]=1;reject(f);}
    // Every bit outside VDATA of one load operand is a constrained field.
    for(unsigned bit=0;bit<32;++bit)if(bit<8||bit>=16){Fixture f;f.put(2,0x80000080^(1u<<bit));reject(f);}
    Fixture f;Report r{};
    for(size_t extent:{size_t(0),size_t(68),size_t(76),size_t(1028),SIZE_MAX}) {
        Request request=f.request();request.fragmentBytes=extent;
        CHECK(validateFragmentBuffer(request,r)!=Status::Ok);
    }
    // Publishing diagnostics cannot change aliased immutable shader storage.
    const auto before=f.bytes;
    CHECK(validateFragmentBuffer(f.request(),*reinterpret_cast<Report*>(f.bytes.data()+256))==Status::InvalidArgument);
    CHECK(f.bytes==before);
}
}
int main(){positive();negative();printf("fragment-buffer admission: %u checks, %u failures\n",checks,failures);return failures?1:0;}
