// SPDX-License-Identifier: MIT
// Exact compiler product and custom launch ABI: userspace/compiler/fixtures/apple-add-one.
// GFX11 raw descriptors and PM4: Mesa35b085c4c067f198ce0b6437d94678e47b9c645f,
// ac_descriptors.c, ac_cmdbuf.c/ac_cmdbuf_cp.c, ac_barrier.c and GC11 registers.
// Existing GC11 preamble policy matches gfx_compute.cpp; clear/copy is unchanged.
#include "hackynavi/gfx_aco_fixture.hpp"
#include "../userspace/compiler/fixtures/apple-add-one/shader_data.hpp"
namespace hackynavi { namespace gfxaco { namespace {
constexpr uint64_t kVaEnd=UINT64_C(1)<<47;
static_assert(fixture::kCodeBytes==kCodeBytes && fixture::kExecutableBytes==kExecutableBytes,
    "fixture launch contract changed");
static_assert(fixture::kCodeAlignment==256,"fixture code alignment changed");
bool pointer(const void* p,size_t bytes,size_t alignment=1) {
    return p&&bytes&&!(reinterpret_cast<uintptr_t>(p)%alignment)&&
        bytes-1<=UINTPTR_MAX-reinterpret_cast<uintptr_t>(p);
}
Status binding(const Request& r,const gfxisolated::Binding& b,uint32_t permissions,bool control) {
    if(b.owner!=r.owner||b.epoch!=r.epoch)return Status::ForeignContext;
    if(!b.id||!b.generation||!b.bytes||b.gpu<4096||b.gpu>=kVaEnd||b.bytes>kVaEnd-b.gpu||
       ((b.gpu|b.bytes|b.bufferOffset|b.allocationBytes)&4095)||
       b.bufferOffset>b.allocationBytes||b.bytes>b.allocationBytes-b.bufferOffset||
       (control&&(b.bytes!=kControlBytes||b.bufferOffset||b.allocationBytes!=kControlBytes)))
        return Status::InvalidBinding;
    return b.access==permissions?Status::Ok:Status::Permission;
}
Status request(const Request& r) {
    if(!r.owner||!r.epoch||r.vmid<1||r.vmid>7)return Status::InvalidArgument;
    const gfxisolated::Binding* b[]={&r.shader,&r.source,&r.destination,&r.indirect,&r.parameters};
    const uint32_t permissions[]={5,1,3,5,1};
    for(size_t n=0;n<5;++n) {
        const Status s=binding(r,*b[n],permissions[n],n==0||n>=3);if(s!=Status::Ok)return s;
    }
    if((r.sourceOffset&3)||(r.destinationOffset&3)||r.sourceOffset>r.source.bytes||
       kDataBytes>r.source.bytes-r.sourceOffset||r.destinationOffset>r.destination.bytes||
       kDataBytes>r.destination.bytes-r.destinationOffset)return Status::InvalidBinding;
    for(size_t n=0;n<5;++n)for(size_t p=0;p<n;++p) {
        // Reject BO aliases even when their generation or VA differs. The
        // native ledger additionally confirms independent actual DMA owners.
        if(b[n]->id==b[p]->id)return Status::AliasedStorage;
        if(b[n]->gpu<b[p]->gpu+b[p]->bytes&&b[p]->gpu<b[n]->gpu+b[n]->bytes)
            return Status::Overlap;
    }
    return Status::Ok;
}
void parameters(const Request& r,uint32_t (&out)[20]) {
    for(auto& v:out)v=0;
    const uint64_t source=r.source.gpu+r.sourceOffset,destination=r.destination.gpu+r.destinationOffset;
    // ac_build_raw_buffer_descriptor(GFX11,false,VA,kDataBytes): stride0,
    // R32_FLOAT identity swizzle, byte NUM_RECORDS and OOB_SELECT_RAW.
    out[0]=uint32_t(source);out[1]=uint32_t(source>>32);out[2]=kDataBytes;out[3]=0x30016fac;
    out[4]=uint32_t(destination);out[5]=uint32_t(destination>>32);out[6]=kDataBytes;out[7]=0x30016fac;
    // Actual KernelDispatchPlan::push_constants order: grid, thread base,
    // group base, total groups. This exact product has no tail region.
    out[8]=kThreads;out[9]=out[10]=1;out[17]=64;out[18]=out[19]=1;
}
uint8_t parameterByte(const uint32_t (&words)[20],size_t n) {
    return n<kParameterBytes?uint8_t(words[n/4]>>(8*(n&3))):0;
}
uint8_t shaderByte(size_t n){return n<kCodeBytes?fixture::kCode[n]:0;}
uint32_t packet(uint32_t op,uint32_t count,bool compute=true) {
    return 0xc0000000U|(count<<16)|(op<<8)|(compute?2U:0U);
}
struct Stream {
    uint32_t words[kMaxCommandDwords];size_t count;bool overflow;
    void add(uint32_t v){if(count<kMaxCommandDwords)words[count++]=v;else overflow=true;}
    void reg(uint32_t offset,uint32_t value){add(packet(0x76,1));add(offset);add(value);}
};
void commands(const Request& r,Stream& s) {
    s.add(packet(0x28,1,false));s.add(0x80000000);s.add(0x80000000);
    s.add(packet(0x58,6,false));s.add(0);s.add(UINT32_MAX);s.add(0x00ffffff);
    s.add(0);s.add(0);s.add(10);s.add(0x43a1); // instruction/scalar/data cache invalidate
    s.add(packet(0x76,3));s.add(0x204);s.add(0);s.add(0);s.add(0); // START XYZ
    s.reg(0x218,0);s.reg(0x222,0); // no scratch, REQ_CTRL
    s.add(packet(0x76,4));s.add(0x224);for(unsigned n=0;n<4;++n)s.add(0);
    s.add(packet(0x79,1,false));s.add(0x22a);s.add(0); // checksum
    for(unsigned group=0;group<2;++group) {
        s.add(packet(0x9b,2));s.add(0x30000000U|(group?0x219:0x216));s.add(UINT32_MAX);s.add(UINT32_MAX);
    }
    s.add(packet(0x9b,4));s.add(0x3000022b);s.add(UINT32_MAX);s.add(UINT32_MAX);s.add(0);s.add(0);
    s.reg(0x22f,0);s.reg(0x27d,0); // no dispatch interleave/tunnel
    s.add(packet(0x76,2));s.add(0x20c);s.add(uint32_t(r.shader.gpu>>8));s.add(uint32_t(r.shader.gpu>>40));
    s.reg(0x212,0x202c0002);s.reg(0x213,0x788);s.reg(0x228,0); // recorded RSRC1/2/3
    s.reg(0x207,64);s.reg(0x208,1);s.reg(0x209,1);
    s.add(packet(0x76,4));s.add(0x240);
    s.add(uint32_t(r.parameters.gpu));s.add(uint32_t(r.parameters.gpu>>32));
    s.add(uint32_t(r.parameters.gpu+kPushOffset));s.add(uint32_t((r.parameters.gpu+kPushOffset)>>32));
    s.reg(0x215,0); // RESOURCE_LIMITS; no external waves/scratch requirements
    s.add(packet(0x15,3));s.add(64);s.add(1);s.add(1);s.add(0x8001); // wave32 direct dispatch
    s.add(packet(0x46,0,false));s.add(0x407); // CS_PARTIAL_FLUSH index4
    s.add(packet(0x42,0,false));s.add(0); // PFP_SYNC_ME before queue-owned fence
    while(!s.overflow&&(s.count&7))s.add(0xffff1000);
}
} // namespace
Status shaderImage(uint8_t* out,size_t capacity) {
    if(!pointer(out,kControlBytes))return Status::InvalidArgument;
    if(capacity<kControlBytes)return Status::BufferTooSmall;
    for(size_t n=0;n<kControlBytes;++n)out[n]=shaderByte(n);
    return Status::Ok;
}
Status parameterImage(const Request& r,uint8_t* out,size_t capacity) {
    const Status valid=request(r);if(valid!=Status::Ok)return valid;
    if(!pointer(out,kControlBytes))return Status::InvalidArgument;
    if(capacity<kControlBytes)return Status::BufferTooSmall;
    uint32_t values[20];parameters(r,values);
    for(size_t n=0;n<kControlBytes;++n)out[n]=parameterByte(values,n);
    return Status::Ok;
}
Status encode(const Request& r,uint32_t* out,size_t capacity,size_t& words) {
    const Status valid=request(r);if(valid!=Status::Ok)return valid;
    if(!out||(reinterpret_cast<uintptr_t>(out)%alignof(uint32_t)))return Status::InvalidArgument;
    Stream s={};commands(r,s);
    if(s.overflow||capacity<s.count)return Status::BufferTooSmall;
    if(!pointer(out,s.count*4,alignof(uint32_t)))return Status::InvalidArgument;
    for(size_t n=0;n<s.count;++n)out[n]=s.words[n];words=s.count;return Status::Ok;
}
Status validate(const Request& r,const uint8_t* shader,size_t shaderBytes,const uint8_t* parameter,
    size_t parameterBytes,const uint32_t* stream,size_t words) {
    const Status valid=request(r);if(valid!=Status::Ok)return valid;
    if(shaderBytes!=kControlBytes||parameterBytes!=kControlBytes||!words||words>kMaxCommandDwords||
       !pointer(shader,kControlBytes)||!pointer(parameter,kControlBytes)||!pointer(stream,words*4,alignof(uint32_t)))
        return Status::InvalidArgument;
    for(size_t n=0;n<kControlBytes;++n)if(shader[n]!=shaderByte(n))return Status::InvalidShader;
    uint32_t values[20];parameters(r,values);
    for(size_t n=0;n<kControlBytes;++n)if(parameter[n]!=parameterByte(values,n))return Status::InvalidParameters;
    Stream canonical={};commands(r,canonical);
    if(canonical.overflow||words!=canonical.count)return Status::InvalidStream;
    for(size_t n=0;n<words;++n)if(stream[n]!=canonical.words[n])return Status::InvalidStream;
    return Status::Ok;
}
const char* statusString(Status s){switch(s){
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(ForeignContext);NAME(InvalidBinding);NAME(Permission);
    NAME(AliasedStorage);NAME(Overlap);NAME(BufferTooSmall);NAME(InvalidShader);NAME(InvalidParameters);NAME(InvalidStream);
#undef NAME
    }return "Unknown";
}
} }
