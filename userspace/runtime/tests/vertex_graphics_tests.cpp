// SPDX-License-Identifier: MIT
// Real recorded shader bytes and wire validation; no device or fake transport.
#include "../graphics_buffer_snapshot.hpp"
#include "hackynavi/gfx11_graphics_product.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace a=hackynavi::nativeabi;
namespace c=hackynavi::compute;
namespace {
unsigned checks=0;
void check(bool value,const char* label) {++checks;if(!value)throw std::runtime_error(label);}
template<class T> bool zero(const T& value) {
    const auto* p=reinterpret_cast<const unsigned char*>(&value);
    for(size_t i=0;i<sizeof(T);++i)if(p[i])return false;
    return true;
}
using Bytes=std::vector<uint8_t>;
Bytes read(const char* path) {
    std::ifstream file(path,std::ios::binary);check(bool(file),"actual fixture opens");
    file.seekg(0,std::ios::end);const auto size=file.tellg();check(size>0&&size<=1024,"bounded actual stage");
    Bytes bytes(static_cast<size_t>(size));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),size);
    check(bool(file),"actual fixture read");return bytes;
}
a::HelloReply hello() {
    a::HelloReply r{};r.header=a::makeHeader(sizeof(r),23,a::kVertexBufferedGraphicsMinor);
    r.capabilities=a::kVertexBufferedGraphicsCapabilities;r.registryId=99;r.vramBytes=24ULL<<30;
    r.cpuVisibleVramBytes=256ULL<<20;r.systemBudget=256ULL<<20;r.vaBegin=0x10000;r.vaEnd=1ULL<<47;
    r.maxAllocationBytes=16ULL<<20;r.vendorId=0x1002;r.deviceId=0x744c;r.gcVersion=0x0b0000;
    r.pageBytes=4096;r.maxResources=64;return r;
}
a::ProductVertexBufferedGraphicsRequest vertexRequest() {
    a::ProductVertexBufferedGraphicsRequest r{};
    r.draw.header=a::makeHeader(sizeof(r),23,a::kVertexBufferedGraphicsMinor);
    r.draw.code={1,2};r.draw.vertexBytes=124;r.draw.fragmentBytes=60;r.draw.target={2,3};
    r.draw.targetLayout={64,64,64,256,16384,16384,256,0,0x2800a,0,0,(63U<<14)|63,0x01000000,0x428};
    r.vertex={3,4};r.vertexOffset=4080;r.vertexBufferBytes=48;return r;
}
void protocol() {
    const auto h=hello();
    check(a::validateHello(h,99,a::kVertexBufferedGraphicsCapabilities)==a::Validation::Ok,"v1.7 actual capability contract");
    a::HelloRequest hi{a::makeHeader(sizeof(hi),0,a::kVertexBufferedGraphicsMinor),a::kVertexBufferedGraphicsCapabilities,0};
    check(a::validateHelloRequest(hi,a::kVertexBufferedGraphicsCapabilities)==a::Validation::Ok,"v1.7 hello request");
    check(a::validateHelloRequest(hi,a::kBufferedGraphicsCapabilities)!=a::Validation::Ok,"older owner cannot claim new classes");
    for(uint64_t bit=1;bit<=a::PositionVertexFragmentBufferGraphics;bit<<=1)if(h.capabilities&bit) {
        auto changed=h;changed.capabilities&=~bit;
        check(a::validateHello(changed,99,a::kVertexBufferedGraphicsCapabilities)!=a::Validation::Ok,"required capability is not optional");
    }
    for(const auto caps:{a::kGraphicsCapabilities,a::kBufferedGraphicsCapabilities,a::kReusableCapabilities})
        check(a::validateHello(h,99,caps)!=a::Validation::Ok,"no silent downgrade of negotiated v1.7");
    const auto original=vertexRequest();a::ProductVertexBufferedGraphicsRequest output{};
    check(a::decodeProductVertexBufferedGraphics(&original,sizeof(original),h,output)==a::Validation::Ok&&
        !std::memcmp(&output,&original,sizeof(output)),"exact vertex wire captures");
    a::ProductVertexFragmentBufferedGraphicsRequest both{};both.vertexDraw=original;
    both.vertexDraw.draw.header.bytes=sizeof(both);both.fragment={4,5};both.fragmentOffset=64;both.fragmentBufferBytes=16;
    a::ProductVertexFragmentBufferedGraphicsRequest combined{};
    check(a::decodeProductVertexFragmentBufferedGraphics(&both,sizeof(both),h,combined)==a::Validation::Ok&&
        !std::memcmp(&combined,&both,sizeof(both)),"exact combined wire captures");
    check(a::decodeProductVertexBufferedGraphics(&both,sizeof(both),h,output)==a::Validation::Size&&zero(output),"combined cannot use vertex selector");
    check(a::decodeProductVertexFragmentBufferedGraphics(&original,sizeof(original),h,combined)==a::Validation::Size&&zero(combined),"vertex cannot use combined selector");
    for(size_t length=0;length<sizeof(original);++length) {
        std::memset(&output,0xa5,sizeof(output));
        check(a::decodeProductVertexBufferedGraphics(&original,length,h,output)==a::Validation::Size&&zero(output),"truncated vertex wire clears capture");
    }
    for(size_t length=0;length<sizeof(both);++length) {
        std::memset(&combined,0xa5,sizeof(combined));
        check(a::decodeProductVertexFragmentBufferedGraphics(&both,length,h,combined)==a::Validation::Size&&zero(combined),"truncated combined wire clears capture");
    }
    Bytes unaligned(sizeof(both)+1);std::memcpy(unaligned.data()+1,&both,sizeof(both));
    check(a::decodeProductVertexFragmentBufferedGraphics(unaligned.data()+1,sizeof(both),h,combined)==a::Validation::Ok,"unaligned copied wire accepted");
    auto rejectVertex=[&](a::ProductVertexBufferedGraphicsRequest r,const char* label) {
        std::memset(&output,0xa5,sizeof(output));
        check(a::decodeProductVertexBufferedGraphics(&r,sizeof(r),h,output)!=a::Validation::Ok&&zero(output),label);
    };
    auto rejectBoth=[&](a::ProductVertexFragmentBufferedGraphicsRequest r,const char* label) {
        std::memset(&combined,0xa5,sizeof(combined));
        check(a::decodeProductVertexFragmentBufferedGraphics(&r,sizeof(r),h,combined)!=a::Validation::Ok&&zero(combined),label);
    };
    for(uint64_t length:{0ULL,16ULL,32ULL,47ULL,49ULL,64ULL,4096ULL}) {
        auto changed=original;changed.vertexBufferBytes=length;rejectVertex(changed,"vertex resource extent must be48");
        auto all=both;all.vertexDraw.vertexBufferBytes=length;rejectBoth(all,"combined vertex extent must be48");
    }
    for(uint64_t offset:{1ULL,15ULL,17ULL,UINT64_MAX-15,UINT64_MAX-31}) {
        auto changed=original;changed.vertexOffset=offset;rejectVertex(changed,"vertex alignment/overflow rejected");
    }
    for(const auto handle:{a::Handle{0,1},a::Handle{3,0},original.draw.code,original.draw.target}) {
        auto changed=original;changed.vertex=handle;rejectVertex(changed,"vertex handle invalid or aliases another resource");
    }
    for(const auto handle:{a::Handle{0,1},a::Handle{4,0},both.vertexDraw.vertex,both.vertexDraw.draw.code,both.vertexDraw.draw.target}) {
        auto changed=both;changed.fragment=handle;rejectBoth(changed,"fragment identity must be separate");
    }
    for(uint64_t length:{0ULL,15ULL,17ULL,48ULL}) {
        auto changed=both;changed.fragmentBufferBytes=length;rejectBoth(changed,"combined fragment extent must be16");
    }
    for(uint64_t offset:{1ULL,15ULL,UINT64_MAX}) {
        auto changed=both;changed.fragmentOffset=offset;rejectBoth(changed,"fragment alignment/overflow rejected");
    }
    auto changed=original;changed.draw.header.epoch++;rejectVertex(changed,"foreign epoch rejected");
    changed=original;changed.draw.reserved=1;rejectVertex(changed,"reserved draw rejected");
    changed=original;changed.draw.header.flags=1;rejectVertex(changed,"unsupported flags rejected");
    changed=original;changed.draw.vertexBytes=0;rejectVertex(changed,"stage bytes are separate from resource bytes");
    for(uint16_t minor=0;minor<a::kVertexBufferedGraphicsMinor;++minor) {
        changed=original;changed.draw.header.minor=minor;
        check(a::validateProductVertexBufferedGraphics(changed,23,minor)==a::Validation::Unsupported,"old profile rejects vertex class");
    }
    // Existing selectors still carry their original sizes in v1.7.
    auto constant=original.draw;constant.header.bytes=sizeof(constant);a::ProductGraphicsRequest oldOut{};
    check(a::decodeProductGraphics(&constant,sizeof(constant),h,oldOut)==a::Validation::Ok,"constant selector remains usable");
    a::ProductBufferedGraphicsRequest fragment{};fragment.draw=constant;fragment.draw.header.bytes=sizeof(fragment);
    fragment.fragment={4,5};fragment.fragmentBytes=16;
    a::ProductBufferedGraphicsRequest fragmentOut{};
    check(a::decodeProductBufferedGraphics(&fragment,sizeof(fragment),h,fragmentOut)==a::Validation::Ok,"fragment-only selector remains usable");
    auto oldHello=h;oldHello.header.minor=a::kBufferedGraphicsMinor;oldHello.capabilities=a::kBufferedGraphicsCapabilities;
    fragment.draw.header.minor=a::kBufferedGraphicsMinor;
    check(a::decodeProductBufferedGraphics(&fragment,sizeof(fragment),oldHello,fragmentOut)==a::Validation::Ok,"unchanged v1.6 still negotiates");
    check(a::decodeProductVertexBufferedGraphics(&original,sizeof(original),oldHello,output)!=a::Validation::Ok,"v1.6 cannot accept vertex message");
    a::ReleaseResultRequest release{a::makeHeader(sizeof(release),23,a::kVertexBufferedGraphicsMinor),7,0};
    check(a::validateReleaseResult(release,h)==a::Validation::Ok,"historical result release preserved");
}
template<class Pipeline> void snapshot(const Bytes& vertex,const Bytes& fragment) {
    auto v=vertex,f=fragment;std::shared_ptr<const Pipeline> held;
    check(bool(Pipeline::capture(v.data(),v.size(),f.data(),f.size(),held)),"real product snapshot captures");
    check(held&&held->vertexBufferIndex()==0&&held->vertexBufferBytes()==48&&held->vertexBufferAlignment()==16,"bounded vertex profile");
    std::fill(v.begin(),v.end(),0);v.clear();v.shrink_to_fit();
    std::fill(f.begin(),f.end(),0);f.clear();f.shrink_to_fit();
    Bytes image(4096,0xa5);check(bool(held->copyImage(image.data(),image.size())),"snapshot outlives input storage");
    hackynavi::gfx11graphics::Layout layout{};
    check(hackynavi::gfx11graphics::layout(vertex.size(),fragment.size(),layout)==hackynavi::gfx11graphics::Status::Ok,"actual stage layout");
    check(!std::memcmp(image.data(),vertex.data(),vertex.size())&&!std::memcmp(image.data()+layout.fragmentOffset,fragment.data(),fragment.size()),"owned actual bytes preserved");
    for(size_t i=vertex.size();i<layout.fragmentOffset;++i)check(image[i]==0,"gap padding zero");
    for(size_t i=layout.fragmentOffset+fragment.size();i<image.size();++i)check(image[i]==0,"image padding zero");
    const auto before=image;
    check(!held->copyImage(image.data(),4095)&&image==before,"failed copy keeps output");
    check(!held->copyImage(const_cast<Pipeline*>(held.get()),4096),"immutable object alias rejected");
    check(bool(held->copyImage(image.data(),4096))&&image==before,"alias attempt did not change image");
    const auto old=held;
    check(!Pipeline::capture(vertex.data(),vertex.size(),fragment.data(),fragment.size(),held)&&held==old,"nonempty output unchanged");
    std::shared_ptr<const Pipeline> empty;
    check(!Pipeline::capture(nullptr,vertex.size(),fragment.data(),fragment.size(),empty)&&!empty,"null stage rejected");
    for(size_t i=0;i<vertex.size();++i) {
        auto bad=vertex;bad[i]^=1;
        check(!Pipeline::capture(bad.data(),bad.size(),fragment.data(),fragment.size(),empty)&&!empty,"actual vertex mutation rejected");
    }
}
}
int main(int argc,char** argv) {
    try {
        check(argc==5,"provide actual buffer vertex, constant fragment, buffer fragment and legacy vertex");
        const auto vertex=read(argv[1]),constant=read(argv[2]),fragment=read(argv[3]),legacy=read(argv[4]);
        protocol();snapshot<c::VertexBufferGraphicsPipeline>(vertex,constant);
        snapshot<c::VertexFragmentBufferGraphicsPipeline>(vertex,fragment);
        std::shared_ptr<const c::VertexBufferGraphicsPipeline> v;
        std::shared_ptr<const c::VertexFragmentBufferGraphicsPipeline> both;
        check(!c::VertexBufferGraphicsPipeline::capture(vertex.data(),vertex.size(),fragment.data(),fragment.size(),v)&&!v,"vertex-only type rejects fragment resource");
        check(!c::VertexFragmentBufferGraphicsPipeline::capture(vertex.data(),vertex.size(),constant.data(),constant.size(),both)&&!both,"combined type requires fragment loads");
        check(!c::VertexBufferGraphicsPipeline::capture(legacy.data(),legacy.size(),constant.data(),constant.size(),v)&&!v,"vertex-buffer type rejects constant vertex");
        std::shared_ptr<const c::GraphicsPipeline> old;
        std::shared_ptr<const c::FragmentBufferGraphicsPipeline> oldBuffer;
        check(!c::GraphicsPipeline::capture(vertex.data(),vertex.size(),constant.data(),constant.size(),old)&&!old,"legacy snapshot does not admit vertex buffers");
        check(!c::FragmentBufferGraphicsPipeline::capture(vertex.data(),vertex.size(),fragment.data(),fragment.size(),oldBuffer)&&!oldBuffer,"fragment-only snapshot still rejects vertex loads");
        check(bool(c::GraphicsPipeline::capture(legacy.data(),legacy.size(),constant.data(),constant.size(),old)),"actual legacy snapshot preserved");
        std::cout<<"{\"success\":true,\"checks\":"<<checks<<",\"gpu_executed\":false}\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
