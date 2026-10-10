// SPDX-License-Identifier: MIT
/*
 * Copyright 2020 Advanced Micro Devices, Inc.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
// Register sequence derived from Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// dcn30_hubp.c:45-66 hubp3_set_vm_system_aperture_settings, selected by
// dcn32_hubp.c:198; amdgpu_dm.c:mmhub_read_system_context selects fb_start/end
// when AGP is disabled. dcn20_hwseq.c:enable_plane clocks and initializes HUBP
// before setting its aperture. Register layouts come from hubp_scanout.cpp.
#include "hackynavi/hubp_aperture.hpp"
namespace hackynavi { namespace hubpaperture { namespace {
using Register=hubpscanout::Register;
class Access {
public:
    Access(const hubpscanout::Layout& l,const hubpscanout::Io& i,uint8_t p,Result& r)
        :layout(l),io(i),pipe(p),result(r),powerStatus(0),powerConfig(0),powerValid(false){}
    Status raw(uint32_t address,uint32_t& value) {
        ++result.reads;result.lastRegister=address;
        if(!io.read32(io.context,address,value))return Status::TransportError;
        result.lastValue=value;
        return value==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
    }
    Status power() {
        uint32_t status=0,config=0,after=0;auto s=raw(address(Register::Power),status);if(s!=Status::Ok)return s;
        if(status!=0&&status!=0x10000000)return Status::PowerUnavailable;
        s=raw(address(Register::PowerConfig),config);if(s!=Status::Ok)return s;
        if(config>1)return Status::PowerUnavailable;
        if(powerValid&&(status!=powerStatus||config!=powerConfig))return Status::PowerUnavailable;
        s=raw(address(Register::Power),after);if(s!=Status::Ok)return s;
        if(after!=status)return Status::PowerUnavailable;
        s=raw(address(Register::PowerConfig),after);if(s!=Status::Ok)return s;
        if(after!=config)return Status::PowerUnavailable;
        s=raw(address(Register::Power),after);if(s!=Status::Ok)return s;
        if(after!=status)return Status::PowerUnavailable;
        powerStatus=status;powerConfig=config;powerValid=true;return Status::Ok;
    }
    Status readAddress(uint32_t offset,uint32_t& value) {
        auto s=power();if(s!=Status::Ok)return s;
        s=raw(offset,value);return s==Status::Ok?power():s;
    }
    Status read(Register reg,uint32_t& value) {return readAddress(address(reg),value);}
    Status write(Register reg,uint32_t value) {
        auto s=power();if(s!=Status::Ok)return s;
        result.writeAttempted=true;++result.writes;
        result.lastRegister=address(reg);result.lastValue=value;
        if(!io.write32(io.context,result.lastRegister,value))return Status::TransportError;
        uint32_t actual=0;s=read(reg,actual);if(s!=Status::Ok)return s;
        return actual==value?Status::Ok:Status::VerifyFailed;
    }
private:
    uint32_t address(Register reg) const {return layout.registers[pipe][size_t(reg)];}
    const hubpscanout::Layout& layout;const hubpscanout::Io& io;uint8_t pipe;Result& result;
    uint32_t powerStatus,powerConfig;bool powerValid;
};
Status finish(Status status,Result& r){r.status=status;return status;}
} // namespace
Status program(const uint8_t* discovery,size_t bytes,uint64_t barBytes,
    const hubpscanout::Io& io,uint8_t pipe,const MemoryLayout& memory,Result& r) {
    r={};
    if(!io.read32||!io.write32||pipe>=hubpscanout::kPipeCount||
       memory.mcWindowStart>memory.mcWindowEndInclusive||
       memory.mcWindowEndInclusive>=(UINT64_C(1)<<48))return finish(Status::InvalidArgument,r);
    hubpscanout::Layout layout={};
    r.layoutStatus=hubpscanout::buildLayout(discovery,bytes,barBytes,layout);
    if(r.layoutStatus!=hubpscanout::Status::Ok)return finish(Status::DiscoveryRejected,r);
    Access access(layout,io,pipe,r);uint32_t fb[5]={};
    for(size_t n=0;n<5;++n) {
        auto s=access.readAddress(layout.framebuffer[n],fb[n]);if(s!=Status::Ok)return finish(s,r);
        if(fb[n]&0xff000000)return finish(Status::AddressMode,r);
    }
    if((uint64_t(fb[0])<<24)!=memory.mcWindowStart||
       ((uint64_t(fb[1])<<24)|0xffffff)!=memory.mcWindowEndInclusive||
       (uint64_t(fb[2])<<24)!=memory.vramPageBaseOffset||fb[3]<=fb[4])return finish(Status::AddressMode,r);
    struct Check {Register reg;uint32_t mask,value;Status failure;};
    const Check checks[]={
        {Register::Control,0x70f00807,3,Status::ActivePipe},
        {Register::Clock,1,1,Status::ClockUnavailable},
        {Register::OtgControl,0x10001,0,Status::ActivePipe},
        {Register::Vmid,UINT32_MAX,0,Status::AddressMode},
        {Register::FlipControl,0x100,0,Status::ActivePipe},
        {Register::CursorControl,1,0,Status::ForeignControl},
        {Register::DmdataControl,7,0,Status::ForeignControl},
        {Register::MallConfig,7,0,Status::ForeignControl}
    };
    for(const auto& check:checks) {
        uint32_t value=0;auto s=access.read(check.reg,value);if(s!=Status::Ok)return finish(s,r);
        if((value&check.mask)!=check.value)return finish(check.failure,r);
    }
    auto s=access.write(Register::ApertureLow,uint32_t(memory.mcWindowStart>>18));
    if(s!=Status::Ok)return finish(s,r);
    s=access.write(Register::ApertureHigh,uint32_t(memory.mcWindowEndInclusive>>18));
    if(s!=Status::Ok)return finish(s,r);
    // REG_SET_2(...,0, ENABLE_L1_TLB,1, SYSTEM_ACCESS_MODE,3), masks 1/0x18.
    return finish(access.write(Register::TlbControl,0x19),r);
}
} }
