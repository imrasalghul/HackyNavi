// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_HDP_HPP
#define HACKYNAVI_HDP_HPP
#include "hackynavi/discovery.hpp"

namespace hackynavi {
enum class HdpStatus {
    Ok, InvalidArgument, DiscoveryRejected, UnsupportedNbio, MissingNbio,
    OutsideBar, TransportError, ReadbackMismatch, DeviceUnavailable, InvalidState
};
struct HdpLayout {
    bool valid;
    uint32_t memoryRemapRegister, registerRemapRegister, memorySizeRegister;
    uint32_t memoryFlushRegister, registerFlushRegister;
    uint64_t barBytes;
};
struct HdpIo {
    void* context;
    // Ordered, uncached, native 32-bit MMIO. The owner orders prior CPU table
    // uploads before the flush write; these callbacks must preserve that order.
    bool (*read32)(void*, uint32_t byteOffset, uint32_t& value);
    bool (*write32)(void*, uint32_t byteOffset, uint32_t value);
};
struct HdpResult {
    uint32_t readsAttempted, writesAttempted;
    bool writesMayHaveOccurred;
};
class HdpState {
public:
    HdpState() : layout_{}, configured_(false), faulted_(false) {}
    HdpState(const HdpState&) = delete;
    HdpState& operator=(const HdpState&) = delete;
    bool configured() const { return configured_ && !faulted_; }
    bool faulted() const { return faulted_; }
    // Only after independently quiescing/resetting hardware and invalidating
    // every caller's old state. This clears CPU bookkeeping, not hardware.
    void clearAfterDeviceReset() { layout_ = HdpLayout{}; configured_ = faulted_ = false; }
private:
    HdpLayout layout_;
    bool configured_, faulted_;
    friend HdpStatus configureHdp(const HdpLayout&, const HdpIo&, HdpState&, HdpResult&);
    friend HdpStatus flushHdp(const HdpIo&, HdpState&, HdpResult&);
};
// Bare-metal Navi31, NBIO 4.3.0, x86 4 KiB host-page policy only. Builds from a
// full validated discovery binary (32-bit base encoding, one die/instance 0).
// Remapped hole is Linux's 0x7f000..0x7ffff. No MMIO, no reserved-hole ownership
// claim. out is zero on failure. Keep a successful layout immutable.
HdpStatus buildHdpLayout(const uint8_t* discovery, size_t bytes, uint64_t barBytes,
                         HdpLayout& out);
// Caller exclusively owns powered/quiescent device, NBIO and the remap hole;
// serializes the whole configure/flush operation against reset and all MMIO
// users; and guarantees it is not an SR-IOV function. configure programs the
// two remap controls and checks readback. Failure may leave hardware changed.
// No automatic register restoration is attempted; faulted state cannot flush.
HdpStatus configureHdp(const HdpLayout& layout, const HdpIo& io,
                       HdpState& state, HdpResult& out);
// Verifies the remap is unchanged, writes zero to the memory-flush hole and
// performs Linux's NBIO memory-size posting read. This implements that CPU HDP
// protocol only, not a GPU TLB flush, GPU completion wait or global cache fence.
// Caller keeps the same BAR mapping/device identity as configureHdp.
HdpStatus flushHdp(const HdpIo& io, HdpState& state, HdpResult& out);
const char* hdpStatusString(HdpStatus status);
} // namespace hackynavi
#endif
