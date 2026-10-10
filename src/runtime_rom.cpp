// SPDX-License-Identifier: MIT
// Protocol adapted from Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// amdgpu_bios.c:amdgpu_soc15_read_bios_from_rom/amdgpu_read_bios_from_rom,
// smuio_v13_0_6.c, nbio_v4_3.c:get_rom_offset, amdgpu_reg_access.c,
// smuio_13_0_6_offset.h and nbio_4_3_0_offset.h/sh_mask.h.
// Copyright 2008, 2022 Advanced Micro Devices, Inc.
// Copyright 2008 Red Hat Inc.
// Copyright 2009 Jerome Glisse.
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
#include "hackynavi/runtime_rom.hpp"
#include "hackynavi/nbio.hpp"
namespace hackynavi { namespace runtimerom {
namespace {
struct Smuio { unsigned count; bool supported; uint64_t base; };
void collect(const DiscoveryIp& ip,void* p) {
    if(ip.hardwareId!=4)return;
    auto& s=*static_cast<Smuio*>(p); ++s.count;
    s.supported=!ip.dieId && !ip.instance && ip.major==13 && !ip.minor && ip.revision==6 &&
        !ip.harvest && !ip.subRevision && !ip.variant && ip.baseAddressCount;
    if(s.supported)s.base=ip.baseAddresses[0];
}
bool inBar(uint32_t reg,uint64_t bytes) {return bytes>=4 && reg<=bytes-4;}
bool conflict(uint32_t reg) {return reg<0x1c || (reg>=0x7f000 && reg<0x80000);}
Status validate(const Layout& l) {
    if(!l.valid || l.romIndex<0xe4*4 || l.romOffsetControl<0xcc23*4 ||
        uint64_t(l.romIndex)+4!=l.romData || uint64_t(l.rsmuIndex)+4!=l.rsmuData ||
        uint64_t(l.rsmuData)+4!=l.rsmuIndexHigh)return Status::InvalidLayout;
    const uint32_t regs[]={l.romIndex,l.romData,l.romOffsetControl,l.rsmuIndex,l.rsmuData,l.rsmuIndexHigh};
    for(unsigned i=0;i<6;++i) {
        if((regs[i]&3) || conflict(regs[i]))return Status::InvalidLayout;
        for(unsigned j=0;j<i;++j)if(regs[i]==regs[j])return Status::InvalidLayout;
        if(i!=2 && !inBar(regs[i],l.barBytes))return Status::OutsideBar;
    }
    return Status::Ok;
}
Status finish(Result& r,Status s) { r.status=s; return s; }
bool read(const Io& io,uint32_t reg,uint32_t& value,Result& r) {
    ++r.reads; r.lastRegister=reg;
    if(!io.read32(io.context,reg,value)) {finish(r,Status::ReadFailed); return false;}
    r.lastValue=value; return true;
}
bool write(const Io& io,uint32_t reg,uint32_t value,Result& r) {
    ++r.writes; r.lastRegister=reg; r.lastValue=value; r.selectorWritesMayHaveOccurred=true;
    if(!io.write32(io.context,reg,value)) {finish(r,Status::WriteFailed); return false;}return true;
}
bool bank(const Layout& l,const Io& io,uint32_t& value,Result& r) {
    if(inBar(l.romOffsetControl,l.barBytes)) {
        if(!read(io,l.romOffsetControl,value,r))return false;
    } else {
        uint32_t high=0,posting=0;
        if(!read(io,l.rsmuIndexHigh,high,r))return false;
        if(high==UINT32_MAX) {finish(r,Status::DeviceUnavailable); return false;}
        if(high) {finish(r,Status::IndirectHighNotZero); return false;}
        if(!write(io,l.rsmuIndex,l.romOffsetControl,r) || !read(io,l.rsmuIndex,posting,r) ||
            !read(io,l.rsmuData,value,r))return false;
    }
    if(value==UINT32_MAX) {finish(r,Status::DeviceUnavailable); return false;}
    value=(value&0x7f)<<17; return true;
}
bool stream(const Layout& l,const Io& io,uint8_t* out,size_t from,size_t end,Result& r) {
    for(size_t at=from;at<end;at+=4) {
        uint32_t value=0; if(!read(io,l.romData,value,r))return false;
        for(unsigned b=0;b<4;++b)out[at+b]=uint8_t(value>>(b*8));
        r.bytesRead=at+4;
    }
    return true;
}
}
Status buildLayout(const uint8_t* bytes,size_t size,uint64_t bar,Layout& out) {
    out=Layout{}; NbioLayout nbio={};
    const auto status=buildNbioLayout(bytes,size,bar,nbio);
    if(status!=NbioStatus::Ok) {
        if(status==NbioStatus::DiscoveryRejected)return Status::DiscoveryRejected;
        if(status==NbioStatus::AddressOverflow)return Status::AddressOverflow;
        if(status==NbioStatus::OutsideBar)return Status::OutsideBar;
        return Status::UnsupportedIp;
    }
    Smuio smu={}; DiscoveryInfo discovery={};
    if(parseDiscovery(bytes,size,discovery,collect,&smu)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(discovery.baseAddresses64Bit || smu.count!=1 || !smu.supported)return Status::UnsupportedIp;
    if(smu.base>UINT32_MAX/4-0xe5)return Status::AddressOverflow;
    Layout l={true,bar,uint32_t((smu.base+0xe4)*4),uint32_t((smu.base+0xe5)*4),
        nbio.strap2Register-(0xd102-0xcc23)*4,nbio.rsmuIndexRegister,nbio.rsmuDataRegister,nbio.rsmuIndexHiRegister};
    const auto valid=validate(l); if(valid==Status::Ok)out=l; return valid;
}
Status acquire(const Layout& l,const Io& io,uint8_t* out,size_t capacity,Result& r) {
    r=Result{};
    const auto valid=validate(l); if(valid!=Status::Ok)return finish(r,valid);
    if(!io.read32 || !io.write32 || !out || capacity<64 || capacity-1>UINTPTR_MAX-reinterpret_cast<uintptr_t>(out))
        return finish(r,Status::InvalidArgument);
    uint32_t start=0;
    if(!bank(l,io,start,r))return r.status;
    r.bankByteOffset=start;
    if(!write(io,l.romIndex,start,r) || !stream(l,io,out,0,64,r))return r.status;
    const char signature[]=" 761295520";
    if(out[0]!=0x55 || out[1]!=0xaa || !out[2])return finish(r,Status::RomRejected);
    for(unsigned i=0;i<sizeof(signature)-1;++i)if(out[0x30+i]!=uint8_t(signature[i]))return finish(r,Status::RomRejected);
    const size_t bytes=size_t(out[2])<<9; r.bytesRequired=bytes;
    if(bytes>capacity)return finish(r,Status::BufferTooSmall);
    if(!stream(l,io,out,64,bytes,r))return r.status;
    uint32_t finalBank=0; if(!bank(l,io,finalBank,r))return r.status;
    if(start!=finalBank)return finish(r,Status::BankChanged);
    ImageInfo image={}; AtomInfo atom={};
    if(parseImage(out,bytes,0,image)!=hackynavi::Status::Ok || image.size!=bytes ||
        image.vendorId!=0x1002 || image.deviceId!=0x744c || image.codeType || !image.checksumValid ||
        parseAtom(out,bytes,image,atom)!=hackynavi::Status::Ok)return finish(r,Status::RomRejected);
    r.image=image; r.atom=atom; return finish(r,Status::Ok);
}
const char* statusString(Status s) {
    switch(s) {
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(DiscoveryRejected);NAME(UnsupportedIp);NAME(InvalidLayout);NAME(OutsideBar);
    NAME(AddressOverflow);NAME(ReadFailed);NAME(WriteFailed);NAME(DeviceUnavailable);NAME(IndirectHighNotZero);
    NAME(RomRejected);NAME(BufferTooSmall);NAME(BankChanged);
#undef NAME
    }
    return "Unknown";
}
} }
