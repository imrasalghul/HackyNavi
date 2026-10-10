// SPDX-License-Identifier: MIT
// Source protocol adapted from Linux commit
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// amdgpu/amdgpu_device.c:amdgpu_device_mode1_reset,cache_pci_state,load_pci_state;
// pm/swsmu/smu13/smu_v13_0_0_ppt.c:mode1_reset,init_msg_ctl;
// pm/swsmu/smu_cmn.c:smu_msg_v1_send_debug_msg;
// pm/swsmu/inc/smu_v13_0.h:SMU13_MODE1_RESET_WAIT_TIME_IN_MS;
// amdgpu/psp_v13_0.c:psp_v13_0_wait_for_bootloader_steady_state;
// amdgpu/amdgpu_atombios.c:amdgpu_atombios_scratch_regs_engine_hung.
// Validation, fail-stop handling and BME-disabled restoration are port policy.
// Copyright 2007-2008, 2020-2021 Advanced Micro Devices, Inc.
// Copyright 2008 Red Hat Inc.
// Copyright 2009 Jerome Glisse.
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
#include "hackynavi/mode1_reset.hpp"

namespace hackynavi {
namespace mode1 {
namespace {
constexpr uint32_t kEngineHung = 0x20000000;
constexpr uint16_t kBusMaster = 4;
struct Registers { uint32_t param, message, response, memorySize, scratch3; };
struct PciState { uint32_t identity, revisionClass, header, bars[6]; uint16_t command; };
Status finish(Result& out, Status status) { out.status = status; return status; }
bool inBar(uint32_t offset, uint64_t bytes) {
    return !(offset & 3) && bytes >= 4 && offset <= bytes - 4;
}
Status resolve(const Navi31IpLayout& layout, const HdpLayout& hdp,
               const Options& options, Registers& regs) {
    if (!layout.valid || !hdp.valid) return Status::UnsupportedLayout;
    const uint16_t ids[6] = {11,34,255,1,42,43};
    const uint8_t majors[6] = {11,3,13,13,6,6};
    for (unsigned i = 0; i < 6; ++i) {
        const auto& b = layout.blocks[i];
        if (b.hardwareId != ids[i] || b.major != majors[i] || b.minor ||
            b.revision || b.dieId || b.instance || !b.baseCount ||
            b.baseCount > kNavi31MaxBases) return Status::UnsupportedLayout;
    }
    // Validate the immutable NBIO layout's structure, not remap live state:
    // MODE1 does not need HDP configured and resets may discard that setup.
    if (hdp.barBytes < 0x80000 || hdp.memoryFlushRegister != 0x7f000 ||
        hdp.registerFlushRegister != 0x7f004 ||
        uint64_t(hdp.memorySizeRegister) + (0x12d - 0xc3) * 4 != hdp.memoryRemapRegister ||
        uint64_t(hdp.memoryRemapRegister) + 4 != hdp.registerRemapRegister)
        return Status::UnsupportedLayout;
    const uint32_t nbio[] = {hdp.memorySizeRegister,hdp.memoryRemapRegister,hdp.registerRemapRegister};
    for (unsigned i = 0; i < 3; ++i)
        if (!inBar(nbio[i],hdp.barBytes) || (nbio[i] >= 0x7f000 && nbio[i] < 0x80000))
            return Status::OutsideBar;
    uint32_t addresses[5] = {};
    const uint32_t raw[3] = {0x275,0x28b,0x276};
    for (unsigned i = 0; i < 3; ++i) {
        ResolvedRegister r = {};
        const auto status = resolveNavi31Register(layout,Navi31Block::Mp1,0,raw[i],hdp.barBytes,r);
        if (status != IpLayoutStatus::Ok)
            return status == IpLayoutStatus::OutsideBar || status == IpLayoutStatus::AddressOverflow ?
                Status::OutsideBar : Status::UnsupportedLayout;
        addresses[i] = r.mmioByteOffset;
    }
    addresses[3] = hdp.memorySizeRegister;
    if (options.biosScratchBaseDwords) {
        const uint64_t offset = (uint64_t(options.biosScratchBaseDwords) + 3) * 4;
        if (offset > UINT32_MAX || !inBar(uint32_t(offset),hdp.barBytes)) return Status::OutsideBar;
        addresses[4] = uint32_t(offset);
    }
    const unsigned count = options.biosScratchBaseDwords ? 5 : 4;
    for (unsigned i = 0; i < count; ++i) {
        if (addresses[i] >= 0x7f000 && addresses[i] < 0x80000) return Status::AliasedRegisters;
        for (unsigned j = 0; j < i; ++j)
            if (addresses[i] == addresses[j]) return Status::AliasedRegisters;
        if (addresses[i] == hdp.memoryRemapRegister || addresses[i] == hdp.registerRemapRegister)
            return Status::AliasedRegisters;
    }
    regs = {addresses[0],addresses[1],addresses[2],addresses[3],addresses[4]};
    return Status::Ok;
}
bool pciRead(const Io& io, uint32_t offset, uint32_t& value, Result& out) {
    out.lastOffset = offset; ++out.pciReads;
    if (!io.readPci32(io.context,offset,value)) { finish(out,Status::PciReadFailed); return false; }
    out.lastValue = value;
    if (value == UINT32_MAX) { finish(out,Status::DeviceUnavailable); return false; }
    return true;
}
bool mmioRead(const Io& io, uint32_t offset, uint32_t& value, Result& out, bool allowAllOnes = false) {
    out.lastOffset = offset; ++out.mmioReads;
    if (!io.read32(io.context,offset,value)) { finish(out,Status::ReadFailed); return false; }
    out.lastValue = value;
    if (!allowAllOnes && value == UINT32_MAX) { finish(out,Status::DeviceUnavailable); return false; }
    return true;
}
bool mmioWrite(const Io& io, uint32_t offset, uint32_t value, Result& out) {
    out.lastOffset = offset; out.lastValue = value;
    ++out.mmioWrites; out.writesMayHaveOccurred = true;
    if (!io.write32(io.context,offset,value)) { finish(out,Status::WriteFailed); return false; }
    return true;
}
bool snapshot(const Io& io, PciState& pci, Result& out) {
    uint32_t command = 0;
    if (!pciRead(io,0,pci.identity,out)) return false;
    if (pci.identity != 0x744c1002) { finish(out,Status::WrongDevice); return false; }
    if (!pciRead(io,4,command,out) || !pciRead(io,8,pci.revisionClass,out) ||
        !pciRead(io,0xc,pci.header,out)) return false;
    pci.command = out.originalCommand = uint16_t(command);
    if (!(pci.command & 2) || (pci.header & 0x7f0000)) {
        finish(out,Status::InvalidPciState); return false;
    }
    for (unsigned i = 0; i < 6; ++i)
        if (!pciRead(io,0x10+i*4,pci.bars[i],out)) return false;
    if ((pci.bars[5] & 7) || !(pci.bars[5] & 0xfffffff0) ||
        (!(pci.bars[4] & 1) && (pci.bars[4] & 6) == 4)) {
        finish(out,Status::InvalidPciState); return false;
    }
    return true;
}
bool verifyPci(const Io& io, const PciState& pci, Result& out) {
    uint32_t value = 0;
    const uint32_t offsets[9] = {0,8,0xc,0x10,0x14,0x18,0x1c,0x20,0x24};
    for (unsigned i = 0; i < 9; ++i) {
        if (!pciRead(io,offsets[i],value,out)) return false;
        const uint32_t wanted = i == 0 ? pci.identity : i == 1 ? pci.revisionClass :
                                i == 2 ? pci.header : pci.bars[i-3];
        // Only header type must survive; cache-line size/latency/BIST fields
        // are not identity. Capabilities are the OS restore callback's job.
        const uint32_t mask = i == 2 ? 0x00ff0000 : UINT32_MAX;
        if ((value & mask) != (wanted & mask)) { finish(out,Status::PciRestoreMismatch); return false; }
    }
    if (!pciRead(io,4,value,out)) return false;
    out.restoredCommand = uint16_t(value);
    if (out.restoredCommand != uint16_t(pci.command & ~kBusMaster)) {
        finish(out,Status::PciRestoreMismatch); return false;
    }
    return true;
}
bool setEngineHung(const Io& io, uint32_t address, bool hung, Result& out) {
    uint32_t value = 0;
    if (!mmioRead(io,address,value,out)) return false;
    return mmioWrite(io,address,hung ? value | kEngineHung : value & ~kEngineHung,out);
}
void blackout(const Io& io, Result& out) {
    io.delayMs(io.context,500);
    out.blackoutCompleted = true;
}
} // namespace

Status reset(const Navi31IpLayout& layout, const HdpLayout& hdp, const Io& io,
             const Options& options, Result& out) {
    out = Result{};
    out.stage = Stage::Preflight;
    if (!io.read32 || !io.write32 || !io.readPci32 || !io.writePci16 ||
        !io.savePciState || !io.restorePciState || !io.delayMs ||
        !io.delayUs || !options.maxMemoryPolls || options.maxMemoryPolls > 1000000 ||
        !options.memoryPollDelayUs || options.memoryPollDelayUs > 10 ||
        uint64_t(options.maxMemoryPolls) * options.memoryPollDelayUs > 1000000)
        return finish(out,Status::InvalidArgument);
    Registers regs = {};
    const auto resolved = resolve(layout,hdp,options,regs);
    if (resolved != Status::Ok) return finish(out,resolved);
    PciState pci = {};
    out.stage = Stage::PciSnapshot;
    if (!snapshot(io,pci,out)) return out.status;
    if (regs.scratch3) {
        out.stage = Stage::MarkEngineHung;
        if (!setEngineHung(io,regs.scratch3,true,out)) return out.status;
    }
    out.stage = Stage::DisableBusMaster;
    const uint16_t disabled = uint16_t(pci.command & ~kBusMaster);
    if (pci.command & kBusMaster) {
        ++out.pciWrites; out.writesMayHaveOccurred = true;
        out.lastOffset = 4; out.lastValue = disabled;
        if (!io.writePci16(io.context,4,disabled)) return finish(out,Status::PciWriteFailed);
    }
    uint32_t command = 0;
    if (!pciRead(io,4,command,out)) return out.status;
    if (uint16_t(command) != disabled) return finish(out,Status::BusMasterDisableFailed);
    out.busMasterDisableVerified = true;
    // Port ordering intentionally differs from Linux: a fresh BME-off OS
    // snapshot permits exact capability-aware restore without transient DMA.
    out.stage = Stage::SavePciState;
    if (!io.savePciState(io.context)) return finish(out,Status::PciSaveFailed);
    out.pciStateSaved = true;
    out.stage = Stage::DebugParameter;
    if (!mmioWrite(io,regs.param,0,out)) return out.status;
    out.stage = Stage::DebugMessage;
    out.resetMayHaveOccurred = true;
    if (!mmioWrite(io,regs.message,2,out)) { blackout(io,out); return out.status; }
    out.stage = Stage::DebugResponse;
    if (!mmioWrite(io,regs.response,0,out)) { blackout(io,out); return out.status; }
    blackout(io,out);
    out.stage = Stage::RestorePciState;
    if (!io.restorePciState(io.context)) return finish(out,Status::PciRestoreFailed);
    out.pciStateRestored = true;
    out.stage = Stage::VerifyPciState;
    if (!verifyPci(io,pci,out)) return out.status;
    out.stage = Stage::MemoryWait;
    for (uint32_t i = 0; i < options.maxMemoryPolls; ++i) {
        uint32_t memory = 0; ++out.memoryPolls;
        if (!mmioRead(io,regs.memorySize,memory,out,true)) return out.status;
        if (memory != UINT32_MAX) {
            // Linux's availability test permits zero; this is deliberately
            // not a new memory-layout/capacity validation after reset.
            out.memorySizeMiB = memory;
            if (regs.scratch3) {
                out.stage = Stage::ClearEngineHung;
                if (!setEngineHung(io,regs.scratch3,false,out)) return out.status;
            }
            out.stage = Stage::Complete;
            return finish(out,Status::Ok);
        }
        if (i + 1 < options.maxMemoryPolls) io.delayUs(io.context,options.memoryPollDelayUs);
    }
    return finish(out,Status::MemoryTimeout);
}
const char* statusString(Status status) {
    switch (status) {
    case Status::Ok: return "Ok";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::UnsupportedLayout: return "UnsupportedLayout";
    case Status::OutsideBar: return "OutsideBar";
    case Status::AliasedRegisters: return "AliasedRegisters";
    case Status::PciReadFailed: return "PciReadFailed";
    case Status::WrongDevice: return "WrongDevice";
    case Status::InvalidPciState: return "InvalidPciState";
    case Status::PciWriteFailed: return "PciWriteFailed";
    case Status::PciSaveFailed: return "PciSaveFailed";
    case Status::BusMasterDisableFailed: return "BusMasterDisableFailed";
    case Status::PciRestoreFailed: return "PciRestoreFailed";
    case Status::PciRestoreMismatch: return "PciRestoreMismatch";
    case Status::ReadFailed: return "ReadFailed";
    case Status::DeviceUnavailable: return "DeviceUnavailable";
    case Status::WriteFailed: return "WriteFailed";
    case Status::MemoryTimeout: return "MemoryTimeout";
    }
    return "Unknown";
}
} // namespace mode1
} // namespace hackynavi
