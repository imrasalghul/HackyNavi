// SPDX-License-Identifier: MIT
/*
 * DCN3.2 mailbox/ATOM protocol adapted from Linux commit
 * 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
 * display/dmub/src/dmub_dcn32.c, dmub_srv.c, dmub/inc/dmub_cmd.h,
 * display/dc/bios/command_table2.c, bios_parser2.c,
 * display/dc/bios/dce112/command_table_helper2_dce112.c,
 * display/dc/link/link_factory.c, gpio/dcn32/hw_translate_dcn32.c,
 * amdgpu/ObjectID.h, atomfirmware.h and dcn_3_2_0_offset.h/sh_mask.h.
 * Copyright 2012-2022 Advanced Micro Devices, Inc.
 *
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
#include "hackynavi/dmcub.hpp"
namespace hackynavi { namespace dmcub {
namespace {
const uint32_t offsets[kRegisterCount]={0x1f6,0x200,0x1e3,0x21a,
    0x1a9,0x1b1,0x1bd,0x1be,0x1d4,0x1d5,0x1d6,0x1d7};
struct Dmu { unsigned count; uint64_t base; bool valid; };
void collect(const DiscoveryIp& ip,void* ctx) {
    if(ip.hardwareId!=271)return;
    auto& d=*static_cast<Dmu*>(ctx); ++d.count;
    d.valid=ip.dieId==0 && ip.instance==0 && ip.major==3 && ip.minor==2 &&
        !ip.revision && !ip.subRevision && !ip.variant && !ip.harvest &&
        ip.baseAddressCount>=3 && ip.baseAddresses[2]<=UINT32_MAX;
    if(d.valid)d.base=ip.baseAddresses[2];
}
bool populate(uint64_t base,uint64_t bytes,Layout& l) {
    l=Layout{}; if(bytes<4 || base>UINT32_MAX/4)return false;
    for(size_t i=0;i<kRegisterCount;++i) {
        const uint64_t r=(base+offsets[i])*4;
        if(r>UINT32_MAX || r>bytes-4)return false;
        l.registers[i]=static_cast<uint32_t>(r);
    }
    l.valid=true; l.barBytes=bytes; return true;
}
bool validLayout(const Layout& l) {
    if(!l.valid || (l.registers[0]&3) || l.registers[0]/4<offsets[0])return false;
    Layout expected{};
    if(!populate(l.registers[0]/4-offsets[0],l.barBytes,expected))return false;
    for(size_t i=0;i<kRegisterCount;++i)if(l.registers[i]!=expected.registers[i])return false;
    return true;
}
uint16_t u16(const uint8_t* p) { return uint16_t(uint16_t(p[0])|(uint16_t(p[1])<<8)); }
uint32_t u32(const uint8_t* p) { return uint32_t(u16(p))|(uint32_t(u16(p+2))<<16); }
void put32(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(i*8)); }
bool physicalEncoder(uint16_t id,uint8_t& phy) {
    if((id&0xf000)!=0x2000)return false;
    const auto n=(id>>8)&15, type=id&255;
    if(n<1 || n>2 || (type!=0x1e && type!=0x20 && type!=0x21))return false;
    phy=uint8_t((type==0x1e?0:type==0x20?2:4)+n-1); return phy<5;
}
bool validRoute(const Route& r) {
    uint8_t phy=0;
    return (r.connectorObject&0xf000)==0x3000 && ((r.connectorObject>>8)&15)>=1 &&
        ((r.connectorObject>>8)&15)<=6 && ((r.connectorObject&255)==0x0c || (r.connectorObject&255)==0x13) &&
        physicalEncoder(r.encoderObject,phy) && phy==r.phy && r.ddc<5 && r.hpd<5;
}
bool hdmi(const Route& r) { return (r.connectorObject&255)==0x0c; }
bool lanes(const Route& r,uint8_t n) { return hdmi(r)?n==4:n==1 || n==2 || n==4; }
bool validPoll(const PollOptions& p) {
    return p.maxPolls && p.maxPolls<=2000000 && p.delayUs && p.delayUs<=100 && uint64_t(p.maxPolls)*p.delayUs<=2000000;
}
bool validIo(const Io& io) { return io.read32 && io.write32 && io.readMemory && io.writeMemory && io.flushForDevice && io.delayUs; }
void header(uint8_t* cmd,uint8_t subtype,uint8_t bytes) {
    for(size_t i=0;i<kSlotBytes;++i)cmd[i]=0;
    cmd[0]=0x80; cmd[1]=subtype; cmd[3]=bytes;
}
} // namespace
Status buildLayout(const uint8_t* data,size_t bytes,uint64_t bar,Layout& out) {
    out=Layout{}; DiscoveryInfo info{}; Dmu d{};
    if(parseDiscovery(data,bytes,info,collect,&d)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(d.count!=1 || !d.valid || info.baseAddresses64Bit || info.dieCount!=1 || info.dies[0].id)return Status::UnsupportedDmu;
    if(!populate(d.base,bar,out))return Status::OutsideBar;
    return Status::Ok;
}
Status parseRoute(const uint8_t* rom,size_t bytes,const ImageInfo& image,const Layout& l,uint8_t path,Route& out) {
    out=Route{}; if(!validLayout(l))return Status::InvalidArgument;
    DisplayInfo display{}; AtomInfo atom{};
    if(parseDisplay(rom,bytes,image,display)!=hackynavi::Status::Ok ||
        parseAtom(rom,bytes,image,atom)!=hackynavi::Status::Ok)return Status::RomRejected;
    if(path>=display.pathCount)return Status::InvalidArgument;
    const auto& p=display.paths[path]; Route route{};
    route.connectorObject=p.displayObjectId; route.encoderObject=p.encoderObjectId;
    if(!physicalEncoder(route.encoderObject,route.phy))return Status::UnsupportedRoute;
    // v1.5 reserved1 is nonzero for USB-C; it needs additional PHY interlocks.
    const uint8_t* d=rom+image.offset+display.table.offset;
    if(u16(d+8+size_t(path)*16+6))return Status::UnsupportedRoute;
    if(!p.recordOffset)return Status::UnsupportedRoute;
    uint8_t i2cId=0,hpdId=0,active=0; bool haveI2c=false,haveHpd=false;
    size_t at=p.recordOffset;
    while(at+2<=display.table.size && d[at]!=0xff && d[at+1]) {
        const uint8_t type=d[at],length=d[at+1];
        if(length<2 || length>display.table.size-at)return Status::RomRejected;
        if(type==1 || type==2) {
            if(length<4 || (type==1?haveI2c:haveHpd))return Status::RomRejected;
            if(type==1) { if(d[at+3])return Status::UnsupportedRoute; i2cId=d[at+2]; haveI2c=true; }
            else { hpdId=d[at+2]; active=d[at+3]; haveHpd=true; }
        }
        at+=length;
    }
    if(!haveI2c || !haveHpd || active>1 || (i2cId&0x80)==0 || (i2cId&15)>=5)return Status::UnsupportedRoute;
    route.ddc=i2cId&15; route.hpdActiveHigh=active!=0;
    const auto& lut=atom.tables[12];
    if(!lut.offset || lut.formatRevision!=2 || lut.contentRevision!=1 || lut.size<12 || (lut.size-4)%8)return Status::RomRejected;
    const uint8_t* gpio=rom+image.offset+lut.offset;
    const uint32_t base=l.registers[0]/4-offsets[0];
    unsigned ddcMatches=0,hpdMatches=0;
    const uint8_t shifts[]={0,8,16,24,26};
    for(size_t i=4;i+8<=lut.size;i+=8) {
        if(gpio[i+6]==i2cId) {
            if(u32(gpio+i)!=base+0x28d1+uint32_t(route.ddc)*4 || gpio[i+4]!=8)return Status::UnsupportedRoute;
            ++ddcMatches;
        }
        if(gpio[i+6]==hpdId) {
            if(u32(gpio+i)!=base+0x28f5)return Status::UnsupportedRoute;
            unsigned h=0; while(h<5 && shifts[h]!=gpio[i+4])++h;
            if(h==5)return Status::UnsupportedRoute;
            route.hpd=uint8_t(h); ++hpdMatches;
        }
    }
    if(ddcMatches!=1 || hpdMatches!=1 || !validRoute(route))return Status::UnsupportedRoute;
    out=route; return Status::Ok;
}
Inbox::Inbox():phase_(Phase::Closed),io_{},regs_{},mailbox_(0),producer_(0),start_(0),pending_(false) {}
void Inbox::clearAfterVerifiedReset() {
    phase_=Phase::Closed; io_=Io{}; mailbox_=0; producer_=start_=0; pending_=false;
    for(size_t i=0;i<kRegisterCount;++i)regs_[i]=0;
}
Status Inbox::fail(Status status) { phase_=Phase::Faulted; return status; }
Status Inbox::read(Register r,uint32_t& value,Result& result) {
    ++result.reads; result.lastRegister=regs_[static_cast<size_t>(r)];
    if(!io_.read32(io_.context,result.lastRegister,value))return fail(Status::TransportError);
    if(value==UINT32_MAX)return fail(Status::DeviceUnavailable);
    return Status::Ok;
}
Status Inbox::verify(Result& r,bool poll) {
    uint32_t values[kRegisterCount]={};
    for(size_t i=0;i<kRegisterCount;++i) {
        const Status s=read(static_cast<Register>(i),values[i],r); if(s!=Status::Ok)return s;
    }
    const auto v=[&](Register reg) { return values[static_cast<size_t>(reg)]; };
    if(!(v(Register::Control)&0x10000) || (v(Register::Control2)&1) || (v(Register::BootStatus)&3)!=3)
        return fail(Status::FirmwareNotReady);
    // CW4 BASE/TOP are the low29 bits of the uC's region3 address.
    // Linux DCN3.2 uses CW4, not the REGION4 mapping used by older ASICs.
    if((v(Register::AxiSpace)&7)!=3 || (v(Register::Cw4Base)&0x1fffffff)!=0x04000000 ||
        (v(Register::Cw4Top)&0x9fffffff)!=0x84004000 ||
        v(Register::Cw4Offset)!=uint32_t(mailbox_) || v(Register::Cw4OffsetHigh)!=uint32_t(mailbox_>>32) ||
        v(Register::InboxBase)!=0x64000000 || v(Register::InboxSize)!=kInboxBytes)
        return fail(Status::MappingMismatch);
    r.wptr=v(Register::Wptr); r.rptr=v(Register::Rptr);
    if(r.wptr>=kInboxBytes || r.rptr>=kInboxBytes || (r.wptr&63) || (r.rptr&63))return fail(Status::PointerCorrupt);
    if(poll) {
        if(r.wptr!=producer_ || (r.rptr!=start_ && r.rptr!=producer_))return fail(Status::PointerCorrupt);
    } else if(r.wptr!=r.rptr)return Status::Busy;
    return Status::Ok;
}
Status Inbox::open(const Layout& l,const Io& io,uint64_t mailbox,Result& r) {
    r=Result{};
    if(phase_!=Phase::Closed)return phase_==Phase::Faulted?Status::Faulted:Status::Busy;
    if(!validLayout(l) || !validIo(io) || (mailbox&255) || mailbox>=(UINT64_C(1)<<48) ||
        kMailboxBytes>(UINT64_C(1)<<48)-mailbox)return Status::InvalidArgument;
    io_=io; mailbox_=mailbox;
    for(size_t i=0;i<kRegisterCount;++i)regs_[i]=l.registers[i];
    Status status=verify(r,false);
    if(status!=Status::Ok) { clearAfterVerifiedReset(); return status; } // only read-only preflight occurred
    producer_=start_=r.wptr; phase_=Phase::Open; return Status::Ok;
}
Status Inbox::send(const uint8_t* cmd,const PollOptions& p,Result& r) {
    r=Result{};
    if(phase_!=Phase::Open)return phase_==Phase::Faulted?Status::Faulted:Status::Closed;
    if(pending_)return Status::Busy;
    if(!validPoll(p))return Status::InvalidArgument;
    Status s=verify(r,false); if(s!=Status::Ok)return s==Status::Busy?fail(s):s;
    if(r.wptr!=producer_)return fail(Status::PointerCorrupt);
    const uint64_t address=mailbox_+producer_;
    // No pointer publication until every byte is written, ordered and read
    // back. Linux dmub_rb_flush_pending likewise reads all64 framebuffer bytes.
    r.slotWritten=true;
    if(!io_.writeMemory(io_.context,address,cmd,kSlotBytes))return fail(Status::TransportError);
    if(!io_.flushForDevice(io_.context))return fail(Status::FlushFailed);
    uint8_t actual[kSlotBytes]={};
    if(!io_.readMemory(io_.context,address,actual,kSlotBytes))return fail(Status::TransportError);
    for(size_t i=0;i<kSlotBytes;++i)if(actual[i]!=cmd[i])return fail(Status::VerifyFailed);
    // Recheck mapping, ownership and idle pointers after staging as well.
    s=verify(r,false); if(s!=Status::Ok)return s==Status::Busy?fail(s):s;
    if(r.wptr!=producer_)return fail(Status::PointerCorrupt);
    start_=producer_; producer_=(producer_+kSlotBytes)%kInboxBytes;
    pending_=true; r.submitted=true; ++r.writes;
    r.lastRegister=regs_[static_cast<size_t>(Register::Wptr)];
    if(!io_.write32(io_.context,r.lastRegister,producer_))return fail(Status::TransportError);
    return waitInternal(p,r);
}
Status Inbox::waitInternal(const PollOptions& p,Result& r) {
    for(uint32_t i=0;i<p.maxPolls;++i) {
        ++r.polls; const Status s=verify(r,true); if(s!=Status::Ok)return s;
        if(r.rptr==producer_) { pending_=false; r.consumed=true; return Status::Ok; }
        if(i+1<p.maxPolls)io_.delayUs(io_.context,p.delayUs);
    }
    return fail(Status::CompletionTimeout);
}
Status Inbox::wait(const PollOptions& p,Result& r) {
    r=Result{};
    if(!pending_)return phase_==Phase::Faulted?Status::Faulted:Status::Closed;
    if(!validPoll(p))return Status::InvalidArgument;
    r.submitted=true; return waitInternal(p,r);
}
Status Inbox::close(Result& r) {
    r=Result{};
    if(phase_!=Phase::Open)return phase_==Phase::Faulted?Status::Faulted:Status::Closed;
    if(pending_)return Status::Busy;
    Status s=verify(r,false); if(s!=Status::Ok)return s==Status::Busy?fail(s):s;
    if(r.wptr!=producer_)return fail(Status::PointerCorrupt);
    clearAfterVerifiedReset(); return Status::Ok;
}
Status Inbox::transmitter(const Route& route,const Transmitter& t,const PollOptions& p,Result& r) {
    r=Result{};
    if(!validRoute(route) ||
        (t.action!=Action::Enable && t.action!=Action::Disable))return Status::InvalidArgument;
    if(t.action==Action::Enable) {
        if(!lanes(route,t.lanes))return Status::InvalidArgument;
        if(hdmi(route)) { if(t.symbolClockKHz<10 || t.symbolClockKHz>600000)return Status::InvalidArgument; }
        else if(t.symbolClockKHz!=162000 && t.symbolClockKHz!=270000 && t.symbolClockKHz!=540000 && t.symbolClockKHz!=810000)return Status::InvalidArgument;
    } else if(t.symbolClockKHz || t.lanes)return Status::InvalidArgument;
    uint8_t cmd[kSlotBytes]; header(cmd,1,60);
    cmd[4]=route.phy; cmd[5]=uint8_t(t.action); cmd[6]=hdmi(route)?3:0; cmd[7]=t.lanes;
    put32(cmd+8,t.symbolClockKHz/10); cmd[12]=route.hpd+1;
    // DCE112 helper deliberately returns digfe_sel=0 on DCN: DIO selects FE.
    cmd[14]=uint8_t(route.connectorObject);
    // Match command_table2's zero-initialized legacy hpo_engine_id minus
    // ENGINE_ID_HPO_0(10) / HPO_DP_0(12). This field is unused for legacy PHY.
    cmd[15]=hdmi(route)?0xf6:0xf4;
    return send(cmd,p,r);
}
Status Inbox::pixelClock(const Route& route,const PixelClock& c,const PollOptions& p,Result& r) {
    r=Result{};
    if(!validRoute(route) || c.crtc>=4 || c.deepColorRatio>3 || (c.miscInfo&8) ||
        (c.pll!=11 && (c.pll<20 || c.pll>24)) || (hdmi(route) && c.pll==11) ||
        (!hdmi(route) && c.deepColorRatio))return Status::InvalidArgument;
    uint8_t cmd[kSlotBytes]; header(cmd,2,16); put32(cmd+4,c.pixelClock100Hz);
    cmd[8]=c.pll; cmd[9]=uint8_t(route.encoderObject); cmd[10]=hdmi(route)?3:0;
    cmd[11]=c.miscInfo; cmd[12]=c.crtc; cmd[13]=c.deepColorRatio;
    return send(cmd,p,r);
}
Status Inbox::setupEncoder(const Route& route,const Encoder& e,const PollOptions& p,Result& r) {
    r=Result{};
    if(!validRoute(route) || e.engine>=5 || !lanes(route,e.lanes) || e.pixelClockKHz<10 ||
        (e.bitsPerColor!=8 && e.bitsPerColor!=10 && e.bitsPerColor!=12 && e.bitsPerColor!=16))return Status::InvalidArgument;
    uint64_t clock=e.pixelClockKHz/10;
    if(hdmi(route))clock=clock*e.bitsPerColor/8;
    if(clock>UINT32_MAX || (hdmi(route) && clock>60000))return Status::InvalidArgument;
    uint8_t cmd[kSlotBytes]; header(cmd,0,12);
    cmd[4]=e.engine; cmd[5]=0x0f; // ATOM_ENCODER_CMD_STREAM_SETUP
    cmd[6]=hdmi(route)?3:0; cmd[7]=e.lanes; put32(cmd+8,uint32_t(clock));
    cmd[12]=e.bitsPerColor==8?2:e.bitsPerColor==10?3:e.bitsPerColor==12?4:5;
    return send(cmd,p,r);
}
Status Inbox::dpDrive(const Route& route,const DpDrive& d,const PollOptions& p,Result& r) {
    r=Result{};
    if(!validRoute(route) || hdmi(route) || !lanes(route,d.lanes) ||
        d.voltageSwing>3 || d.preEmphasis>3 || d.postCursor2>3 || d.voltageSwing+d.preEmphasis>3 ||
        (d.postCursor2 && d.symbolClockKHz!=540000) ||
        (d.symbolClockKHz!=162000 && d.symbolClockKHz!=270000 && d.symbolClockKHz!=540000 && d.symbolClockKHz!=810000))return Status::InvalidArgument;
    uint8_t cmd[kSlotBytes]; header(cmd,1,60);
    cmd[4]=route.phy; cmd[5]=11;
    // link_service_types.h:dpcd_training_lane_set; MAX bits are not drive levels.
    cmd[6]=uint8_t(d.voltageSwing|(d.preEmphasis<<3)|(d.postCursor2<<6));cmd[7]=d.lanes;
    put32(cmd+8,d.symbolClockKHz/10);cmd[12]=route.hpd+1;cmd[14]=uint8_t(route.connectorObject);
    // Linux's VSEMPH caller leaves signal/hpo_engine_id zero, hence HPO_0 subtraction.
    cmd[15]=0xf6;
    return send(cmd,p,r);
}
const char* statusString(Status s) {
    switch(s) {
#define S(x) case Status::x:return #x
    S(Ok);S(InvalidArgument);S(DiscoveryRejected);S(UnsupportedDmu);S(OutsideBar);
    S(RomRejected);S(UnsupportedRoute);S(Closed);S(Busy);S(Faulted);S(FirmwareNotReady);
    S(MappingMismatch);S(PointerCorrupt);S(TransportError);S(DeviceUnavailable);S(FlushFailed);
    S(VerifyFailed);S(CompletionTimeout);
#undef S
    } return "Unknown";
}
} }
