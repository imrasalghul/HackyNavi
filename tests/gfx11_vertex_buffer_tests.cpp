// SPDX-License-Identifier: MIT
#include "hackynavi/gfx11_graphics_product.hpp"
#include "fixtures/gfx11_vertex_buffer_shader.hpp"
#include "fixtures/gfx11_buffer_shader.hpp"
#include <array>
#include <stdio.h>
#include <string.h>
using namespace hackynavi::gfx11graphics;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("line %u: %s\n",unsigned(__LINE__),#x);}}while(0)
struct Fixture {
    alignas(Report) std::array<uint32_t,1024> image{};
    Request request{};
    explicit Fixture(bool both=false) {
        memcpy(image.data(),vertex_buffer_fixture::vertex,sizeof(vertex_buffer_fixture::vertex));
        const size_t bytes=both?sizeof(buffer_shader_fixture::fragment):sizeof(vertex_buffer_fixture::constantFragment);
        memcpy(image.data()+64,both?buffer_shader_fixture::fragment:vertex_buffer_fixture::constantFragment,bytes);
        request={reinterpret_cast<const uint8_t*>(image.data()),4096,sizeof(vertex_buffer_fixture::vertex),bytes};
    }
};
void rejected(const Request& r,bool both=false) {
    Report out{};out.vertex.rsrc1=1;out.fragment.rsrc2=1;
    CHECK((both?validateVertexFragmentBuffers(r,out):validateVertexBuffer(r,out))!=Status::Ok);
    CHECK(out.vertex.rsrc1==0&&out.fragment.rsrc2==0&&out.image.imageBytes==0);
}
void positives() {
    for(bool both:{false,true}) {
        Fixture f(both);Report out{};
        CHECK((both?validateVertexFragmentBuffers(f.request,out):validateVertexBuffer(f.request,out))==Status::Ok);
        CHECK(out.vertex.executableBytes==104&&out.vertex.instructionCount==19&&out.vertex.rsrc2==10);
        CHECK(out.vertex.requiredVgprs==6&&out.vertex.rsrc1==0x2c0002&&out.vertex.waveSize==32);
        CHECK(out.fragment.rsrc2==(both?8u:0u));
        CHECK(validate(f.request,out)!=Status::Ok);CHECK(validateFragmentBuffer(f.request,out)!=Status::Ok);
        rejected(f.request,!both);
    }
    // Equivalent register allocation and four cyclic load orders. Position
    // data follows memory offset provenance, not an expected register number.
    for(unsigned address=1;address<24;++address)for(unsigned rotation=0;rotation<4;++rotation) {
        Fixture f;uint32_t* p=f.image.data();unsigned dest[4]{},next=1;
        for(unsigned i=0;i<4;++i){while(next==address)++next;dest[i]=next++;}
        p[6]=0xd6470000|address;p[19]=0;
        for(unsigned i=0;i<4;++i) {
            const unsigned component=(i+rotation)%4;
            p[9+2*i]=0xe0500000|component*4;p[10+2*i]=0x80420000|dest[component]<<8|address;
            p[19]|=dest[component]<<(component*8);
        }
        Report out{};CHECK(validateVertexBuffer(f.request,out)==Status::Ok);
        // Only the last load may overwrite the address register.
        const unsigned last=(3+rotation)%4;
        p[16]=0x80420000|address<<8|address;
        p[19]=(p[19]&~(255u<<(last*8)))|(address<<(last*8));
        CHECK(validateVertexBuffer(f.request,out)==Status::Ok);
    }
}
void negatives() {
    // Every single-bit mutation of the real vertex product must fail when
    // corresponding operands/export provenance/control flow are unchanged.
    for(unsigned n=0;n<31;++n)for(unsigned bit=0;bit<32;++bit) {
        Fixture f;f.image[n]^=1u<<bit;rejected(f.request);
    }
    for(size_t n=0;n<sizeof(vertex_buffer_fixture::vertex);n+=4) {
        Fixture f;f.request.vertexBytes=n;rejected(f.request);
    }
    {Fixture f;f.image[31]=1;rejected(f.request);}
    {Fixture f;f.image[1023]=1;rejected(f.request);}
    {Fixture f;f.request.imageBytes=4095;rejected(f.request);}
    {Fixture f;f.request.image=nullptr;rejected(f.request);}
    // A source address destroyed by the first load is unsafe even if all
    // final exports are adjusted to point at the corresponding loaded values.
    {Fixture f;f.image[10]=0x80420505;f.image[16]=0x80420605;f.image[19]=0x06030205;rejected(f.request);}
    {Fixture f;f.image[6]=0xd6470000;rejected(f.request);}
    {Fixture f;f.image[6]=0xd6470018;rejected(f.request);}
    {Fixture f;auto before=f.image;CHECK(validateVertexBuffer(f.request,*reinterpret_cast<Report*>(f.image.data()))==Status::InvalidArgument);CHECK(f.image==before);}
    {Fixture f(true);f.image[64+10]^=1;rejected(f.request,true);}
}
}
int main(){positives();negatives();printf("vertex buffer admission: %u checks, %u failures\n",checks,failures);return failures?1:0;}
