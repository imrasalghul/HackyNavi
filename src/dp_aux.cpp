// SPDX-License-Identifier: MIT
/*
 * AUX/GPIO register protocol adapted from Linux commit
 * 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
 * display/dc/dce/dce_aux.c, dc/dc_ddc_types.h,
 * dc/dio/dcn10/dcn10_link_encoder.c, dc/dio/dcn32/dcn32_dio_link_encoder.c,
 * dc/gpio/hw_gpio.c, hw_ddc.c, ddc_regs.h, dcn32/hw_factory_dcn32.c,
 * dc/resource/dcn32/dcn32_resource.c and dcn_3_2_0_offset.h/sh_mask.h.
 * Copyright 2012-2022 Advanced Micro Devices, Inc.
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
#include "hackynavi/dp_aux.hpp"
namespace hackynavi { namespace dpaux {
namespace {
const uint32_t offsets[kRegisterCount] = {
    0x1f50,0x1f51,0x1f52,0x1f53,0x1f54,0x1f56,0x1f59,0x1f5a,0x1f5b,
    0x28d0,0x291d
};
constexpr uint32_t kGpioBits = 0x10101, kArbReq = 0x10000;
struct Dmu { unsigned count; uint64_t base; bool valid; };
void collect(const DiscoveryIp& ip, void* ctx) {
    if (ip.hardwareId != 271) return;
    Dmu& d = *static_cast<Dmu*>(ctx); ++d.count;
    d.valid = ip.dieId == 0 && ip.instance == 0 && ip.major == 3 && ip.minor == 2 &&
        ip.revision == 0 && !ip.subRevision && !ip.variant && !ip.harvest &&
        ip.baseAddressCount >= 3 && ip.baseAddresses[2] <= UINT32_MAX;
    if (d.valid) d.base = ip.baseAddresses[2];
}
bool populate(uint64_t base, uint64_t bytes, Layout& l) {
    l = Layout{};
    if (bytes < 4 || base > UINT32_MAX/4) return false;
    for (size_t e=0; e<kEngineCount; ++e) for (size_t r=0; r<kRegisterCount; ++r) {
        const uint64_t stride = r < 9 ? e*0x1c : r == 9 ? e*4 : 0;
        const uint64_t off = (base + offsets[r] + stride)*4;
        if (off > UINT32_MAX || off > bytes-4) return false;
        l.registers[e][r] = static_cast<uint32_t>(off);
    }
    l.barBytes = bytes; l.valid = true; return true;
}
bool validLayout(const Layout& l) {
    if (!l.valid || (l.registers[0][0]&3) || l.registers[0][0]/4 < offsets[0]) return false;
    Layout expected{};
    if (!populate(l.registers[0][0]/4-offsets[0],l.barBytes,expected)) return false;
    for (size_t e=0;e<kEngineCount;++e) for (size_t r=0;r<kRegisterCount;++r)
        if (expected.registers[e][r] != l.registers[e][r]) return false;
    return true;
}
void clearResult(Result& r) { r = Result{}; r.reply = 0xff; }
bool native(Operation o) { return o == Operation::NativeWrite || o == Operation::NativeRead; }
bool writing(Operation o) { return o == Operation::NativeWrite || o == Operation::I2cWrite; }
bool validRequest(const Request& q) {
    switch (q.operation) {
    case Operation::NativeWrite: case Operation::NativeRead:
        if (q.mot || !q.length || q.address > 0xfffff || q.length-1U > 0xfffff-q.address)
            return false;
        break;
    case Operation::I2cWrite: case Operation::I2cRead:
        if (q.address > 0x7f) return false;
        break;
    case Operation::I2cWriteStatus:
        if (q.address > 0x7f || q.length) return false;
        break;
    default: return false;
    }
    return q.length <= kMaxPayload && (!writing(q.operation) || !q.length || q.writeData);
}
} // namespace
Status buildLayout(const uint8_t* b,size_t n,uint64_t barBytes,Layout& out) {
    out = Layout{}; DiscoveryInfo info{}; Dmu d{};
    if (parseDiscovery(b,n,info,collect,&d) != DiscoveryStatus::Ok) return Status::DiscoveryRejected;
    if (!d.count) return Status::MissingDmu;
    if (d.count != 1 || !d.valid || info.baseAddresses64Bit || info.dieCount != 1 || info.dies[0].id)
        return Status::UnsupportedDmu;
    Layout l{};
    if (!populate(d.base,barBytes,l)) return Status::OutsideBar;
    out = l; return Status::Ok;
}
Engine::Engine() : phase_(Phase::Closed), io_{}, reg_{}, gpioSaved_(0),
    gpioAuxSaved_(0), engine_(0), hpd_(0), i2cAddress_(0), motOpen_(false) {}
void Engine::clearAfterVerifiedReset() {
    phase_ = Phase::Closed; io_ = Io{};
    for (size_t i=0;i<kRegisterCount;++i) reg_[i]=0;
    gpioSaved_=gpioAuxSaved_=0; engine_=hpd_=i2cAddress_=0; motOpen_=false;
}
Status Engine::read(Register r,uint32_t& value,Result& result) {
    result.lastRegister=reg_[static_cast<size_t>(r)]; ++result.reads;
    if (!io_.read32(io_.context,result.lastRegister,value)) {
        phase_=Phase::Faulted; return Status::TransportError;
    }
    if (value == UINT32_MAX) { phase_=Phase::Faulted; return Status::DeviceUnavailable; }
    return Status::Ok;
}
Status Engine::write(Register r,uint32_t value,Result& result) {
    result.lastRegister=reg_[static_cast<size_t>(r)]; ++result.writes;
    if (!io_.write32(io_.context,result.lastRegister,value)) {
        phase_=Phase::Faulted; return Status::TransportError;
    }
    return Status::Ok;
}
Status Engine::update(Register r,uint32_t mask,uint32_t value,Result& out,bool verify) {
    uint32_t old=0; Status s=read(r,old,out);
    if (s!=Status::Ok) return s;
    s=write(r,(old&~mask)|(value&mask),out);
    if (s!=Status::Ok || !verify) return s;
    s=read(r,old,out);
    if (s!=Status::Ok) return s;
    if ((old&mask)!=(value&mask)) { phase_=Phase::Faulted; return Status::VerifyFailed; }
    return Status::Ok;
}
Status Engine::wait(Register r,uint32_t mask,uint32_t value,uint32_t count,
                    uint32_t delay,Status timeout,Result& out) {
    for (uint32_t i=0;i<count;++i) {
        uint32_t v=0; ++out.polls; Status s=read(r,v,out);
        if (s!=Status::Ok) return s;
        if (r==Register::Status) out.rawStatus=v;
        if ((v&mask)==value) return Status::Ok;
        io_.delayUs(io_.context,delay);
    }
    phase_=Phase::Faulted; return timeout;
}
Status Engine::owned(Result& out) {
    uint32_t v=0; Status s=read(Register::Arbitration,v,out);
    if (s!=Status::Ok) return s;
    // Ownership is the hardware grant, not persistence of the request strobe.
    // The request bit also aliases SW_PENDING_USE_AUX_REG_REQ.
    if ((v&0xc)!=4) { phase_=Phase::Faulted; return Status::OwnershipLost; }
    return Status::Ok;
}
#define TRY_AUX(expression) do { const Status status_ = (expression); if (status_ != Status::Ok) return status_; } while (0)
Status Engine::open(const Layout& l,const Io& io,uint8_t engine,uint8_t hpd,Result& out) {
    clearResult(out);
    if (phase_!=Phase::Closed) return phase_==Phase::Faulted?Status::Faulted:Status::Busy;
    if (!validLayout(l) || !io.read32 || !io.write32 || !io.delayUs || engine>=5 || hpd>=5)
        return Status::InvalidArgument;
    io_=io; engine_=engine; hpd_=hpd;
    for (size_t r=0;r<kRegisterCount;++r) reg_[r]=l.registers[engine][r];
    uint32_t arb=0,control=0,sw=0,pending=0;
    TRY_AUX(read(Register::Arbitration,arb,out));
    if ((arb&0xc)==8 || (arb&0xc)==12 || (arb&kArbReq)) return Status::Busy;
    TRY_AUX(read(Register::Control,control,out));
    TRY_AUX(read(Register::SwControl,sw,out));
    TRY_AUX(read(Register::Status,pending,out));
    if ((control&0x30) || (sw&1) || (pending&2)) return Status::Busy;
    TRY_AUX(read(Register::GpioMask,gpioSaved_,out));
    TRY_AUX(read(Register::GpioAux,gpioAuxSaved_,out));
    phase_=Phase::Opening; // Any later ambiguity retains exclusive ownership.
    // DCN32 hw_factory uses DDC1..5; hardware mode clears both GPIO mask
    // bits. hw_ddc AUX mode selects AUX_PAD and clears DDC_PAD_I2CMODE.
    TRY_AUX(update(Register::GpioMask,kGpioBits,0x10000,out,true));
    TRY_AUX(update(Register::GpioAux,1U<<(12+engine_),0,out,true));
    if (!(control&1)) {
        TRY_AUX(write(Register::Control,control|0x11,out));
        TRY_AUX(wait(Register::Control,0x20,0x20,11,1,Status::ResetTimeout,out));
        TRY_AUX(write(Register::Control,(control|1)&~0x10U,out));
        TRY_AUX(wait(Register::Control,0x20,0,11,1,Status::ResetTimeout,out));
    }
    TRY_AUX(update(Register::Arbitration,0x30000,kArbReq,out));
    TRY_AUX(owned(out));
    // enc32_hw_init plus dcn10_aux_initialize. Refclock divider is not guessed;
    // platform display bring-up must establish it before opening AUX.
    TRY_AUX(write(Register::RxControl0,0x103d1110,out));
    TRY_AUX(write(Register::TxControl,0x21c7a,out));
    TRY_AUX(update(Register::Control,0x710100,static_cast<uint32_t>(hpd)<<20,out,true));
    TRY_AUX(update(Register::RxControl0,0x700,0,out,true));
    TRY_AUX(update(Register::RxControl1,0x1ff00,69U<<8,out,true));
    TRY_AUX(read(Register::Control,control,out));
    if ((control&0x31)!=1) { phase_=Phase::Faulted; return Status::VerifyFailed; }
    TRY_AUX(owned(out));
    phase_=Phase::Open; return Status::Ok;
}
Status Engine::transfer(const Request& q,Result& out) {
    clearResult(out);
    if (!validRequest(q)) return Status::InvalidArgument;
    if (phase_!=Phase::Open) return phase_==Phase::Faulted?Status::Faulted:Status::Closed;
    // A native transaction in an unfinished I2C sequence would silently strand
    // the bus. The owner must send a non-MOT I2C transaction or close first.
    if (motOpen_ && native(q.operation)) return Status::Busy;
    out.attempts=1;
    TRY_AUX(owned(out));
    uint32_t value=0;
    TRY_AUX(read(Register::Control,value,out));
    if ((value&0x31)!=1) { phase_=Phase::Faulted; return Status::OwnershipLost; }
    // W1C only SW_DONE_ACK; never echo LS/GTC ACK bits observed in a read.
    TRY_AUX(read(Register::Interrupt,value,out));
    TRY_AUX(write(Register::Interrupt,(value&~0x2222U)|2U,out));
    TRY_AUX(wait(Register::Status,1,0,240,10,Status::CompletionTimeout,out));
    const uint32_t bytes=(q.length?4U:3U)+(writing(q.operation)?q.length:0U);
    TRY_AUX(update(Register::SwControl,0x1f00f1,bytes<<16,out));
    uint32_t data=0;
    TRY_AUX(read(Register::Data,data,out));
    data&=~0x801fff01U; // Explicit index0, write direction, header byte.
    const uint32_t command=static_cast<uint8_t>(q.operation)|(q.mot?0x40U:0);
    data|=0x80000000U|((command|(q.address>>16))<<8);
    TRY_AUX(write(Register::Data,data,out));
    data=(data&~0x8000ff00U)|(((q.address>>8)&0xff)<<8);
    TRY_AUX(write(Register::Data,data,out));
    data=(data&~0xff00U)|((q.address&0xff)<<8);
    TRY_AUX(write(Register::Data,data,out));
    if (q.length) {
        data=(data&~0xff00U)|((q.length-1U)<<8);
        TRY_AUX(write(Register::Data,data,out));
    }
    if (writing(q.operation)) for (size_t i=0;i<q.length;++i) {
        data=(data&~0xff00U)|(static_cast<uint32_t>(q.writeData[i])<<8);
        TRY_AUX(write(Register::Data,data,out));
    }
    TRY_AUX(owned(out));
    // Set the conservative I2C bus-ownership state before a possibly ambiguous
    // GO. Only an observed non-MOT completion releases the sink's I2C bus.
    if (!native(q.operation)) { motOpen_=true; i2cAddress_=static_cast<uint8_t>(q.address); }
    TRY_AUX(read(Register::SwControl,value,out));
    out.submitted=true;
    TRY_AUX(write(Register::SwControl,value|1U,out));
    TRY_AUX(wait(Register::Status,1,1,240,10,Status::CompletionTimeout,out));
    out.completed=true;
    TRY_AUX(owned(out));
    value=out.rawStatus;
    if (value&0x200) return Status::HpdDisconnected;
    if (value&0xf0) return Status::ReplyTimeout;
    // Linux ignores MIN_COUNT_VIOL (CTS debugging bit); reject malformed
    // symbols, overflow and partial-byte replies without consuming FIFO data.
    if (value&0xde4d00) return Status::InvalidReply;
    const uint32_t count=(value>>24)&31;
    if (!count || count>17) return Status::InvalidReply;
    // REG_UPDATE_SEQ_3 performs ONE initial read followed by three cached
    // writes. Extra RMW reads are unsafe on a side-effecting FIFO window.
    TRY_AUX(read(Register::Data,value,out));
    value&=~0x1f0000U;
    TRY_AUX(write(Register::Data,value,out));
    value|=0x80000000U;
    TRY_AUX(write(Register::Data,value,out));
    value|=1U;
    TRY_AUX(write(Register::Data,value,out));
    TRY_AUX(read(Register::Data,value,out));
    out.reply=static_cast<uint8_t>((value>>12)&15);
    if (out.reply==1 || (!native(q.operation) && out.reply==4)) return Status::Nack;
    if (out.reply==2 || (!native(q.operation) && out.reply==8)) return Status::Deferred;
    if (out.reply!=0) return Status::InvalidReply;
    const uint32_t payload=count-1;
    if (writing(q.operation) || q.operation==Operation::I2cWriteStatus) {
        if (native(q.operation) && payload) return Status::InvalidReply;
        if (payload>1) return Status::InvalidReply;
    } else if (payload>q.length) return Status::InvalidReply;
    for (uint32_t i=0;i<payload;++i) {
        TRY_AUX(read(Register::Data,value,out));
        out.data[i]=static_cast<uint8_t>(value>>8);
    }
    out.length=static_cast<uint8_t>(payload);
    if ((writing(q.operation) || q.operation==Operation::I2cWriteStatus) && payload)
        return Status::WritePending;
    if (!writing(q.operation) && q.operation!=Operation::I2cWriteStatus && payload!=q.length)
        return Status::ShortReply;
    if (!native(q.operation)) motOpen_=q.mot;
    return Status::Ok;
}
Status Engine::transferWithRetries(const Request& original,Result& out) {
    clearResult(out);
    if (!validRequest(original)) return Status::InvalidArgument;
    Request q=original;
    uint32_t timeouts=0,invalid=0,shorts=0;
    for (uint32_t i=0;i<32;++i) {
        Result one{}; const Status s=transfer(q,one);
        const uint32_t reads=out.reads,writes=out.writes,polls=out.polls,attempts=out.attempts;
        const bool submitted=out.submitted;
        out=one; out.reads+=reads; out.writes+=writes; out.polls+=polls;
        out.attempts+=attempts; out.submitted=out.submitted||submitted;
        if (s==Status::Ok) return s;
        if (s==Status::WritePending ||
            (s==Status::Deferred && one.reply==8 && q.operation==Operation::I2cWrite)) {
            q.operation=Operation::I2cWriteStatus; q.length=0; q.writeData=nullptr;
        } else if (s==Status::Deferred) {
            // AUX DEFER means the transaction was not accepted; repeat it.
        } else if (s==Status::ReplyTimeout) {
            if (++timeouts>=3) return s;
        } else if (s==Status::InvalidReply) {
            if (++invalid>=2) return s;
        } else if (s==Status::ShortReply) {
            if (!native(q.operation)) return s;
            if (++shorts>=7) return s;
        } else return s;
        if (i!=31) io_.delayUs(io_.context,300);
    }
    return Status::RetryExhausted;
}
Status Engine::close(Result& out) {
    clearResult(out);
    if (phase_==Phase::Closed) return Status::Ok;
    if (phase_!=Phase::Open) return Status::Faulted;
    if (motOpen_) {
        const Request stop{Operation::I2cWrite,i2cAddress_,0,false,nullptr};
        const Status s=transferWithRetries(stop,out);
        if (s!=Status::Ok) { phase_=Phase::Faulted; return s; }
    }
    TRY_AUX(owned(out));
    // Restore selected GPIO/pad fields only; preserve other connectors' bits.
    TRY_AUX(update(Register::GpioMask,kGpioBits,gpioSaved_,out,true));
    TRY_AUX(update(Register::GpioAux,1U<<(12+engine_),gpioAuxSaved_,out,true));
    TRY_AUX(update(Register::Arbitration,0x30000,0x20000,out));
    uint32_t arb=0; TRY_AUX(read(Register::Arbitration,arb,out));
    // A posted release is verified through the ownership field itself. A
    // cleared request strobe alone is insufficient (it may clear on grant).
    // No undocumented release ACK or reset is fabricated.
    if ((arb&kArbReq) || (arb&0xc)==4 || (arb&0xc)==12) {
        phase_=Phase::Faulted; return Status::VerifyFailed;
    }
    clearAfterVerifiedReset(); return Status::Ok;
}
#undef TRY_AUX
const char* statusString(Status s) {
    switch (s) {
#define AUX_CASE(x) case Status::x: return #x
    AUX_CASE(Ok); AUX_CASE(InvalidArgument); AUX_CASE(DiscoveryRejected); AUX_CASE(MissingDmu);
    AUX_CASE(UnsupportedDmu); AUX_CASE(OutsideBar); AUX_CASE(Closed); AUX_CASE(Busy);
    AUX_CASE(Faulted); AUX_CASE(TransportError); AUX_CASE(DeviceUnavailable);
    AUX_CASE(OwnershipLost); AUX_CASE(ResetTimeout); AUX_CASE(CompletionTimeout);
    AUX_CASE(HpdDisconnected); AUX_CASE(ReplyTimeout); AUX_CASE(InvalidReply); AUX_CASE(Nack);
    AUX_CASE(Deferred); AUX_CASE(ShortReply); AUX_CASE(WritePending); AUX_CASE(VerifyFailed);
    AUX_CASE(RetryExhausted);
#undef AUX_CASE
    }
    return "Unknown";
}
} }
