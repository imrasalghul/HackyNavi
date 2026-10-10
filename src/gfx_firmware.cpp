// SPDX-License-Identifier: MIT
// Firmware range/ordering and autoload protocol adapted from Linux commit
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0, amdgpu_ucode.h/.c,
// amdgpu_rlc.c, amdgpu_psp.c and gfx_v11_0.c; generated GC11.0.0 offsets/masks.
// Bounded parsing, owned staging and strict error propagation are original.
// Copyright 2014, 2020-2025 Advanced Micro Devices, Inc.
// Copyright 2008 Red Hat Inc. Copyright 2009 Jerome Glisse.
//
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
#include "hackynavi/gfx_firmware.hpp"

namespace hackynavi {
namespace gfxfw {
namespace {
uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
bool bounded(const FirmwareHeader& h,FirmwareRange r,bool optional=false) {
    if (!r.size && optional) return true;
    return r.size && !(r.offset&3) && !(r.size&3) && r.offset>=h.headerSizeBytes &&
        r.offset<=h.sizeBytes && r.size<=h.sizeBytes-r.offset;
}
bool overlap(FirmwareRange a,FirmwareRange b) {
    return a.size && b.size && uint64_t(a.offset)<uint64_t(b.offset)+b.size &&
        uint64_t(b.offset)<uint64_t(a.offset)+a.size;
}
View view(const Files& f,File file) {
    switch(file) {
    case File::Smu: return f.smu; case File::Sdma: return f.sdma;
    case File::Pfp: return f.pfp; case File::Me: return f.me; case File::Mec: return f.mec;
    case File::Imu: return f.imu; case File::Rlc: return f.rlc; case File::Count: break;
    }
    return View{};
}
Status finish(Result& r,Status s) { r.status=s; return s; }
bool supported(const Navi31IpLayout& l) {
    if(!l.valid) return false;
    const uint16_t ids[]={11,34,255,1,42,43}; const uint8_t majors[]={11,3,13,13,6,6};
    for(size_t i=0;i<kNavi31BlockCount;++i) {
        const auto& b=l.blocks[i];
        if(b.hardwareId!=ids[i] || b.major!=majors[i] || b.minor || b.revision ||
            b.instance || b.dieId || !b.baseCount || b.baseCount>kNavi31MaxBases) return false;
    }
    return true;
}
bool add(Plan& p,File f,FirmwareRange r,uint32_t type) {
    if(!r.size) return true;
    if(p.count>=kMaximumSegments || r.size>kVramTransferMaxBytes) return false;
    p.segments[p.count++] = Segment{f,r,type};
    const uint64_t rounded=(uint64_t(r.size)+4095)&~uint64_t(4095);
    if(rounded>p.stagingBytes) p.stagingBytes=rounded;
    return true;
}
bool pollValid(const pspring::PollOptions& p) {
    return p.maxPolls && p.maxPolls<=1000000 && p.delayUs && p.delayUs<=1000 &&
        uint64_t(p.maxPolls)*p.delayUs<=10000000;
}
Status resolve(const Navi31IpLayout& ips,uint64_t bytes,uint32_t (&regs)[3]) {
    const uint32_t base[]={0,1,1},offset[]={0xf40,0x4e82,0x2807};
    for(unsigned i=0;i<3;++i) {
        ResolvedRegister r={};
        const auto s=resolveNavi31Register(ips,Navi31Block::Gc,base[i],offset[i],bytes,r);
        if(s!=IpLayoutStatus::Ok) return s==IpLayoutStatus::OutsideBar ||
            s==IpLayoutStatus::AddressOverflow ? Status::OutsideBar:Status::UnsupportedLayout;
        regs[i]=r.mmioByteOffset;
        for(unsigned j=0;j<i;++j) if(regs[i]==regs[j]) return Status::UnsupportedLayout;
    }
    return Status::Ok;
}
bool read(const pspring::Io& io,uint32_t reg,uint32_t& value,Result& out) {
    ++out.readAttempts; out.lastRegister=reg;
    if(!io.read32(io.context,reg,value)) { finish(out,Status::ReadFailed); return false; }
    out.lastValue=value;
    if(value==UINT32_MAX) { finish(out,Status::DeviceUnavailable); return false; }
    return true;
}
bool mesHalted(const pspring::Io& io,uint32_t reg,Result& out) {
    uint32_t value=0;
    if(!read(io,reg,value,out)) return false;
    if((value&0x7c000000)!=0x40000000) { finish(out,Status::ActiveMes); return false; }
    return true;
}
} // namespace

hackynavi::Status parseImu(const uint8_t* data,size_t bytes,ImuFirmware& out) {
    out=ImuFirmware{}; ImuFirmware f={};
    auto status=parseFirmwareHeader(data,bytes,f.header);
    if(status!=hackynavi::Status::Ok) return status;
    if(f.header.headerMajor!=1 || f.header.headerMinor!=0) return hackynavi::Status::UnsupportedVersion;
    if(f.header.headerSizeBytes<48) return hackynavi::Status::Malformed;
    f.instruction=FirmwareRange{f.header.payloadOffset,le32(data+32)};
    const uint32_t dataBytes=le32(data+40);
    if(f.instruction.size>f.header.payloadSize || dataBytes>f.header.payloadSize-f.instruction.size)
        return hackynavi::Status::Malformed;
    f.data=FirmwareRange{f.header.payloadOffset+f.instruction.size,dataBytes};
    if(!bounded(f.header,f.instruction) || !bounded(f.header,f.data)) return hackynavi::Status::Malformed;
    out=f; return hackynavi::Status::Ok;
}
hackynavi::Status parseRlc(const uint8_t* data,size_t bytes,RlcFirmware& out) {
    out=RlcFirmware{}; RlcFirmware f={};
    auto status=parseFirmwareHeader(data,bytes,f.header);
    if(status!=hackynavi::Status::Ok) return status;
    if(f.header.headerMajor!=2 || f.header.headerMinor!=3) return hackynavi::Status::UnsupportedVersion;
    if(f.header.headerSizeBytes<204) return hackynavi::Status::Malformed;
    // v2.0 prefix104B, v2.1 adds52B, v2.2 adds16B, v2.3 adds32B.
    const unsigned sizeOffsets[]={116,132,148,156,164,180,196};
    for(unsigned i=0;i<7;++i) f.segments[i]=FirmwareRange{
        le32(data+sizeOffsets[i]+4),le32(data+sizeOffsets[i])};
    f.segments[7]=FirmwareRange{f.header.payloadOffset,f.header.payloadSize};
    for(unsigned i=0;i<8;++i) {
        if(!bounded(f.header,f.segments[i],i<7)) return hackynavi::Status::Malformed;
        for(unsigned j=0;j<i;++j) if(overlap(f.segments[i],f.segments[j])) return hackynavi::Status::Malformed;
    }
    out=f; return hackynavi::Status::Ok;
}
Status plan(const Files& files,const Navi31IpLayout& ips,uint8_t revision,Plan& out,Result& r) {
    out=Plan{}; r=Result{}; r.file=File::Count; r.parserStatus=hackynavi::Status::InvalidArgument;
    if(revision!=0xc8) return finish(r,Status::UnsupportedDevice);
    if(!supported(ips)) return finish(r,Status::UnsupportedLayout);
    Plan p={}; FirmwareHeader smu={}; r.file=File::Smu;
    r.parserStatus=parseFirmwareHeader(files.smu.bytes,files.smu.size,smu);
    if(r.parserStatus!=hackynavi::Status::Ok) return finish(r,Status::FirmwareRejected);
    if(smu.headerMajor!=2 || smu.headerMinor!=1 || smu.headerSizeBytes<44 ||
        smu.ipMajor!=13 || smu.ipMinor) return finish(r,Status::UnsupportedFirmware);
    const FirmwareRange smuCode={smu.payloadOffset,smu.payloadSize};
    if(!bounded(smu,smuCode) || !add(p,File::Smu,smuCode,18)) return finish(r,Status::InvalidSegment);
    r.file=File::Sdma; SdmaFirmwareV2 sdma={};
    r.parserStatus=parseSdmaFirmwareV2(files.sdma.bytes,files.sdma.size,sdma);
    if(r.parserStatus!=hackynavi::Status::Ok) return finish(r,Status::FirmwareRejected);
    if(sdma.header.ipMajor!=6 || sdma.header.ipMinor) return finish(r,Status::UnsupportedFirmware);
    if(!bounded(sdma.header,sdma.context) || !bounded(sdma.header,sdma.control) ||
        overlap(sdma.context,sdma.control) || !add(p,File::Sdma,sdma.context,71) ||
        !add(p,File::Sdma,sdma.control,72)) return finish(r,Status::InvalidSegment);
    GfxFirmwareV2 cp[3]={}; const File cpFile[]={File::Pfp,File::Me,File::Mec};
    for(unsigned i=0;i<3;++i) {
        r.file=cpFile[i]; const View v=view(files,r.file);
        r.parserStatus=parseGfxFirmwareV2(v.bytes,v.size,cp[i]);
        if(r.parserStatus!=hackynavi::Status::Ok) return finish(r,Status::FirmwareRejected);
        if(cp[i].header.ipMajor!=11 || cp[i].header.ipMinor) return finish(r,Status::UnsupportedFirmware);
        // PSP uses the COMMON payload offset for RS64 code, not the explicit
        // ucode_offset used by direct loading. Validate the actual PSP range.
        cp[i].instruction.offset=cp[i].header.payloadOffset;
        if(!bounded(cp[i].header,cp[i].instruction) || !bounded(cp[i].header,cp[i].data) ||
            overlap(cp[i].instruction,cp[i].data) ||
            !add(p,cpFile[i],cp[i].instruction,87+i)) return finish(r,Status::InvalidSegment);
    }
    for(unsigned i=0;i<3;++i) for(unsigned stack=0;stack<(i==2?4U:2U);++stack)
        if(!add(p,cpFile[i],cp[i].data,90+2*i+stack)) return finish(r,Status::InvalidSegment);
    r.file=File::Imu; ImuFirmware imu={};
    r.parserStatus=parseImu(files.imu.bytes,files.imu.size,imu);
    if(r.parserStatus!=hackynavi::Status::Ok) return finish(r,Status::FirmwareRejected);
    if(imu.header.ipMajor!=11 || imu.header.ipMinor) return finish(r,Status::UnsupportedFirmware);
    if(!add(p,File::Imu,imu.instruction,68) || !add(p,File::Imu,imu.data,69)) return finish(r,Status::InvalidSegment);
    r.file=File::Rlc; RlcFirmware rlc={};
    r.parserStatus=parseRlc(files.rlc.bytes,files.rlc.size,rlc);
    if(r.parserStatus!=hackynavi::Status::Ok) return finish(r,Status::FirmwareRejected);
    if(rlc.header.ipMajor!=11 || rlc.header.ipMinor) return finish(r,Status::UnsupportedFirmware);
    const uint32_t rlcTypes[]={22,20,21,26,48,25,7,8};
    for(unsigned i=0;i<8;++i) if(!add(p,File::Rlc,rlc.segments[i],rlcTypes[i])) return finish(r,Status::InvalidSegment);
    out=p; r.file=File::Count; return finish(r,Status::Ok);
}

struct Access {
    static Status fail(State& s,Result& r,Status status) {
        s.phase_=Phase::Faulted; r.stagingRetentionRequired=s.stagingRetain_;
        return finish(r,status);
    }
    static Status load(const Files& files,const Navi31IpLayout& ips,uint8_t revision,
        const VramAllocator& allocator,VramAllocationHandle staging,const VramTransferIo& transfer,
        const HdpIo& hdpIo,HdpState& hdp,const pspring::Io& io,pspring::State& ring,
        const pspring::PollOptions& polls,State& s,Result& r) {
        r=Result{}; r.stagingRetentionRequired=s.stagingRetain_;
        if(s.phase_!=Phase::Fresh) return finish(r,Status::InvalidState);
        Plan p={}; Status status=plan(files,ips,revision,p,r);
        if(status!=Status::Ok) return status;
        if(!pollValid(polls) || !transfer.read32 || !transfer.write32 || !hdpIo.read32 || !hdpIo.write32 ||
            !io.read32 || !io.write32 || !io.flushHdp || !io.beforeReadback || !io.delayUs || !io.delayMs)
            return finish(r,Status::InvalidArgument);
        if(!hdp.configured() || ring.phase()!=pspring::Phase::Running || !ring.tmrReady() || ring.pending())
            return finish(r,Status::InvalidState);
        uint32_t regs[3]={}; status=resolve(ips,transfer.barBytes,regs);
        if(status!=Status::Ok) return finish(r,status);
        VramRange range={}; uint64_t mc=0,physical=0;
        if(allocator.lookup(staging,range)!=VramAllocatorStatus::Ok ||
            allocator.addresses(staging,mc,physical)!=VramAllocatorStatus::Ok || (mc&4095))
            return finish(r,Status::InvalidAllocation);
        if(range.bytes<p.stagingBytes) return finish(r,Status::OutsideAllocation);
        // Validate against PSP control allocations BEFORE touching any bytes.
        const auto valid=pspring::validateInputAllocation(ring,staging,0,static_cast<uint32_t>(p.stagingBytes));
        if(valid!=pspring::Status::Ok) { r.psp.status=valid; return finish(r,Status::InvalidAllocation); }
        s.phase_=Phase::Loading; r.stage=Stage::Guard;
        if(!mesHalted(io,regs[2],r)) return fail(s,r,r.status);
        for(size_t i=0;i<p.count;++i) {
            const Segment& segment=p.segments[i]; const View v=view(files,segment.file);
            r.segment=i; r.file=segment.file; r.stage=Stage::Upload;
            if(uploadVram(allocator,staging,0,v.bytes+segment.range.offset,segment.range.size,transfer,r.transfer)!=VramTransferStatus::Ok)
                return fail(s,r,Status::TransferFailed);
            ++r.verifiedSegments; r.stage=Stage::Publish;
            r.hdpStatus=flushHdp(hdpIo,hdp,r.hdp);
            if(r.hdpStatus!=HdpStatus::Ok) return fail(s,r,Status::VisibilityFailed);
            r.stage=Stage::Load; s.stagingRetain_=true; r.firmwareMayBeLoaded=true;
            if(pspring::submitLoadIp(io,ring,staging,0,segment.range.size,segment.pspType,r.psp)!=pspring::Status::Ok)
                return fail(s,r,Status::PspFailed);
            r.stage=Stage::WaitLoad;
            if(pspring::wait(io,ring,polls,r.psp)!=pspring::Status::Ok) return fail(s,r,Status::PspFailed);
            s.stagingRetain_=false;
            s.loaded_[s.count_++]=LoadedSegment{segment.pspType,segment.range.size,r.psp.firmwareAddress};
            ++r.acceptedSegments;
        }
        r.stage=Stage::Guard;
        if(!mesHalted(io,regs[2],r)) return fail(s,r,r.status);
        r.stage=Stage::Autoload;
        // A false submit can have landed; retain TMR and report the attempt.
        r.autoloadSubmitted=true;
        if(pspring::submitAutoloadRlc(io,ring,r.psp)!=pspring::Status::Ok) return fail(s,r,Status::PspFailed);
        r.stage=Stage::WaitAutoload;
        if(pspring::wait(io,ring,polls,r.psp)!=pspring::Status::Ok) return fail(s,r,Status::PspFailed);
        r.stage=Stage::WaitBootload;
        for(uint32_t i=0;i<polls.maxPolls;++i) {
            ++r.polls; uint32_t cp=0,boot=0;
            if(!read(io,regs[0],cp,r) || !read(io,regs[1],boot,r)) return fail(s,r,r.status);
            if(!cp && (boot&0x80000000)) {
                if(!mesHalted(io,regs[2],r)) return fail(s,r,r.status);
                s.phase_=Phase::Ready; r.stage=Stage::Complete; return finish(r,Status::Ok);
            }
            io.delayUs(io.context,polls.delayUs);
        }
        return fail(s,r,Status::AutoloadTimeout);
    }
};
Status load(const Files& f,const Navi31IpLayout& ips,uint8_t revision,const VramAllocator& a,
    VramAllocationHandle handle,const VramTransferIo& transfer,const HdpIo& hdpIo,HdpState& hdp,
    const pspring::Io& io,pspring::State& ring,const pspring::PollOptions& polls,State& state,Result& out) {
    return Access::load(f,ips,revision,a,handle,transfer,hdpIo,hdp,io,ring,polls,state,out);
}
const char* statusString(Status s) {
    switch(s) {
    case Status::Ok: return "ok"; case Status::InvalidArgument: return "invalid graphics firmware arguments";
    case Status::UnsupportedDevice: return "unsupported PCI revision";
    case Status::FirmwareRejected: return "firmware bounds or header rejected";
    case Status::UnsupportedFirmware: return "unsupported firmware version or IP";
    case Status::InvalidSegment: return "empty, overlapping or oversized firmware segment";
    case Status::UnsupportedLayout: return "unsupported Navi31 layout";
    case Status::OutsideBar: return "graphics registers outside BAR";
    case Status::InvalidAllocation: return "invalid or aliased firmware staging allocation";
    case Status::OutsideAllocation: return "firmware staging allocation too small";
    case Status::InvalidState: return "graphics firmware or PSP state invalid";
    case Status::ActiveMes: return "MES must remain halted for this profile";
    case Status::TransferFailed: return "firmware upload or readback failed";
    case Status::VisibilityFailed: return "firmware HDP publication failed";
    case Status::PspFailed: return "PSP firmware command or completion failed";
    case Status::ReadFailed: return "graphics status read failed";
    case Status::DeviceUnavailable: return "graphics status all ones";
    case Status::AutoloadTimeout: return "graphics firmware autoload timeout";
    }
    return "unknown graphics firmware status";
}
} // namespace gfxfw
} // namespace hackynavi
