// SPDX-License-Identifier: MIT
/*
 * Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */
// Bounded adaptation of Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// bios_parser2.c:get_firmware_info_v3_4/get_vram_info_v30/get_soc_bb_info;
// dcn32_clk_mgr.c:dcn32_get_vco_frequency_from_reg + construct (100MHz DFS);
// dcn32_dccg.c:dccg32_get_dccg_ref_freq; dcn20_hubbub.c:hubbub2_get_dchub_ref_freq.
// Field layout: atomfirmware.h:atom_display_controller_info_v4_5.
#include "hackynavi/display_model_input.hpp"
namespace hackynavi { namespace displaymodel { namespace {
uint16_t le16(const uint8_t* p){return uint16_t(p[0])|uint16_t(uint16_t(p[1])<<8);}
uint32_t le32(const uint8_t* p){return uint32_t(le16(p))|(uint32_t(le16(p+2))<<16);}
constexpr uint32_t kPll=0x16e37*4,kTimer=0x525;
struct Dmu {unsigned count;bool valid;uint64_t base;};
void collect(const DiscoveryIp& ip,void* context) {
    if(ip.hardwareId!=271)return;
    auto& d=*static_cast<Dmu*>(context);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&
        !ip.subRevision&&!ip.variant&&!ip.harvest&&ip.baseAddressCount>=3;
    if(d.valid)d.base=ip.baseAddresses[2];
}
bool boardShape(const BoardInfo& b) {
    return b.vendorId==0x1002&&b.deviceId==0x744c&&b.crystalKHz==100000&&b.i2cKHz&&
        b.memoryType==0x70&&b.channelCount&&b.channelCount<=24&&b.channelWidthRaw==4&&b.channelWidthBytes==2&&
        b.maxDisplayPipes==4;
}
struct Access {
    const Io& io;uint64_t start,last;uint32_t budget;Result& r;
    Status ready() {
        if(!io.valid(io.context))return Status::Revoked;
        const uint64_t now=io.timeUs(io.context);
        if(now<last)return Status::ClockWentBackwards;
        last=now;r.elapsedUs=now-start;
        return r.elapsedUs<budget?Status::Ok:Status::Timeout;
    }
    Status read(uint32_t address,uint32_t& value) {
        Status s=ready();if(s!=Status::Ok)return s;
        ++r.reads;r.lastRegister=address;
        if(!io.read32(io.context,address,value))return Status::TransportError;
        r.lastValue=value;s=ready();
        return s!=Status::Ok?s:value==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
    }
};
} // namespace
Status parseBoard(const uint8_t* bytes,size_t size,const ImageInfo& selected,BoardInfo& out) {
    out={};ImageInfo image={};AtomInfo atom={};FirmwareInfo firmware={};VramInfo vram={};
    if(!bytes)return Status::InvalidArgument;
    if(parseImage(bytes,size,selected.offset,image)!=hackynavi::Status::Ok||!image.checksumValid||
        image.vendorId!=0x1002||image.deviceId!=0x744c||image.codeType||
        parseAtom(bytes,size,image,atom)!=hackynavi::Status::Ok||
        parseFirmwareInfo(bytes,size,image,firmware)!=hackynavi::Status::Ok||
        parseVram(bytes,size,image,vram)!=hackynavi::Status::Ok)return Status::RomRejected;
    const auto& table=atom.tables[27];
    if(!table.offset||table.formatRevision!=4||table.contentRevision!=5||table.size<212)
        return Status::UnsupportedBoard;
    const uint8_t* p=bytes+image.offset+table.offset;
    BoardInfo b={};b.vendorId=image.vendorId;b.deviceId=image.deviceId;
    b.subsystemVendorId=atom.subsystemVendorId;b.subsystemDeviceId=atom.subsystemDeviceId;
    b.firmwareTable=firmware.table;b.displayControllerTable=table;b.vramTable=vram.table;
    b.firmwareRevision=firmware.firmwareRevision;b.crystalKHz=uint32_t(le16(p+12))*10;
    b.i2cKHz=uint32_t(le16(p+14))*10;b.maxDisplayPipes=p[42];
    b.rawMclkChange100ns=le32(p+56);b.rawSrExit100ns=le32(p+60);b.rawSrEnterExit100ns=le32(p+64);
    b.memoryType=vram.memoryType;b.channelCount=vram.channelCount;b.channelWidthRaw=vram.channelWidthRaw;
    // bios_parser2:get_vram_info_v30: exponent encodes bits, divide by8.
    // Module alternatives are not additional populated memory channels.
    if(vram.channelWidthRaw<3||vram.channelWidthRaw>7)return Status::UnsupportedBoard;
    b.channelWidthBytes=uint16_t((1u<<vram.channelWidthRaw)/8);
    if(!boardShape(b)||le16(p+38)!=0x744c)return Status::UnsupportedBoard;
    out=b;return Status::Ok;
}
Status decodeReferences(const BoardInfo& board,uint32_t pll,uint32_t timer,ReferenceInfo& out) {
    out={};
    if(!boardShape(board))return Status::UnsupportedBoard;
    if(pll==UINT32_MAX||timer==UINT32_MAX)return Status::DeviceUnavailable;
    if(!(timer&0x1000))return Status::UnsupportedReference;
    ReferenceInfo r={};r.rawPllRequest=pll;r.rawTimerControl=timer;r.timerDivider=uint8_t(timer&0xf);
    r.dccgReferenceKHz=board.crystalKHz;
    r.dchubReferenceKHz=board.crystalKHz/(r.timerDivider==2?2:1);
    // Match Linux fixed31_32 exactly: integer at low9, fractional high16
    // occupy bits31:16 of the 32-bit fractional component.
    const uint64_t vco=uint64_t(pll&0x1ff)*100000+(uint64_t(pll&0xffff0000)*100000>>32);
    if(!vco||vco>UINT32_MAX||r.dchubReferenceKHz<40000||r.dchubReferenceKHz>60000)
        return Status::UnsupportedReference;
    r.programmedVcoKHz=uint32_t(vco);out=r;return Status::Ok;
}
Status captureReferences(const uint8_t* discovery,size_t bytes,uint64_t bar,const BoardInfo& board,
    const Io& io,uint32_t timeout,ReferenceInfo& out,Result& result) {
    out={};result={};auto finish=[&](Status s){result.status=s;return s;};
    if(!io.valid||!io.read32||!io.timeUs||!timeout||timeout>2000000)return finish(Status::InvalidArgument);
    if(!boardShape(board))return finish(Status::UnsupportedBoard);
    DiscoveryInfo info={};Dmu d={};Navi31IpLayout ip={};IpLayoutDiagnostic diagnostic={};
    if(buildNavi31IpLayout(discovery,bytes,ip,diagnostic)!=IpLayoutStatus::Ok||
       parseDiscovery(discovery,bytes,info,collect,&d)!=DiscoveryStatus::Ok||
       d.count!=1||!d.valid||info.baseAddresses64Bit||info.dieCount!=1||info.dies[0].id)
        return finish(Status::DiscoveryRejected);
    if(bar<4||d.base>UINT32_MAX/4-kTimer||kPll>bar-4)return finish(Status::OutsideBar);
    const uint32_t timer=uint32_t((d.base+kTimer)*4);
    if(timer>bar-4||timer==kPll)return finish(Status::OutsideBar);
    const uint64_t start=io.timeUs(io.context);Access access={io,start,start,timeout,result};
    uint32_t pll[2]={},control[2]={};
    for(unsigned pass=0;pass<2;++pass) {
        auto status=access.read(kPll,pll[pass]);if(status!=Status::Ok)return finish(status);
        status=access.read(timer,control[pass]);if(status!=Status::Ok)return finish(status);
    }
    if(pll[0]!=pll[1]||control[0]!=control[1])return finish(Status::ChangedReference);
    ReferenceInfo r={};auto status=decodeReferences(board,pll[0],control[0],r);
    if(status!=Status::Ok)return finish(status);
    status=access.ready();if(status!=Status::Ok)return finish(status);
    r.pllRequestRegister=kPll;r.timerControlRegister=timer;out=r;return finish(Status::Ok);
}
} }
