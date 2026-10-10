// SPDX-License-Identifier: MIT
/*
 * GFXHUB3/MMHUB3 GART register sequence adapted from Linux
 * gfxhub_v3_0.c, mmhub_v3_0.c and gmc_v11_0.c at
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "hackynavi/gart.hpp"

namespace hackynavi {
namespace gart {
namespace {
// Every generated register below has BASE_IDX=0. Offsets are DWORDS until
// resolveNavi31Register converts them to BAR bytes. Context stride=1 DWORD,
// invalidation engine stride=1 DWORD and range-pair stride=2 DWORDS.
enum Reg {
    Context0, PtLo, PtHi, StartLo, StartHi, EndLo, EndHi,
    AgpBase, AgpBot, AgpTop, SystemLow, SystemHigh, DefaultLo, DefaultHi,
    FaultDefaultLo, FaultDefaultHi, FaultControl2, TlbControl, L2Control,
    L2Control2, L2Control3, L2Control4, L2Control5,
    IdentityLowLo, IdentityLowHi, IdentityHighLo, IdentityHighHi,
    IdentityOffsetLo, IdentityOffsetHi, InvalidateRangeLo, InvalidateRangeHi,
    FaultControl, Semaphore, Request, Ack, PrivateInvalidate, CpDebug, RegCount
};
// gc_11_0_0_offset.h and mmhub_3_0_0_offset.h at the pinned Linux commit.
const uint32_t registers[2][RegCount] = {
    {0x1688,0x16f3,0x16f4,0x1713,0x1714,0x1733,0x1734,
     0x167c,0x167b,0x167a,0x167d,0x167e,0x15a8,0x15a9,
     0x15cb,0x15cc,0x15c5,0x167f,0x15bc,0x15bd,0x15be,0x15d4,0x15da,
     0x15ce,0x15cf,0x15d0,0x15d1,0x15d2,0x15d3,0x16cf,0x16d0,
     0x15c4,0x1699,0x16ab,0x16bd,0,0x1e1f},
    {0x0740,0x07ab,0x07ac,0x07cb,0x07cc,0x07eb,0x07ec,
     0x08f0,0x08ef,0x08ee,0x08f1,0x08f2,0x08d8,0x08d9,
     0x070f,0x0710,0x0709,0x08f3,0x0700,0x0701,0x0702,0x0718,0x071e,
     0x0712,0x0713,0x0714,0x0715,0x0716,0x0717,0x0787,0x0788,
     0x0708,0x0751,0x0763,0x0775,0x071b,0}
};
constexpr uint64_t kPhysicalLimit = 1ULL << 48;
constexpr uint64_t kDmaLimit = 1ULL << 44;
constexpr uint64_t kVaHole = 1ULL << 47;
constexpr uint64_t kPage = 4096;
constexpr uint64_t kFourGiB = 1ULL << 32;
constexpr uint32_t kInvalidateRequest = 0x00f80001;
// All five L2/L1 PTE/PDE invalidations, flush_type0, VMID0, no fault clear.
constexpr uint32_t kFaultDefaults = 0x00001ffc;
constexpr uint32_t kCrashFaults = 0xc0000000;

Status finish(Result& out, Status status) { out.status = status; return status; }
bool ioValid(const Io& io) {
    return io.read32 && io.write32 && io.delayUs && io.flushHdp;
}
bool range(uint64_t start, uint64_t bytes, uint64_t limit) {
    return bytes && start < limit && bytes <= limit - start;
}
bool pageAligned(uint64_t value) { return (value & (kPage - 1)) == 0; }
bool overlaps(uint64_t a, uint64_t aBytes, uint64_t b, uint64_t bBytes) {
    return a < b + bBytes && b < a + aBytes;
}
bool configValid(const Config& c) {
    uint64_t physicalBase = 0;
    if (vramPhysicalAddress(c.memory, 0, c.memory.vramBytes, physicalBase) !=
        MemoryLayoutStatus::Ok) return false;
    if (c.memory.mcWindowStart != c.memory.vramStart ||
        (c.memory.mcWindowStart & 0xffffff) ||
        (c.memory.mcWindowEndInclusive & 0xffffff) != 0xffffff ||
        c.memory.mcWindowEndInclusive >= kPhysicalLimit ||
        c.memory.mcWindowEndInclusive < c.memory.vramEndInclusive)
        return false;
    if (!pageAligned(c.tablePhysicalAddress) || !pageAligned(c.tableBytes) ||
        !range(c.tablePhysicalAddress, c.tableBytes, kPhysicalLimit) ||
        c.tablePhysicalAddress < physicalBase ||
        c.tablePhysicalAddress - physicalBase >= c.memory.vramBytes ||
        c.tableBytes > c.memory.vramBytes - (c.tablePhysicalAddress - physicalBase))
        return false;
    if ((c.apertureStart & (kFourGiB - 1)) || !pageAligned(c.apertureBytes) ||
        c.apertureBytes > kMaxApertureBytes ||
        !range(c.apertureStart, c.apertureBytes, kVaHole) ||
        c.tableBytes < (c.apertureBytes / kPage) * 8 ||
        overlaps(c.apertureStart, c.apertureBytes, c.memory.mcWindowStart,
                 c.memory.mcWindowEndInclusive - c.memory.mcWindowStart + 1))
        return false;
    if (!pageAligned(c.scratchPhysicalAddress) ||
        c.scratchPhysicalAddress < physicalBase ||
        c.scratchPhysicalAddress - physicalBase >= c.memory.vramBytes ||
        kPage > c.memory.vramBytes - (c.scratchPhysicalAddress - physicalBase) ||
        overlaps(c.scratchPhysicalAddress, kPage, c.tablePhysicalAddress, c.tableBytes))
        return false;
    return pageAligned(c.dummyDmaAddress) && range(c.dummyDmaAddress, kPage, kDmaLimit);
}

Status read(const Io& io, uint32_t address, uint32_t& value, Result& out) {
    out.lastOffset = address;
    ++out.readAttempts;
    if (!io.read32(io.context, address, value)) return finish(out, Status::ReadFailed);
    out.lastValue = value;
    if (value == UINT32_MAX) return finish(out, Status::DeviceUnavailable);
    return Status::Ok;
}
Status write(const Io& io, uint32_t address, uint32_t value, Result& out) {
    out.lastOffset = address;
    out.lastValue = value;
    ++out.writeAttempts;
    out.writesMayHaveOccurred = true;
    if (!io.write32(io.context, address, value)) return finish(out, Status::WriteFailed);
    return Status::Ok;
}
} // namespace

Plan::Plan() : steps_{}, count_(0), hub_(Hub::Gfx), semaphore_(0), request_(0),
    ack_(0), privateInvalidate_(0), privateValue_(0), valid_(false), consumed_(false) {}
void Plan::reset() {
    for (size_t i = 0; i < kMaxSteps; ++i) steps_[i] = Step{};
    count_ = 0; hub_ = Hub::Gfx;
    semaphore_ = request_ = ack_ = privateInvalidate_ = privateValue_ = 0;
    valid_ = consumed_ = false;
}

struct Builder {
    static void reset(Plan& plan) { plan.reset(); }
    Plan& plan;
    const Navi31IpLayout& layout;
    uint64_t barBytes;
    Hub hub;
    const Io& io;
    Result& out;
    uint32_t resolved[RegCount];

    bool add(StepKind kind, uint32_t address = 0, uint32_t value = 0,
             uint32_t readMask = UINT32_MAX) {
        if (plan.count_ == kMaxSteps) {
            finish(out, Status::PlanCapacityExceeded); return false;
        }
        plan.steps_[plan.count_++] = {kind, address, value, readMask};
        return true;
    }
    bool put(Reg reg, uint32_t value) { return add(StepKind::Write, resolved[reg], value); }
    bool resolve(uint32_t raw, uint32_t& value) {
        ResolvedRegister r = {};
        const auto status = resolveNavi31Register(layout,
            hub == Hub::Gfx ? Navi31Block::Gc : Navi31Block::Mmhub,
            0, raw, barBytes, r);
        if (status != IpLayoutStatus::Ok) {
            finish(out, status == IpLayoutStatus::OutsideBar ||
                status == IpLayoutStatus::AddressOverflow ? Status::OutsideBar :
                Status::UnsupportedLayout);
            return false;
        }
        value = r.mmioByteOffset;
        return true;
    }
    bool setup() {
        const size_t which = hub == Hub::Gfx ? 0 : 1;
        if (!ioValid(io) || (hub != Hub::Gfx && hub != Hub::Mm)) {
            finish(out, Status::InvalidArgument); return false;
        }
        const IpRegisterBlock& block = layout.blocks[hub == Hub::Gfx ? 0 : 1];
        if (!layout.valid || block.hardwareId != (hub == Hub::Gfx ? 11 : 34) ||
            block.major != (hub == Hub::Gfx ? 11 : 3) || block.minor ||
            block.revision || block.dieId || block.instance) {
            finish(out, Status::UnsupportedLayout); return false;
        }
        plan.hub_ = hub;
        for (size_t reg = 0; reg != RegCount; ++reg) {
            if (!registers[which][reg]) continue; // Absent hub-specific register.
            uint32_t raw = registers[which][reg];
            if (reg == Semaphore || reg == Request || reg == Ack) raw += 17;
            if (!resolve(raw, resolved[reg])) return false;
        }
        // Validate the highest array element before adding writes or reading.
        uint32_t ignored;
        if (!resolve(registers[which][Context0] + 15, ignored) ||
            !resolve(registers[which][InvalidateRangeHi] + 17 * 2, ignored))
            return false;
        plan.semaphore_ = resolved[Semaphore];
        plan.request_ = resolved[Request];
        plan.ack_ = resolved[Ack];
        plan.privateInvalidate_ = resolved[PrivateInvalidate];
        return true;
    }
    bool snapshot(Reg reg, uint32_t& value) {
        return read(io, resolved[reg], value, out) == Status::Ok;
    }
    bool preparePrivate() {
        if (hub == Hub::Gfx) return true;
        uint32_t value = 0;
        if (!snapshot(PrivateInvalidate, value)) return false;
        plan.privateValue_ = value | 0x02000000; // Linux private CID2 invalidation.
        return true;
    }
    bool disableContexts() {
        for (uint32_t context = 0; context < 16; ++context)
            if (!add(StepKind::Write, resolved[Context0] + context * 4, 0)) return false;
        return true;
    }
    bool invalidationRanges() {
        for (uint32_t engine = 0; engine < 18; ++engine)
            if (!add(StepKind::Write, resolved[InvalidateRangeLo] + engine * 8, UINT32_MAX) ||
                !add(StepKind::Write, resolved[InvalidateRangeHi] + engine * 8, 0x1f)) return false;
        return true;
    }
    bool flush() { return add(StepKind::FlushHdp) && add(StepKind::Invalidate); }
    bool fullEngine17Range() {
        return add(StepKind::Write, resolved[InvalidateRangeLo] + 17 * 8, UINT32_MAX) &&
            add(StepKind::Write, resolved[InvalidateRangeHi] + 17 * 8, 0x1f);
    }

    bool enable(const Config& c) {
        if (!configValid(c)) { finish(out, Status::InvalidArgument); return false; }
        if (!setup()) return false;
        uint32_t fault2, tlb, l2, l2inv, fault, cp = 0;
        if (!snapshot(FaultControl2, fault2) || !snapshot(TlbControl, tlb) ||
            !snapshot(L2Control, l2) || !snapshot(L2Control2, l2inv) ||
            !snapshot(FaultControl, fault) ||
            (hub == Hub::Gfx && !snapshot(CpDebug, cp)) || !preparePrivate()) return false;
        // These masks match in the GC11/MMHUB3 generated sh_mask headers.
        tlb = (tlb & ~0x00003ff9U) | 0x00001859U;
        l2 = (l2 & ~0x03fc0903U) | 0x00080801U;
        // Our narrow owner disables every context first; VMID0 then starts from
        // zero, avoiding inherited interrupt/retry/block-size settings.
        const uint64_t root = c.tablePhysicalAddress | 1 | (c.tableSnooped ? 4 : 0);
        const uint64_t end = c.apertureStart + c.apertureBytes - 1;
        if (!disableContexts() ||
            !put(PtLo, static_cast<uint32_t>(root)) || !put(PtHi, static_cast<uint32_t>(root >> 32)) ||
            !put(StartLo, static_cast<uint32_t>(c.apertureStart >> 12)) ||
            !put(StartHi, static_cast<uint32_t>(c.apertureStart >> 44)) ||
            !put(EndLo, static_cast<uint32_t>(end >> 12)) || !put(EndHi, static_cast<uint32_t>(end >> 44)) ||
            !put(AgpBase, 0) || !put(AgpBot, 0x00ffffff) || !put(AgpTop, 0) ||
            !put(SystemLow, static_cast<uint32_t>(c.memory.vramStart >> 18)) ||
            !put(SystemHigh, static_cast<uint32_t>(c.memory.vramEndInclusive >> 18)) ||
            !put(DefaultLo, static_cast<uint32_t>(c.scratchPhysicalAddress >> 12)) ||
            !put(DefaultHi, static_cast<uint32_t>(c.scratchPhysicalAddress >> 44)) ||
            !put(FaultDefaultLo, static_cast<uint32_t>(c.dummyDmaAddress >> 12)) ||
            !put(FaultDefaultHi, static_cast<uint32_t>(c.dummyDmaAddress >> 44)) ||
            !put(FaultControl2, fault2 | 0x00040000) || !put(TlbControl, tlb) ||
            !put(L2Control, l2) || !put(L2Control2, l2inv | 3) ||
            // Both distinct upstream CNTL3 defaults yield this value after
            // BANK_SELECT=9 and BIGK_FRAGMENT_SIZE=6 (translate-further off).
            !put(L2Control3, 0x80130009) || !put(L2Control4, 1) ||
            !put(L2Control5, 0x00003fe0) || !put(Context0, 1) ||
            !put(IdentityLowLo, UINT32_MAX) || !put(IdentityLowHi, 0xf) ||
            !put(IdentityHighLo, 0) || !put(IdentityHighHi, 0) ||
            !put(IdentityOffsetLo, 0) || !put(IdentityOffsetHi, 0) ||
            !invalidationRanges() || !add(StepKind::FlushHdp)) return false;
        if (hub == Hub::Gfx && !put(CpDebug, cp | 0x00008000)) return false;
        // Explicitly clear crash bits for our selected redirect policy. Linux's
        // MMHUB true branch assumes reset crash bits; stale crash bits are unsafe.
        if (!put(FaultControl, (fault & ~kCrashFaults) | kFaultDefaults) ||
            !flush() || !add(StepKind::PostingRead, resolved[Context0], 1)) return false;
        plan.valid_ = true;
        return true;
    }
    bool disable() {
        if (!setup()) return false;
        uint32_t tlb, l2;
        if (!snapshot(TlbControl, tlb) || !snapshot(L2Control, l2) || !preparePrivate()) return false;
        // Clients are already quiesced by the owner. Complete an invalidation
        // while caches still operate, then follow Linux's disable sequence.
        if (!fullEngine17Range() || !flush() || !disableContexts() || !put(TlbControl, tlb & ~0x41U) ||
            !put(L2Control, l2 & ~1U) || !put(L2Control3, 0)) return false;
        // A posting read of an unrelated register does not confirm that every
        // translation context stopped. Resource retirement needs these actual
        // disabled states, not merely successful MMIO write callbacks.
        for (uint32_t context = 0; context < 16; ++context)
            if (!add(StepKind::PostingRead, resolved[Context0] + context * 4, 0)) return false;
        // Only the shutdown bits matter here; unrelated hardware fields need
        // not remain equal to the earlier RMW snapshot.
        if (!add(StepKind::PostingRead, resolved[TlbControl], 0, 0x41) ||
            !add(StepKind::PostingRead, resolved[L2Control], 0, 1) ||
            !add(StepKind::PostingRead, resolved[L2Control3], 0)) return false;
        plan.valid_ = true;
        return true;
    }
    bool flushOnly() {
        // A standalone flush cannot assume an earlier enable left engine17's
        // address range intact. Establish Linux's full range before the request.
        if (!setup() || !preparePrivate() || !fullEngine17Range() || !flush()) return false;
        plan.valid_ = true;
        return true;
    }
};

Status prepareEnable(const Navi31IpLayout& layout, uint64_t barBytes, Hub hub,
                     const Config& config, const Io& io, Plan& plan, Result& out) {
    Builder::reset(plan); out = Result{}; out.stage = Stage::Preflight;
    Builder b = {plan, layout, barBytes, hub, io, out, {}};
    if (!b.enable(config)) { Builder::reset(plan); return out.status; }
    return finish(out, Status::Ok);
}
Status prepareDisable(const Navi31IpLayout& layout, uint64_t barBytes, Hub hub,
                      const Io& io, Plan& plan, Result& out) {
    Builder::reset(plan); out = Result{}; out.stage = Stage::Preflight;
    Builder b = {plan, layout, barBytes, hub, io, out, {}};
    if (!b.disable()) { Builder::reset(plan); return out.status; }
    return finish(out, Status::Ok);
}
Status prepareFlush(const Navi31IpLayout& layout, uint64_t barBytes, Hub hub,
                    const Io& io, Plan& plan, Result& out) {
    Builder::reset(plan); out = Result{}; out.stage = Stage::Preflight;
    Builder b = {plan, layout, barBytes, hub, io, out, {}};
    if (!b.flushOnly()) { Builder::reset(plan); return out.status; }
    return finish(out, Status::Ok);
}

namespace {
Status releaseSemaphore(const Io& io, uint32_t address, Result& out) {
    out.stage = Stage::SemaphoreRelease;
    const Status status = write(io, address, 0, out);
    if (status == Status::Ok) out.semaphoreMayBeHeld = false;
    return status;
}
Status releaseAfterFailure(const Io& io, uint32_t address, Result& out) {
    const Status failure = out.status;
    const Stage stage = out.stage;
    const uint32_t offset = out.lastOffset, value = out.lastValue;
    (void)releaseSemaphore(io, address, out);
    out.stage = stage; out.lastOffset = offset; out.lastValue = value;
    return finish(out, failure);
}
Status invalidate(const Io& io, const PollOptions& poll, Hub hub,
                  uint32_t semaphore, uint32_t request, uint32_t ack,
                  uint32_t privateRegister, uint32_t privateValue, Result& out) {
    const bool useSemaphore = hub == Hub::Mm;
    uint32_t value = 0;
    if (useSemaphore) {
        out.stage = Stage::SemaphoreAcquire;
        bool acquired = false;
        for (uint32_t i = 0; i < poll.maxPolls; ++i) {
            ++out.semaphorePolls;
            // A failed read may have acquired the read-to-acquire semaphore.
            // Do not release an uncertain lock that might belong to another user.
            out.semaphoreMayBeHeld = true;
            if (read(io, semaphore, value, out) != Status::Ok) return out.status;
            out.semaphoreMayBeHeld = (value & 1) != 0;
            if (out.semaphoreMayBeHeld) { acquired = true; break; }
            if (i + 1 < poll.maxPolls) io.delayUs(io.context, poll.delayUs);
        }
        if (!acquired) return finish(out, Status::SemaphoreTimeout);
    }
    out.stage = Stage::InvalidateRequest;
    if (write(io, request, kInvalidateRequest, out) != Status::Ok)
        return useSemaphore ? releaseAfterFailure(io, semaphore, out) : out.status;
    out.stage = Stage::InvalidateAck;
    bool completed = false;
    for (uint32_t i = 0; i < poll.maxPolls; ++i) {
        ++out.ackPolls;
        if (read(io, ack, value, out) != Status::Ok)
            return useSemaphore ? releaseAfterFailure(io, semaphore, out) : out.status;
        if (value & 1) { completed = true; break; }
        if (i + 1 < poll.maxPolls) io.delayUs(io.context, poll.delayUs);
    }
    if (!completed) {
        finish(out, Status::InvalidateTimeout);
        return useSemaphore ? releaseAfterFailure(io, semaphore, out) : out.status;
    }
    if (useSemaphore) {
        if (releaseSemaphore(io, semaphore, out) != Status::Ok) return out.status;
        out.stage = Stage::PrivateInvalidate;
        if (write(io, privateRegister, privateValue, out) != Status::Ok) return out.status;
        // Linux requires a readback to post/complete this private invalidation;
        // it does not require the self-clearing bit to read as either0 or1.
        if (read(io, privateRegister, value, out) != Status::Ok) return out.status;
    }
    return Status::Ok;
}
} // namespace

Status executePlan(Plan& plan, const Io& io, const PollOptions& poll, Result& out) {
    out = Result{}; out.stage = Stage::Preflight;
    if (!plan.valid_ || !ioValid(io) || poll.maxPolls == 0 || poll.maxPolls > 100000 ||
        poll.delayUs == 0 || poll.delayUs > 10) return finish(out, Status::InvalidArgument);
    if (plan.consumed_) return finish(out, Status::PlanConsumed);
    plan.consumed_ = true;
    for (size_t i = 0; i < plan.count_; ++i) {
        const Step& step = plan.steps_[i];
        Status status = Status::Ok;
        switch (step.kind) {
        case StepKind::Write:
            out.stage = Stage::Registers;
            status = write(io, step.byteOffset, step.value, out);
            break;
        case StepKind::FlushHdp:
            out.stage = Stage::HdpVisibility;
            out.writesMayHaveOccurred = true;
            if (!io.flushHdp(io.context)) status = finish(out, Status::HdpFlushFailed);
            break;
        case StepKind::Invalidate:
            status = invalidate(io, poll, plan.hub_, plan.semaphore_, plan.request_,
                plan.ack_, plan.privateInvalidate_, plan.privateValue_, out);
            break;
        case StepKind::PostingRead: {
            out.stage = Stage::PostingRead;
            uint32_t value = 0;
            status = read(io, step.byteOffset, value, out);
            if (status == Status::Ok &&
                (value & step.readMask) != (step.value & step.readMask))
                status = finish(out, Status::ReadbackMismatch);
            break;
        }
        }
        if (status != Status::Ok) return finish(out, status);
    }
    out.stage = Stage::Complete;
    return finish(out, Status::Ok);
}

const char* statusString(Status status) {
    switch (status) {
    case Status::Ok: return "ok";
    case Status::InvalidArgument: return "invalid GART argument";
    case Status::UnsupportedLayout: return "unsupported Navi31 register layout";
    case Status::OutsideBar: return "GART register outside BAR";
    case Status::PlanCapacityExceeded: return "GART plan capacity exceeded";
    case Status::ReadFailed: return "GART register read failed";
    case Status::DeviceUnavailable: return "GART register returned all ones";
    case Status::WriteFailed: return "GART register write failed";
    case Status::HdpFlushFailed: return "required HDP flush failed";
    case Status::SemaphoreTimeout: return "GART invalidate semaphore timeout";
    case Status::InvalidateTimeout: return "GART invalidation acknowledgment timeout";
    case Status::ReadbackMismatch: return "GART posting read mismatch";
    case Status::PlanConsumed: return "GART plan already consumed";
    }
    return "unknown GART status";
}

} // namespace gart
} // namespace hackynavi
