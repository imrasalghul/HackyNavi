// SPDX-License-Identifier: MIT
// GFX11 shader/dispatch constants: AMD libdrm75254bf2390c10644ffb35a90fc8f18f196f9f0c.
// Cache, six-SE preamble and packet fields: Mesa35b085c4c067f198ce0b6437d94678e47b9c645f,
// ac_cmdbuf.c/ac_cmdbuf_cp.c/ac_barrier.c, registers/pkt3.json and GFX11 PM4 tables.
// See gfx11_compute_data.hpp and THIRD-PARTY-NOTICES.md for AMD/Mesa notices.
#include "hackynavi/gfx_compute.hpp"
#include "gfx11_compute_data.hpp"
namespace hackynavi { namespace gfxcompute {
namespace {
constexpr uint64_t kAddressLimit=UINT64_C(1)<<47;
bool range(const Buffer& b) { return b.bytes && b.gpu<kAddressLimit && b.bytes<=kAddressLimit-b.gpu; }
bool overlap(const Buffer& a,const Buffer& b) { return a.gpu<b.gpu+b.bytes && b.gpu<a.gpu+a.bytes; }
uint32_t packet(uint32_t op,uint32_t count,bool compute=true) {
    return 0xc0000000U|(count<<16)|(op<<8)|(compute?2U:0U);
}
struct Stream {
    uint32_t words[kMaxCommandDwords]; size_t count;
    void add(uint32_t v) { words[count++]=v; }
    void reg(uint32_t offset,uint32_t value) { add(packet(0x76,1));add(offset);add(value); }
    void descriptor(uint32_t regOffset,uint64_t gpu,uint32_t bytes) {
        add(packet(0x76,4));add(regOffset);add(uint32_t(gpu));
        add(uint32_t(gpu>>32)|0x100000U);add(bytes/16);add(0x1003dfac);
    }
};
}
Status shaderImage(uint8_t* destination,size_t capacity) {
    static_assert(sizeof(kClearShader)<=kCopyShaderOffset,"clear shader overlaps copy shader");
    static_assert(kCopyShaderOffset+sizeof(kCopyShader)<=kShaderImageBytes,"shader image too small");
    if(!destination)return Status::InvalidArgument;
    if(capacity<kShaderImageBytes)return Status::BufferTooSmall;
    for(size_t i=0;i<kShaderImageBytes/4;++i) {
        uint32_t word=0xbf9f0000; // AMD's instruction-prefetch padding.
        if(i<sizeof(kClearShader)/4)word=kClearShader[i];
        else if(i>=kCopyShaderOffset/4 && i<kCopyShaderOffset/4+sizeof(kCopyShader)/4)
            word=kCopyShader[i-kCopyShaderOffset/4];
        for(unsigned j=0;j<4;++j)destination[4*i+j]=uint8_t(word>>(8*j));
    }
    return Status::Ok;
}
Status encode(const Request& r,uint32_t* destination,size_t capacity,size_t& words) {
    if(!destination || (r.operation!=Operation::Clear && r.operation!=Operation::Copy) ||
        !r.bytes || (r.bytes&15) || r.bytes>kMaxTransferBytes)return Status::InvalidArgument;
    if(!range(r.shader) || !range(r.destination) || r.shader.bytes<kShaderImageBytes ||
        (r.shader.gpu&255) || (r.destination.gpu&15) || r.bytes>r.destination.bytes)
        return Status::InvalidRange;
    if(overlap(r.shader,r.destination))return Status::OverlappingMemory;
    if(r.operation==Operation::Copy) {
        if(!range(r.source) || (r.source.gpu&15) || r.bytes>r.source.bytes)return Status::InvalidRange;
        if(overlap(r.source,r.destination) || overlap(r.source,r.shader))return Status::OverlappingMemory;
    } else if(r.source.gpu || r.source.bytes)return Status::InvalidArgument;
    Stream s={};
    s.add(packet(0x28,1,false));s.add(0x80000000);s.add(0x80000000); // CONTEXT_CONTROL
    s.add(packet(0x58,6,false));s.add(0); // ACQUIRE_MEM at PFP, which also synchronizes ME.
    s.add(UINT32_MAX);s.add(0x00ffffff);s.add(0);s.add(0);s.add(10);s.add(0x43a1);
    // Invalidate instruction/scalar/vector/L1/L2 and metadata caches. The owner
    // has already waited the previous submission's release/writeback fence.
    s.add(packet(0x76,3));s.add(0x204);s.add(0);s.add(0);s.add(0); // START_X/Y/Z
    s.reg(0x218,0);s.reg(0x222,0); // no scratch; REQ_CTRL
    s.add(packet(0x76,4));s.add(0x224);for(unsigned i=0;i<4;++i)s.add(0);
    s.add(packet(0x79,1,false));s.add(0x22a);s.add(0); // COMPUTE_SHADER_CHKSUM
    // Source-indexed masks respect the CP's kernel CU mask. Navi31 has six
    // shader engines; explicitly disable the two nonexistent SE slots.
    for(unsigned group=0;group<2;++group) {
        const uint32_t offset=group?0x219:0x216;
        s.add(packet(0x9b,2));s.add(0x30000000U|offset);s.add(UINT32_MAX);s.add(UINT32_MAX);
    }
    s.add(packet(0x9b,4));s.add(0x3000022b);s.add(UINT32_MAX);s.add(UINT32_MAX);s.add(0);s.add(0);
    s.reg(0x22f,0);s.reg(0x27d,0); // DISPATCH_INTERLEAVE/TUNNEL disabled.
    const uint64_t shader=r.shader.gpu+(r.operation==Operation::Copy?kCopyShaderOffset:0);
    s.add(packet(0x76,2));s.add(0x20c);s.add(uint32_t(shader>>8));s.add(uint32_t(shader>>40));
    s.reg(0x212,0x400c0041);s.reg(0x213,0x90); // source RSRC1 with WGP_MODE bit29 clear; RSRC2
    s.reg(0x207,64);s.reg(0x208,1);s.reg(0x209,1);
    // Both source binaries include their instruction-prefetch tail (316/312B).
    // Three128B units cover those bytes within each owned1KiB slot. libdrm's
    // maximum63-unit test setting could prefetch beyond our4KiB allocation.
    const size_t codeBytes=r.operation==Operation::Copy?sizeof(kCopyShader):sizeof(kClearShader);
    s.reg(0x228,uint32_t((codeBytes+127)/128)<<4);
    if(r.operation==Operation::Clear) {
        s.descriptor(0x240,r.destination.gpu,r.bytes);
        s.add(packet(0x76,4));s.add(0x244);for(uint32_t v:r.pattern)s.add(v);
    } else { s.descriptor(0x240,r.source.gpu,r.bytes);s.descriptor(0x244,r.destination.gpu,r.bytes); }
    s.reg(0x215,0); // RESOURCE_LIMITS
    s.add(packet(0x15,3));s.add((r.bytes/16+63)/64);s.add(1);s.add(1);s.add(1);
    s.add(packet(0x46,0,false));s.add(0x407); // CS_PARTIAL_FLUSH, event index4.
    s.add(packet(0x42,0,false));s.add(0); // PFP_SYNC_ME before subsequent state/CPU fence.
    while(s.count&7)s.add(0xffff1000);
    if(capacity<s.count)return Status::BufferTooSmall;
    for(size_t i=0;i<s.count;++i)destination[i]=s.words[i];
    words=s.count;return Status::Ok;
}
const char* statusString(Status status) {switch(status){
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(InvalidRange);NAME(OverlappingMemory);NAME(BufferTooSmall);
#undef NAME
    }return "Unknown";
}
} }
