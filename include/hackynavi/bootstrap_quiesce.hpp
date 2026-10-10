// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_BOOTSTRAP_QUIESCE_HPP
#define HACKYNAVI_BOOTSTRAP_QUIESCE_HPP
#include "hackynavi/ip_layout.hpp"

namespace hackynavi {
namespace bootstrap {
enum class Status {
    Ok, InvalidArgument, UnsupportedLayout, OutsideBar, AliasedRegisters,
    ReadFailed, DeviceUnavailable, PriorDriverRequiresReset, ActiveVmContext,
    ActiveMes, ActiveSdmaQueue, UnsupportedArchitecture, WriteFailed,
    ControlReadbackMismatch, IdleTimeout, AtomPostRequired
};
enum class Stage {
    Preflight, BootstrapGuards, StopCp, StopSdma, VerifyControls,
    IdleWait, RecheckGuards, Complete, PostedState
};
struct Io {
    void* context;
    bool (*read32)(void*, uint32_t byteOffset, uint32_t& value);
    // false can mean that a write reached hardware before failure was detected.
    bool (*write32)(void*, uint32_t byteOffset, uint32_t value);
    void (*delayUs)(void*, uint32_t microseconds); // Includes one fixed 50us delay.
};
struct PollOptions { uint32_t maxPolls, delayUs; }; // 1..100000, 1..10us.
struct Result {
    Status status;
    Stage stage;
    uint32_t lastOffset, lastValue, readAttempts, writeAttempts, idlePolls;
    bool writesMayHaveOccurred, rs64;
    uint32_t biosScratch7, sosSignOfLife;
};

constexpr size_t kGuardInventoryWords = 77;
constexpr size_t kGuardInventoryMaskWords = (kGuardInventoryWords + 31) / 32;
enum class GuardInventoryStatus { Ok, InvalidArgument, LayoutRejected, AccessDenied,
    ReadFailed, DeviceUnavailable, PriorDriverRequiresReset };
struct GuardInventoryIo {
    void* context;
    bool (*valid)(void*); // Current native Operation/map/power lifetime, not a cached flag.
    bool (*read32)(void*, uint32_t byteOffset, uint32_t& value);
};
struct GuardInventory {
    GuardInventoryStatus status;
    Status layoutStatus;
    uint32_t offsets[kGuardInventoryWords], values[kGuardInventoryWords];
    uint32_t guardMasks[kGuardInventoryWords], guardExpected[kGuardInventoryWords];
    uint32_t attempted[kGuardInventoryMaskWords], readSucceeded[kGuardInventoryMaskWords];
    uint32_t valid[kGuardInventoryMaskWords], guardCompared[kGuardInventoryMaskWords];
    uint32_t guardMismatch[kGuardInventoryMaskWords];
    uint32_t reads, lastIndex, lastOffset, lastValue;
    bool lastReadSucceeded, complete, architectureCompared, architectureMatch, rs64;
    uint32_t sosAfter;
    bool sosAfterAttempted, sosAfterReadSucceeded, sosAfterValid;
};
// Bounded historical pre-control inventory using the same resolved CP/SDMA
// register table as quiesceCpSdma. Requires the same powered/awake/exclusive
// GC/SDMA/MMHUB/MP0 cold access contract below. valid is checked before/after
// each successful read. No writes, delays, polling or allocation occur.
// Initial nonzero SOS stops before GC/SDMA reads; final SOS rechecks cold state.
// Other semantic guard mismatches do not hide independently readable words.
// Ok/complete describes 78 valid reads, even with guardMismatch or mismatched
// architecture; it is NEVER admission, quiescence, ownership or stop proof.
// Order: SOS,ME,legacyMEC,RS64MEC,MEC_ISA,GFX_ARCH,MES,CP_STAT,GRBM_STATUS,
// GFX contexts0..15,MM contexts0..15, then each SDMA's queue0..7 RB/IB,F32,STATUS.
// Unattempted/callback-failed values remain zero. All-ones/post-read revocation
// keeps raw callback data with readSucceeded set and valid clear. Stops globally
// on those failures. First/final SOS mismatches have valid raw observations.
// Caller retains output off stack; all input/output/callback storage disjoint.
GuardInventoryStatus captureGuardInventory(const Navi31IpLayout&, uint64_t barBytes,
    const GuardInventoryIo&, GuardInventory&);
const char* guardInventoryStatusString(GuardInventoryStatus);

// Read-only Linux Navi31 first-attach decision: nonzero MP0 C2PMSG81 requires
// ASIC reset; otherwise BIOS scratch7 bit0x200 records completed ASIC_Init.
// biosScratchRegisterDword comes from the bounded FirmwareInfo3.4/3.5 decoder,
// not a CPU address. Validates both register offsets before any access and
// rechecks SOS after scratch7. Ok selects the posted cold path only; it proves
// neither GPU quiescence nor exclusive ownership and does not execute POST.
// PriorDriverRequiresReset / AtomPostRequired need their respective real
// hardware protocols before continuing. Caller excludes reset/PM/removal.
Status checkPostedColdState(const Navi31IpLayout&, uint64_t barBytes,
    uint32_t biosScratchRegisterDword, const Io&, Result&);

// A bounded, real register protocol for the CP and the two SDMA6 engines only.
// This is NOT whole-device quiescence, an ownership lease, or permission to
// replace GART. VCN/JPEG, LSDMA, display/DMUB, PSP/SMU and external actors must
// separately be excluded or quiesced. Caller owns the bare-metal Navi31, holds
// GC/SDMA/MMHUB/MP0 powered and awake, excludes every submission/doorbell,
// GRBM-index change, reset, removal and competing MMIO user for the entire call.
// PCI open and these status reads alone do not establish those prerequisites.
// Use the immutable layout built by buildNavi31IpLayout, ordered uncached MMIO,
// and a sleepable context. Layout, Io and result must not overlap.
//
// No reset, firmware loading, GRBM selection, queue programming or resume is
// attempted. All addresses are resolved before callbacks. Before any write,
// reject nonzero PSP sOS sign-of-life (Linux requires an ASIC reset on init),
// any enabled GFX/MM VM context0..15, a running/active MES, and enabled SDMA
// queues1..7. Mixed CP architectures are rejected. The selected MEC control is
// stopped using Linux's PFP/ME and legacy/RS64 MEC stop sequence; RS64 includes
// its four pipe-reset bits. SDMA queue0 RB/IB are disabled and both F32s halted.
// Checks halt/disable readback, CP_STAT==0, GRBM.GUI_ACTIVE==0 and both SDMA IDLE
// bits, then repeats guards and control readbacks. It never resumes old work.
//
// Ok means only this scoped protocol completed. It is not durable proof that
// other engines cannot access memory. A failure after writes leaves partial
// stop/reset state; retain affected ownership and use actual recovery, never
// assume rollback or automatically resume firmware. No allocation occurs.
Status quiesceCpSdma(const Navi31IpLayout&, uint64_t barBytes,
                    const Io&, const PollOptions&, Result&);
const char* statusString(Status);
} // namespace bootstrap
} // namespace hackynavi
#endif
