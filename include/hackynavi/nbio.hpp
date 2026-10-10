// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_NBIO_HPP
#define HACKYNAVI_NBIO_HPP
#include "hackynavi/discovery.hpp"

namespace hackynavi {
enum class NbioStatus {
    Ok, InvalidArgument, DiscoveryRejected, MissingNbio, UnsupportedNbio,
    AddressOverflow, InvalidLayout, OutsideBar, InvalidState,
    InvalidDoorbellAssignment, TransportError, DeviceUnavailable,
    ReadbackMismatch, SelfRingAlreadyEnabled, IndirectHighNotZero
};
struct NbioLayout {
    bool valid;
    // Absolute register BYTE addresses, from discovery DWORD bases plus the
    // generated offsets. Targets outside BAR5 use the RSMU indirect window.
    uint32_t strap2Register, doorbellApertureRegister;
    uint32_t selfRingBaseLowRegister, selfRingBaseHighRegister, selfRingControlRegister;
    // These three addresses must fit BAR5. INDEX_HI is read, never written.
    uint32_t rsmuIndexRegister, rsmuDataRegister, rsmuIndexHiRegister;
    uint64_t barBytes;
};
struct NbioIo {
    void* context;
    // Ordered uncached native 32-bit BAR5 MMIO; offsets are bytes, not DWORDs.
    // false does not guarantee a write had no effect. Preserve ordering across
    // callbacks, including the posting reads following indirect writes.
    bool (*read32)(void*, uint32_t byteOffset, uint32_t& value);
    bool (*write32)(void*, uint32_t byteOffset, uint32_t value);
};
struct NbioResult {
    uint32_t readsAttempted, writesAttempted;
    uint32_t selectorWritesAttempted, controlWritesAttempted;
    bool writesMayHaveOccurred;
};
struct NbioDoorbellAssignment {
    // Caller confirms a final, assigned PCI memory BAR2 resource belonging to
    // this device, including its PCI bus address (not a kernel virtual address
    // or blindly translated CPU physical address). Keep it stable until fini.
    bool externallyVerified;
    uint64_t busAddress, bytes;
};
class NbioState {
public:
    NbioState() : layout_{}, initialized_(false), faulted_(false),
        doorbellEnabled_(false), selfRingEnabled_(false) {}
    NbioState(const NbioState&) = delete;
    NbioState& operator=(const NbioState&) = delete;
    bool initialized() const { return initialized_ && !faulted_; }
    bool faulted() const { return faulted_; }
    // Last successful requests, not live hardware observations; false also
    // means unknown after a fault. No externally inherited state is asserted.
    bool doorbellEnabled() const { return initialized() && doorbellEnabled_; }
    bool selfRingEnabled() const { return initialized() && selfRingEnabled_; }
    // CPU bookkeeping only. Caller first independently resets/quiesces the
    // device and invalidates all dependent state. This performs no MMIO.
    void clearAfterDeviceReset() {
        layout_ = NbioLayout{};
        initialized_ = faulted_ = doorbellEnabled_ = selfRingEnabled_ = false;
    }
private:
    NbioLayout layout_;
    bool initialized_, faulted_, doorbellEnabled_, selfRingEnabled_;
    friend NbioStatus initializeNbioRegisters(const NbioLayout&, const NbioIo&, NbioState&, NbioResult&);
    friend NbioStatus setNbioDoorbellAperture(const NbioIo&, NbioState&, bool, NbioResult&);
    friend NbioStatus enableNbioSelfRing(const NbioIo&, NbioState&, const NbioDoorbellAssignment&, NbioResult&);
    friend NbioStatus disableNbioApertures(const NbioIo&, NbioState&, NbioResult&);
};

// Validates the complete discovery binary before selecting exactly one die-0,
// instance-0 NBIF_HWID108, version4.3.0, six or more 32-bit DWORD bases. Rejects
// 64-bit encoding, harvest, subrevision/variant, register aliases and overflow.
// RSMU must fit the borrowed BAR extent; other fixed targets may be indirect.
// out is zero on failure. Keep a successful layout immutable. No MMIO occurs.
NbioStatus buildNbioLayout(const uint8_t* data, size_t bytes, uint64_t barBytes,
                           NbioLayout& out);

// All hardware operations require exclusive ownership of a powered bare-metal
// Navi31 (not an SR-IOV VF), appropriate reset/quiescence already established,
// and caller serialization across the WHOLE call versus reset, suspend and all
// other register users. Serialize the RSMU window as a unit; a single device
// lock shared with VRAM transfers is suitable. RSMU is physically distinct
// from MM_INDEX/MM_DATA: MM_INDEX_HI is untouched. Before every indirect
// transaction RSMU_INDEX_HI is read and must be zero; it is never written.
// No selector restore is attempted. Even an indirect read writes INDEX.
// Every call preflights the whole layout and callbacks before MMIO. Result is
// zeroed on entry. Hardware failures fault state, stop immediately and require
// independent recovery; no rollback or successful cleanup is implied.

// Only clears STRAP_NO_SOFT_RESET_DEV0_F2 (bit7), preserving other strap bits.
// This does not reset the GPU. To follow soc21_common_hw_init, caller next
// configures HDP via the separate module, then enables the doorbell aperture.
// ASPM/link configuration, interrupt setup and queue doorbells are not included.
NbioStatus initializeNbioRegisters(const NbioLayout& layout, const NbioIo& io,
                                  NbioState& state, NbioResult& out);
// RMWs only BIF_DOORBELL_APER_EN bit0, with masked readback verification.
// Disabling does not disable self-ring; Linux deliberately separates them.
NbioStatus setNbioDoorbellAperture(const NbioIo& io, NbioState& state,
                                 bool enable, NbioResult& out);
// Optional LATE operation: all PCI BAR resizing/reassignment must be finished.
// Requires this state to have enabled the doorbell aperture. Rejects an already
// enabled self-ring (including inherited hardware state), writes LOW then HIGH,
// verifies both, then writes EN=1, MODE=1, SIZE=0 exactly as Linux. SIZE=0 is the
// hardware encoding used by Linux, not a byte count derived from BAR size.
// This only configures an aperture; no doorbell index/ring allocation occurs.
NbioStatus enableNbioSelfRing(const NbioIo& io, NbioState& state,
                             const NbioDoorbellAssignment& assignment, NbioResult& out);
// Linux common_hw_fini ordering: disable ordinary aperture, then write zero to
// self-ring control. Also handles an inherited self-ring not enabled by us.
// Does not touch base registers or restore strap. Success clears initialization
// bookkeeping; failure can leave either aperture active and faults the state.
NbioStatus disableNbioApertures(const NbioIo& io, NbioState& state, NbioResult& out);
const char* nbioStatusString(NbioStatus status);
} // namespace hackynavi
#endif
