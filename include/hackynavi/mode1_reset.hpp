// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_MODE1_RESET_HPP
#define HACKYNAVI_MODE1_RESET_HPP
#include "hackynavi/ip_layout.hpp"
#include "hackynavi/hdp.hpp"

namespace hackynavi {
namespace mode1 {
enum class Status {
    Ok, InvalidArgument, UnsupportedLayout, OutsideBar, AliasedRegisters,
    PciReadFailed, WrongDevice, InvalidPciState, PciWriteFailed,
    PciSaveFailed, BusMasterDisableFailed, PciRestoreFailed, PciRestoreMismatch,
    ReadFailed, DeviceUnavailable, WriteFailed, MemoryTimeout
};
enum class Stage {
    Preflight, PciSnapshot, MarkEngineHung, SavePciState, DisableBusMaster,
    DebugParameter, DebugMessage, DebugResponse, RestorePciState,
    VerifyPciState, MemoryWait, ClearEngineHung, Complete
};
struct Io {
    void* context;
    bool (*read32)(void*, uint32_t byteOffset, uint32_t&);
    // A failed write may still have reached the device.
    bool (*write32)(void*, uint32_t byteOffset, uint32_t);
    bool (*readPci32)(void*, uint32_t configByteOffset, uint32_t&);
    // Must be a 16-bit PCI command write, never a command/status DWORD RMW.
    bool (*writePci16)(void*, uint32_t configByteOffset, uint16_t);
    // Fresh OS capability-aware state capture AFTER BME is cleared/verified,
    // without changing D0, decoding or the active command. Keep the snapshot
    // alive across the operation. Unlike Linux, save follows BME disable so
    // restoring the exact saved command cannot accidentally restart DMA.
    bool (*savePciState)(void*);
    // Restore the BME-off snapshot with BME kept clear THROUGHOUT. A stale BME-on
    // restore followed by clearing BME is not sufficient. Must restore D0 and
    // configured resources/capabilities, never reuse an unrelated stale shadow.
    // A kernel adapter must implement actual OS shadow freshness/lifetime;
    // ordinary IOPCIDevice saveDeviceState(0) may power down the device and
    // does not by itself meet the save callback's requirements.
    bool (*restorePciState)(void*);
    void (*delayMs)(void*, uint32_t); // Must wait at least the supplied interval.
    void (*delayUs)(void*, uint32_t);
};
struct Options {
    uint32_t maxMemoryPolls; // 1..1000000
    uint32_t memoryPollDelayUs; // 1..10, product with maxMemoryPolls <=1000000
    // Validated ATOM FirmwareInfo BIOS-scratch base in DWORDS, not bytes.
    // Zero means no BIOS annotation is available. Never guess this address.
    uint32_t biosScratchBaseDwords;
};
struct Result {
    Status status;
    Stage stage;
    uint32_t lastOffset, lastValue;
    uint32_t mmioReads, mmioWrites, pciReads, pciWrites, memoryPolls;
    uint32_t memorySizeMiB;
    uint16_t originalCommand, restoredCommand;
    bool writesMayHaveOccurred, pciStateSaved, busMasterDisableVerified;
    bool resetMayHaveOccurred, blackoutCompleted, pciStateRestored;
};

// Active Linux Navi31/MP1 13.0.0 bare-metal SMU MODE1 sequence, normal parameter
// zero only: optional scratch3 ENGINE_HUNG, BME disable, fresh OS PCI snapshot,
// DEBUG_PARAM=0, DEBUG_MSG=2, DEBUG_RESP=0, fixed 500ms no-access interval, PCI
// restore retaining BME=0, NBIO memsize availability, clear ENGINE_HUNG.
// There is no debug-mailbox ACK/readiness exchange in the upstream protocol.
// The MP0 13.0.0 bootloader-steady-state wrapper is a no-op; no PSP ACK invented.
//
// Caller must establish a running supported PMFW and exclusive SMU mailbox,
// own and keep the bare-metal device powered, suspend/coordinate its sibling
// audio function, stop host submissions, and exclude every other MMIO/config,
// reset/power/removal actor for the FULL call (including the blackout). PCI open
// or an IP layout does not establish those conditions. Use a sleepable context
// and ordered uncached native MMIO. PCI config callbacks must stay available
// while MMIO is forbidden. Use immutable successful buildNavi31IpLayout and
// buildHdpLayout outputs referring to the same device and BAR5 mapping.
//
// All addresses/arguments are validated before callbacks. Once reset trigger
// is attempted, even an ambiguous write failure observes the 500ms blackout;
// failures do not automatically restore PCI, resume DMA, or clear ENGINE_HUNG.
// Retain affected allocations, maps and ownership for explicit recovery.
// On success BME remains disabled. Ok means this source protocol completed
// with PCI/MMIO available, NOT an ACK proving every engine reset, ASIC POST,
// firmware upload, GART readiness, preserved VRAM, or permission to free old
// GPU resources. The caller must perform actual POST/reinitialization and
// obtain any stronger retirement proof it requires. No allocation occurs.
Status reset(const Navi31IpLayout&, const HdpLayout&, const Io&,
             const Options&, Result&);
const char* statusString(Status);
} // namespace mode1
} // namespace hackynavi
#endif
