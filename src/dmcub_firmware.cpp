// SPDX-License-Identifier: MIT
// DMCUB protocol follows Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// amdgpu_ucode.c, amdgpu_psp.c, amdgpu_dm_dmub.c, dmub_srv.c,
// dmub_dcn32.c, dmub_cmd.h and generated DCN3.2 registers.
// Copyright 2018-2022 Advanced Micro Devices, Inc.
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
#include "hackynavi/dmcub_firmware.hpp"
namespace hackynavi { namespace dmcubfw {
namespace {
enum Reg { Control,Control2,Scratch0,Scratch7,Scratch14,Scratch15,Gpint,Axi,
    Support,HubReset,Cw0Top,Cw1Top,Cw2Top,Cw3Off,Cw3Hi,Cw3Base,Cw3Top,
    Cw4Off,Cw4Hi,Cw4Base,Cw4Top,Cw5Off,Cw5Hi,Cw5Base,Cw5Top,
    Cw6Off,Cw6Hi,Cw6Base,Cw6Top,Cw7Top,Region5Off,Region5Hi,Region5Top,
    InboxBase,InboxSize,InboxWp,InboxRp,Out1Base,Out1Size,Out1Wp,Out1Rp,
    Out0Base,Out0Size,Out0Wp,Out0Rp,RegCount };
const uint32_t offsets[]={0x1f6,0x200,0x1e3,0x1ea,0x1f1,0x1f2,0x1f8,0x21a,
    0xca,0x342,0x1ad,0x1ae,0x1af,0x1bb,0x1bc,0x1a8,0x1b0,
    0x1bd,0x1be,0x1a9,0x1b1,0x1bf,0x1c0,0x1aa,0x1b2,
    0x1c1,0x1c2,0x1ab,0x1b3,0x1b4,0x198,0x199,0x1a2,
    0x1d4,0x1d5,0x1d6,0x1d7,0x1dc,0x1dd,0x1de,0x1df,0x1d8,0x1d9,0x1da,0x1db};
uint32_t u32(const uint8_t* p) { return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24; }
uint32_t aligned(uint32_t n,uint32_t a) { return (n+a-1)&~(a-1); }
bool pollValid(const PollOptions& p) { return p.iterations && p.iterations<=1000000 && p.intervalUs && p.intervalUs<=1000 && uint64_t(p.iterations)*p.intervalUs<=1000000; }
bool ioValid(const Io& io) { return io.read32 && io.write32 && io.publishMemory && io.delayUs; }
bool populate(uint64_t base,uint64_t bar,Layout& out) {
    out=Layout{}; if(bar<4)return false;
    for(unsigned i=0;i<RegCount;++i) {
        const uint64_t r=(base+offsets[i])*4;
        if(r>UINT32_MAX || r>bar-4 || r<0x1c || (r>=0x7f000 && r<0x80000))return false;
        out.regs[i]=uint32_t(r);
    }
    out.valid=true; out.barBytes=bar; return true;
}
bool layoutValid(const Layout& l) {
    if(!l.valid || (l.regs[Control]&3) || l.regs[Control]/4<offsets[Control])return false;
    Layout check={}; if(!populate(l.regs[Control]/4-offsets[Control],l.barBytes,check))return false;
    for(unsigned i=0;i<50;++i)if(l.regs[i]!=check.regs[i])return false; return true;
}
bool overlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn) { return a<=b?b-a<an:a-b<bn; }
Status end(Result& r,Status status) { r.status=status; return status; }
bool read(const Layout& l,const Io& io,Reg reg,uint32_t& value,Result& r) {
    r.lastRegister=l.regs[reg]; ++r.reads;
    if(!io.read32(io.context,r.lastRegister,value)) { end(r,Status::ReadFailed); return false; }
    r.lastValue=value; if(value==UINT32_MAX) { end(r,Status::DeviceUnavailable); return false; }return true;
}
bool write(const Layout& l,const Io& io,Reg reg,uint32_t value,Result& r) {
    r.lastRegister=l.regs[reg]; r.lastValue=value; ++r.writes;
    if(!io.write32(io.context,r.lastRegister,value)) { end(r,Status::WriteFailed); return false; }return true;
}
bool match(const Layout& l,const Io& io,Reg reg,uint32_t mask,uint32_t value,Result& r) {
    uint32_t actual=0; if(!read(l,io,reg,actual,r))return false;
    if((actual&mask)!=value) { end(r,Status::ReadbackMismatch); return false; }return true;
}
bool store(const Layout& l,const Io& io,Reg reg,uint32_t value,Result& r,uint32_t mask=UINT32_MAX) {
    return write(l,io,reg,value,r) && match(l,io,reg,mask,value&mask,r);
}
bool rmw(const Layout& l,const Io& io,Reg reg,uint32_t mask,uint32_t value,Result& r) {
    uint32_t old=0; return read(l,io,reg,old,r) && store(l,io,reg,(old&~mask)|(value&mask),r,mask);
}
bool wait(const Layout& l,const Io& io,const PollOptions& p,Reg reg,uint32_t mask,uint32_t expected,Status failure,Result& r) {
    for(uint32_t i=0;i<p.iterations;++i) {
        uint32_t value=0; ++r.polls; if(!read(l,io,reg,value,r))return false;
        if((value&mask)==expected)return true;
        if(i+1<p.iterations)io.delayUs(io.context,p.intervalUs);
    }
    end(r,failure); return false;
}
bool reset(const Layout& l,const Io& io,const PollOptions& p,Result& r) {
    uint32_t control=0,resetValue=0;
    r.stage=Stage::StopFirmware;
    if(!read(l,io,Control,control,r) || !read(l,io,Control2,resetValue,r))return false;
    if((control&0x10000) && !(resetValue&1)) {
        uint32_t command=0;
        if(!read(l,io,Gpint,command,r))return false;
        if(command&0xf0000000) { end(r,Status::Busy); return false; }
        if(!write(l,io,Gpint,0x10020000,r) ||
            !wait(l,io,p,Scratch7,UINT32_MAX,0xdeaddead,Status::StopTimeout,r) ||
            !wait(l,io,p,Control,0x100000,0x100000,Status::StopTimeout,r))return false;
    }
    r.stage=Stage::Reset;
    if(!rmw(l,io,Control2,1,1,r))return false;
    io.delayUs(io.context,1);
    if(!rmw(l,io,Control,0x10000,0,r))return false;
    const Reg tops[]={Cw2Top,Cw3Top,Cw4Top,Cw5Top,Cw6Top,Cw7Top,Region5Top};
    for(auto reg:tops)if(!rmw(l,io,reg,0x80000000,0,r))return false;
    const Reg clear[]={InboxRp,InboxWp,Out1Rp,Out1Wp,Out0Rp,Out0Wp,Scratch0,Gpint};
    for(auto reg:clear)if(!store(l,io,reg,0,r))return false;
    return match(l,io,Control2,1,1,r) && match(l,io,Control,0x10000,0,r);
}
} // namespace
Status parse(const uint8_t* data,size_t bytes,Image& out) {
    out=Image{}; Image x={};
    if(parseFirmwareHeader(data,bytes,x.header)!=hackynavi::Status::Ok)return Status::FirmwareRejected;
    const auto& h=x.header;
    if(h.headerMajor!=1 || h.headerMinor || h.ipMajor!=3 || h.ipMinor!=2)return Status::UnsupportedFirmware;
    if(h.headerSizeBytes<40 || h.payloadOffset&3 || h.payloadSize&3 || h.payloadSize>4*1024*1024)
        return Status::FirmwareRejected;
    x.pspImage={h.payloadOffset,u32(data+32)}; x.bss.size=u32(data+36);
    if(x.pspImage.size>h.payloadSize || x.bss.size>h.payloadSize-x.pspImage.size ||
        x.pspImage.size+x.bss.size!=h.payloadSize || x.pspImage.size<576)return Status::FirmwareRejected;
    x.bss.offset=h.payloadOffset+x.pspImage.size;
    if(x.bss.size)return Status::UnsupportedFirmware;
    const uint8_t* meta=nullptr;
    for(uint32_t footer=256;footer<=512 && !meta;footer+=256)for(uint32_t pad=0;pad<16;++pad) {
        if(x.pspImage.size<256+footer+64+pad)continue;
        const uint32_t at=x.pspImage.offset+x.pspImage.size-footer-64-pad;
        if(u32(data+at)==0x444d5542) { meta=data+at; x.metadataOffset=at; x.footerBytes=footer; break; }
    }
    if(!meta || meta[16]!=1)return Status::FirmwareRejected;
    x.firmwareStateBytes=u32(meta+4); x.traceBytes=u32(meta+8); x.version=u32(meta+12);
    x.sharedStateBytes=u32(meta+20); x.featureBits=u32(meta+28);
    x.sharedStateFeatures=uint16_t(meta[24])|uint16_t(uint16_t(meta[25])<<8);
    if(x.version!=h.ucodeVersion || !x.version || x.firmwareStateBytes<64 || x.firmwareStateBytes>1024*1024 ||
        x.traceBytes<64 || x.traceBytes>1024*1024)return Status::FirmwareRejected;
    // DCN3.2 setup_windows does not map shared state; don't silently accept a
    // newer image which requires it or the extended host-side features.
    if(x.sharedStateBytes || x.sharedStateFeatures || x.featureBits)return Status::UnsupportedFirmware;
    x.instruction={x.pspImage.offset+256,x.pspImage.size-256-x.footerBytes}; out=x; return Status::Ok;
}
Status plan(const Image& x,uint32_t vbiosBytes,Plan& out) {
    out=Plan{};
    if(x.header.ipMajor!=3 || x.header.ipMinor!=2 || x.bss.size || !x.version ||
        !vbiosBytes || vbiosBytes>1024*1024 || x.firmwareStateBytes<64 || x.firmwareStateBytes>1024*1024 ||
        x.traceBytes<64 || x.traceBytes>1024*1024 || x.sharedStateBytes || x.sharedStateFeatures || x.featureBits)return Status::InvalidArgument;
    const uint32_t sizes[]={vbiosBytes,16384,x.traceBytes,x.firmwareStateBytes}; uint32_t top=0;
    for(unsigned i=0;i<4;++i) { const uint32_t base=aligned(top,256); out.regions[i]={base,aligned(sizes[i],64)}; top=base+out.regions[i].bytes; }
    out.totalBytes=aligned(top,4096); return Status::Ok;
}
Status buildLayout(const uint8_t* discovery,size_t bytes,uint64_t bar,Layout& out) {
    out=Layout{}; dmcub::Layout inbox={};
    if(dmcub::buildLayout(discovery,bytes,bar,inbox)!=dmcub::Status::Ok)return Status::UnsupportedLayout;
    return populate(inbox.registers[0]/4-offsets[Control],bar,out)?Status::Ok:Status::OutsideBar;
}
struct Access {
    static Status finish(const State& s,Result& r,Status status) {
        r.stagingRetentionRequired=s.stagingRetain_; r.runtimeRetentionRequired=s.runtimeRetain_;
        r.firmwareMayBeLoaded=s.loaded_; return end(r,status);
    }
    static Status fault(State& s,Result& r,Status status) { s.phase_=Phase::Faulted; return finish(s,r,status); }
    static Status load(const uint8_t* bytes,size_t size,const VramAllocator& a,VramAllocationHandle staging,
        const VramTransferIo& transfer,const HdpIo& hdpIo,HdpState& hdp,const pspring::Io& io,
        pspring::State& ring,const pspring::PollOptions& p,State& s,Result& r) {
        r=Result{};
        if(s.phase_!=Phase::Fresh)return finish(s,r,Status::InvalidState);
        Image x={}; const auto parsed=parse(bytes,size,x); if(parsed!=Status::Ok)return finish(s,r,parsed);
        if(!hdp.configured() || !ring.tmrReady() || pspring::validateInputAllocation(ring,staging,0,x.pspImage.size)!=pspring::Status::Ok)
            return finish(s,r,Status::InvalidAllocation);
        if(!p.maxPolls || p.maxPolls>1000000 || !p.delayUs || p.delayUs>1000 || uint64_t(p.maxPolls)*p.delayUs>10000000)
            return finish(s,r,Status::InvalidArgument);
        s.image_=x; r.stage=Stage::Upload;
        if(uploadVram(a,staging,0,bytes+x.pspImage.offset,x.pspImage.size,transfer,r.transfer)!=VramTransferStatus::Ok)
            return fault(s,r,Status::TransferFailed);
        r.stage=Stage::Publish;
        if(flushHdp(hdpIo,hdp,r.hdp)!=HdpStatus::Ok)return fault(s,r,Status::VisibilityFailed);
        r.stage=Stage::PspLoad; s.stagingRetain_=true; s.loaded_=true;
        if(pspring::submitLoadIp(io,ring,staging,0,x.pspImage.size,kPspFirmwareType,r.psp)!=pspring::Status::Ok ||
            pspring::wait(io,ring,p,r.psp)!=pspring::Status::Ok)return fault(s,r,Status::PspFailed);
        s.stagingRetain_=false; s.phase_=Phase::Loaded; r.stage=Stage::Complete; return finish(s,r,Status::Ok);
    }
    static Status boot(const Layout& l,const uint8_t* rom,size_t romBytes,const ImageInfo& image,
        const Memory& memory,const Io& io,const PollOptions& p,State& s,Result& r) {
        r=Result{};
        if(s.phase_!=Phase::Loaded)return finish(s,r,Status::InvalidState);
        if(!layoutValid(l))return finish(s,r,Status::UnsupportedLayout);
        if(!ioValid(io) || !pollValid(p) || !rom || !memory.cpu)return finish(s,r,Status::InvalidArgument);
        AtomInfo atom={}; ImageInfo checked={}; Plan regions={};
        if(parseImage(rom,romBytes,image.offset,checked)!=hackynavi::Status::Ok || checked.codeType ||
            checked.vendorId!=0x1002 || checked.deviceId!=0x744c || !checked.checksumValid ||
            parseAtom(rom,romBytes,checked,atom)!=hackynavi::Status::Ok ||
            plan(s.image_,uint32_t(checked.size),regions)!=Status::Ok)return finish(s,r,Status::RomRejected);
        const uint64_t mc=memory.gpuMcAddress,limit=UINT64_C(1)<<48;
        const uintptr_t cpu=reinterpret_cast<uintptr_t>(memory.cpu),source=reinterpret_cast<uintptr_t>(rom);
        if((mc&4095) || (cpu&3) || mc>=limit || regions.totalBytes>limit-mc || memory.bytes<regions.totalBytes ||
            regions.totalBytes-1>UINTPTR_MAX-cpu || romBytes>UINTPTR_MAX-source ||
            overlap(cpu,regions.totalBytes,source,romBytes))return finish(s,r,Status::InvalidAllocation);
        s.layout_=l; s.memory_=memory; s.plan_=regions; s.phase_=Phase::Booting;
        // PSP owns secure instruction/stack windows and mode3. Runtime windows
        // use GPU MC/GART addresses directly on DCN3.2, without FB subtraction.
        if(!match(l,io,Support,0x10000,0x10000,r) || !match(l,io,Axi,7,3,r) ||
            !match(l,io,Cw0Top,0x80000000,0x80000000,r) || !match(l,io,Cw1Top,0x80000000,0x80000000,r))
            return fault(s,r,r.status==Status::ReadbackMismatch?Status::UnsupportedHardware:r.status);
        // Conservative ownership from first potentially landed reset write.
        s.runtimeRetain_=true;
        if(!reset(l,io,p,r))return fault(s,r,r.status);
        r.stage=Stage::InitializeMemory;
        for(uint32_t i=0;i<regions.totalBytes;++i)memory.cpu[i]=0;
        for(size_t i=0;i<checked.size;++i)memory.cpu[regions.regions[0].offset+i]=rom[checked.offset+i];
        if(!io.publishMemory(io.context))return fault(s,r,Status::VisibilityFailed);
        r.stage=Stage::Windows;
        for(unsigned i=0;i<4;++i) {
            const auto& region=regions.regions[i]; const uint64_t address=mc+region.offset;
            const Reg first=static_cast<Reg>(Cw3Off+i*4); const uint32_t base=0x63000000+i*0x01000000;
            if(!store(l,io,first,uint32_t(address),r) || !store(l,io,static_cast<Reg>(first+1),uint32_t(address>>32),r) ||
                !store(l,io,static_cast<Reg>(first+2),base,r,0x1fffffff) ||
                !store(l,io,static_cast<Reg>(first+3),0x80000000|((base+region.bytes)&0x1fffffff),r))return fault(s,r,r.status);
        }
        const auto& trace=regions.regions[2]; const uint64_t traceMc=mc+trace.offset;
        if(!store(l,io,Region5Off,uint32_t(traceMc),r) || !store(l,io,Region5Hi,uint32_t(traceMc>>32),r) ||
            !store(l,io,Region5Top,0x80000000|(trace.bytes-1),r) ||
            !store(l,io,Out0Base,0xa0000010,r) || !store(l,io,Out0Size,trace.bytes-16,r) ||
            !store(l,io,InboxBase,0x64000000,r) || !store(l,io,InboxSize,8192,r) ||
            !store(l,io,Out1Base,0x64002000,r) || !store(l,io,Out1Size,8192,r) ||
            !store(l,io,Scratch14,0,r) || !store(l,io,Scratch15,0,r))return fault(s,r,r.status);
        r.stage=Stage::ReleaseReset;
        if(!rmw(l,io,HubReset,0x100,0,r) || !rmw(l,io,Control,0x90000,0x90000,r) ||
            !rmw(l,io,Control2,1,0,r))return fault(s,r,r.status);
        r.stage=Stage::WaitBoot;
        if(!wait(l,io,p,Scratch0,3,3,Status::BootTimeout,r) || !match(l,io,Axi,7,3,r) ||
            !match(l,io,Control,0x10000,0x10000,r) || !match(l,io,Control2,1,0,r))return fault(s,r,r.status);
        r.stage=Stage::VerifyVersion;
        if(!write(l,io,Gpint,0x10010000,r) || !wait(l,io,p,Gpint,UINT32_MAX,0x00010000,Status::BootTimeout,r))
            return fault(s,r,r.status);
        uint32_t version=0; if(!read(l,io,Scratch7,version,r))return fault(s,r,r.status);
        if(version!=s.image_.version)return fault(s,r,Status::VersionMismatch);
        s.phase_=Phase::Ready; r.stage=Stage::Complete; return finish(s,r,Status::Ok);
    }
    static Status stop(const Io& io,const PollOptions& p,State& s,Result& r) {
        r=Result{};
        if(!s.runtimeRetain_ || (s.phase_!=Phase::Ready && s.phase_!=Phase::Faulted))return finish(s,r,Status::InvalidState);
        if(!ioValid(io) || !pollValid(p))return finish(s,r,Status::InvalidArgument);
        if(!reset(s.layout_,io,p,r))return fault(s,r,r.status);
        s.runtimeRetain_=false; s.phase_=Phase::Stopped; r.stage=Stage::Complete; return finish(s,r,Status::Ok);
    }
};
Status load(const uint8_t* b,size_t n,const VramAllocator& a,VramAllocationHandle h,const VramTransferIo& t,
    const HdpIo& hi,HdpState& hs,const pspring::Io& pi,pspring::State& ps,const pspring::PollOptions& po,State& s,Result& r) {
    return Access::load(b,n,a,h,t,hi,hs,pi,ps,po,s,r);
}
Status boot(const Layout& l,const uint8_t* r,size_t n,const ImageInfo& im,const Memory& m,const Io& io,const PollOptions& p,State& s,Result& out) {
    return Access::boot(l,r,n,im,m,io,p,s,out);
}
Status stop(const Io& io,const PollOptions& p,State& s,Result& r) { return Access::stop(io,p,s,r); }
Status stopInheritedCold(const Navi31IpLayout& ip,const Layout& l,const Io& io,const PollOptions& p,Result& r) {
    r=Result{};
    psp::Layout pspLayout={};
    if(!layoutValid(l) || psp::buildLayout(ip,l.barBytes,pspLayout)!=psp::Status::Ok)
        return end(r,Status::UnsupportedLayout);
    if(!io.read32 || !io.write32 || !io.delayUs || !pollValid(p))return end(r,Status::InvalidArgument);
    for(unsigned pass=0;pass<2;++pass) {
        uint32_t sos=0;r.lastRegister=pspLayout.heartbeat;++r.reads;
        if(!io.read32(io.context,r.lastRegister,sos))return end(r,Status::ReadFailed);
        r.lastValue=sos;
        if(sos==UINT32_MAX)return end(r,Status::DeviceUnavailable);
        if(sos)return end(r,Status::InvalidState);
        if(!pass && !reset(l,io,p,r))return r.status;
    }
    r.stage=Stage::Complete;return end(r,Status::Ok);
}
const char* statusString(Status s) {
    switch(s) {
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(FirmwareRejected);NAME(UnsupportedFirmware);NAME(RomRejected);
    NAME(UnsupportedLayout);NAME(OutsideBar);NAME(InvalidState);NAME(InvalidAllocation);NAME(TransferFailed);
    NAME(VisibilityFailed);NAME(PspFailed);NAME(ReadFailed);NAME(WriteFailed);NAME(DeviceUnavailable);
    NAME(ReadbackMismatch);NAME(UnsupportedHardware);NAME(Busy);NAME(StopTimeout);NAME(BootTimeout);NAME(VersionMismatch);
#undef NAME
    }
    return "Unknown";
}
} }
