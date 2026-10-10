// SPDX-License-Identifier: MIT
/*
 * Copyright 2012-16 Advanced Micro Devices, Inc.
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
// Pinned Linux3857c2fe: DCN32 integer presets/DFS slice addresses;
// dce100/dcn20 divider conversions; generated DCN3.2 DENTIST register fields.
#include "hackynavi/display_dentist.hpp"
namespace hackynavi { namespace displaydentist { namespace {
constexpr uint32_t kPll=0x16e37*4;
constexpr uint32_t kDfs[kClockCount]={0x16e69*4,0x16e6c*4,0x16e6f*4,0x16e72*4,0x16e75*4};
uint32_t encodeDivider(uint64_t divider) {
    if(divider<64)return divider<8?8:uint32_t(divider);
    if(divider<128)return 64+uint32_t((divider-64)/2);
    if(divider<248)return 96+uint32_t((divider-128)/4);
    return divider<512?126:127;
}
struct Dmu {unsigned count;bool valid;uint64_t base;};
void collect(const DiscoveryIp& ip,void* context) {
    if(ip.hardwareId!=271)return;
    auto& d=*static_cast<Dmu*>(context);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&
        !ip.subRevision&&!ip.variant&&!ip.harvest&&ip.baseAddressCount>=3;
    if(d.valid)d.base=ip.baseAddresses[1];
}
bool valid(const Layout& l) {
    if(!l.valid||l.barBytes<4||l.pll!=kPll||l.pll>l.barBytes-4||
        (l.dentist&3)||l.dentist<0x64*4||l.dentist>l.barBytes-4)return false;
    for(size_t i=0;i<kClockCount;++i)
        if(l.dfs[i]!=kDfs[i]||l.dfs[i]>l.barBytes-4||l.dentist==l.dfs[i])return false;
    return l.dentist!=l.pll;
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
        auto s=ready();if(s!=Status::Ok)return s;
        ++r.reads;r.lastRegister=address;
        const bool read=io.read32(io.context,address,value);r.lastValue=value;
        s=ready();if(s!=Status::Ok)return s;
        return !read?Status::ReadFailed:value==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
    }
};
} // namespace
Status decodeDivider(uint32_t did,uint32_t& divider) {
    divider=0;if(did<8||did>127)return Status::UnsupportedDivider;
    divider=did<64?did:did<96?64+2*(did-64):did<126?128+4*(did-96):248+264*(did-126);
    return Status::Ok;
}
Status plan(uint32_t vco,uint32_t required,Preset& out) {
    out={};if(!vco||!required)return Status::InvalidArgument;
    if(required>1950000||uint64_t(required)*2>vco)return Status::UnrepresentableFrequency;
    const uint64_t numerator=uint64_t(vco)*4;
    const uint64_t firstDivider=numerator/required;
    const uint64_t preset=numerator/firstDivider;
    const uint32_t did=encodeDivider(numerator/preset);
    uint32_t divider=0;
    if(decodeDivider(did,divider)!=Status::Ok)return Status::UnsupportedDivider;
    const uint64_t programmed=numerator/divider;
    if(!preset||preset/1000==0||preset>1950000||programmed>1950000||
        programmed<required||preset/1000>UINT16_MAX)return Status::UnrepresentableFrequency;
    out={required,uint32_t(preset),uint32_t(programmed),divider,uint16_t(preset/1000),uint8_t(did)};
    return Status::Ok;
}
Status buildLayout(const uint8_t* bytes,size_t count,uint64_t bar,Layout& out) {
    out={};Dmu d={};DiscoveryInfo discovery={};Navi31IpLayout ip={};IpLayoutDiagnostic diagnostic={};
    if(buildNavi31IpLayout(bytes,count,ip,diagnostic)!=IpLayoutStatus::Ok||
        parseDiscovery(bytes,count,discovery,collect,&d)!=DiscoveryStatus::Ok||d.count!=1||!d.valid||
        discovery.baseAddresses64Bit||discovery.dieCount!=1||discovery.dies[0].id)return Status::DiscoveryRejected;
    if(bar<4||d.base>UINT32_MAX/4-0x64)return Status::OutsideBar;
    Layout l={};l.valid=true;l.barBytes=bar;l.pll=kPll;l.dentist=uint32_t((d.base+0x64)*4);
    for(size_t i=0;i<kClockCount;++i)l.dfs[i]=kDfs[i];
    if(!valid(l))return Status::OutsideBar;
    out=l;return Status::Ok;
}
Status capture(const Layout& l,const displaymodel::BoardInfo& board,
    const displaymodel::ReferenceInfo& refs,uint32_t mask,const Io& io,uint32_t budget,
    Observation& out,Result& result) {
    out={};result={};auto finish=[&](Status s){result.status=s;return s;};
    if(!valid(l)||!mask||(mask&~kAllClocks)||!io.valid||!io.read32||!io.timeUs||
        !budget||budget>2000000)return finish(Status::InvalidArgument);
    displaymodel::ReferenceInfo decoded={};
    if(displaymodel::decodeReferences(board,refs.rawPllRequest,refs.rawTimerControl,decoded)!=displaymodel::Status::Ok||
        refs.pllRequestRegister!=l.pll||refs.programmedVcoKHz!=decoded.programmedVcoKHz||
        refs.dccgReferenceKHz!=decoded.dccgReferenceKHz||refs.dchubReferenceKHz!=decoded.dchubReferenceKHz)
        return finish(Status::ReferenceMismatch);
    const uint64_t start=io.timeUs(io.context);Access access={io,start,start,budget,result};
    uint32_t values[2][kClockCount+2]={};
    for(unsigned pass=0;pass<2;++pass) {
        auto s=access.read(l.pll,values[pass][0]);if(s!=Status::Ok)return finish(s);
        s=access.read(l.dentist,values[pass][1]);if(s!=Status::Ok)return finish(s);
        for(size_t i=0;i<kClockCount;++i){s=access.read(l.dfs[i],values[pass][i+2]);if(s!=Status::Ok)return finish(s);}
    }
    for(size_t i=0;i<kClockCount+2;++i)if(values[0][i]!=values[1][i])return finish(Status::ChangedProgramming);
    if(values[0][0]!=refs.rawPllRequest)return finish(Status::ReferenceMismatch);
    Observation observed={};observed.pll=values[0][0];observed.dentist=values[0][1];
    observed.programmedVcoKHz=decoded.programmedVcoKHz;
    for(size_t i=0;i<kClockCount;++i) {
        observed.rawDfs[i]=values[0][i+2];uint32_t divider=0;
        if(decodeDivider(observed.rawDfs[i],divider)!=Status::Ok)continue;
        const uint64_t frequency=uint64_t(decoded.programmedVcoKHz)*4/divider;
        if(!frequency||frequency>UINT32_MAX)continue;
        observed.validClockMask|=1u<<i;observed.programmedKHz[i]=uint32_t(frequency);
    }
    if((observed.validClockMask&mask)!=mask)return finish(Status::UnsupportedDivider);
    observed.dispChangeDone=(observed.dentist&0x80000)!=0;
    observed.dppChangeDone=(observed.dentist&0x100000)!=0;
    observed.dispDividerAgrees=(observed.validClockMask&1)&&
        (observed.dentist&0x7f)==observed.rawDfs[0]&&((observed.dentist>>8)&0x7f)==observed.rawDfs[0];
    auto s=access.ready();if(s!=Status::Ok)return finish(s);
    out=observed;return finish(Status::Ok);
}
} }
