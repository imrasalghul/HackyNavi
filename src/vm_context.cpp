// SPDX-License-Identifier: MIT
/*
 * Register fields/sequences adapted from Linux gfxhub_v3_0.c, mmhub_v3_0.c,
 * gmc_v11_0.c, amdgpu_vm.c and gc_11_0_0/mmhub_3_0_0 offset/sh_mask headers at
 * 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0.
 * Copyright 2021 Advanced Micro Devices, Inc.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include "hackynavi/vm_context.hpp"
namespace hackynavi { namespace vmcontext {
namespace {
enum Reg { Control, PtLo, PtHi, StartLo, StartHi, EndLo, EndHi,
    Tlb, L2, L2Three, IdentityLowLo, IdentityLowHi, IdentityHighLo, IdentityHighHi,
    IdentityOffsetLo, IdentityOffsetHi, RangeLo, RangeHi, Semaphore, Request, Ack,
    Private, RegCount };
static_assert(RegCount <= 24, "context register storage");
const uint32_t offsets[2][RegCount] = {
    {0x1688,0x16f3,0x16f4,0x1713,0x1714,0x1733,0x1734,
     0x167f,0x15bc,0x15be,0x15ce,0x15cf,0x15d0,0x15d1,0x15d2,0x15d3,
     0x16cf,0x16d0,0x1699,0x16ab,0x16bd,0},
    {0x0740,0x07ab,0x07ac,0x07cb,0x07cc,0x07eb,0x07ec,
     0x08f3,0x0700,0x0702,0x0712,0x0713,0x0714,0x0715,0x0716,0x0717,
     0x0787,0x0788,0x0751,0x0763,0x0775,0x071b}
};
// gfxhub_v3_0_setup_vmid_config/mmhub_v3_0_setup_vmid_config: ENABLE bit0,
// PAGE_TABLE_DEPTH bits1:2=3, PAGE_TABLE_BLOCK_SIZE bits3:6=(9-9)=0.
// gmc_v11_0_vm_fault_interrupt_state and CONTEXT1_CNTL generated masks:
// range/dummy/PDE0/valid/read/write/execute interrupt bits9/11/13/15/17/19/21
// sum to0x2aaa00. Unlike default-page redirection, these report actual faults.
// Default bits10/12/14/16/18/20/22 and retry bits7/8 stay zero. IH service and
// device recovery remain required before any untrusted context can execute.
// gmc_v11_0_sw_init selects256TiB, block9, max_level3; amdgpu_vm_adjust_size
// maps num_level3 to PDB2: PDB2->PDB1->PDB0->PTB,512 entries at each level.
constexpr uint32_t kContext = 0x002aaa07;
constexpr uint64_t kPage = 4096, kLowVaEnd = 1ULL << 47;
bool configValid(const Config& c) {
    const uint64_t limit = c.root.domain == gmc11::MemoryDomain::VramPhysical ? (1ULL << 48) :
        c.root.domain == gmc11::MemoryDomain::SystemDma ? (1ULL << 44) : 0;
    return c.vmid >= 1 && c.vmid <= 7 && c.epoch && limit && c.root.bytes == kPage &&
        !(c.root.address % kPage) && c.root.address <= limit - kPage &&
        c.vaBegin >= kPage && c.vaBegin < c.vaEnd && c.vaEnd <= kLowVaEnd &&
        !(c.vaBegin % kPage) && !(c.vaEnd % kPage);
}
uint64_t rootValue(const Config& c) {
    return c.root.address | 1 | (c.root.snooped ? 4 : 0) |
        (c.root.domain == gmc11::MemoryDomain::SystemDma ? 2 : 0);
}
bool valid(const Io& io, const Poll& p) {
    return io.read32 && io.write32 && io.delayUs && io.publishTablesAndHdp && p.iterations &&
        p.iterations <= 100000 && p.delayUs && p.delayUs <= 10 && uint64_t(p.iterations) * p.delayUs <= 100000;
}
}
Context::Context() : phase_(Phase::Fresh), config_{}, hub_(Hub::Gfx), registers_{}, retained_(false) {}
struct Access {
    Context& c; const Io& io; const Poll& poll; Result& out;
    void configure(const Config& config, Hub hub) { c.config_ = config; c.hub_ = hub; }
    void phase(Phase value) { c.phase_ = value; }
    Status finish(Status status) {
        out.status = status; out.retentionRequired = c.retained_;
        if (status != Status::Ok && c.retained_) c.phase_ = Phase::Faulted;
        return status;
    }
    bool read(Reg reg, uint32_t& value, bool allowAllOnes = false) {
        ++out.reads; out.lastOffset = c.registers_[reg];
        if (!io.read32(io.context, out.lastOffset, value)) { finish(Status::ReadFailed); return false; }
        out.lastValue = value;
        if (value == UINT32_MAX && !allowAllOnes) { finish(Status::DeviceUnavailable); return false; }
        return true;
    }
    bool write(Reg reg, uint32_t value) {
        c.retained_ = true; out.retentionRequired = true; out.writesMayHaveOccurred = true;
        ++out.writes; out.lastOffset = c.registers_[reg]; out.lastValue = value;
        if (!io.write32(io.context, out.lastOffset, value)) { finish(Status::WriteFailed); return false; }
        return true;
    }
    bool expect(Reg reg, uint32_t expected, uint32_t mask = UINT32_MAX, Status failure = Status::ReadbackMismatch) {
        uint32_t v = 0;
        if (!read(reg, v, expected == UINT32_MAX)) return false;
        if ((v & mask) != expected) { finish(failure); return false; }
        return true;
    }
    bool globals() {
        out.stage = Stage::Guard;
        // Exact disabled identity aperture used by upstream GFXHUB/MMHUB.
        // All-ones IdentityLowLo is intentional; the other guards distinguish
        // that legal value from an unavailable device returning all ones.
        return expect(Tlb,0x41,0x41,Status::GlobalState) &&
            expect(L2,0x00080001,0x00180001,Status::GlobalState) &&
            expect(L2Three,0x80130009,UINT32_MAX,Status::GlobalState) &&
            expect(IdentityLowLo,UINT32_MAX,UINT32_MAX,Status::GlobalState) &&
            expect(IdentityLowHi,0xf,UINT32_MAX,Status::GlobalState) &&
            expect(IdentityHighLo,0,UINT32_MAX,Status::GlobalState) &&
            expect(IdentityHighHi,0,UINT32_MAX,Status::GlobalState) &&
            expect(IdentityOffsetLo,0,UINT32_MAX,Status::GlobalState) &&
            expect(IdentityOffsetHi,0,UINT32_MAX,Status::GlobalState);
    }
    bool rootMatches() {
        const uint64_t root = rootValue(c.config_), begin = c.config_.vaBegin >> 12, end = (c.config_.vaEnd - 1) >> 12;
        return expect(PtLo,uint32_t(root)) && expect(PtHi,uint32_t(root >> 32)) &&
            expect(StartLo,uint32_t(begin)) && expect(StartHi,uint32_t(begin >> 32)) &&
            expect(EndLo,uint32_t(end)) && expect(EndHi,uint32_t(end >> 32));
    }
    bool release() {
        out.stage = Stage::Release;
        if (!write(Semaphore,0)) return false;
        out.semaphoreMayBeHeld = false; return true;
    }
    bool failAndRelease() {
        const auto status = out.status; const auto stage = out.stage;
        const uint32_t offset = out.lastOffset, value = out.lastValue;
        (void)release(); out.status = status; out.stage = stage; out.lastOffset = offset; out.lastValue = value;
        return false;
    }
    bool publish() {
        out.stage = Stage::Publish; c.retained_ = true; out.writesMayHaveOccurred = true; out.retentionRequired = true;
        if (!io.publishTablesAndHdp(io.context)) { finish(Status::PublishFailed); return false; }
        return true;
    }
    bool invalidate() {
        // Owner serializes engine17 against GART and every other VM owner.
        out.stage = Stage::Invalidate;
        if (!write(RangeLo,UINT32_MAX) || !write(RangeHi,0x1f) ||
            !expect(RangeLo,UINT32_MAX) || !expect(RangeHi,0x1f)) return false;
        if (!publish()) return false;
        if (c.hub_ == Hub::Mm) {
            out.stage = Stage::Semaphore;
            bool acquired = false;
            for (uint32_t n = 0; n < poll.iterations; ++n) {
                uint32_t v = 0; ++out.polls; out.semaphoreMayBeHeld = true;
                // Failed read-to-acquire is ambiguous; never release a lock
                // whose ownership wasn't observed. Preserve this fact.
                if (!read(Semaphore,v)) return false;
                out.semaphoreMayBeHeld = bool(v & 1);
                if (out.semaphoreMayBeHeld) { acquired = true; break; }
                if (n + 1 < poll.iterations) io.delayUs(io.context,poll.delayUs);
            }
            if (!acquired) { finish(Status::SemaphoreTimeout); return false; }
        }
        const uint32_t mask = 1U << c.config_.vmid;
        out.stage = Stage::Invalidate;
        if (!write(Request,0x00f80000 | mask)) return c.hub_ == Hub::Mm ? failAndRelease() : false;
        out.stage = Stage::Ack; bool ack = false;
        for (uint32_t n = 0; n < poll.iterations; ++n) {
            uint32_t v = 0; ++out.polls;
            if (!read(Ack,v)) return c.hub_ == Hub::Mm ? failAndRelease() : false;
            if (v & mask) { ack = true; break; }
            if (n + 1 < poll.iterations) io.delayUs(io.context,poll.delayUs);
        }
        if (!ack) { finish(Status::InvalidateTimeout); return c.hub_ == Hub::Mm ? failAndRelease() : false; }
        if (c.hub_ == Hub::Mm) {
            if (!release()) return false;
            out.stage = Stage::PrivateInvalidate; uint32_t v = 0;
            if (!read(Private,v) || !write(Private,v | 0x02000000) || !read(Private,v)) return false;
        }
        return true;
    }
    bool resolve(const Navi31IpLayout& layout, uint64_t barBytes, Hub hub, uint8_t vmid) {
        if (hub != Hub::Gfx && hub != Hub::Mm) { finish(Status::InvalidArgument); return false; }
        const size_t index = hub == Hub::Gfx ? 0 : 1;
        const auto& block = layout.blocks[index];
        if (!layout.valid || block.hardwareId != (index ? 34 : 11) || block.major != (index ? 3 : 11) ||
            block.minor || block.revision || block.instance || block.dieId) { finish(Status::UnsupportedLayout); return false; }
        for (size_t n = 0; n < RegCount; ++n) {
            if (!offsets[index][n]) continue;
            uint32_t raw = offsets[index][n];
            if (n == Control) raw += vmid;
            else if (n >= PtLo && n <= EndHi) raw += 2 * vmid;
            else if (n == RangeLo || n == RangeHi) raw += 34;
            else if (n == Semaphore || n == Request || n == Ack) raw += 17;
            ResolvedRegister r{};
            const auto status = resolveNavi31Register(layout,index ? Navi31Block::Mmhub : Navi31Block::Gc,0,raw,barBytes,r);
            if (status != IpLayoutStatus::Ok) {
                finish(status == IpLayoutStatus::OutsideBar || status == IpLayoutStatus::AddressOverflow ?
                    Status::OutsideBar : Status::UnsupportedLayout); return false;
            }
            c.registers_[n] = r.mmioByteOffset;
        }
        return true;
    }
};
Status enable(const Navi31IpLayout& layout,uint64_t barBytes,Hub hub,const Config& config,
              const Io& io,const Poll& poll,Context& context,Result& out) {
    out = {}; out.stage = Stage::Preflight; Access a{context,io,poll,out};
    if (!valid(io,poll) || !configValid(config)) return a.finish(Status::InvalidArgument);
    if (context.phase() != Phase::Fresh) return a.finish(Status::InvalidState);
    if (!a.resolve(layout,barBytes,hub,config.vmid) || !a.globals() ||
        !a.expect(Control,0,UINT32_MAX,Status::InheritedContext)) return out.status;
    // Root ownership is remembered BEFORE the first potentially visible write.
    a.configure(config,hub);
    const uint64_t root = rootValue(config), begin = config.vaBegin >> 12, end = (config.vaEnd - 1) >> 12;
    out.stage = Stage::Root;
    if (!a.write(PtLo,uint32_t(root)) || !a.write(PtHi,uint32_t(root >> 32)) ||
        !a.write(StartLo,uint32_t(begin)) || !a.write(StartHi,uint32_t(begin >> 32)) ||
        !a.write(EndLo,uint32_t(end)) || !a.write(EndHi,uint32_t(end >> 32)) ||
        !a.rootMatches() || !a.globals() || !a.expect(Control,0) || !a.publish()) return out.status;
    out.stage = Stage::Control;
    // Upstream sets up enabled user VMIDs before per-VMID root invalidation.
    // Scheduler exclusion is mandatory: no IB may arrive until this whole
    // sequence succeeds on both hubs. The extra publish inside invalidate is
    // intentional and follows the source's HDP-before-request rule.
    if (!a.write(Control,kContext) || !a.expect(Control,kContext) || !a.invalidate() ||
        !a.expect(Control,kContext) || !a.rootMatches()) return out.status;
    a.phase(Phase::Enabled); out.stage = Stage::Complete; return a.finish(Status::Ok);
}
Status reactivate(Context& context,const Io& io,const Poll& poll,Result& out) {
    out = {}; out.stage = Stage::Preflight; Access a{context,io,poll,out};
    if (!valid(io,poll)) return a.finish(Status::InvalidArgument);
    if (context.phase() != Phase::Detached || !context.requiresHardwareRetirement())
        return a.finish(Status::InvalidState);
    // Keep the original private hub/layout/root/epoch. A disabled context is
    // not producer retirement: only the native queue's completed-job owner
    // may establish the separate right to edit and republish this same tree.
    if (!a.globals() || !a.expect(Control,0) || !a.rootMatches() || !a.publish() ||
        !a.globals() || !a.expect(Control,0) || !a.rootMatches()) return out.status;
    out.stage = Stage::Control;
    if (!a.write(Control,kContext) || !a.expect(Control,kContext) || !a.invalidate() ||
        !a.globals() || !a.expect(Control,kContext) || !a.rootMatches()) return out.status;
    a.phase(Phase::Enabled); out.stage = Stage::Complete; return a.finish(Status::Ok);
}
Status flush(Context& context,const Io& io,const Poll& poll,Result& out) {
    out = {}; out.stage = Stage::Preflight; Access a{context,io,poll,out};
    if (!valid(io,poll)) return a.finish(Status::InvalidArgument);
    if (context.phase() != Phase::Enabled) return a.finish(Status::InvalidState);
    if (!a.globals() || !a.expect(Control,kContext) || !a.rootMatches() || !a.invalidate() ||
        !a.expect(Control,kContext) || !a.rootMatches()) return out.status;
    out.stage = Stage::Complete; return a.finish(Status::Ok);
}
Status detach(Context& context,const Io& io,const Poll& poll,Result& out) {
    out = {}; out.stage = Stage::Preflight; Access a{context,io,poll,out};
    if (!valid(io,poll)) return a.finish(Status::InvalidArgument);
    if (context.phase() != Phase::Enabled) return a.finish(Status::InvalidState);
    if (!a.globals() || !a.expect(Control,kContext) || !a.rootMatches() || !a.invalidate()) return out.status;
    out.stage = Stage::Control;
    // Flush while the context/caches still operate, then disable the selected
    // context (same order as upstream gart_disable, without global shutdown).
    if (!a.write(Control,0) || !a.expect(Control,0)) return out.status;
    a.phase(Phase::Detached); out.stage = Stage::Complete; return a.finish(Status::Ok);
}
const char* statusString(Status status) {
    switch (status) {
    case Status::Ok:return "ok"; case Status::InvalidArgument:return "invalid VM context argument";
    case Status::InvalidState:return "invalid VM context phase"; case Status::UnsupportedLayout:return "unsupported VM hub";
    case Status::OutsideBar:return "VM register outside BAR"; case Status::InheritedContext:return "inherited VM context";
    case Status::GlobalState:return "unsafe global VM state"; case Status::ReadFailed:return "VM register read failed";
    case Status::DeviceUnavailable:return "VM register unavailable"; case Status::WriteFailed:return "VM register write failed";
    case Status::ReadbackMismatch:return "VM register readback mismatch"; case Status::PublishFailed:return "table/HDP publish failed";
    case Status::SemaphoreTimeout:return "VM invalidate semaphore timeout"; case Status::InvalidateTimeout:return "VM invalidate timeout";
    }
    return "unknown VM context status";
}
} }
