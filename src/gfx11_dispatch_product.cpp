// SPDX-License-Identifier: MIT
// GFX11 dispatch registers/raw descriptors: Mesa35b085c4c067f198ce0b6437d94678e47b9c645f,
// ac_descriptors.c and ac_cmdbuf.c. Inter-region ordering: ac_barrier.c:289-301
// and ac_cmdbuf_cp.c:399-429 (graphics PFP/ME, not a MEC queue).
#include "hackynavi/gfx11_dispatch_product.hpp"
namespace hackynavi { namespace gfx11dispatch { namespace {
constexpr uint64_t kVaEnd=UINT64_C(1)<<47;
bool pointer(const void* p,size_t bytes,size_t alignment=1) {
    return p&&bytes&&!(reinterpret_cast<uintptr_t>(p)%alignment)&&
        bytes-1<=UINTPTR_MAX-reinterpret_cast<uintptr_t>(p);
}
Status binding(const Request& r,const gfxisolated::Binding& b,uint32_t access,size_t controlBytes=0) {
    if(b.owner!=r.owner||b.epoch!=r.epoch)return Status::ForeignContext;
    if(!b.id||!b.generation||!b.bytes||b.gpu<4096||b.gpu>=kVaEnd||b.bytes>kVaEnd-b.gpu||
       ((b.gpu|b.bytes|b.bufferOffset|b.allocationBytes)&4095)||
       b.bufferOffset>b.allocationBytes||b.bytes>b.allocationBytes-b.bufferOffset||
       (controlBytes&&(b.bytes!=controlBytes||b.bufferOffset||b.allocationBytes!=controlBytes)))
        return Status::InvalidBinding;
    return b.access==access?Status::Ok:Status::Permission;
}
const gfxisolated::Binding& bindingAt(const Request& r,size_t n) {
    if(n<r.variantCount)return r.variants[n].shader;
    n-=r.variantCount;
    if(n<r.resourceCount)return r.resources[n].mapping;
    return n==r.resourceCount?r.indirect:r.parameters;
}
bool localEqual(const uint32_t* a,const uint32_t* b) {
    return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2];
}
uint32_t packet(uint32_t op,uint32_t count,bool compute=true) {
    return 0xc0000000U|(count<<16)|(op<<8)|(compute?2U:0U);
}
// The same bounded emitter can count, write or compare; it allocates no command
// array on the kernel stack. All checks precede the writing pass.
struct Stream {
    uint32_t* output;const uint32_t* comparison;size_t comparisonWords,count;bool mismatch;
    void add(uint32_t v) {
        if(count>=kMaxCommandDwords)mismatch=true;
        else {
            if(output)output[count]=v;
            if(comparison&&(count>=comparisonWords||comparison[count]!=v))mismatch=true;
        }
        ++count;
    }
    void reg(uint32_t offset,uint32_t value){add(packet(0x76,1));add(offset);add(value);}
    void acquire() {
        // Full VA range. ENGINE_SEL=0 is PFP, implemented by hardware as
        // ME ACQUIRE + PFP_SYNC_ME. Invalidation: I$, scalar/vector/L1,
        // L2 + metadata; writeback: L2 + metadata. This is not an idle wait.
        add(packet(0x58,6,false));add(0);add(UINT32_MAX);add(0x00ffffff);
        add(0);add(0);add(10);add(0xc3b1);
    }
    void barrier() {
        // Completion must precede cache operations: otherwise one region can
        // read stale data or overwrite storage still used by an earlier one.
        add(packet(0x46,0,false));add(0x407); // CS_PARTIAL_FLUSH, event index4
        acquire();
    }
};
void commands(const Request& r,const Workspace& w,Stream& s) {
    s.add(packet(0x28,1,false));s.add(0x80000000);s.add(0x80000000);
    s.acquire();
    s.add(packet(0x76,3));s.add(0x204);s.add(0);s.add(0);s.add(0); // START XYZ
    s.reg(0x218,0);s.reg(0x222,0); // scratch and REQ_CTRL disabled
    s.add(packet(0x76,4));s.add(0x224);for(unsigned n=0;n<4;++n)s.add(0);
    s.add(packet(0x79,1,false));s.add(0x22a);s.add(0); // checksum
    for(unsigned group=0;group<2;++group) {
        s.add(packet(0x9b,2));s.add(0x30000000U|(group?0x219:0x216));s.add(UINT32_MAX);s.add(UINT32_MAX);
    }
    s.add(packet(0x9b,4));s.add(0x3000022b);s.add(UINT32_MAX);s.add(UINT32_MAX);s.add(0);s.add(0);
    s.reg(0x22f,0);s.reg(0x27d,0); // dispatch interleave and tunnel disabled
    for(size_t n=0;n<w.regionCount;++n) {
        const Region& region=w.regions[n];const Variant& variant=r.variants[region.variant];
        const gfx11product::Report& program=w.variants[region.variant];
        s.add(packet(0x76,2));s.add(0x20c);
        s.add(uint32_t(variant.shader.gpu>>8));s.add(uint32_t(variant.shader.gpu>>40));
        s.reg(0x212,program.rsrc1);s.reg(0x213,program.rsrc2);s.reg(0x228,program.rsrc3);
        s.reg(0x207,region.local[0]);s.reg(0x208,region.local[1]);s.reg(0x209,region.local[2]);
        const uint64_t push=r.parameters.gpu+kDescriptorBytes+n*kPushBytes;
        s.add(packet(0x76,4));s.add(0x240);
        s.add(uint32_t(r.parameters.gpu));s.add(uint32_t(r.parameters.gpu>>32));
        s.add(uint32_t(push));s.add(uint32_t(push>>32));
        s.reg(0x215,0); // no external wave or scratch requirements
        s.add(packet(0x15,3));
        for(unsigned d=0;d<3;++d)s.add(region.groups[d]);
        s.add(0x8001); // COMPUTE_SHADER_EN, CS_W32_EN; direct, no predication
        s.barrier();
    }
    s.add(packet(0x42,0,false));s.add(0); // final PFP_SYNC_ME before queue-owned fence
    while(s.count<kMaxCommandDwords&&(s.count&7))s.add(0xffff1000);
}
uint32_t parameterWord(const Request& r,const Workspace& w,size_t word) {
    if(word<kDescriptorBytes/4) {
        const uint32_t bindingIndex=uint32_t(word/4);
        for(size_t n=0;n<r.resourceCount;++n)if(r.resources[n].binding==bindingIndex) {
            const Resource& resource=r.resources[n];
            const uint64_t address=resource.mapping.gpu+resource.offset;
            switch(word&3) {
                case 0:return uint32_t(address);
                case 1:return uint32_t(address>>32);
                case 2:return uint32_t(resource.bytes);
                default:return 0x30016fac; // stride0, byte bounds, raw OOB
            }
        }
        return 0;
    }
    word-=kDescriptorBytes/4;
    return word<w.regionCount*12?w.regions[word/12].push[word%12]:0;
}
uint8_t parameterByte(const Request& r,const Workspace& w,size_t n) {
    return uint8_t(parameterWord(r,w,n/4)>>(8*(n&3)));
}
} // namespace
Status prepare(const Request& r,Workspace& w) {
    w={};
    if(!r.owner||!r.epoch||r.vmid<1||r.vmid>7||!r.variantCount||r.variantCount>kMaxVariants||
       r.resourceCount>kMaxBindings||!pointer(r.variants,r.variantCount*sizeof(Variant),alignof(Variant))||
       (r.resourceCount&&!pointer(r.resources,r.resourceCount*sizeof(Resource),alignof(Resource))))
        return Status::InvalidArgument;
    uint64_t threads=1,localThreads=1;
    for(unsigned d=0;d<3;++d) {
        if(!r.grid[d]||!r.nominalLocal[d]||r.nominalLocal[d]>gfx11product::kMaxLocalThreads||
           threads>UINT64_MAX/r.grid[d])return Status::InvalidDimensions;
        threads*=r.grid[d];localThreads*=r.nominalLocal[d];
    }
    if(localThreads>gfx11product::kMaxLocalThreads)return Status::InvalidDimensions;
    for(size_t n=0;n<r.variantCount;++n) {
        const Variant& v=r.variants[n];
        if(!v.program.codeBytes||v.program.codeBytes>gfx11product::kMaxCodeBytes)return Status::InvalidShader;
        const size_t pageBytes=(v.program.codeBytes+4095)&~size_t(4095);
        Status result=binding(r,v.shader,gfxisolated::Read|gfxisolated::Execute,pageBytes);
        if(result!=Status::Ok)return result;
        if(!pointer(v.program.code,pageBytes))return Status::InvalidShader;
        for(size_t byte=v.program.codeBytes;byte<pageBytes;++byte)
            if(v.program.code[byte])return Status::InvalidShader;
        if(gfx11product::validate(v.program,w.variants[n])!=gfx11product::Status::Ok)return Status::InvalidShader;
        w.readMask|=w.variants[n].readMask;w.writeMask|=w.variants[n].writeMask;
        for(size_t p=0;p<n;++p)
            if(localEqual(v.program.local,r.variants[p].program.local))return Status::InvalidShader;
    }
    bool used[kMaxVariants]={};
    for(unsigned mask=0;mask<8;++mask) {
        Region region={};bool nonempty=true;
        for(unsigned d=0;d<3;++d) {
            const uint32_t full=r.grid[d]/r.nominalLocal[d],tail=r.grid[d]%r.nominalLocal[d];
            region.local[d]=r.nominalLocal[d];region.groups[d]=full;
            region.push[d]=r.grid[d];region.push[9+d]=full+(tail?1:0);
            if(mask&(1U<<d)) {
                if(!tail)nonempty=false;
                region.local[d]=tail;region.groups[d]=1;
                region.push[3+d]=full*r.nominalLocal[d];region.push[6+d]=full;
            }
            if(!region.groups[d])nonempty=false;
        }
        if(!nonempty)continue;
        size_t variant=0;
        while(variant<r.variantCount&&!localEqual(region.local,r.variants[variant].program.local))++variant;
        if(variant==r.variantCount)return Status::MissingVariant;
        used[variant]=true;region.variant=uint32_t(variant);w.regions[w.regionCount++]=region;
    }
    for(size_t n=0;n<r.variantCount;++n)if(!used[n])return Status::UnusedVariant;
    const uint32_t needed=w.readMask|w.writeMask;uint32_t supplied=0;
    for(size_t n=0;n<r.resourceCount;++n) {
        const Resource& resource=r.resources[n];
        if(resource.binding>=kMaxBindings)return Status::InvalidResources;
        const uint32_t bit=uint32_t(1)<<resource.binding;
        if(!(needed&bit)||(supplied&bit))return Status::InvalidResources;
        supplied|=bit;
        Status result=binding(r,resource.mapping,gfxisolated::Read|((w.writeMask&bit)?gfxisolated::Write:0));
        if(result!=Status::Ok)return result;
        if(!resource.bytes||resource.bytes>UINT32_MAX||(resource.offset&3)||(resource.bytes&3)||
           resource.offset>resource.mapping.bytes||resource.bytes>resource.mapping.bytes-resource.offset)
            return Status::InvalidResources;
    }
    if(supplied!=needed)return Status::InvalidResources;
    Status result=binding(r,r.indirect,gfxisolated::Read|gfxisolated::Execute,kControlBytes);
    if(result!=Status::Ok)return result;
    result=binding(r,r.parameters,gfxisolated::Read,kControlBytes);if(result!=Status::Ok)return result;
    const size_t bindings=r.variantCount+r.resourceCount+2;
    for(size_t n=0;n<bindings;++n)for(size_t p=0;p<n;++p) {
        const auto& a=bindingAt(r,n);const auto& b=bindingAt(r,p);
        if(a.id==b.id)return Status::AliasedStorage;
        if(a.gpu<b.gpu+b.bytes&&b.gpu<a.gpu+a.bytes)return Status::Overlap;
    }
    Stream count={};commands(r,w,count);
    if(count.mismatch||count.count>kMaxCommandDwords)return Status::BufferTooSmall;
    w.commandWords=count.count;return Status::Ok;
}
Status parameterImage(const Request& r,Workspace& w,uint8_t* out,size_t capacity) {
    const Status result=prepare(r,w);if(result!=Status::Ok)return result;
    if(!pointer(out,kControlBytes))return Status::InvalidArgument;
    if(capacity<kControlBytes)return Status::BufferTooSmall;
    for(size_t n=0;n<kControlBytes;++n)out[n]=parameterByte(r,w,n);
    return Status::Ok;
}
Status encode(const Request& r,Workspace& w,uint32_t* out,size_t capacity,size_t& words) {
    const Status result=prepare(r,w);if(result!=Status::Ok)return result;
    if(!pointer(out,w.commandWords*4,alignof(uint32_t)))return Status::InvalidArgument;
    if(capacity<w.commandWords)return Status::BufferTooSmall;
    Stream stream={};stream.output=out;commands(r,w,stream);words=stream.count;return Status::Ok;
}
Status validate(const Request& r,Workspace& w,const uint8_t* parameters,size_t parameterBytes,
    const uint32_t* commandsInput,size_t words) {
    const Status result=prepare(r,w);if(result!=Status::Ok)return result;
    if(parameterBytes!=kControlBytes||!words||words>kMaxCommandDwords||
       !pointer(parameters,kControlBytes)||!pointer(commandsInput,words*4,alignof(uint32_t)))
        return Status::InvalidArgument;
    for(size_t n=0;n<kControlBytes;++n)
        if(parameters[n]!=parameterByte(r,w,n))return Status::InvalidParameters;
    Stream comparison={};comparison.comparison=commandsInput;comparison.comparisonWords=words;
    commands(r,w,comparison);
    return comparison.mismatch||comparison.count!=words?Status::InvalidStream:Status::Ok;
}
const char* statusString(Status s){switch(s){
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(ForeignContext);NAME(InvalidBinding);NAME(Permission);
    NAME(AliasedStorage);NAME(Overlap);NAME(InvalidShader);NAME(InvalidDimensions);NAME(MissingVariant);
    NAME(UnusedVariant);NAME(InvalidResources);NAME(BufferTooSmall);NAME(InvalidParameters);NAME(InvalidStream);
#undef NAME
    }return "Unknown";
}
} }
