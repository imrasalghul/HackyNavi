// SPDX-License-Identifier: MIT
// Register protocol derived from Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// dcn20_hubp.c:hubp2_set_blank_regs, dcn32_hubp.c:phantom_hubp_post_enable,
// dcn20_hwseq.c:plane_atomic_disable and generated DCN3.2 register fields.
// Copyright 2012-2022 Advanced Micro Devices, Inc.
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
#include "hackynavi/boot_display_stop.hpp"
namespace hackynavi { namespace bootdisplaystop {
namespace {
constexpr size_t kControl=static_cast<size_t>(bootdisplay::PipeRegister::Control);
constexpr uint32_t kVolatile=0x70ff000a,kW1Clear=0x84000000;
bool read(const Io& io,uint32_t offset,uint32_t& value,Result& r) {
    r.lastRegister=offset;r.lastValue=0;r.lastReadSucceeded=false;++r.reads;
    if(!io.read32(io.context,offset,value)){r.status=Status::ReadFailed;return false;}
    r.lastValue=value;r.lastReadSucceeded=true;
    if(value==UINT32_MAX){r.status=Status::DeviceUnavailable;return false;}return true;
}
bool equal(const Io& io,uint32_t offset,uint32_t expected,uint32_t mask,Status fail,Result& r) {
    uint32_t value=0;if(!read(io,offset,value,r))return false;
    if((value&mask)!=(expected&mask)){r.status=fail;return false;}return true;
}
bool power(const Io& io,const bootdisplay::Layout& l,const bootdisplay::Snapshot& s,size_t p,Result& r) {
    // The actual cold capture admitted current PGFSM0 or2 and the matching
    // request. Preserve every raw bit here, including desired bit28: either
    // initial OFF encoding is allowed, but a later 0x80000000/0x90000000
    // change invalidates this retained epoch before any gated-window access.
    r.pipe=uint8_t(p);r.lastPowerProbe={};r.lastPowerProbe.pipe=uint8_t(p);
    const uint32_t registers[]={l.power[p],l.powerConfig[p],l.power[p]};
    const uint32_t expected[]={s.power[p],s.powerConfig[p],s.power[p]};
    uint32_t* values[]={&r.lastPowerProbe.statusBefore,&r.lastPowerProbe.config,&r.lastPowerProbe.statusAfter};
    for(unsigned i=0;i<3;++i) {
        uint32_t value=0;const bool ok=read(io,registers[i],value,r);
        if(r.lastReadSucceeded){*values[i]=value;r.lastPowerProbe.readsSucceededMask|=uint8_t(1U<<i);}
        if(!ok)return false;
        if(value!=expected[i]){r.status=Status::PowerChanged;return false;}
    }
    return true;
}
}
struct Access {
    static Status finish(const State& s,Result& r,Status status) {
        r.status=status;r.retentionRequired=s.retentionRequired();r.blankedMask=s.blanked_;
        r.writesMayHaveOccurred=s.wrote_;return status;
    }
    static Status fail(State& s,Result& r){s.phase_=Phase::Faulted;return finish(s,r,r.status);}
    static bool guardedRead(State& s,const Io& io,size_t p,uint32_t reg,uint32_t& value,Result& r) {
        return power(io,s.layout_,s.capture_.snapshot,p,r) && read(io,reg,value,r);
    }
    static bool clock(State& s,const Io& io,size_t p,Result& r) {
        uint32_t value=0;if(!guardedRead(s,io,p,s.clocks_[p],value,r))return false;
        if(!(value&1)){r.status=Status::ClockUnavailable;return false;}return true;
    }
    static uint32_t expectedControl(const State& s,size_t p) {
        uint32_t expected=s.capture_.snapshot.pipe[p][kControl];
        if(s.blanked_&(1U<<p))expected=(expected&~(0x1000U|kW1Clear))|1;
        return expected;
    }
    static bool observe(State& s,const Io& io,bool drained,Result& r) {
        const auto& l=s.layout_;const auto& snapshot=s.capture_.snapshot;
        for(size_t p=0;p<bootdisplay::kPipeCount;++p)if(!power(io,l,snapshot,p,r))return false;
        r.pipe=0xff;
        for(size_t i=0;i<bootdisplay::kVgaCount;++i)
            if(!equal(io,l.vga[i],snapshot.vga[i],UINT32_MAX,Status::SnapshotChanged,r))return false;
        for(size_t i=0;i<bootdisplay::kGlobalRegisterCount;++i)
            if(!equal(io,l.global[i],snapshot.global[i],UINT32_MAX,Status::SnapshotChanged,r))return false;
        for(size_t p=0;p<bootdisplay::kPipeCount;++p) {
            if(s.capture_.poweredOffMask&(1U<<p))continue;
            for(size_t i=0;i<bootdisplay::kPipeRegisterCount;++i) {
                uint32_t value=0;if(!guardedRead(s,io,p,l.pipe[p][i],value,r))return false;
                const uint32_t mask=i==kControl?~kVolatile:UINT32_MAX;
                const uint32_t expected=i==kControl?expectedControl(s,p):snapshot.pipe[p][i];
                if((value^expected)&mask){r.status=Status::SnapshotChanged;return false;}
                if(i==kControl && (drained || (expected&1)) && (value&3)!=3) {
                    r.status=Status::DrainTimeout;return false;
                }
            }
            if(!(snapshot.pipe[p][kControl]&1) && !clock(s,io,p,r))return false;
        }
        for(size_t p=0;p<bootdisplay::kPipeCount;++p)if(!power(io,l,snapshot,p,r))return false;
        return true;
    }
    static bool drain(State& s,const Io& io,size_t p,const PollOptions& options,bool blank,Result& r) {
        for(uint32_t i=0;i<options.iterations;++i) {
            uint32_t value=0;++r.polls;
            if(!clock(s,io,p,r) || !guardedRead(s,io,p,s.layout_.pipe[p][kControl],value,r))return false;
            if(((value^expectedControl(s,p))&~kVolatile) || (blank && !(value&1))) {
                r.status=Status::ReadbackMismatch;return false;
            }
            if(value&2)return true;
            if(i+1<options.iterations)io.delayUs(io.context,options.intervalUs);
        }
        r.status=Status::DrainTimeout;return false;
    }
    static Status capture(const bootdisplay::Layout& l,const MemoryLayout& m,const Io& io,State& s,Result& r) {
        r=Result{};r.pipe=r.lastPowerProbe.pipe=0xff;
        if(s.phase_!=Phase::Fresh)return finish(s,r,Status::InvalidState);
        if(!io.read32)return finish(s,r,Status::InvalidArgument);
        bootdisplay::Capture captured={};
        r.captureStatus=bootdisplay::capturePostedCold(l,m,{io.context,io.read32},captured);
        r.reads=captured.readsAttempted;r.lastRegister=captured.lastRegister;
        r.pipe=captured.failedPipe;r.lastValue=captured.lastValue;
        r.lastReadSucceeded=captured.lastReadSucceeded;r.lastPowerProbe=captured.lastPowerProbe;
        if(r.captureStatus!=bootdisplay::Status::Ok)return finish(s,r,Status::CaptureRejected);
        // Clock control is the next DWORD after DCHUBP_CNTL on all four HUBPs.
        for(size_t p=0;p<bootdisplay::kPipeCount;++p) {
            const uint64_t offset=uint64_t(l.pipe[p][kControl])+4;
            if(l.barBytes<4 || offset>UINT32_MAX || offset>l.barBytes-4)return finish(s,r,Status::InvalidArgument);
            s.clocks_[p]=uint32_t(offset);
        }
        s.layout_=l;s.capture_=captured;
        // Do not publish an armed state until all initial active clocks and
        // retained observations have been independently rechecked.
        if(!observe(s,io,false,r) || !equal(io,l.sos,0,UINT32_MAX,Status::SnapshotChanged,r)) {
            s.capture_=bootdisplay::Capture{};s.layout_=bootdisplay::Layout{};
            return finish(s,r,r.status);
        }
        s.phase_=Phase::Armed;return finish(s,r,Status::Ok);
    }
    static Status stop(const Io& io,const PollOptions& p,State& s,Result& r) {
        r=Result{};r.pipe=r.lastPowerProbe.pipe=0xff;
        if(s.phase_!=Phase::Armed)return finish(s,r,Status::InvalidState);
        if(!io.read32 || !io.write32 || !io.delayUs || !p.iterations || !p.intervalUs ||
            p.intervalUs>100 || uint64_t(p.iterations)*p.intervalUs>100000)return finish(s,r,Status::InvalidArgument);
        if(!observe(s,io,false,r))return fail(s,r);
        for(size_t pipe=0;pipe<bootdisplay::kPipeCount;++pipe) {
            if((s.capture_.poweredOffMask&(1U<<pipe)) || (s.capture_.snapshot.pipe[pipe][kControl]&1))continue;
            uint32_t value=0;
            if(!clock(s,io,pipe,r) || !guardedRead(s,io,pipe,s.layout_.pipe[pipe][kControl],value,r))return fail(s,r);
            // Match hubp2's pre-drain, then confirm drain again after the real
            // blank request as hubp32's phantom post-enable explicitly does.
            if(value && !drain(s,io,pipe,p,false,r))return fail(s,r);
            if(!guardedRead(s,io,pipe,s.layout_.pipe[pipe][kControl],value,r))return fail(s,r);
            if((value^expectedControl(s,pipe))&~kVolatile){r.status=Status::SnapshotChanged;return fail(s,r);}
            value=(value&~(0x1000U|kW1Clear))|1;
            if(!power(io,s.layout_,s.capture_.snapshot,pipe,r))return fail(s,r);
            s.wrote_=true;s.blanked_|=uint8_t(1U<<pipe);++r.writes;r.lastRegister=s.layout_.pipe[pipe][kControl];r.lastValue=value;
            r.lastReadSucceeded=false;
            if(!io.write32(io.context,r.lastRegister,value)){r.status=Status::WriteFailed;return fail(s,r);}
            if(!drain(s,io,pipe,p,true,r))return fail(s,r);
        }
        if(!observe(s,io,true,r) || !observe(s,io,true,r))return fail(s,r);
        s.phase_=Phase::Stopped;r.pipe=0xff;return finish(s,r,Status::Ok);
    }
};
Status capture(const bootdisplay::Layout& l,const MemoryLayout& m,const Io& io,State& s,Result& r) {return Access::capture(l,m,io,s,r);}
Status stop(const Io& io,const PollOptions& p,State& s,Result& r) {return Access::stop(io,p,s,r);}
const char* statusString(Status s){switch(s){
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(InvalidState);NAME(CaptureRejected);NAME(ReadFailed);
    NAME(WriteFailed);NAME(DeviceUnavailable);NAME(SnapshotChanged);NAME(PowerChanged);
    NAME(ClockUnavailable);NAME(ReadbackMismatch);NAME(DrainTimeout);
#undef NAME
    }return "Unknown";}
} }
