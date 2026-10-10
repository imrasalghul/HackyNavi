// SPDX-License-Identifier: MIT
// Cold admission policy using Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// vcn_v4_0_stop/disable_static_power_gating, jpeg_v4_0_stop/is_idle,
// lsdma_v6_0_wait_pio_status and their generated register fields.
// Copyright 2021-2022 Advanced Micro Devices, Inc.
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
#include "hackynavi/cold_aux.hpp"
namespace hackynavi { namespace coldaux {
namespace {
struct Field { uint32_t offset,mask,value; };
const Field vcnFields[kVcnRegisters]={
    {2,0x80000107,0}, // static, awake, no dynamic PG autonomous restore
    {0x85,0x1ff,0}, // all unified/legacy/media ring enable bits
    {0x196,0x10000200,0x10000000}, // VCPU reset asserted, clock disabled
    {0x147,0x2004,0x2004}, // LMI + LMI_UMC reset asserted
    {0x106,8,8}, // VCPU register access disabled
    {0x2ae,0x100,0x100}, // UMC arbiter stalled
    {0x2b1,0x2cf,0x24f}, // Linux LMI/UMC clean set; pending MC write clear
    {0x143,7,0}}; // stop path clears status after VCPU/LMI reset
const Field jpegFields[kJpegRegisters]={
    {3,0x80000011,0},{0x641,1,1},{0x6d2,1,1},
    {0x649,0x1fff,3}, // both ring/IB done, no timeout/trap/preemption
    {0x69f,0x1ffff,0x1bfff}}; // all JMI clients clean, MC_WRITE_PENDING clear
const Field lsdmaFields[kLsdmaRegisters]={
    {0x80,0x1001,0},{0x8a,1,0},{0x87,1,0},
    {0xd8,0x1001,0},{0xe2,1,0},{0xdf,1,0},
    {0x73,0x60000,0}, // no autonomous context/world switch
    {0x32,0xb0078fff,0x90000000}, // PIO idle/empty, no queued command/error
    {0x25,0x002e3201,0x000e2001}, // engine/MC idle; no IB/packet/read stall
    {0x26,0x0001be7f,0x27f}, // copy engine idle; no full/stalled queues
    {0x3e,0x003fffff,0x001001ff}, // translation read queues empty/no fault
    {0x3f,0xf03fffff,0x501001ff}}; // translation/write-data queues empty
struct Bases { uint64_t vcn[2],lsdma,mp0;unsigned seen[4];bool bad; };
void collect(const DiscoveryIp& ip,void* context) {
    auto& b=*static_cast<Bases*>(context); unsigned which=4;
    if(ip.hardwareId==12)which=ip.instance<2?ip.instance:4;
    else if(ip.hardwareId==91)which=2;
    else if(ip.hardwareId==255)which=3;
    else return;
    if(which==4){b.bad=true;return;}
    ++b.seen[which];
    if(ip.dieId || ip.harvest || ip.subRevision || ip.minor || ip.baseAddressCount<2 ||
        (which<2?(ip.major!=4 || ip.revision!=which || ip.variant!=which):
        (ip.instance || ip.major!=(which==2?6:13) || ip.revision || ip.variant))) {b.bad=true;return;}
    if(which<2)b.vcn[which]=ip.baseAddresses[1];
    else if(which==2)b.lsdma=ip.baseAddresses[0];else b.mp0=ip.baseAddresses[0];
}
bool address(uint64_t base,uint32_t reg,uint64_t bytes,uint32_t& out) {
    const uint64_t value=(base+reg)*4;
    if(base>UINT32_MAX/4 || bytes<4 || value>UINT32_MAX || value>bytes-4 ||
        value<0x1c || (value>=0x7f000 && value<0x80000))return false;
    out=static_cast<uint32_t>(value);return true;
}
bool populate(const Bases& b,uint64_t bytes,Layout& out) {
    out=Layout{};out.barBytes=bytes;
    if(!address(b.mp0,0x91,bytes,out.sos))return false;
    for(size_t v=0;v<2;++v) {
        if(!address(b.vcn[v],1,bytes,out.vcnPower[v]) || !address(b.vcn[v],0,bytes,out.vcnPowerConfig[v]))return false;
        for(size_t i=0;i<kVcnRegisters;++i)if(!address(b.vcn[v],vcnFields[i].offset,bytes,out.vcn[v][i]))return false;
    }
    for(size_t i=0;i<kJpegRegisters;++i)if(!address(b.vcn[0],jpegFields[i].offset,bytes,out.jpeg[i]))return false;
    for(size_t i=0;i<kLsdmaRegisters;++i)if(!address(b.lsdma,lsdmaFields[i].offset,bytes,out.lsdma[i]))return false;
    // Compare every distinct semantic address; no cross-base alias is allowed.
    uint32_t all[1+4+2*kVcnRegisters+kJpegRegisters+kLsdmaRegisters]={};size_t n=0;
    all[n++]=out.sos;
    for(size_t v=0;v<2;++v){all[n++]=out.vcnPower[v];all[n++]=out.vcnPowerConfig[v];
        for(size_t i=0;i<kVcnRegisters;++i)all[n++]=out.vcn[v][i];}
    for(size_t i=0;i<kJpegRegisters;++i)all[n++]=out.jpeg[i];
    for(size_t i=0;i<kLsdmaRegisters;++i)all[n++]=out.lsdma[i];
    for(size_t i=0;i<n;++i)for(size_t j=0;j<i;++j)if(all[i]==all[j])return false;
    out.valid=true;return true;
}
bool valid(const Layout& l) {
    if(!l.valid || (l.sos&3) || l.sos/4<0x91 || (l.lsdma[0]&3) || l.lsdma[0]/4<0x80)return false;
    Bases b={};b.mp0=l.sos/4-0x91;b.lsdma=l.lsdma[0]/4-0x80;
    for(size_t v=0;v<2;++v){if((l.vcnPower[v]&3) || !l.vcnPower[v])return false;b.vcn[v]=l.vcnPower[v]/4-1;}
    Layout expected={};if(!populate(b,l.barBytes,expected) || l.sos!=expected.sos)return false;
    for(size_t v=0;v<2;++v)if(l.vcnPowerConfig[v]!=expected.vcnPowerConfig[v])return false;
    for(size_t v=0;v<2;++v)for(size_t i=0;i<kVcnRegisters;++i)if(l.vcn[v][i]!=expected.vcn[v][i])return false;
    for(size_t i=0;i<kJpegRegisters;++i)if(l.jpeg[i]!=expected.jpeg[i])return false;
    for(size_t i=0;i<kLsdmaRegisters;++i)if(l.lsdma[i]!=expected.lsdma[i])return false;
    return true;
}
Status finish(Result& r,Status s){r.status=s;return s;}
bool read(const Io& io,uint32_t reg,uint32_t& value,Result& r) {
    r.lastRegister=reg;++r.reads;
    if(!io.read32(io.context,reg,value)){finish(r,Status::ReadFailed);return false;}
    r.lastValue=value;
    if(value==UINT32_MAX){finish(r,Status::DeviceUnavailable);return false;}return true;
}
bool match(const Io& io,uint32_t reg,const Field& field,Status failure,Result& r) {
    uint32_t value=0;r.mask=field.mask;r.expected=field.value;
    if(!read(io,reg,value,r))return false;
    if((value&field.mask)!=field.value){finish(r,failure);return false;}return true;
}
bool powered(uint32_t value,size_t instance,bool jpeg,Result& r) {
    const uint32_t media=value&0x3f3fffff;
    // Linux's powered register-access configurations: normal/static mode,
    // including VCN1's physically absent subblocks. Never accept OFF alone.
    if((jpeg?(value&0x00c00000)!=0:(media!=(instance?0x02200800U:0U) && media!=0x2a2a8aa0U))) {
        finish(r,Status::PowerUnavailable);return false;
    }return true;
}
bool power(const Layout& l,const Io& io,size_t instance,bool jpeg,Result& r) {
    uint32_t value=0;r.instance=static_cast<uint8_t>(instance);
    return read(io,l.vcnPower[instance],value,r) && powered(value,instance,jpeg,r);
}
bool classifyPower(const Layout& l,const Io& io,size_t instance,bool jpeg,bool& off,Result& r) {
    uint32_t status=0,config=0,after=0;r.instance=static_cast<uint8_t>(instance);
    const uint32_t mask=jpeg?0x00c00000:0x3f3fffff;
    const uint32_t disabled=jpeg?0x00800000:0x2a2aaaaa;
    if(!read(io,l.vcnPower[instance],status,r))return false;
    off=(status&mask)==disabled;
    if(!off && !powered(status,instance,jpeg,r))return false;
    // Linux's completed static-power-off encoding, with no outstanding ON
    // request in the corresponding two-bit CONFIG fields. Check the current
    // status again after CONFIG; never probe the powered-off client window.
    if(!read(io,l.vcnPowerConfig[instance],config,r) || !read(io,l.vcnPower[instance],after,r))return false;
    if((after&mask)!=(status&mask)){finish(r,Status::PowerUnavailable);return false;}
    // CONFIG=0 leaves the corresponding tile unchanged (the Linux JPEG write
    // zeros every unrelated tile). 1 requests ON; 2 requests OFF. Reject3 and
    // contrary requests, including wake requests against an OFF admission.
    // VCN1's absent B/TB subblocks stay OFF even in Linux's all-ON request.
    const uint32_t absent=(!jpeg && instance && !off && (status&mask)==0x02200800)?0x02200800:0;
    for(unsigned shift=0;shift<30;shift+=2)if(mask&(3U<<shift)) {
        const uint32_t request=(config>>shift)&3,current=(status>>shift)&3;
        if(request==3 || (request && request!=(current?2U:1U) && !(request==1 && (absent&(3U<<shift))))) {
            finish(r,Status::PowerUnavailable);return false;
        }
    }
    return true;
}
}
Status buildLayout(const uint8_t* bytes,size_t count,uint64_t barBytes,Layout& out) {
    out=Layout{};DiscoveryInfo info={};Bases b={};
    if(parseDiscovery(bytes,count,info,collect,&b)!=DiscoveryStatus::Ok)return Status::DiscoveryRejected;
    if(b.bad || info.baseAddresses64Bit || info.dieCount!=1 || info.dies[0].id)return Status::UnsupportedLayout;
    for(unsigned countSeen:b.seen)if(countSeen!=1)return Status::UnsupportedLayout;
    Layout candidate={};if(!populate(b,barBytes,candidate))return Status::OutsideBar;
    out=candidate;return Status::Ok;
}
Status checkStopped(const Layout& l,const Io& io,Result& r) {
    r=Result{};r.instance=0xff;
    if(!io.read32 || !valid(l))return finish(r,Status::InvalidArgument);
    const Field zero={0,UINT32_MAX,0};
    uint8_t offMask=0;
    for(unsigned pass=0;pass<2;++pass) {
        if(!match(io,l.sos,zero,Status::PriorDriverRequiresReset,r))return r.status;
        uint8_t observedOff=0;
        for(size_t v=0;v<2;++v) {
            bool off=false;if(!classifyPower(l,io,v,false,off,r))return r.status;
            if(off){observedOff|=uint8_t(1U<<v);continue;}
            for(size_t i=0;i<kVcnRegisters;++i) {
                if(!power(l,io,v,false,r) || !match(io,l.vcn[v][i],vcnFields[i],Status::VcnNotStopped,r) ||
                    !power(l,io,v,false,r))return r.status;
            }
        }
        bool jpegOff=false;if(!classifyPower(l,io,0,true,jpegOff,r))return r.status;
        if(jpegOff)observedOff|=4;
        else for(size_t i=0;i<kJpegRegisters;++i)
                if(!power(l,io,0,true,r) || !match(io,l.jpeg[i],jpegFields[i],Status::JpegNotStopped,r) ||
                    !power(l,io,0,true,r))return r.status;
        if(pass && observedOff!=offMask)return finish(r,Status::PowerUnavailable);
        offMask=observedOff;
        r.instance=0xff;
        for(size_t i=0;i<kLsdmaRegisters;++i)
            if(!match(io,l.lsdma[i],lsdmaFields[i],Status::LsdmaNotIdle,r))return r.status;
    }
    for(unsigned index=0;index<3;++index){bool off=false;
        if(!classifyPower(l,io,index==1?1:0,index==2,off,r))return r.status;
        if(off!=bool(offMask&(1U<<index)))return finish(r,Status::PowerUnavailable);
    }
    if(!match(io,l.sos,zero,Status::PriorDriverRequiresReset,r))return r.status;
    r.poweredOffMask=offMask;r.instance=0xff;
    return finish(r,Status::Ok);
}
const char* statusString(Status s){switch(s){
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(DiscoveryRejected);NAME(UnsupportedLayout);NAME(OutsideBar);
    NAME(ReadFailed);NAME(DeviceUnavailable);NAME(PriorDriverRequiresReset);NAME(PowerUnavailable);
    NAME(VcnNotStopped);NAME(JpegNotStopped);NAME(LsdmaNotIdle);
#undef NAME
    }return "Unknown";}
} }
