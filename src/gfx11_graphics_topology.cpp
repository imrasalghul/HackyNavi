// SPDX-License-Identifier: MIT
// Facts/field layouts: Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0,
// discovery.h GC1.2, amdgpu_discovery_get_gfx_info; gfx_v11_0.c:2010-2107,
// 7310-7398; gc_11_0_0_offset/sh_mask.h. CU distribution: Mesa35b085c4,
// ac_gpu_info.c:1244-1313. No user WGP-disable policy is written here.
#include "hackynavi/gfx11_graphics_topology.hpp"
#include "hackynavi/discovery.hpp"
#include "hackynavi/ip_layout.hpp"
namespace hackynavi { namespace graphicstopology { namespace {
uint16_t u16(const uint8_t* p){return uint16_t(uint16_t(p[0])|uint16_t(p[1])<<8);}
uint32_t u32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
uint32_t mask(unsigned n){return (uint32_t(1)<<n)-1;}
unsigned count(uint32_t v){unsigned n=0;while(v){v&=v-1;++n;}return n;}
bool metadata(const Metadata& m) {
    return m.maxSe && m.maxSe<=kMaxSe && m.maxSaPerSe && m.maxSaPerSe<=kMaxSa &&
        m.maxRbPerSe && m.maxRbPerSe<=28/m.maxSe && !(m.maxRbPerSe%m.maxSaPerSe) &&
        m.maxWgpPerSa && m.maxWgpPerSa<=16;
}
bool layout(const Layout& l) {
    const uint32_t all[]={l.selector,l.saHardware,l.saUser,l.rbHardware,l.rbUser,l.wgpHardware,l.wgpUser};
    for(unsigned n=0;n<7;++n) {
        if((all[n]&3) || l.barBytes<4 || all[n]>l.barBytes-4 ||
            all[n]==0 || all[n]==4 || all[n]==0x18 || (all[n]>=0x7f000 && all[n]<0x80000))return false;
        for(unsigned j=0;j<n;++j)if(all[n]==all[j])return false;
    }
    return true;
}
Status read(const Io& io,uint32_t address,uint32_t& value,Trace& t) {
    ++t.readsAttempted;
    if(!io.read32(io.context,address,value))return Status::TransportError;
    return value==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status selector(const Layout& l,const Io& io,uint32_t expected,Trace& t) {
    uint32_t value=0;const Status status=read(io,l.selector,value,t);
    return status!=Status::Ok?status:value!=expected?Status::Changed:Status::Ok;
}
Status select(const Layout& l,const Io& io,uint32_t value,bool restore,Trace& t) {
    ++t.selectorWritesAttempted;if(restore)++t.restoreWritesAttempted;
    if(!io.write32(io.context,l.selector,value))return Status::TransportError;
    ++t.selectorWritesCompleted;
    const Status status=selector(l,io,value,t);
    if(status==Status::Ok && restore)++t.restoreWritesCompleted;
    return status;
}
Status globals(const Layout& l,const Io& io,Raw& r,Trace& t) {
    const uint32_t address[]={l.saHardware,l.saUser,l.rbHardware,l.rbUser};
    uint32_t* values[]={&r.saHardware,&r.saUser,&r.rbHardware,&r.rbUser};
    for(unsigned n=0;n<4;++n){const auto status=read(io,address[n],*values[n],t);if(status!=Status::Ok)return status;}
    return Status::Ok;
}
bool sameGlobals(const Raw& a,const Raw& b) {
    return a.saHardware==b.saHardware && a.saUser==b.saUser && a.rbHardware==b.rbHardware && a.rbUser==b.rbUser;
}
uint32_t activeSa(const Metadata& m,const Raw& r) {
    return mask(m.maxSe*m.maxSaPerSe)&~((r.saHardware|r.saUser)>>8);
}
Status sweep(const Metadata& m,const Layout& l,const Io& io,Raw& out,Trace& t) {
    Status status=read(io,l.selector,out.originalSelector,t);
    if(status!=Status::Ok)return status;
    if(!validSelector(out.originalSelector,m))return Status::InvalidSelector;
    if(t.originalReadComplete && out.originalSelector!=t.originalSelector)return Status::Changed;
    // Establish a stable exact prior selector before any mutation. One read
    // alone cannot certify the value that a later restoration will publish.
    status=selector(l,io,out.originalSelector,t);if(status!=Status::Ok)return status;
    t.originalSelector=out.originalSelector;t.originalReadComplete=true;
    status=globals(l,io,out,t);if(status!=Status::Ok)return status;
    const uint32_t sa=activeSa(m,out);bool changedSelector=false;
    for(unsigned se=0;se<m.maxSe;++se)for(unsigned sh=0;sh<m.maxSaPerSe;++sh) {
        const unsigned bit=se*m.maxSaPerSe+sh,slot=se*kMaxSa+sh;
        if(!(sa&(1u<<bit)))continue; // never inspect a disabled shader array
        const uint32_t selected=0x40000000u|(se<<16)|(sh<<8);
        status=select(l,io,selected,false,t);if(status!=Status::Ok)return status;
        changedSelector=true;
        status=read(io,l.wgpHardware,out.wgpHardware[slot],t);if(status!=Status::Ok)return status;
        status=read(io,l.wgpUser,out.wgpUser[slot],t);if(status!=Status::Ok)return status;
        status=selector(l,io,selected,t);if(status!=Status::Ok)return status;
        out.observedSa|=1u<<bit;
    }
    Raw after{};status=globals(l,io,after,t);if(status!=Status::Ok)return status;
    if(!sameGlobals(out,after))return Status::Changed;
    // No restoration after a failed transport/selector/global observation.
    // This is a normal, journaled last operation of a fully successful sweep.
    status=changedSelector?select(l,io,out.originalSelector,true,t):selector(l,io,out.originalSelector,t);
    if(status!=Status::Ok)return status;
    ++t.sweepsCompleted;return Status::Ok;
}
Status derive(const Metadata& m,const Raw& raw,Facts& f) {
    f={};f.maxSe=m.maxSe;f.maxSaPerSe=m.maxSaPerSe;f.maxRenderBackends=m.maxSe*m.maxRbPerSe;
    f.activeSaMask=activeSa(m,raw);
    if(raw.observedSa!=f.activeSaMask)return Status::Changed;
    const unsigned rbPerSa=m.maxRbPerSe/m.maxSaPerSe;
    uint32_t allowedRb=0;
    for(unsigned se=0;se<m.maxSe;++se) {
        bool enabled=false;
        for(unsigned sa=0;sa<m.maxSaPerSe;++sa) {
            const unsigned bit=se*m.maxSaPerSe+sa,slot=se*kMaxSa+sa;
            if(!(f.activeSaMask&(1u<<bit)))continue;
            const uint32_t wgps=mask(m.maxWgpPerSa)&~((raw.wgpHardware[slot]|raw.wgpUser[slot])>>16);
            for(unsigned wgp=0;wgp<m.maxWgpPerSa;++wgp)if(wgps&(1u<<wgp))f.cuMask[slot]|=3u<<(wgp*2);
            f.activeCu+=count(f.cuMask[slot]);enabled|=f.cuMask[slot]!=0;
            allowedRb|=mask(rbPerSa)<<(bit*rbPerSa);
        }
        f.activeSe+=unsigned(enabled);
    }
    f.activeRbMask=allowedRb&mask(f.maxRenderBackends)&~((raw.rbHardware|raw.rbUser)>>4);
    if(!f.activeSe || !f.activeCu || !f.activeRbMask)return Status::NoActiveUnits;
    f.minGoodCuPerSa=(f.activeCu/(f.activeSe*m.maxSaPerSe*2))*2;
    return f.minGoodCuPerSa?Status::Ok:Status::NoActiveUnits;
}
}
bool validSelector(uint32_t value,const Metadata& m) {
    if(!metadata(m) || (value&0x1f0000ffu))return false; // reserved fields/instance0 only
    const unsigned se=(value>>16)&255,sa=(value>>8)&255;
    return (value&0x80000000u ? se==0 : se<m.maxSe) && (value&0x20000000u ? sa==0 : sa<m.maxSaPerSe);
}
Status build(const uint8_t* data,size_t bytes,uint64_t bar,Metadata& m,Layout& l) {
    m={};l={};DiscoveryInfo info{};Navi31IpLayout ips{};IpLayoutDiagnostic diagnostic{};
    if(parseDiscovery(data,bytes,info)!=DiscoveryStatus::Ok ||
        buildNavi31IpLayout(data,bytes,ips,diagnostic)!=IpLayoutStatus::Ok)return Status::DiscoveryRejected;
    const auto& table=info.tables[1];
    if(info.tableCount<=1 || table.size!=132 || !table.offset)return Status::UnsupportedMetadata;
    const uint8_t* p=data+table.offset;
    if(u32(p)!=0x4347 || u16(p+4)!=1 || u16(p+6)!=2 || u32(p+8)!=132)return Status::UnsupportedMetadata;
    const uint32_t wgp0=u32(p+16),wgp1=u32(p+20);
    if(wgp0>16 || wgp1>16 || wgp0+wgp1>16)return Status::UnsupportedMetadata;
    const Metadata declared={u32(p+12),u32(p+12+16*4),u32(p+24),wgp0+wgp1};
    if(!metadata(declared))return Status::UnsupportedMetadata;
    Layout candidate{};candidate.barBytes=bar;
    uint32_t* targets[]={&candidate.selector,&candidate.saHardware,&candidate.saUser,&candidate.rbHardware,
        &candidate.rbUser,&candidate.wgpHardware,&candidate.wgpUser};
    const uint32_t bases[]={1,0,1,0,1,0,1},offsets[]={0x2200,0xfe9,0x5b92,0x13dd,0x5b94,0x100f,0x5b90};
    for(unsigned n=0;n<7;++n){ResolvedRegister reg{};
        if(resolveNavi31Register(ips,Navi31Block::Gc,bases[n],offsets[n],bar,reg)!=IpLayoutStatus::Ok)return Status::InvalidLayout;
        *targets[n]=reg.mmioByteOffset;}
    if(!layout(candidate))return Status::InvalidLayout;
    m=declared;l=candidate;return Status::Ok;
}
Status capture(const Metadata& m,const Layout& l,const Io& io,Snapshot& out,Trace& trace) {
    out={};trace={};
    if(!io.read32 || !io.write32)return Status::InvalidArgument;
    if(!metadata(m))return Status::UnsupportedMetadata;
    if(!layout(l))return Status::InvalidLayout;
    Raw first{},second{};Status status=sweep(m,l,io,first,trace);
    if(status!=Status::Ok)return status;
    status=sweep(m,l,io,second,trace);if(status!=Status::Ok)return status;
    if(first.originalSelector!=second.originalSelector || first.observedSa!=second.observedSa || !sameGlobals(first,second))return Status::Changed;
    for(unsigned n=0;n<kSlots;++n)if(first.wgpHardware[n]!=second.wgpHardware[n] || first.wgpUser[n]!=second.wgpUser[n])return Status::Changed;
    Facts facts{};status=derive(m,second,facts);if(status!=Status::Ok)return status;
    out={second,facts};return Status::Ok;
}
} }
