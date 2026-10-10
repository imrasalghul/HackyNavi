// SPDX-License-Identifier: MIT
// NBIO operations and RSMU protocol adapted from Linux commit
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// soc21.c:554-560,946-949,964-994; nbio_v4_3.c:151-179,313-321,343-350;
// amdgpu_reg_access.c:363-382,494-511,591-609,768-791;
// nbio_4_3_0_offset.h and nbio_4_3_0_sh_mask.h (named registers below).
// amdgpu_doorbell_mgr.c:204-211 supplies the assigned PCI BAR2 resource.
// https://github.com/torvalds/linux/tree/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd
// Copyright 2021, 2025 Advanced Micro Devices, Inc.
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
#include "hackynavi/nbio.hpp"

namespace hackynavi {
namespace {
const uint32_t kStrapMask = 0x80;
const uint32_t kDoorbellMask = 1;
const uint32_t kSelfRingMask = 0x000fff03; // EN, MODE, SIZE.
struct Bases { uint64_t value[6]; unsigned count; bool supported; };
void collect(const DiscoveryIp& ip, void* context) {
    if (ip.hardwareId != 108) return;
    Bases& b = *static_cast<Bases*>(context);
    ++b.count;
    b.supported = ip.dieId == 0 && ip.instance == 0 && ip.major == 4 &&
        ip.minor == 3 && ip.revision == 0 && !ip.harvest && !ip.subRevision &&
        !ip.variant && ip.baseAddressCount >= 6;
    if (b.supported) for (unsigned i = 0; i < 6; ++i) b.value[i] = ip.baseAddresses[i];
}
bool inBar(uint32_t address, uint64_t bytes) {
    return !(address & 3u) && bytes >= 4 && address <= bytes - 4;
}
bool conflict(uint32_t address) {
    // Existing VRAM selector/data and the separately owned HDP remap hole.
    return address == 0 || address == 4 || address == 0x18 ||
        (address >= 0x7f000 && address < 0x80000);
}
NbioStatus validate(const NbioLayout& l) {
    if (!l.valid) return NbioStatus::InvalidLayout;
    const uint32_t registers[] = {l.strap2Register, l.doorbellApertureRegister,
        l.selfRingBaseLowRegister, l.selfRingBaseHighRegister, l.selfRingControlRegister,
        l.rsmuIndexRegister, l.rsmuDataRegister, l.rsmuIndexHiRegister};
    for (unsigned i = 0; i < 8; ++i) {
        if ((registers[i] & 3u) || conflict(registers[i])) return NbioStatus::InvalidLayout;
        for (unsigned j = 0; j < i; ++j)
            if (registers[i] == registers[j]) return NbioStatus::InvalidLayout;
        if (i >= 5 && !inBar(registers[i], l.barBytes)) return NbioStatus::OutsideBar;
    }
    // All arithmetic widened before addition; public layout tampering cannot
    // wrap these adjacency/BASE_IDX2 relations or substitute a window register.
    if (uint64_t(l.rsmuIndexRegister) + 4 != l.rsmuDataRegister ||
        uint64_t(l.rsmuIndexRegister) + 8 != l.rsmuIndexHiRegister ||
        uint64_t(l.doorbellApertureRegister) + (0xf3 - 0xc0) * 4 != l.selfRingBaseHighRegister ||
        uint64_t(l.selfRingBaseHighRegister) + 4 != l.selfRingBaseLowRegister ||
        uint64_t(l.selfRingBaseLowRegister) + 4 != l.selfRingControlRegister ||
        l.doorbellApertureRegister < 0xc0 * 4 || l.strap2Register < 0xd102 * 4)
        return NbioStatus::InvalidLayout;
    return NbioStatus::Ok;
}
bool resolve(uint64_t base, uint32_t offset, uint32_t& result) {
    if (base > UINT32_MAX / 4u || offset > UINT32_MAX / 4u - base) return false;
    result = static_cast<uint32_t>((base + offset) * 4);
    return true;
}
bool readMmio(const NbioIo& io, uint32_t address, uint32_t& value, NbioResult& out) {
    ++out.readsAttempted;
    return io.read32(io.context, address, value);
}
bool writeMmio(const NbioIo& io, uint32_t address, uint32_t value,
               bool selector, NbioResult& out) {
    ++out.writesAttempted;
    if (selector) ++out.selectorWritesAttempted; else ++out.controlWritesAttempted;
    out.writesMayHaveOccurred = true;
    return io.write32(io.context, address, value);
}
// Only the five fixed controls in a preflighted layout reach these helpers.
// The RSMU index takes an absolute BYTE address. Its posting read is not an
// equality check or evidence that an old selector can safely be restored.
NbioStatus checkIndirectHigh(const NbioLayout& l, const NbioIo& io, NbioResult& out) {
    uint32_t high = 0;
    if (!readMmio(io, l.rsmuIndexHiRegister, high, out)) return NbioStatus::TransportError;
    // Linux extended accessors clear HI after use. A previous actor need not
    // have done so; never let stale high bits redirect a 32-bit target.
    if (high == UINT32_MAX) return NbioStatus::DeviceUnavailable;
    return high ? NbioStatus::IndirectHighNotZero : NbioStatus::Ok;
}
NbioStatus readRegister(const NbioLayout& l, const NbioIo& io, uint32_t address,
                        uint32_t& value, NbioResult& out) {
    if (inBar(address, l.barBytes))
        return readMmio(io, address, value, out) ? NbioStatus::Ok : NbioStatus::TransportError;
    const NbioStatus high = checkIndirectHigh(l, io, out);
    if (high != NbioStatus::Ok) return high;
    uint32_t posting = 0;
    return writeMmio(io, l.rsmuIndexRegister, address, true, out) &&
        readMmio(io, l.rsmuIndexRegister, posting, out) &&
        readMmio(io, l.rsmuDataRegister, value, out) ? NbioStatus::Ok : NbioStatus::TransportError;
}
NbioStatus writeRegister(const NbioLayout& l, const NbioIo& io, uint32_t address,
                         uint32_t value, NbioResult& out) {
    if (inBar(address, l.barBytes))
        return writeMmio(io, address, value, false, out) ? NbioStatus::Ok : NbioStatus::TransportError;
    const NbioStatus high = checkIndirectHigh(l, io, out);
    if (high != NbioStatus::Ok) return high;
    uint32_t posting = 0;
    return writeMmio(io, l.rsmuIndexRegister, address, true, out) &&
        readMmio(io, l.rsmuIndexRegister, posting, out) &&
        writeMmio(io, l.rsmuDataRegister, value, false, out) &&
        readMmio(io, l.rsmuDataRegister, posting, out) ? NbioStatus::Ok : NbioStatus::TransportError;
}
NbioStatus maskedReadback(const NbioLayout& l, const NbioIo& io, uint32_t address,
                          uint32_t value, uint32_t mask, NbioResult& out) {
    uint32_t actual = 0;
    const NbioStatus read = readRegister(l, io, address, actual, out);
    if (read != NbioStatus::Ok) return read;
    if (actual == UINT32_MAX) return NbioStatus::DeviceUnavailable;
    return (actual & mask) == (value & mask) ? NbioStatus::Ok : NbioStatus::ReadbackMismatch;
}
NbioStatus updateBit(const NbioLayout& l, const NbioIo& io, uint32_t address,
                     uint32_t mask, bool enable, NbioResult& out) {
    uint32_t value = 0;
    NbioStatus status = readRegister(l, io, address, value, out);
    if (status != NbioStatus::Ok) return status;
    if (value == UINT32_MAX) return NbioStatus::DeviceUnavailable;
    value = (value & ~mask) | (enable ? mask : 0u);
    status = writeRegister(l, io, address, value, out);
    if (status != NbioStatus::Ok) return status;
    return maskedReadback(l, io, address, value, mask, out);
}
NbioStatus preflight(const NbioLayout& layout, const NbioIo& io) {
    if (!io.read32 || !io.write32) return NbioStatus::InvalidArgument;
    return validate(layout);
}
bool validAssignment(const NbioDoorbellAssignment& a) {
    // PCI memory BAR ranges are nonempty powers of two aligned to their size.
    // This validates declaration arithmetic, not ownership or platform mapping.
    return a.externallyVerified && a.busAddress && a.bytes >= 4 &&
        !(a.bytes & (a.bytes - 1)) && !(a.busAddress & (a.bytes - 1)) &&
        a.bytes - 1 <= UINT64_MAX - a.busAddress;
}
} // namespace

NbioStatus buildNbioLayout(const uint8_t* data, size_t bytes, uint64_t barBytes,
                           NbioLayout& out) {
    out = NbioLayout{};
    DiscoveryInfo info = {};
    Bases b = {};
    if (parseDiscovery(data, bytes, info, collect, &b) != DiscoveryStatus::Ok)
        return NbioStatus::DiscoveryRejected;
    if (!b.count) return NbioStatus::MissingNbio;
    if (info.baseAddresses64Bit || info.dieCount != 1 || info.dies[0].id != 0 ||
        b.count != 1 || !b.supported) return NbioStatus::UnsupportedNbio;
    NbioLayout l = {};
    // Generated BASE_IDX1: RSMU INDEX=0, DATA=1, INDEX_HI=2.
    // BASE_IDX2: RCC_DOORBELL_APER_EN=c0, SELFRING HIGH=f3, LOW=f4, CNTL=f5.
    // BASE_IDX5: RCC_DEV0_EPF2_STRAP2=d102.
    if (!resolve(b.value[1], 0, l.rsmuIndexRegister) ||
        !resolve(b.value[1], 1, l.rsmuDataRegister) ||
        !resolve(b.value[1], 2, l.rsmuIndexHiRegister) ||
        !resolve(b.value[2], 0xc0, l.doorbellApertureRegister) ||
        !resolve(b.value[2], 0xf3, l.selfRingBaseHighRegister) ||
        !resolve(b.value[2], 0xf4, l.selfRingBaseLowRegister) ||
        !resolve(b.value[2], 0xf5, l.selfRingControlRegister) ||
        !resolve(b.value[5], 0xd102, l.strap2Register)) return NbioStatus::AddressOverflow;
    l.valid = true; l.barBytes = barBytes;
    const NbioStatus status = validate(l);
    if (status != NbioStatus::Ok) return status;
    out = l;
    return NbioStatus::Ok;
}
NbioStatus initializeNbioRegisters(const NbioLayout& layout, const NbioIo& io,
                                  NbioState& state, NbioResult& out) {
    out = NbioResult{};
    const NbioStatus checked = preflight(layout, io);
    if (checked != NbioStatus::Ok) return checked;
    if (state.initialized_ || state.faulted_) return NbioStatus::InvalidState;
    state.layout_ = layout;
    const NbioStatus status = updateBit(layout, io, layout.strap2Register, kStrapMask, false, out);
    state.initialized_ = status == NbioStatus::Ok;
    state.faulted_ = status != NbioStatus::Ok;
    return status;
}
NbioStatus setNbioDoorbellAperture(const NbioIo& io, NbioState& state,
                                 bool enable, NbioResult& out) {
    out = NbioResult{};
    if (!state.initialized()) return NbioStatus::InvalidState;
    const NbioStatus checked = preflight(state.layout_, io);
    if (checked != NbioStatus::Ok) return checked;
    const NbioStatus status = updateBit(state.layout_, io,
        state.layout_.doorbellApertureRegister, kDoorbellMask, enable, out);
    if (status == NbioStatus::Ok) state.doorbellEnabled_ = enable;
    else state.faulted_ = true;
    return status;
}
NbioStatus enableNbioSelfRing(const NbioIo& io, NbioState& state,
                             const NbioDoorbellAssignment& assignment, NbioResult& out) {
    out = NbioResult{};
    if (!state.initialized() || !state.doorbellEnabled_ || state.selfRingEnabled_)
        return NbioStatus::InvalidState;
    const NbioStatus checked = preflight(state.layout_, io);
    if (checked != NbioStatus::Ok) return checked;
    if (!validAssignment(assignment)) return NbioStatus::InvalidDoorbellAssignment;
    const NbioLayout& l = state.layout_;
    uint32_t control = 0, low = 0, high = 0;
    const uint32_t wantedLow = static_cast<uint32_t>(assignment.busAddress);
    const uint32_t wantedHigh = static_cast<uint32_t>(assignment.busAddress >> 32);
    NbioStatus status = readRegister(l, io, l.selfRingControlRegister, control, out);
    if (status == NbioStatus::Ok) {
        if (control == UINT32_MAX) status = NbioStatus::DeviceUnavailable;
        else if (control & 1) status = NbioStatus::SelfRingAlreadyEnabled;
        else {
            status = writeRegister(l, io, l.selfRingBaseLowRegister, wantedLow, out);
            if (status == NbioStatus::Ok)
                status = writeRegister(l, io, l.selfRingBaseHighRegister, wantedHigh, out);
            if (status == NbioStatus::Ok)
                status = readRegister(l, io, l.selfRingBaseLowRegister, low, out);
            if (status == NbioStatus::Ok)
                status = readRegister(l, io, l.selfRingBaseHighRegister, high, out);
            if (status == NbioStatus::Ok && (low != wantedLow || high != wantedHigh))
                status = NbioStatus::ReadbackMismatch;
            if (status == NbioStatus::Ok)
                status = writeRegister(l, io, l.selfRingControlRegister, 3, out);
            if (status == NbioStatus::Ok)
                status = maskedReadback(l, io, l.selfRingControlRegister, 3, kSelfRingMask, out);
        }
    }
    if (status == NbioStatus::Ok) state.selfRingEnabled_ = true;
    else state.faulted_ = true;
    return status;
}
NbioStatus disableNbioApertures(const NbioIo& io, NbioState& state, NbioResult& out) {
    out = NbioResult{};
    if (!state.initialized()) return NbioStatus::InvalidState;
    const NbioStatus checked = preflight(state.layout_, io);
    if (checked != NbioStatus::Ok) return checked;
    const NbioLayout& l = state.layout_;
    NbioStatus status = updateBit(l, io, l.doorbellApertureRegister, kDoorbellMask, false, out);
    if (status == NbioStatus::Ok) {
        status = writeRegister(l, io, l.selfRingControlRegister, 0, out);
        if (status == NbioStatus::Ok)
            status = maskedReadback(l, io, l.selfRingControlRegister, 0, kSelfRingMask, out);
    }
    if (status == NbioStatus::Ok) {
        state.layout_ = NbioLayout{};
        state.initialized_ = state.doorbellEnabled_ = state.selfRingEnabled_ = false;
    }
    else state.faulted_ = true;
    return status;
}
const char* nbioStatusString(NbioStatus status) {
    switch (status) {
    case NbioStatus::Ok: return "ok";
    case NbioStatus::InvalidArgument: return "invalid NBIO arguments";
    case NbioStatus::DiscoveryRejected: return "discovery rejected";
    case NbioStatus::MissingNbio: return "NBIO missing";
    case NbioStatus::UnsupportedNbio: return "unsupported NBIO layout";
    case NbioStatus::AddressOverflow: return "NBIO register address exceeds 32-bit byte domain";
    case NbioStatus::InvalidLayout: return "invalid NBIO control or register-window layout";
    case NbioStatus::OutsideBar: return "NBIO RSMU window outside BAR";
    case NbioStatus::InvalidState: return "NBIO state requires initialization or independent recovery";
    case NbioStatus::InvalidDoorbellAssignment: return "unverified or invalid assigned doorbell BAR";
    case NbioStatus::TransportError: return "NBIO MMIO transport failure";
    case NbioStatus::DeviceUnavailable: return "NBIO device unavailable";
    case NbioStatus::ReadbackMismatch: return "NBIO register readback mismatch";
    case NbioStatus::SelfRingAlreadyEnabled: return "self-ring aperture already enabled";
    case NbioStatus::IndirectHighNotZero: return "RSMU high index is not zero";
    }
    return "unknown NBIO status";
}
} // namespace hackynavi
