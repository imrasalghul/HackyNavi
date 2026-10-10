// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_BOOT_DISPLAY_HPP
#define HACKYNAVI_BOOT_DISPLAY_HPP
#include "hackynavi/discovery.hpp"
#include "hackynavi/memory_layout.hpp"
#include "hackynavi/vram_allocator.hpp"

namespace hackynavi {
namespace bootdisplay {
constexpr size_t kPipeCount = 4;
constexpr size_t kVgaCount = 6;
enum class PipeRegister : size_t {
    Control, SurfaceConfig, TilingConfig, ViewportStart, ViewportDimension,
    Pitch, SurfaceControl, FlipControl, FlipControl2, Vmid,
    AddressLo, AddressHi, InUseLo, InUseHi, EarliestLo, EarliestHi,
    TlbControl, ApertureLow, ApertureHigh, CursorControl, DmdataControl,
    RequestSize, Count
};
constexpr size_t kPipeRegisterCount = static_cast<size_t>(PipeRegister::Count);
enum class GlobalRegister : size_t { FramebufferBase, FramebufferTop,
    FramebufferOffset, AgpBottom, AgpTop, Count };
constexpr size_t kGlobalRegisterCount = static_cast<size_t>(GlobalRegister::Count);
enum class Status {
    Ok, InvalidArgument, DiscoveryRejected, UnsupportedDmu, MissingDmu,
    OutsideBar, TransportError, DeviceUnavailable, PowerUnavailable, UnstableSnapshot,
    LegacyVga, UnsupportedPlane, UnsupportedAddressMode, OutsideVram, PriorDriverRequiresReset
};
struct Layout {
    bool valid;
    uint64_t barBytes;
    uint32_t power[kPipeCount]; // Always-on DOMAIN0..3_PG_STATUS, BASE_IDX2.
    uint32_t powerConfig[kPipeCount]; // DOMAIN0..3_PG_CONFIG, BASE_IDX2.
    uint32_t sos; // MP0 C2PMSG81 when discovery includes exact MP0 13.0.0.
    uint32_t vga[kVgaCount], global[kGlobalRegisterCount];
    uint32_t pipe[kPipeCount][kPipeRegisterCount];
};
struct Io {
    void* context;
    // Ordered uncached native32 reads at BAR BYTE offsets. No write callback.
    bool (*read32)(void*, uint32_t byteOffset, uint32_t& value);
};
struct Snapshot {
    uint32_t power[kPipeCount];
    uint32_t powerConfig[kPipeCount];
    uint32_t vga[kVgaCount], global[kGlobalRegisterCount];
    uint32_t pipe[kPipeCount][kPipeRegisterCount];
};
struct Plane {
    uint8_t pipe, bytesPerPixel;
    uint32_t pitchPixels, x, y, width, height;
    uint64_t surfaceMcAddress, footprintBytes;
    // VRAM-relative, rounded outward to4KiB, including preceding rows/padding.
    // This is an exclusion interval, never an ownership/free-space claim.
    VramRange reservation;
};
struct PowerProbe {
    uint8_t pipe; // 0xff until a power probe begins.
    // A bit means that read's callback succeeded, including an all-ones value.
    // It does not mean the value passed power-state validation. No missing
    // read can be inferred from the zero-initialized corresponding field.
    uint8_t readsSucceededMask; // bit0=statusBefore, bit1=config, bit2=statusAfter.
    uint32_t statusBefore, config, statusAfter;
};
struct PowerInventory {
    PowerProbe probes[kPipeCount]; // Unstarted probes have pipe0xff/mask0.
    uint8_t failedPipe; // 0xff on success or preflight failure.
    uint32_t readsAttempted, lastRegister, lastValue;
    bool lastReadSucceeded; // Includes all-ones; no power-state interpretation.
};
struct Capture {
    bool complete; // true only on Ok; every one of the4 HUBPs was classified.
    // Both register passes completed and matched, even if later plane decoding
    // rejected the format. No authority/ownership claim. Powered-off pipes'
    // HUBP windows were skipped and their zero storage is not an observation.
    bool snapshotReadComplete;
    uint8_t failedPipe; // 0xff for a global/preflight failure.
    uint8_t poweredOffMask; // Valid only on Ok; caller must keep these pipes OFF.
    uint32_t readsAttempted, lastRegister;
    uint32_t lastValue; // Last callback's value, zero if that callback failed.
    bool lastReadSucceeded; // Includes all-ones; not a usable-device claim.
    PowerProbe lastPowerProbe; // Raw last probe, retained even on validation failure.
    size_t planeCount;
    Plane planes[kPipeCount];
    Snapshot snapshot; // Diagnostic first pass; may be partial on error.
};

// Full validated discovery; exact unharvested DMU HWID271 v3.2.0, die0/instance0,
// 32-bit DWORD bases. Generated DCN3.2 VGA registers use BASE_IDX1; DCN/HUBP
// registers and HUBP power-status registers use BASE_IDX2. Resolves and bounds
// every read before live access.
// No MMIO. Input and output must not overlap. out is zero on error;
// successful layout must remain immutable.
Status buildLayout(const uint8_t* discovery, size_t bytes, uint64_t barBytes,
                   Layout& out);

// Independent diagnostic inventory of always-on DOMAIN0..3 power controls.
// At most12 ordered reads: STATUS/CONFIG/STATUS for each pipe. Ok means only
// that every callback succeeded without all-ones; arbitrary/changed values
// remain raw evidence, not proof of a readable HUBP or any reservation. Stops
// globally on transport failure/all-ones and preserves per-read validity.
// No SOS/VGA/HUBP/global-window access and no register writes. Caller excludes
// reset/removal/PCI power changes and supplies readable always-on controls.
// Input/output/callback state must not overlap or change during this call.
Status inventoryPower(const Layout&, const Io&, PowerInventory& out);

// Read-only, allocation-free, bounded to798 reads (two fixed passes plus power
// checks). Caller guarantees powered/readable DCN always-on power controls, a
// stable initialized memory layout, bare-metal Navi31, and excludes device
// reset/power/removal for safe MMIO. A PGFSM/current+desired power-state check
// precedes EVERY HUBP-window read, with whole-device checks bracketing each
// pass and postchecks per pipe. Current PGFSM0 permits desired0 or desired1
// with CONFIG0/1 (GATE0 and either FORCEON value), matching Linux's current
// PGFSM power-on test. Raw status and CONFIG remain exact across guards and
// both passes. Non-on or unresolved status aborts
// before the corresponding window read; no power gate is changed. Observing
// OFF does not prove outstanding hubbub requests drained, so it also returns
// incomplete. Status samples do not lock hardware against a later power change.
// Diagnostic capture may observe a running display; two passes detect
// observed changes but cannot prove a coherent or durable snapshot. Even Ok
// may be used ONLY diagnostically unless the caller separately excludes all
// firmware/OS modesets, flips and other display programmers for the ENTIRE
// capture AND while consuming its reservation intervals. PCI open, matching
// passes and complete=true do not establish that exclusion or memory ownership.
// Input, output and callback state must not overlap or change during capture.
//
// Supports only linear, unrotated packed RGB with no alpha plane/DCC/TMZ,
// VMID0 and the Linux-programmed TLB0x19 MC framebuffer/system-aperture policy.
// Firmware TLB0 remains unsupported until its address domain is established.
// FB registers must exactly match the memory layout; an AGP window must be
// disabled or wholly disjoint from FB. Primary and earliest-in-use addresses
// must agree, with no pending/locked or stereo/GSL/triple-buffer flip. Ordinary
// INUSE is additionally compared only in NO_LATCH mode; its raw value must
// still be stable across passes. Fixed pending timing fields, 1D/2D linear
// dimensions and inactive DCC metadata-layout flags are allowed; 3D/reserved
// dimensions and all data swizzle/reserved bits are rejected. VGA, cursor and
// dynamic-metadata fetches are rejected. A blank
// HUBP is skipped only when no outstanding request is reported in both passes.
// IN_BLANK, VTG selection, TTU_DISABLE or zero dimensions alone do not establish
// that a powered, unblanked HUBP cannot fetch later; such pipes are not skipped.
// Unknown formats/modes or incomplete evidence return failure and no intervals.
//
// The reservation covers the current linear scanout footprint; it does not
// describe the framebuffer object's allocation size or all display/firmware
// memory. In particular this does not inventory DMCUB, writeback, MALL save
// areas, PSP, training buffers, or other engines. Excluding these ranges alone
// does not establish a free VRAM arena. No register is written or enabled.
Status capture(const Layout&, const MemoryLayout&, const Io&, Capture& out);
// Firmware first-attach variant. Requires exact MP0 discovery and actual zero
// SOS before/between/after passes; the separate BIOS-post check remains required.
// Completed OFF (PGFSM2, either desired bit, CONFIG gate1/forceon0) skips the entire
// gated HUBP/cursor/DMDATA window. Both passes and final power checks must agree.
// Linux DCN3.2 waits for this PGFSM encoding to complete power-off; this cold
// policy is not a drain proof for an earlier driver's outstanding requests.
// Caller excludes every power requester and keeps poweredOffMask OFF while
// using reservations. Unknown/transitioning/contrary power requests fail.
// Other capture limitations and the full-lifetime external exclusion apply.
Status capturePostedCold(const Layout&, const MemoryLayout&, const Io&, Capture& out);
const char* statusString(Status);
} // namespace bootdisplay
} // namespace hackynavi
#endif
