// SPDX-License-Identifier: MIT
// Register definitions/protocol: Linux 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0,
// gfx_v11_0.c:4807, nbio_v4_3.c:get_rev_id, amdgpu_reg_access.c RSMU accessors.
#include "hackynavi/gfx11_color_topology.hpp"
#include "hackynavi/ip_layout.hpp"
#include "hackynavi/nbio.hpp"

namespace hackynavi { namespace colortopology { namespace {
bool inBar(uint32_t address, uint64_t bytes) {
    return !(address & 3) && bytes >= 4 && address <= bytes - 4;
}
bool forbidden(uint32_t address) {
    return !address || address == 4 || address == 0x18 ||
        (address >= 0x7f000 && address < 0x80000);
}
bool valid(const Layout& l) {
    const uint32_t addresses[] = {l.gbAddrConfig,l.revisionStrap,l.index,l.data,l.indexHi};
    for (unsigned i=0;i<5;++i) {
        if ((addresses[i]&3) || forbidden(addresses[i])) return false;
        for(unsigned j=0;j<i;++j) if(addresses[i]==addresses[j]) return false;
        if(i!=1 && !inBar(addresses[i],l.barBytes)) return false;
    }
    return uint64_t(l.index)+4==l.data && uint64_t(l.index)+8==l.indexHi &&
        l.revisionStrap>=0x16*4;
}
bool same(const Identity& a,const Identity& b) {
    return a.vendorDevice==b.vendorDevice && a.revisionClass==b.revisionClass &&
        a.subsystem==b.subsystem && a.bar4==b.bar4 && a.bar5==b.bar5 &&
        a.command==b.command && a.headerType==b.headerType && a.reserved==b.reserved;
}
bool valid(const Identity& p) {
    return p.vendorDevice==0x744c1002 && p.revisionClass!=UINT32_MAX &&
        (p.revisionClass&255)==0xc8 && (p.revisionClass>>24)==3 &&
        p.subsystem!=UINT32_MAX && p.bar4!=UINT32_MAX &&
        ((p.bar4&1) || (p.bar4&6)!=4) && p.bar5!=UINT32_MAX &&
        !(p.bar5&7) && (p.bar5&~15u) && p.command!=0xffff && (p.command&6)==6 &&
        !(p.headerType&0x7f) && !p.reserved;
}
Status pci(const Io& io,Identity& value,Trace& trace) {
    ++trace.identitiesAttempted;
    if(!io.identity(io.context,value)) return Status::TransportError;
    return valid(value)?Status::Ok:Status::DeviceUnavailable;
}
Status read(const Io& io,uint32_t address,uint32_t& value,Trace& trace) {
    ++trace.readsAttempted;
    if(!io.read32(io.context,address,value)) return Status::TransportError;
    return value==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status snapshot(const Layout& l,const Io& io,Snapshot& out,Trace& trace) {
    Status status=pci(io,out.pci,trace);
    if(status!=Status::Ok) return status;
    status=read(io,l.gbAddrConfig,out.topology.gb_addr_config,trace);
    if(status!=Status::Ok) return status;
    if(inBar(l.revisionStrap,l.barBytes)) {
        status=read(io,l.revisionStrap,out.topology.revision_strap,trace);
    } else {
        uint32_t high=0,posting=0;
        status=read(io,l.indexHi,high,trace);
        if(status!=Status::Ok) return status;
        if(high) return Status::IndirectHighNotZero;
        ++trace.selectorWritesAttempted;
        if(!io.write32(io.context,l.index,l.revisionStrap)) return Status::TransportError;
        ++trace.selectorWritesCompleted;
        status=read(io,l.index,posting,trace);
        if(status!=Status::Ok) return status;
        ++trace.postingReadsCompleted;
        status=read(io,l.data,out.topology.revision_strap,trace);
    }
    if(status!=Status::Ok) return status;
    Identity after{};status=pci(io,after,trace);
    if(status!=Status::Ok) return status;
    if(!same(out.pci,after)) return Status::Changed;
    const HNColorRequest request={1,1};
    if(hn_color_target_validate_inputs(&out.topology,&request)!=HN_COLOR_OK)
        return Status::UnsupportedTopology;
    ++trace.snapshotsCompleted;return Status::Ok;
}
} // namespace
Status buildLayout(const uint8_t* data,size_t bytes,uint64_t bar,Layout& out) {
    out={};Navi31IpLayout gc{};IpLayoutDiagnostic diagnostic{};NbioLayout nbio{};
    if(buildNavi31IpLayout(data,bytes,gc,diagnostic)!=IpLayoutStatus::Ok ||
        buildNbioLayout(data,bytes,bar,nbio)!=NbioStatus::Ok) return Status::DiscoveryRejected;
    ResolvedRegister gb{};
    if(resolveNavi31Register(gc,Navi31Block::Gc,0,0x13de,bar,gb)!=IpLayoutStatus::Ok ||
        nbio.doorbellApertureRegister<(0xc0-0x16)*4) return Status::InvalidLayout;
    // Both registers use NBIF BASE2; derive its verified byte base through
    // buildNbioLayout rather than a fixed ASIC absolute-address table.
    const Layout candidate={gb.mmioByteOffset,nbio.doorbellApertureRegister-(0xc0-0x16)*4,
        nbio.rsmuIndexRegister,nbio.rsmuDataRegister,nbio.rsmuIndexHiRegister,bar};
    if(!valid(candidate)) return Status::InvalidLayout;
    out=candidate;return Status::Ok;
}
Status capture(const Layout& l,const Io& io,Snapshot& out,Trace& trace) {
    out={};trace={};
    if(!io.identity || !io.read32 || !io.write32) return Status::InvalidArgument;
    if(!valid(l)) return Status::InvalidLayout;
    Snapshot first{},second{};
    Status status=snapshot(l,io,first,trace);
    if(status!=Status::Ok) return status;
    status=snapshot(l,io,second,trace);
    if(status!=Status::Ok) return status;
    if(!same(first.pci,second.pci) ||
        first.topology.gb_addr_config!=second.topology.gb_addr_config ||
        first.topology.revision_strap!=second.topology.revision_strap) return Status::Changed;
    out=second;return Status::Ok;
}
} }
