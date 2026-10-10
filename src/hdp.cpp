// SPDX-License-Identifier: MIT
// NBIO remap and CPU HDP flush sequence adapted from Linux
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0: nbio_v4_3.c:31-36,474-480,
// amdgpu_hdp.c:51-60, hdp_v6_0.c:140-143; generated nbio_4_3_0_offset.h
// regBIF_BX0_REMAP_HDP_* and regRCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE, BASE_IDX=2.
// Copyright 2021, 2023 Advanced Micro Devices, Inc.
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
#include "hackynavi/hdp.hpp"

namespace hackynavi {
namespace {
const uint32_t kHole = 0x7f000;
struct Nbio { uint64_t base; unsigned count; bool valid; };
void collect(const DiscoveryIp& ip, void* context) {
    if (ip.hardwareId != 108) return; // NBIF_HWID -> Linux NBIO_HWIP.
    auto& nbio = *static_cast<Nbio*>(context);
    ++nbio.count;
    nbio.valid = ip.dieId == 0 && ip.instance == 0 && ip.major == 4 &&
        ip.minor == 3 && ip.revision == 0 && !ip.harvest && !ip.subRevision &&
        !ip.variant && ip.baseAddressCount >= 3 && ip.baseAddresses[2] <= UINT32_MAX;
    if (nbio.valid) nbio.base = ip.baseAddresses[2];
}
bool inBar(uint32_t offset, uint64_t size) {
    return (offset & 3u) == 0 && size >= 4 && offset <= size - 4;
}
bool validLayout(const HdpLayout& layout) {
    if (!layout.valid || layout.memoryFlushRegister != kHole ||
        layout.registerFlushRegister != kHole + 4 || layout.barBytes < kHole + 4096)
        return false;
    const uint32_t registers[] = {layout.memoryRemapRegister, layout.registerRemapRegister,
        layout.memorySizeRegister, layout.memoryFlushRegister, layout.registerFlushRegister};
    for (unsigned i = 0; i < 5; ++i) {
        if (!inBar(registers[i], layout.barBytes)) return false;
        // Never allow discovered register controls to overlap the reserved hole.
        if (i < 3 && registers[i] >= kHole && registers[i] < kHole + 4096) return false;
        for (unsigned j = 0; j < i; ++j) if (registers[i] == registers[j]) return false;
    }
    return layout.registerRemapRegister == uint64_t(layout.memoryRemapRegister) + 4 &&
        uint64_t(layout.memorySizeRegister) + (0x12d - 0xc3) * 4 == layout.memoryRemapRegister;
}
bool read(const HdpIo& io, uint32_t offset, uint32_t& value, HdpResult& out) {
    ++out.readsAttempted;
    return io.read32(io.context, offset, value);
}
bool write(const HdpIo& io, uint32_t offset, uint32_t value, HdpResult& out) {
    ++out.writesAttempted;
    out.writesMayHaveOccurred = true;
    return io.write32(io.context, offset, value);
}
HdpStatus verifyRemap(const HdpLayout& layout, const HdpIo& io, HdpResult& out) {
    uint32_t memory = 0, registers = 0;
    if (!read(io, layout.memoryRemapRegister, memory, out) ||
        !read(io, layout.registerRemapRegister, registers, out)) return HdpStatus::TransportError;
    if (memory == UINT32_MAX || registers == UINT32_MAX) return HdpStatus::DeviceUnavailable;
    return memory == kHole && registers == kHole + 4 ? HdpStatus::Ok : HdpStatus::ReadbackMismatch;
}
}
HdpStatus buildHdpLayout(const uint8_t* data, size_t bytes, uint64_t barBytes,
                         HdpLayout& out) {
    out = HdpLayout{};
    DiscoveryInfo info = {};
    Nbio nbio = {};
    if (parseDiscovery(data, bytes, info, collect, &nbio) != DiscoveryStatus::Ok)
        return HdpStatus::DiscoveryRejected;
    if (!nbio.count) return HdpStatus::MissingNbio;
    if (info.baseAddresses64Bit || info.dieCount != 1 || info.dies[0].id != 0 ||
        nbio.count != 1 || !nbio.valid) return HdpStatus::UnsupportedNbio;
    // All generated offsets below use discovered NBIO base index 2 in DWORDs.
    if (nbio.base > UINT32_MAX / 4u - 0x12e) return HdpStatus::OutsideBar;
    HdpLayout layout = {true, uint32_t((nbio.base + 0x12d) * 4),
        uint32_t((nbio.base + 0x12e) * 4), uint32_t((nbio.base + 0xc3) * 4),
        kHole, kHole + 4, barBytes};
    if (!validLayout(layout)) return HdpStatus::OutsideBar;
    out = layout;
    return HdpStatus::Ok;
}
HdpStatus configureHdp(const HdpLayout& layout, const HdpIo& io,
                       HdpState& state, HdpResult& out) {
    out = HdpResult{};
    if (!validLayout(layout) || !io.read32 || !io.write32) return HdpStatus::InvalidArgument;
    if (state.configured_ || state.faulted_) return HdpStatus::InvalidState;
    state.layout_ = layout;
    HdpStatus result = HdpStatus::TransportError;
    if (write(io, layout.memoryRemapRegister, layout.memoryFlushRegister, out) &&
        write(io, layout.registerRemapRegister, layout.registerFlushRegister, out))
        result = verifyRemap(layout, io, out);
    state.configured_ = result == HdpStatus::Ok;
    state.faulted_ = !state.configured_;
    return result;
}
HdpStatus flushHdp(const HdpIo& io, HdpState& state, HdpResult& out) {
    out = HdpResult{};
    if (!io.read32 || !io.write32) return HdpStatus::InvalidArgument;
    if (!state.configured() || !validLayout(state.layout_)) return HdpStatus::InvalidState;
    HdpStatus status = verifyRemap(state.layout_, io, out);
    if (status == HdpStatus::Ok) {
        uint32_t memoryMiB = 0;
        if (!write(io, state.layout_.memoryFlushRegister, 0, out) ||
            !read(io, state.layout_.memorySizeRegister, memoryMiB, out)) status = HdpStatus::TransportError;
        else if (!memoryMiB || memoryMiB == UINT32_MAX) status = HdpStatus::DeviceUnavailable;
    }
    if (status != HdpStatus::Ok) { state.faulted_ = true; state.configured_ = false; }
    return status;
}
const char* hdpStatusString(HdpStatus status) {
    switch (status) {
    case HdpStatus::Ok: return "ok";
    case HdpStatus::InvalidArgument: return "invalid HDP arguments";
    case HdpStatus::DiscoveryRejected: return "discovery rejected";
    case HdpStatus::UnsupportedNbio: return "unsupported NBIO layout";
    case HdpStatus::MissingNbio: return "NBIO missing";
    case HdpStatus::OutsideBar: return "HDP register outside BAR or remap collision";
    case HdpStatus::TransportError: return "HDP MMIO transport failure";
    case HdpStatus::ReadbackMismatch: return "HDP remap readback mismatch";
    case HdpStatus::DeviceUnavailable: return "HDP device unavailable";
    case HdpStatus::InvalidState: return "HDP state requires initialization or reset";
    }
    return "unknown HDP status";
}
} // namespace hackynavi
