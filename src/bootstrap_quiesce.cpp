// SPDX-License-Identifier: MIT
// Register sequences/definitions adapted from Linux commit
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// amdgpu/soc21.c:soc21_need_reset_on_init;
// amdgpu/amdgpu_atombios.c:amdgpu_atombios_scratch_need_asic_init;
// atombios.h:ATOM_S7_ASIC_INIT_COMPLETE_MASK;
// amdgpu/gfx_v11_0.c:gfx_v11_0_cp_gfx_enable,cp_compute_enable,is_idle;
// amdgpu/sdma_v6_0.c:sdma_v6_0_gfx_stop,enable,is_idle;
// include/asic_reg/{gc/gc_11_0_0,mp/mp_13_0_0,mmhub/mmhub_3_0_0}_offset.h
// and gc_11_0_0_sh_mask.h. The restrictive bootstrap guards, address validation
// and failure propagation are independently authored. Unlike Linux's internal
// void/log-only stop helpers, every failed access/timeout is returned here.
// Copyright 2020-2021 Advanced Micro Devices, Inc.
//
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
#include "hackynavi/bootstrap_quiesce.hpp"

namespace hackynavi {
namespace bootstrap {
namespace {
constexpr uint32_t kMeHalt = 0x14000000;
constexpr uint32_t kLegacyMecHalt = 0x50000000;
constexpr uint32_t kRs64HaltReset = 0x400f0000;
constexpr uint32_t kRs64Active = 0x3c000000;
constexpr uint32_t kMesMask = 0x7c000000, kMesHalt = 0x40000000;
enum Reg {
    Sos, MeControl, LegacyMec, Rs64Mec, MecIsa, GfxArchitecture, MesControl,
    CpStatus, GrbmStatus, GfxContext0, MmContext0 = GfxContext0 + 16,
    Sdma0 = MmContext0 + 16, Sdma1 = Sdma0 + 18, RegCount = Sdma1 + 18
};
// Each SDMA group has queue0..7 RB/IB pairs, F32 and STATUS at indices16/17.
struct Registers { uint32_t address[RegCount]; };
struct Controls { uint32_t me, mec, rb[2], ib[2], f32[2]; bool rs64; };
Status finish(Result& out, Status status) { out.status = status; return status; }
bool read(const Io& io, uint32_t address, uint32_t& value, Result& out) {
    out.lastOffset = address; ++out.readAttempts;
    if (!io.read32(io.context, address, value)) {
        finish(out, Status::ReadFailed); return false;
    }
    out.lastValue = value;
    if (value == UINT32_MAX) { finish(out, Status::DeviceUnavailable); return false; }
    return true;
}
bool write(const Io& io, uint32_t address, uint32_t value, Result& out) {
    out.lastOffset = address; out.lastValue = value;
    ++out.writeAttempts; out.writesMayHaveOccurred = true;
    if (!io.write32(io.context, address, value)) {
        finish(out, Status::WriteFailed); return false;
    }
    return true;
}
bool matches(const Io& io, uint32_t address, uint32_t mask, uint32_t value,
             Status failure, Result& out) {
    uint32_t observed = 0;
    if (!read(io, address, observed, out)) return false;
    if ((observed & mask) != value) { finish(out, failure); return false; }
    return true;
}
Status resolve(const Navi31IpLayout& layout, uint64_t barBytes, Registers& regs) {
    if (!layout.valid) return Status::UnsupportedLayout;
    const uint16_t ids[6] = {11, 34, 255, 1, 42, 43};
    const uint8_t majors[6] = {11, 3, 13, 13, 6, 6};
    for (size_t i = 0; i < 6; ++i) {
        const auto& block = layout.blocks[i];
        if (block.hardwareId != ids[i] || block.major != majors[i] ||
            block.minor || block.revision || block.dieId || block.instance ||
            !block.baseCount || block.baseCount > kNavi31MaxBases)
            return Status::UnsupportedLayout;
    }
    const uint32_t raw[9] = {0x91, 0x803, 0x802, 0x2904, 0x2903, 0x2a00,
                             0x2807, 0xf40, 0xda4};
    for (size_t i = 0; i < RegCount; ++i) {
        ResolvedRegister r = {};
        IpLayoutStatus status;
        if (i >= Sdma0) {
            const size_t engine = i >= Sdma1 ? 1 : 0;
            const size_t index = i - (engine ? Sdma1 : Sdma0);
            const uint32_t offset = index < 16 ?
                uint32_t(0x80 + (index / 2) * 0x58 + (index % 2) * 0xa) :
                index == 16 ? 0x589a : 0x25;
            status = resolveNavi31SdmaRegister(layout, uint32_t(engine), offset, barBytes, r);
        } else if (i >= GfxContext0) {
            const bool mm = i >= MmContext0;
            status = resolveNavi31Register(layout, mm ? Navi31Block::Mmhub : Navi31Block::Gc,
                0, uint32_t((mm ? 0x740 : 0x1688) + i - (mm ? MmContext0 : GfxContext0)),
                barBytes, r);
        } else {
            status = resolveNavi31Register(layout, i == Sos ? Navi31Block::Mp0 : Navi31Block::Gc,
                i >= MeControl && i <= MesControl ? 1 : 0, raw[i], barBytes, r);
        }
        if (status != IpLayoutStatus::Ok)
            return status == IpLayoutStatus::OutsideBar || status == IpLayoutStatus::AddressOverflow ?
                Status::OutsideBar : Status::UnsupportedLayout;
        regs.address[i] = r.mmioByteOffset;
        for (size_t j = 0; j < i; ++j)
            if (regs.address[j] == r.mmioByteOffset) return Status::AliasedRegisters;
    }
    return Status::Ok;
}
bool guards(const Registers& r, const Io& io, Result& out) {
    if (!matches(io, r.address[Sos], UINT32_MAX, 0, Status::PriorDriverRequiresReset, out) ||
        !matches(io, r.address[MesControl], kMesMask, kMesHalt, Status::ActiveMes, out)) return false;
    for (size_t i = GfxContext0; i < Sdma0; ++i)
        if (!matches(io, r.address[i], 1, 0, Status::ActiveVmContext, out)) return false;
    for (unsigned engine = 0; engine < 2; ++engine) {
        const size_t base = engine ? Sdma1 : Sdma0;
        for (size_t i = 2; i < 16; ++i)
            if (!matches(io, r.address[base + i], 1, 0, Status::ActiveSdmaQueue, out)) return false;
    }
    return true;
}
bool snapshot(const Registers& r, const Io& io, Controls& c, Result& out) {
    uint32_t gfx = 0, mec = 0;
    if (!read(io, r.address[GfxArchitecture], gfx, out) ||
        !read(io, r.address[MecIsa], mec, out)) return false;
    if ((gfx & 1) != (mec & 1)) { finish(out, Status::UnsupportedArchitecture); return false; }
    c.rs64 = out.rs64 = (mec & 1) != 0;
    if (!read(io, r.address[MeControl], c.me, out) ||
        !read(io, r.address[c.rs64 ? Rs64Mec : LegacyMec], c.mec, out)) return false;
    for (unsigned engine = 0; engine < 2; ++engine) {
        const size_t base = engine ? Sdma1 : Sdma0;
        if (!read(io, r.address[base], c.rb[engine], out) ||
            !read(io, r.address[base + 1], c.ib[engine], out) ||
            !read(io, r.address[base + 16], c.f32[engine], out)) return false;
    }
    return true;
}
bool verify(const Registers& r, const Io& io, const Controls& c, Result& out) {
    if (!matches(io, r.address[MeControl], kMeHalt, kMeHalt, Status::ControlReadbackMismatch, out) ||
        !matches(io, r.address[c.rs64 ? Rs64Mec : LegacyMec],
            c.rs64 ? kRs64HaltReset | kRs64Active : kLegacyMecHalt,
            c.rs64 ? kRs64HaltReset : kLegacyMecHalt, Status::ControlReadbackMismatch, out) ||
        !matches(io, r.address[GfxArchitecture], 1, c.rs64 ? 1 : 0, Status::ControlReadbackMismatch, out) ||
        !matches(io, r.address[MecIsa], 1, c.rs64 ? 1 : 0, Status::ControlReadbackMismatch, out)) return false;
    for (unsigned engine = 0; engine < 2; ++engine) {
        const size_t base = engine ? Sdma1 : Sdma0;
        if (!matches(io, r.address[base], 1, 0, Status::ControlReadbackMismatch, out) ||
            !matches(io, r.address[base + 1], 1, 0, Status::ControlReadbackMismatch, out) ||
            !matches(io, r.address[base + 16], 1, 1, Status::ControlReadbackMismatch, out)) return false;
    }
    return true;
}
} // namespace

GuardInventoryStatus captureGuardInventory(const Navi31IpLayout& layout, uint64_t barBytes,
    const GuardInventoryIo& io, GuardInventory& out) {
    static_assert(RegCount == kGuardInventoryWords, "Inventory must cover actual resolved register table");
    out = GuardInventory{}; out.status = GuardInventoryStatus::InvalidArgument;
    out.layoutStatus = Status::InvalidArgument; out.lastIndex = UINT32_MAX;
    if (!io.valid || !io.read32) return out.status;
    Registers regs = {};
    out.layoutStatus = resolve(layout, barBytes, regs);
    if (out.layoutStatus != Status::Ok) return out.status = GuardInventoryStatus::LayoutRejected;
    for (size_t i = 0; i < kGuardInventoryWords; ++i) {
        out.offsets[i] = regs.address[i];
        if (i == Sos) { out.guardMasks[i] = UINT32_MAX; out.guardExpected[i] = 0; }
        else if (i == MesControl) { out.guardMasks[i] = kMesMask; out.guardExpected[i] = kMesHalt; }
        else if (i >= GfxContext0 && i < Sdma0) out.guardMasks[i] = 1;
        else if (i >= Sdma0) {
            const size_t index = i - (i >= Sdma1 ? Sdma1 : Sdma0);
            if (index >= 2 && index < 16) out.guardMasks[i] = 1;
        }
    }
    // The extra iteration is the ending SOS bracket, not another catalogue word.
    for (size_t i = 0; i <= kGuardInventoryWords; ++i) {
        const bool end = i == kGuardInventoryWords;
        const uint32_t offset = regs.address[end ? Sos : i];
        out.lastIndex = uint32_t(i); out.lastOffset = offset;
        out.lastValue = 0; out.lastReadSucceeded = false;
        if (!io.valid(io.context)) return out.status = GuardInventoryStatus::AccessDenied;
        const size_t slot = end ? 0 : i / 32;
        const uint32_t bit = UINT32_C(1) << (end ? 0 : i % 32);
        if (end) out.sosAfterAttempted = true; else out.attempted[slot] |= bit;
        ++out.reads; uint32_t value = 0;
        if (!io.read32(io.context, offset, value)) return out.status = GuardInventoryStatus::ReadFailed;
        out.lastValue = value; out.lastReadSucceeded = true;
        if (end) { out.sosAfter = value; out.sosAfterReadSucceeded = true; }
        else { out.values[i] = value; out.readSucceeded[slot] |= bit; }
        if (!io.valid(io.context)) return out.status = GuardInventoryStatus::AccessDenied;
        if (value == UINT32_MAX) return out.status = GuardInventoryStatus::DeviceUnavailable;
        if (end) out.sosAfterValid = true; else out.valid[slot] |= bit;
        if (!end && out.guardMasks[i]) {
            out.guardCompared[slot] |= bit;
            if ((value & out.guardMasks[i]) != out.guardExpected[i]) out.guardMismatch[slot] |= bit;
        }
        if ((i == Sos || end) && value) return out.status = GuardInventoryStatus::PriorDriverRequiresReset;
        if (i == GfxArchitecture) {
            out.architectureCompared = true;
            out.rs64 = (out.values[MecIsa] & 1) != 0;
            out.architectureMatch = (value & 1) == (out.values[MecIsa] & 1);
        }
    }
    out.complete = true; return out.status = GuardInventoryStatus::Ok;
}
const char* guardInventoryStatusString(GuardInventoryStatus s) {
    switch (s) {
#define NAME(x) case GuardInventoryStatus::x: return #x
    NAME(Ok); NAME(InvalidArgument); NAME(LayoutRejected); NAME(AccessDenied);
    NAME(ReadFailed); NAME(DeviceUnavailable); NAME(PriorDriverRequiresReset);
#undef NAME
    } return "Unknown";
}

Status quiesceCpSdma(const Navi31IpLayout& layout, uint64_t barBytes,
                    const Io& io, const PollOptions& poll, Result& out) {
    out = Result{}; out.stage = Stage::Preflight;
    if (!io.read32 || !io.write32 || !io.delayUs || !poll.maxPolls || poll.maxPolls > 100000 ||
        !poll.delayUs || poll.delayUs > 10) return finish(out, Status::InvalidArgument);
    Registers regs = {};
    const Status resolved = resolve(layout, barBytes, regs);
    if (resolved != Status::Ok) return finish(out, resolved);
    out.stage = Stage::BootstrapGuards;
    Controls control = {};
    if (!guards(regs, io, out) || !snapshot(regs, io, control, out)) return out.status;
    out.stage = Stage::StopCp;
    if (!write(io, regs.address[MeControl], control.me | kMeHalt, out) ||
        !write(io, regs.address[control.rs64 ? Rs64Mec : LegacyMec], control.rs64 ?
            (control.mec & ~kRs64Active) | kRs64HaltReset | 0x10 :
            control.mec | kLegacyMecHalt, out)) return out.status;
    io.delayUs(io.context, 50); // Linux's MEC stop settling delay.
    out.stage = Stage::StopSdma;
    // Linux disables queue0 on both instances before halting either F32.
    for (unsigned engine = 0; engine < 2; ++engine) {
        const size_t base = engine ? Sdma1 : Sdma0;
        if (!write(io, regs.address[base], control.rb[engine] & ~1U, out) ||
            !write(io, regs.address[base + 1], control.ib[engine] & ~1U, out)) return out.status;
    }
    for (unsigned engine = 0; engine < 2; ++engine)
        if (!write(io, regs.address[(engine ? Sdma1 : Sdma0) + 16], control.f32[engine] | 1, out))
            return out.status;
    out.stage = Stage::VerifyControls;
    if (!verify(regs, io, control, out)) return out.status;
    out.stage = Stage::IdleWait;
    bool idle = false;
    for (uint32_t i = 0; i < poll.maxPolls; ++i) {
        uint32_t cp = 0, grbm = 0, sdma0 = 0, sdma1 = 0;
        ++out.idlePolls;
        if (!read(io, regs.address[CpStatus], cp, out) ||
            !read(io, regs.address[GrbmStatus], grbm, out) ||
            !read(io, regs.address[Sdma0 + 17], sdma0, out) ||
            !read(io, regs.address[Sdma1 + 17], sdma1, out)) return out.status;
        if (!cp && !(grbm & 0x80000000) && (sdma0 & sdma1 & 1)) { idle = true; break; }
        if (i + 1 < poll.maxPolls) io.delayUs(io.context, poll.delayUs);
    }
    if (!idle) return finish(out, Status::IdleTimeout);
    out.stage = Stage::RecheckGuards;
    if (!guards(regs, io, out) || !verify(regs, io, control, out)) return out.status;
    out.stage = Stage::Complete;
    return finish(out, Status::Ok);
}
Status checkPostedColdState(const Navi31IpLayout& layout, uint64_t barBytes,
    uint32_t scratchDword, const Io& io, Result& out) {
    out = Result{}; out.stage = Stage::Preflight;
    if (!io.read32 || !scratchDword) return finish(out, Status::InvalidArgument);
    Registers registers = {};
    const auto resolved = resolve(layout, barBytes, registers);
    if (resolved != Status::Ok) return finish(out, resolved);
    const uint64_t scratchByte = (uint64_t(scratchDword) + 7) * 4;
    if (scratchByte > UINT32_MAX || barBytes < 4 || scratchByte > barBytes - 4 ||
        scratchByte < 0x1c || (scratchByte >= 0x7f000 && scratchByte < 0x80000))
        return finish(out, Status::OutsideBar);
    // A malformed ROM base must not turn a scratch check into reading one of
    // the resolved engine/control/mailbox registers under another name.
    for (size_t i = 0; i < RegCount; ++i)
        if (scratchByte == registers.address[i]) return finish(out, Status::AliasedRegisters);
    out.stage = Stage::PostedState;
    if (!read(io, registers.address[Sos], out.sosSignOfLife, out)) return out.status;
    if (out.sosSignOfLife) return finish(out, Status::PriorDriverRequiresReset);
    if (!read(io, static_cast<uint32_t>(scratchByte), out.biosScratch7, out) ||
        !read(io, registers.address[Sos], out.sosSignOfLife, out)) return out.status;
    if (out.sosSignOfLife) return finish(out, Status::PriorDriverRequiresReset);
    if (!(out.biosScratch7 & 0x200)) return finish(out, Status::AtomPostRequired);
    out.stage = Stage::Complete;
    return finish(out, Status::Ok);
}
const char* statusString(Status status) {
    switch (status) {
    case Status::Ok: return "bootstrap operation completed";
    case Status::InvalidArgument: return "invalid bootstrap arguments";
    case Status::UnsupportedLayout: return "unsupported Navi31 bootstrap layout";
    case Status::OutsideBar: return "bootstrap register outside BAR";
    case Status::AliasedRegisters: return "bootstrap registers alias";
    case Status::ReadFailed: return "bootstrap register read failed";
    case Status::DeviceUnavailable: return "bootstrap register returned all ones";
    case Status::PriorDriverRequiresReset: return "PSP sOS indicates prior driver; ASIC reset required";
    case Status::ActiveVmContext: return "existing VM translation context enabled";
    case Status::ActiveMes: return "MES is active or not halted";
    case Status::ActiveSdmaQueue: return "unsupported SDMA queue enabled";
    case Status::UnsupportedArchitecture: return "mixed CP architecture selection";
    case Status::WriteFailed: return "bootstrap register write failed";
    case Status::ControlReadbackMismatch: return "bootstrap stop control readback mismatch";
    case Status::IdleTimeout: return "CP/SDMA idle timeout";
    case Status::AtomPostRequired: return "BIOS scratch7 lacks ASIC_Init completion; ATOM POST required";
    }
    return "unknown bootstrap status";
}
} // namespace bootstrap
} // namespace hackynavi
