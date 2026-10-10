// SPDX-License-Identifier: MIT
// Register names/offsets: Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0
// dcn_3_2_0_offset.h. Read/stop semantics: dcn32_optc.c, dcn10_optc.c,
// dcn32_hubp.c, dcn30_hubp.c, dcn30_hubbub.c and dcn20_vmid.c.
#include "hackynavi/boot_display_context.hpp"
namespace hackynavi { namespace bootdisplaycontext { namespace {
const uint16_t pipeOffsets[kPipeRegisters]={0x52e,0x1b41,0x1b85,0x5f4,0x5f6,0x5f7};
const uint16_t pipeStrides[kPipeRegisters]={1,0x80,0x80,0xdc,0xdc,0xdc};
const uint16_t vmOffsets[kVmRegisters]={0x47a,0x559,0x55a,0x55b,0x55c,0x55d,0x55e,0x55f};
bool on(uint32_t value){return value==0 || value==0x10000000;}
bool offset(uint64_t base,uint32_t relative,uint64_t bytes,uint32_t& out) {
    if(base>UINT32_MAX/4 || relative>UINT32_MAX/4-base || bytes<4)return false;
    const uint64_t address=(base+relative)*4;
    if(address>bytes-4)return false;
    out=uint32_t(address);return true;
}
bool populate(uint64_t base,uint64_t bytes,Layout& out) {
    out={};out.barBytes=bytes;
    for(size_t p=0;p<kPipeCount;++p) {
        if(!offset(base,0x81+uint32_t(p)*2,bytes,out.power[p]) ||
           !offset(base,0x80+uint32_t(p)*2,bytes,out.powerConfig[p]))return false;
        for(size_t r=0;r<kPipeRegisters;++r)
            if(!offset(base,pipeOffsets[r]+uint32_t(p)*pipeStrides[r],bytes,
                       out.registers[p*kPipeRegisters+r]))return false;
    }
    for(size_t r=0;r<kVmRegisters;++r)
        if(!offset(base,vmOffsets[r],bytes,out.registers[24+r]))return false;
    // Fixed, single-base definitions are distinct; verify rather than trusting
    // a caller's valid flag or a generated table when reconstructing a layout.
    for(size_t r=0;r<kRegisterCount;++r) {
        for(size_t q=0;q<r;++q)if(out.registers[r]==out.registers[q])return false;
        for(size_t p=0;p<kPipeCount;++p)
            if(out.registers[r]==out.power[p] || out.registers[r]==out.powerConfig[p])return false;
    }
    out.valid=true;return true;
}
bool validLayout(const Layout& l) {
    if(!l.valid || (l.registers[24]&3) || l.registers[24]/4<vmOffsets[0])return false;
    Layout expected={};
    if(!populate(l.registers[24]/4-vmOffsets[0],l.barBytes,expected))return false;
    for(size_t p=0;p<kPipeCount;++p)
        if(l.power[p]!=expected.power[p] || l.powerConfig[p]!=expected.powerConfig[p])return false;
    for(size_t r=0;r<kRegisterCount;++r)if(l.registers[r]!=expected.registers[r])return false;
    return true;
}
Status read(const Io& io,uint32_t address,uint32_t& value,Capture& c) {
    c.lastRegister=address;c.lastValue=0;c.lastReadSucceeded=false;
    if(!io.valid(io.context))return Status::AccessDenied;
    ++c.readsAttempted;uint32_t raw=0;
    if(!io.read32(io.context,address,raw))return Status::ReadFailed;
    c.lastReadSucceeded=true;c.lastValue=raw;value=raw;
    if(!io.valid(io.context))return Status::AccessDenied;
    return raw==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status guard(const Layout& l,const Io& io,size_t pass,size_t p,bool initial,Capture& c) {
    c.failedPipe=uint8_t(p);c.lastPowerProbe={};
    auto& probe=c.lastPowerProbe;probe.pipe=uint8_t(p);
    auto status=read(io,l.power[p],probe.statusBefore,c);
    if(c.lastReadSucceeded)probe.readsSucceededMask|=1;
    if(status!=Status::Ok)return status;
    if(!on(probe.statusBefore))return Status::PowerUnavailable;
    status=read(io,l.powerConfig[p],probe.config,c);
    if(c.lastReadSucceeded)probe.readsSucceededMask|=2;
    if(status!=Status::Ok)return status;
    if(probe.config>1)return Status::PowerUnavailable;
    status=read(io,l.power[p],probe.statusAfter,c);
    if(c.lastReadSucceeded)probe.readsSucceededMask|=4;
    if(status!=Status::Ok)return status;
    if(probe.statusAfter!=probe.statusBefore)return Status::PowerUnavailable;
    if(initial) {
        c.powerStatus[pass][p]=probe.statusBefore;c.powerConfig[pass][p]=probe.config;
        c.powerValidMask[pass]|=uint8_t(1U<<p);
        if(pass && (c.powerStatus[0][p]!=probe.statusBefore || c.powerConfig[0][p]!=probe.config)) {
            c.powerChangedMask|=uint8_t(1U<<p);return Status::Changed;
        }
    } else if(c.powerStatus[pass][p]!=probe.statusBefore || c.powerConfig[pass][p]!=probe.config) {
        c.powerChangedMask|=uint8_t(1U<<p);return Status::Changed;
    }
    return Status::Ok;
}
Status sample(const Layout& l,const Io& io,size_t pass,size_t r,Capture& c) {
    c.failedRegisterIndex=uint8_t(r);const uint32_t bit=uint32_t(1)<<r,before=c.readsAttempted;
    const auto status=read(io,l.registers[r],c.values[pass][r],c);
    if(c.readsAttempted!=before)c.attemptedMask[pass]|=bit;
    if(c.lastReadSucceeded)c.readSucceededMask[pass]|=bit;
    if(status==Status::Ok) {
        c.validMask[pass]|=bit;
        if(pass && (c.validMask[0]&bit) && c.values[0][r]!=c.values[1][r])c.changedMask|=bit;
    }
    return status;
}
} // namespace
Status buildLayout(const uint8_t* discovery,size_t bytes,uint64_t barBytes,Layout& out) {
    out={};bootdisplay::Layout base={};
    const auto status=bootdisplay::buildLayout(discovery,bytes,barBytes,base);
    if(status!=bootdisplay::Status::Ok) {
        if(status==bootdisplay::Status::DiscoveryRejected)return Status::DiscoveryRejected;
        if(status==bootdisplay::Status::OutsideBar)return Status::OutsideBar;
        return Status::UnsupportedDmu;
    }
    Layout selected={};
    if(!populate(base.global[0]/4-0x475,barBytes,selected))return Status::OutsideBar;
    out=selected;return Status::Ok;
}
Status capture(const Layout& l,const Io& io,Capture& c) {
    c={};c.failedPipe=c.failedPass=c.failedRegisterIndex=0xff;c.lastPowerProbe.pipe=0xff;
    if(!io.valid || !io.read32 || !validLayout(l))return Status::InvalidArgument;
    for(size_t pass=0;pass<kPasses;++pass) {
        c.failedPass=uint8_t(pass);c.failedRegisterIndex=0xff;
        for(size_t p=0;p<kPipeCount;++p) {
            const auto status=guard(l,io,pass,p,true,c);if(status!=Status::Ok)return status;
        }
        c.failedPipe=0xff;
        // Gather timing and guarded HUBP evidence before VM context words:
        // an all-ones VM word must not conceal earlier useful topology reads.
        // No register is read through an indirect port.
        for(size_t p=0;p<kPipeCount;++p)for(size_t r=0;r<3;++r) {
            const auto status=sample(l,io,pass,p*kPipeRegisters+r,c);if(status!=Status::Ok)return status;
        }
        for(size_t p=0;p<kPipeCount;++p) {
            for(size_t r=3;r<kPipeRegisters;++r) {
                c.failedRegisterIndex=uint8_t(p*kPipeRegisters+r);
                auto status=guard(l,io,pass,p,false,c);if(status!=Status::Ok)return status;
                status=sample(l,io,pass,p*kPipeRegisters+r,c);if(status!=Status::Ok)return status;
            }
            c.failedRegisterIndex=0xff;
            const auto status=guard(l,io,pass,p,false,c);if(status!=Status::Ok)return status;
        }
        c.failedRegisterIndex=0xff;
        c.failedPipe=0xff;
        for(size_t r=24;r<kRegisterCount;++r) {
            const auto status=sample(l,io,pass,r,c);if(status!=Status::Ok)return status;
        }
        c.failedRegisterIndex=0xff;
        for(size_t p=0;p<kPipeCount;++p) {
            const auto status=guard(l,io,pass,p,false,c);if(status!=Status::Ok)return status;
        }
        ++c.completedPasses;
    }
    c.readsComplete=true;c.failedPipe=c.failedPass=c.failedRegisterIndex=0xff;
    return c.changedMask?Status::Changed:Status::Ok;
}
const char* statusString(Status s) {
    switch(s) {
#define NAME(x) case Status::x:return #x
        NAME(Ok);NAME(InvalidArgument);NAME(DiscoveryRejected);NAME(UnsupportedDmu);NAME(OutsideBar);
        NAME(AccessDenied);NAME(ReadFailed);NAME(DeviceUnavailable);NAME(PowerUnavailable);NAME(Changed);
#undef NAME
    }return "Unknown";
}
} }
