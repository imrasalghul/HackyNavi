// SPDX-License-Identifier: MIT
/*
 * DCN3.2 HUBP protocol adapted from Linux commit
 * 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0: dcn10_hubp.c,
 * dcn20_hubp.c, dcn30_hubp.c, dcn32_hubp.c, dcn32_hwseq.c and
 * dcn_3_2_0_offset.h/sh_mask.h. No DML policy is reproduced here.
 * Copyright 2012-2022 Advanced Micro Devices, Inc.
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "hackynavi/hubp_scanout.hpp"
namespace hackynavi { namespace hubpscanout { namespace {
// DWORD offsets, all BASE_IDX2. HUBP/HUBPREQ/HUBPRET/CURSOR stride is 0xdc;
// DOMAIN0..3 use 2, and the matching OTG uses 0x80.
const uint32_t offsets[kRegisterCount]={
    0x81,0x80,0x5f4,0x5f3,0x5e5,0x5e7,0x5f1,0x607,
    0x5e9,0x5ea,0x5ed,0x5ee,0x61a,0x61b,0x61c,0x609,
    0x60b,0x60a,0x621,0x620,0x625,0x624,0x66c,0x61f,
    0x642,0x634,0x635,0x678,0x684,0x5f6,0x1b41,0x1b44
};
constexpr uint32_t kFault=0x70f00800,kControlClear=0x84000000,kPending=0x100;
constexpr uint32_t kCrossbar=0x00f00000,kSurfaceControl=0x000f7f7f;
struct Dmu { uint32_t count;uint64_t base;bool valid; };
void collect(const DiscoveryIp& ip,void* context) {
    if(ip.hardwareId!=271)return;
    auto& d=*static_cast<Dmu*>(context);++d.count;
    d.valid=!ip.dieId && !ip.instance && ip.major==3 && ip.minor==2 && !ip.revision &&
        !ip.subRevision && !ip.variant && !ip.harvest && ip.baseAddressCount>=3 && ip.baseAddresses[2]<=UINT32_MAX;
    if(d.valid)d.base=ip.baseAddresses[2];
}
bool populate(uint64_t base,uint64_t bytes,Layout& out) {
    out={};if(bytes<4 || base>UINT32_MAX/4)return false;
    for(size_t p=0;p<kPipeCount;++p)for(size_t r=0;r<kRegisterCount;++r) {
        const uint64_t stride=r<=size_t(Register::PowerConfig)?2:r>=size_t(Register::OtgControl)?0x80:0xdc;
        const uint64_t address=(base+offsets[r]+p*stride)*4;
        if(address>UINT32_MAX || address>bytes-4)return false;
        out.registers[p][r]=uint32_t(address);
    }
    for(size_t i=0;i<5;++i){
        const uint64_t address=(base+0x475+i)*4;
        if(address>UINT32_MAX || address>bytes-4)return false;
        out.framebuffer[i]=uint32_t(address);
    }
    out.valid=true;out.barBytes=bytes;return true;
}
bool validLayout(const Layout& l) {
    if(!l.valid || (l.registers[0][0]&3) || l.registers[0][0]/4<offsets[0])return false;
    Layout expected={};if(!populate(l.registers[0][0]/4-offsets[0],l.barBytes,expected))return false;
    for(size_t p=0;p<kPipeCount;++p)for(size_t r=0;r<kRegisterCount;++r)
        if(l.registers[p][r]!=expected.registers[p][r])return false;
    for(size_t i=0;i<5;++i)if(l.framebuffer[i]!=expected.framebuffer[i])return false;
    return true;
}
bool validPoll(const PollOptions& p) {
    return p.iterations && p.intervalUs && p.intervalUs<=100 && uint64_t(p.iterations)*p.intervalUs<=100000;
}
bool sameGeometry(const Plan& a,const Plan& b) {
    return a.pitch==b.pitch && a.viewportStart==b.viewportStart && a.viewportDimension==b.viewportDimension &&
        a.surfaceConfig==b.surfaceConfig && a.crossbar==b.crossbar;
}
struct Value { Register reg;uint32_t mask,value; };
} // namespace
Status buildLayout(const uint8_t* bytes,size_t length,uint64_t bar,Layout& out) {
    out={};DiscoveryInfo info={};Dmu d={};
    if(parseDiscovery(bytes,length,info,collect,&d)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(d.count!=1 || !d.valid || info.baseAddresses64Bit || info.dieCount!=1 || info.dies[0].id)return Status::UnsupportedDmu;
    return populate(d.base,bar,out)?Status::Ok:Status::OutsideBar;
}
Status plan(const MemoryLayout& memory,const VramAllocator& allocator,const Surface& surface,Plan& out) {
    out={};
    if(!surface.pitchPixels || surface.pitchPixels>16384 || (surface.pitchPixels&63) ||
        !surface.width || !surface.height || surface.width>0x3fff || surface.height>0x3fff ||
        surface.x>0x3fff || surface.y>0x3fff || surface.x>=surface.pitchPixels ||
        surface.width>surface.pitchPixels-surface.x || (surface.byteOffset&255))return Status::InvalidArgument;
    uint32_t format=0,crossbar=0;
    switch(surface.format){
    case Format::Argb8888:format=8;crossbar=0xe00000;break;
    case Format::Abgr8888:format=8;crossbar=0xb00000;break;
    case Format::Argb2101010:format=10;crossbar=0xe00000;break;
    case Format::Abgr2101010:format=10;crossbar=0xb00000;break;
    default:return Status::InvalidArgument;
    }
    VramRange range={};uint64_t mc=0,physical=0,checkedMc=0,checkedPhysical=0;
    if(allocator.lookup(surface.allocation,range)!=VramAllocatorStatus::Ok ||
        allocator.addresses(surface.allocation,mc,physical)!=VramAllocatorStatus::Ok)return Status::InvalidAllocation;
    if(vramMcAddress(memory,range.offset,range.bytes,checkedMc)!=MemoryLayoutStatus::Ok ||
        vramPhysicalAddress(memory,range.offset,range.bytes,checkedPhysical)!=MemoryLayoutStatus::Ok ||
        mc!=checkedMc || physical!=checkedPhysical)return Status::InvalidAllocation;
    const uint64_t footprint=uint64_t(surface.pitchPixels)*4*(uint64_t(surface.y)+surface.height);
    if(surface.byteOffset>=range.bytes || footprint>range.bytes-surface.byteOffset)return Status::OutsideAllocation;
    // Complete pitch padding is covered, including rows before viewport.y.
    if(vramMcAddress(memory,range.offset+surface.byteOffset,footprint,mc)!=MemoryLayoutStatus::Ok ||
        vramPhysicalAddress(memory,range.offset+surface.byteOffset,footprint,physical)!=MemoryLayoutStatus::Ok ||
        !mc || (mc&255) || mc>0xffffffffffffULL || footprint-1>0xffffffffffffULL-mc)return Status::OutsideAllocation;
    out.surface=surface;out.mcAddress=mc;out.footprintBytes=footprint;
    out.pitch=surface.pitchPixels-1;out.viewportStart=surface.x|(surface.y<<16);
    out.viewportDimension=surface.width|(surface.height<<16);out.surfaceConfig=format;out.crossbar=crossbar;
    return Status::Ok;
}
Controller::Controller():layout_{},io_{},pipe_(0),memory_{},allocator_(nullptr),current_{},pending_{},phase_(Phase::Closed),touched_(false),powerStatus_(0),powerConfig_(0),powerValid_(false) {}
void Controller::clearAfterVerifiedReset() {
    layout_={};io_={};pipe_=0;memory_={};allocator_=nullptr;current_={};pending_={};phase_=Phase::Closed;touched_=false;powerStatus_=powerConfig_=0;powerValid_=false;
}
Status Controller::finish(Status s,Result& r,bool fault) {
    r.status=s;
    if(s!=Status::Ok && fault){
        if(touched_)phase_=Phase::Faulted;
        else clearAfterVerifiedReset();
    }
    r.retentionRequired=touched_;return s;
}
Status Controller::rawRead(uint32_t address,uint32_t& value,Result& r) {
    r.lastRegister=address;++r.reads;
    if(!io_.read32(io_.context,address,value))return Status::TransportError;
    r.lastValue=value;return value==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status Controller::power(Result& r) {
    uint32_t before=0,config=0,after=0;
    auto s=rawRead(layout_.registers[pipe_][size_t(Register::Power)],before,r);if(s!=Status::Ok)return s;
    if(before!=0&&before!=0x10000000)return Status::PowerUnavailable;
    s=rawRead(layout_.registers[pipe_][size_t(Register::PowerConfig)],config,r);if(s!=Status::Ok)return s;
    if(config>1)return Status::PowerUnavailable;
    if(powerValid_&&(before!=powerStatus_||config!=powerConfig_))return Status::PowerUnavailable;
    s=rawRead(layout_.registers[pipe_][size_t(Register::Power)],after,r);if(s!=Status::Ok)return s;
    if(after!=before)return Status::PowerUnavailable;
    s=rawRead(layout_.registers[pipe_][size_t(Register::PowerConfig)],after,r);if(s!=Status::Ok)return s;
    if(after!=config)return Status::PowerUnavailable;
    s=rawRead(layout_.registers[pipe_][size_t(Register::Power)],after,r);if(s!=Status::Ok)return s;
    if(after!=before)return Status::PowerUnavailable;
    powerStatus_=before;powerConfig_=config;powerValid_=true;return Status::Ok;
}
Status Controller::read(Register reg,uint32_t& value,Result& r) {
    auto s=power(r);if(s!=Status::Ok)return s;
    s=rawRead(layout_.registers[pipe_][size_t(reg)],value,r);return s==Status::Ok?power(r):s;
}
Status Controller::write(Register reg,uint32_t value,Result& r) {
    auto s=power(r);if(s!=Status::Ok)return s;
    uint32_t clock=0;s=rawRead(layout_.registers[pipe_][size_t(Register::Clock)],clock,r);if(s!=Status::Ok)return s;
    if(!(clock&1))return Status::ClockUnavailable;
    r.lastRegister=layout_.registers[pipe_][size_t(reg)];r.lastValue=value;
    // A failing transport can have delivered the write: pin before dispatch.
    r.writeAttempted=touched_=true;++r.writes;
    return io_.write32(io_.context,r.lastRegister,value)?power(r):Status::TransportError;
}
Status Controller::update(Register reg,uint32_t mask,uint32_t value,Result& r) {
    uint32_t old=0;auto s=read(reg,old,r);if(s!=Status::Ok)return s;
    uint32_t desired=(old&~mask)|(value&mask);
    if(reg==Register::Control)desired&=~kControlClear;
    if(reg==Register::Interrupt)desired&=0xf; // Never replay W1C or status bits.
    s=write(reg,desired,r);if(s!=Status::Ok)return s;
    s=read(reg,old,r);if(s!=Status::Ok)return s;
    return (old&mask)==(value&mask)?Status::Ok:Status::VerifyFailed;
}
Status Controller::verifyAddressMode(const Plan& p,Result& r) {
    uint32_t fb[5]={};auto s=power(r);if(s!=Status::Ok)return s;
    for(size_t i=0;i<5;++i){s=rawRead(layout_.framebuffer[i],fb[i],r);if(s!=Status::Ok)return s;
        if(fb[i]&0xff000000)return Status::AddressMode;}
    if((uint64_t(fb[0])<<24)!=memory_.mcWindowStart ||
        ((uint64_t(fb[1])<<24)|0xffffff)!=memory_.mcWindowEndInclusive ||
        (uint64_t(fb[2])<<24)!=memory_.vramPageBaseOffset || fb[3]<=fb[4])return Status::AddressMode;
    uint32_t vmid=0,tlb=0,lo=0,hi=0;
    s=read(Register::Vmid,vmid,r);if(s!=Status::Ok)return s;
    s=read(Register::TlbControl,tlb,r);if(s!=Status::Ok)return s;
    s=read(Register::ApertureLow,lo,r);if(s!=Status::Ok)return s;
    s=read(Register::ApertureHigh,hi,r);if(s!=Status::Ok)return s;
    if(vmid || tlb!=0x19 || ((lo|hi)&0xc0000000) || lo>hi || p.mcAddress<(uint64_t(lo)<<18) ||
        p.mcAddress+p.footprintBytes-1>((uint64_t(hi)<<18)|0x3ffff))return Status::AddressMode;
    return power(r);
}
Status Controller::verifyProgrammed(Result& r) {
    Plan live={};auto s=plan(memory_,*allocator_,current_.surface,live);if(s!=Status::Ok)return s;
    if(live.mcAddress!=current_.mcAddress || !sameGeometry(live,current_))return Status::InvalidAllocation;
    const Plan* address=&current_;
    if(phase_==Phase::FlipPending){
        s=plan(memory_,*allocator_,pending_.surface,live);if(s!=Status::Ok)return s;
        if(live.mcAddress!=pending_.mcAddress || !sameGeometry(live,pending_))return Status::InvalidAllocation;
        address=&pending_;
    }
    s=verifyAddressMode(*address,r);if(s!=Status::Ok)return s;
    const Value values[]={
        {Register::SurfaceConfig,UINT32_MAX,current_.surfaceConfig},
        {Register::TilingConfig,~0xa00u,0},{Register::RequestSize,7,0},
        {Register::Pitch,UINT32_MAX,current_.pitch},
        {Register::ViewportStart,UINT32_MAX,current_.viewportStart},
        {Register::ViewportDimension,UINT32_MAX,current_.viewportDimension},
        {Register::SecondaryStart,UINT32_MAX,current_.viewportStart},
        {Register::SecondaryDimension,UINT32_MAX,current_.viewportDimension},
        {Register::SurfaceControl,UINT32_MAX,0},{Register::ReturnControl,kCrossbar,current_.crossbar},
        {Register::AddressHi,UINT32_MAX,uint32_t(address->mcAddress>>32)},
        {Register::AddressLo,UINT32_MAX,uint32_t(address->mcAddress)},
        {Register::Control,0xf0,uint32_t(pipe_)<<4}
    };
    for(const auto& v:values){uint32_t value=0;s=read(v.reg,value,r);if(s!=Status::Ok)return s;
        if((value&v.mask)!=v.value)return Status::ForeignControl;}
    return Status::Ok;
}
Status Controller::guard(bool blank,Result& r) {
    uint32_t value=0;auto s=read(Register::Clock,value,r);if(s!=Status::Ok)return s;
    if(!(value&1))return Status::ClockUnavailable;
    s=read(Register::Control,value,r);if(s!=Status::Ok)return s;
    if(value&kFault)return Status::HardwareFault;
    if(value&4)return Status::ForeignControl;
    if(bool(value&1)!=blank || (phase_==Phase::Closed && (value&3)!=3))return Status::ActivePipe;
    const Value values[]={
        {Register::FlipControl,~0x102u,0},{Register::FlipControl2,~0x10ffu,0},
        {Register::CursorControl,1,0},{Register::DmdataControl,7,0},
        {Register::MallConfig,7,0},{Register::OtgInterlace,1,0},
        {Register::Interrupt,4,0}
    };
    for(const auto& v:values){s=read(v.reg,value,r);if(s!=Status::Ok)return s;
        if((value&v.mask)!=v.value)return Status::ForeignControl;}
    return phase_==Phase::Closed?Status::Ok:verifyProgrammed(r);
}
Status Controller::configure(const Layout& layout,const Io& io,uint8_t pipe,const MemoryLayout& memory,
    const VramAllocator& allocator,const Surface& surface,Result& r) {
    r={};
    if(phase_!=Phase::Closed && phase_!=Phase::Stopped)return finish(Status::InvalidState,r,false);
    if(!validLayout(layout) || pipe>=kPipeCount || !io.read32 || !io.write32 || !io.delayUs)
        return finish(Status::InvalidArgument,r,false);
    Plan prepared={};auto s=plan(memory,allocator,surface,prepared);if(s!=Status::Ok)return finish(s,r,false);
    clearAfterVerifiedReset();layout_=layout;io_=io;pipe_=pipe;memory_=memory;allocator_=&allocator;current_=prepared;
    s=guard(true,r);if(s!=Status::Ok)return finish(s,r);
    s=verifyAddressMode(current_,r);if(s!=Status::Ok)return finish(s,r);
    uint32_t value=0;s=read(Register::FlipControl,value,r);if(s!=Status::Ok)return finish(s,r);
    if(value&kPending)return finish(Status::ActivePipe,r);
    s=read(Register::Interrupt,value,r);if(s!=Status::Ok)return finish(s,r);
    if(value&5)return finish(Status::ForeignControl,r);
    // Only owned surface fields are programmed. Existing requestor/DLG/TTU,
    // watermarks, DET base, topology and alpha/green crossbar are preserved.
    const Value values[]={
        {Register::SurfaceConfig,UINT32_MAX,current_.surfaceConfig},
        {Register::TilingConfig,0x19f,0},{Register::RequestSize,7,0},
        {Register::SurfaceControl,kSurfaceControl,0},
        {Register::Pitch,UINT32_MAX,current_.pitch},
        {Register::ViewportStart,UINT32_MAX,current_.viewportStart},
        {Register::ViewportDimension,UINT32_MAX,current_.viewportDimension},
        {Register::SecondaryStart,UINT32_MAX,current_.viewportStart},
        {Register::SecondaryDimension,UINT32_MAX,current_.viewportDimension},
        {Register::ReturnControl,kCrossbar,current_.crossbar},
        {Register::Control,0x10f0,uint32_t(pipe_)<<4}
    };
    for(const auto& v:values){s=update(v.reg,v.mask,v.value,r);if(s!=Status::Ok)return finish(s,r);}
    s=programAddress(current_,true,r);if(s!=Status::Ok)return finish(s,r);
    phase_=Phase::Prepared;s=guard(true,r);return finish(s,r);
}
Status Controller::programAddress(const Plan& p,bool immediate,Result& r) {
    auto s=update(Register::FlipControl,2,immediate?2:0,r);if(s!=Status::Ok)return s;
    // VMID is already verified zero, including for immediate flips where the
    // DCN3 implementation deliberately does not program VMID_SETTINGS_0.
    if(!immediate){s=update(Register::Vmid,0xf,0,r);if(s!=Status::Ok)return s;}
    s=write(Register::AddressHi,uint32_t(p.mcAddress>>32),r);if(s!=Status::Ok)return s;
    s=write(Register::AddressLo,uint32_t(p.mcAddress),r);if(s!=Status::Ok)return s;
    uint32_t hi=0,lo=0;s=read(Register::AddressHi,hi,r);if(s!=Status::Ok)return s;
    s=read(Register::AddressLo,lo,r);if(s!=Status::Ok)return s;
    return hi==uint32_t(p.mcAddress>>32) && lo==uint32_t(p.mcAddress)?Status::Ok:Status::VerifyFailed;
}
Status Controller::waitAddress(const Plan& p,const PollOptions& poll,Result& r) {
    for(uint32_t n=0;n<poll.iterations;++n){
        ++r.polls;auto s=guard(false,r);if(s!=Status::Ok)return s;
        uint32_t before=0,after=0,lo=0,hi=0,elo=0,ehi=0;
        s=read(Register::FlipControl,before,r);if(s!=Status::Ok)return s;
        // Two complete observations avoid accepting a torn pair; low then high
        // follows hubp2_is_flip_pending's register read order.
        bool matches=true;
        for(size_t observation=0;observation<2;++observation){
            s=read(Register::InUseLo,lo,r);if(s!=Status::Ok)return s;
            s=read(Register::InUseHi,hi,r);if(s!=Status::Ok)return s;
            s=read(Register::EarliestLo,elo,r);if(s!=Status::Ok)return s;
            s=read(Register::EarliestHi,ehi,r);if(s!=Status::Ok)return s;
            matches=matches && hi==uint32_t(p.mcAddress>>32) && lo==uint32_t(p.mcAddress) &&
                ehi==uint32_t(p.mcAddress>>32) && elo==uint32_t(p.mcAddress);
        }
        s=read(Register::FlipControl,after,r);if(s!=Status::Ok)return s;
        if(!((before|after)&kPending) && matches)return power(r);
        if(n+1<poll.iterations)io_.delayUs(io_.context,poll.intervalUs);
    }
    return Status::Timeout;
}
Status Controller::start(const PollOptions& poll,Result& r) {
    r={};if(phase_!=Phase::Prepared)return finish(Status::InvalidState,r,false);
    if(!validPoll(poll))return finish(Status::InvalidArgument,r,false);
    auto s=guard(true,r);if(s!=Status::Ok)return finish(s,r);
    uint32_t otg=0;s=read(Register::OtgControl,otg,r);if(s!=Status::Ok)return finish(s,r);
    if((otg&0x10001)!=0x10001 || (otg&0x300000))return finish(Status::InvalidState,r,false);
    s=update(Register::Control,0x1001,0,r);if(s!=Status::Ok)return finish(s,r);
    s=waitAddress(current_,poll,r);if(s!=Status::Ok)return finish(s,r);
    phase_=Phase::Running;return finish(Status::Ok,r);
}
Status Controller::submitFlip(const Surface& surface,Result& r) {
    r={};if(phase_!=Phase::Running)return finish(Status::InvalidState,r,false);
    Plan next={};auto s=plan(memory_,*allocator_,surface,next);if(s!=Status::Ok)return finish(s,r,false);
    if(!sameGeometry(current_,next))return finish(Status::InvalidArgument,r,false);
    s=guard(false,r);if(s!=Status::Ok)return finish(s,r);
    uint32_t flip=0;s=read(Register::FlipControl,flip,r);if(s!=Status::Ok)return finish(s,r);
    if(flip&kPending)return finish(Status::ForeignControl,r);
    s=verifyAddressMode(next,r);if(s!=Status::Ok)return finish(s,r);
    // Publish the new pinned handle before any potentially delivered write.
    pending_=next;phase_=Phase::FlipPending;s=programAddress(pending_,false,r);return finish(s,r);
}
Status Controller::completeFlip(const PollOptions& poll,Result& r) {
    r={};if(phase_!=Phase::FlipPending)return finish(Status::InvalidState,r,false);
    if(!validPoll(poll))return finish(Status::InvalidArgument,r,false);
    auto s=waitAddress(pending_,poll,r);if(s!=Status::Ok)return finish(s,r);
    current_=pending_;pending_={};phase_=Phase::Running;return finish(Status::Ok,r);
}
Status Controller::setFlipInterrupt(bool enabled,Result& r) {
    r={};if(phase_!=Phase::Prepared && phase_!=Phase::Running && phase_!=Phase::FlipPending)
        return finish(Status::InvalidState,r,false);
    auto s=guard(phase_==Phase::Prepared,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Interrupt,5,enabled?1:0,r);return finish(s,r);
}
Status Controller::acknowledgeFlipInterrupt(bool& occurred,Result& r) {
    r={};occurred=false;
    if(phase_!=Phase::Prepared && phase_!=Phase::Running && phase_!=Phase::FlipPending)
        return finish(Status::InvalidState,r,false);
    uint32_t value=0;auto s=read(Register::Interrupt,value,r);if(s!=Status::Ok)return finish(s,r);
    occurred=(value&0x10000)!=0;
    if(occurred)s=write(Register::Interrupt,(value&0xf)|0x100,r);
    return finish(s,r);
}
Status Controller::waitControl(uint32_t mask,uint32_t expected,const PollOptions& poll,Result& r) {
    for(uint32_t n=0;n<poll.iterations;++n){
        ++r.polls;uint32_t clock=0,value=0;auto s=read(Register::Clock,clock,r);if(s!=Status::Ok)return s;
        if(!(clock&1))return Status::ClockUnavailable;
        s=read(Register::Control,value,r);if(s!=Status::Ok)return s;
        if(value&kFault)return Status::HardwareFault;
        if(value&4)return Status::ForeignControl;
        if((value&mask)==expected)return Status::Ok;
        if(n+1<poll.iterations)io_.delayUs(io_.context,poll.intervalUs);
    }
    return Status::Timeout;
}
Status Controller::stop(const PollOptions& poll,Result& r) {
    r={};if(phase_!=Phase::Prepared && phase_!=Phase::Running && phase_!=Phase::FlipPending)
        return finish(Status::InvalidState,r,false);
    if(!validPoll(poll))return finish(Status::InvalidArgument,r,false);
    auto s=guard(phase_==Phase::Prepared,r);if(s!=Status::Ok)return finish(s,r);
    // Linux hubp2 pre-drain, followed by hubp32's explicit post-blank drain.
    s=waitControl(2,2,poll,r);if(s!=Status::Ok)return finish(s,r);
    s=update(Register::Control,0x1001,1,r);if(s!=Status::Ok)return finish(s,r);
    s=waitControl(3,3,poll,r);if(s!=Status::Ok)return finish(s,r);
    bool cleared=false;
    for(uint32_t n=0;n<poll.iterations;++n){
        ++r.polls;uint32_t flip=0;s=read(Register::FlipControl,flip,r);if(s!=Status::Ok)return finish(s,r);
        if(!(flip&kPending)){cleared=true;break;}
        if(n+1<poll.iterations)io_.delayUs(io_.context,poll.intervalUs);
    }
    if(!cleared)return finish(Status::Timeout,r);
    s=update(Register::Interrupt,5,0,r);if(s!=Status::Ok)return finish(s,r);
    for(size_t n=0;n<2;++n){
        s=guard(true,r);if(s!=Status::Ok)return finish(s,r);
        s=waitControl(3,3,poll,r);if(s!=Status::Ok)return finish(s,r);
        uint32_t flip=0;s=read(Register::FlipControl,flip,r);if(s!=Status::Ok)return finish(s,r);
        if(flip&kPending)return finish(Status::ForeignControl,r);
    }
    touched_=false;phase_=Phase::Stopped;return finish(Status::Ok,r);
}
} } // namespace hackynavi::hubpscanout
