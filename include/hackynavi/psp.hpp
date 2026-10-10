// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_PSP_HPP
#define HACKYNAVI_PSP_HPP
#include "hackynavi/firmware.hpp"
#include "hackynavi/ip_layout.hpp"
#include "hackynavi/vram_transfer.hpp"

namespace hackynavi {
namespace psp {
constexpr size_t kStagingBytes = 1024U * 1024U;
constexpr size_t kMaxBootComponents = 9;
enum class Status {
    Ok, AlreadyRunning, InvalidArgument, InvalidState, UnsupportedLayout,
    OutsideBar, InvalidFirmware, UnsupportedFirmware, InvalidAllocation,
    ReadFailed, WriteFailed, DeviceUnavailable, BootloaderTimeout,
    HeartbeatTimeout, UnexpectedSos, UploadFailed, HdpFlushFailed
};
enum class Stage {
    Preflight, InitialSos, BootloaderReady, Upload, HdpFlush, PublishAddress,
    IssueCommand, BootloaderComplete, SosDelay, SosBaseline, SosHeartbeat,
    SosVersion, Complete
};
enum class Phase { Fresh, Running, Loaded, AlreadyRunning, Faulted };
struct Layout {
    bool valid;
    uint64_t barBytes;
    uint32_t command, address, heartbeat, version; // BAR BYTE offsets.
};
struct Component {
    uint32_t firmwareType; // Command purpose, Linux PSP firmware type enum.
    uint32_t sourceType;   // SPL deliberately uses KDB bytes on this source path.
    uint32_t command;
    FirmwareRange image;
};
struct SosImage {
    uint32_t count, sosVersion;
    Component components[kMaxBootComponents]; // Linux boot order, SOS last.
    FirmwareRange toc, registerList; // Used only by later KM-ring commands.
};
struct Io {
    void* context;
    // Ordered uncached native accesses. False writes may still reach hardware.
    bool (*read32)(void*, uint32_t byteOffset, uint32_t&);
    bool (*write32)(void*, uint32_t byteOffset, uint32_t);
    // Perform the prepared NBIO4.3 HDP flush/posting read, not just a CPU fence.
    bool (*flushHdp)(void*);
    void (*delayUs)(void*, uint32_t);
    void (*delayMs)(void*, uint32_t); // At least the requested interval.
};
struct Options {
    uint32_t bootloaderPolls, heartbeatPolls; // Each 1..1000000.
    uint32_t pollDelayUs; // 1..1000; each count*delay <=10000000 us.
};
struct Result {
    Status status;
    Stage stage;
    uint32_t componentIndex, componentType, sourceType, commandValue;
    uint32_t completedComponents, reads, writes, polls, lastRegister, lastValue;
    uint32_t baselineHeartbeat, observedHeartbeat, sosVersion;
    uint64_t stagingMcAddress;
    bool stagingWritesMayHaveOccurred, mailboxWritesMayHaveOccurred;
    bool commandMayHaveOccurred, sosCommandMayHaveOccurred;
    VramTransferResult transfer; // Last upload, including partial progress.
};
class State {
public:
    State() : phase_(Phase::Fresh) {}
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;
    Phase phase() const { return phase_; }
private:
    Phase phase_;
    friend struct Access;
};

// Pure validation. Exact MP0 13.0.0 on a valid Navi31 IP layout only. The
// layout must refer to the same live device as the allocator and callbacks.
Status buildLayout(const Navi31IpLayout&, uint64_t barBytes, Layout&);
// Exact PSP container2.0/IP13.0; require SYS+SOS, reject duplicate/overlapping,
// zero-sized and unknown components, and unsupported IPKEYMGR. TOC/RL are
// retained for subsequent ring setup, not bootloader commands. This is bounds
// and compatibility validation, not signature verification/authentication.
Status parseSosImage(const uint8_t*, size_t, SosImage&);

// Actual cold bootloader protocol for Navi31. Entire firmware/argument/range
// validation precedes callbacks. Uses the live allocator handle, not a caller
// invented MC address. Allocation must cover1MiB and its MC address must be
// 1MiB aligned. Each component is copied to CPU workspace, zero-padded to1MiB,
// uploaded+readback-verified with MM_INDEX/HI/DATA, HDP-flushed, and submitted
// using C2PMSG36=MC>>20 then C2PMSG35=command. Reuse occurs only after exact
// C2PMSG35==0x80000000 completion. SOS uses a20ms delay followed by a heartbeat
// baseline and a subsequent nonzero change, then reads its running version.
// Pinned Linux (also v6.6/v6.12) sends KDB bytes for the SPL command when SPL is
// present. We expose this as sourceType=3; do not substitute PSP14 behavior.
//
// Caller provides trusted family-selected firmware and >=1MiB CPU workspace.
// All objects, buffers and callback state are non-overlapping; firmware stays
// immutable. Workspace must be writable and is modified after preflight.
// Caller exclusively owns PSP mailbox, MM-index window and allocation for the
// full call; no callbacks may mutate allocator/layout or recurse. Maintain
// D0, posted valid VRAM/MC configuration, prepared HDP remap, power/reset/removal
// exclusion, and prior firmware/training/scanout reservations. This routine
// does not establish those prerequisites or authenticate firmware on the CPU.
//
// A nonzero initial heartbeat returns AlreadyRunning without hardware writes,
// seals State, and MUST NOT be treated as this firmware successfully loaded.
// Establish/reset the prior owner's state before cold boot. Every hardware
// failure seals State as Faulted. Retain staging and all affected ownership on
// failure; no rollback, retry, reset, freeing, or DMA retirement is implied.
// Reconstruct State only after independently establishing a fresh hardware
// reset epoch. Even an ambiguous SOS command write waits20ms before returning.
// Ok only establishes this SOS boot protocol completed; KM ring, TMR, TOC/RL,
// trusted SDMA firmware loading and queue execution are subsequent operations.
Status loadSos(const Layout&, const VramAllocator&, VramAllocationHandle,
    const uint8_t* firmware, size_t firmwareBytes,
    uint8_t* workspace, size_t workspaceBytes, const Io&, const Options&,
    State&, Result&);
const char* statusString(Status);
} // namespace psp
} // namespace hackynavi
#endif
