// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_EARLY_MEDIA_HPP
#define HACKYNAVI_EARLY_MEDIA_HPP
#include "hackynavi/bootstrap_quiesce.hpp"
#include "hackynavi/cold_aux.hpp"

namespace hackynavi { namespace earlymedia {
enum class Status { Ok,InvalidArgument,InvalidState,UnsupportedLayout,OutsideBar,
    OwnerUnavailable,OwnerRevoked,OwnerHoldFailed,ClockFailed,ClockRegressed,
    DeadlineExpired,ReadFailed,WriteFailed,DeviceUnavailable,FirmwareNotRunning,
    PostedStateRejected,ResponseTimeout,CommandFailed,UnknownCommand,
    BadPrerequisite,Busy,UnknownResponse,InvalidVersion,AuxiliaryNotStopped };
enum class Stage { Preflight,Owner,PostedState,FirmwareFlags,PreviousResponse,
    ClearResponse,Argument,Message,Response,Output,Auxiliary,Complete };
enum class Phase { Fresh,Preparing,Stopped,Faulted };
enum class Operation { Prepare,Verify };
struct Binding { uint64_t ownerInstance,powerGeneration,resetGeneration; };
// Identity records select a real retained native owner; they are not authority.
struct Limits { uint32_t totalTimeoutUs,responseTimeoutUs,maxPolls,pollDelayUs; };
struct Registers { uint32_t message,argument,response,firmwareFlagsSmn; };
struct Access;
class Layout {
public:
    Layout():valid_(false),barBytes_(0),scratchDwords_(0),ips_{},aux_{},regs_{} {}
    bool valid() const { return valid_; }
    Registers registers() const { return regs_; }
    const coldaux::Layout& auxiliary() const { return aux_; }
private:
    bool valid_; uint64_t barBytes_; uint32_t scratchDwords_;
    Navi31IpLayout ips_; coldaux::Layout aux_; Registers regs_;
    friend struct Access;
};
struct Io {
    void* context;
    // Enter the actual shared PCI/PM/reset/firmware/memory gate. Prepare is
    // admissible only before any new allocation/translation: preserve all
    // inherited VRAM/system-memory ownership while a media domain can wake.
    // Also exclude ALL MP1 mailbox and PCIe/SMN selector users. No cached
    // epoch flag may substitute for acquiring and revalidating this owner.
    bool (*enter)(void*,const Binding&,Operation);
    void (*leave)(void*);
    bool (*valid)(void*,const Binding&); // Revocation check around every I/O.
    // Persistently retain the real native owner/provider/map before selector
    // or mailbox writes. Deterministic, CPU-only contract: true means that
    // the hold actually exists; false means no new hold and no hardware I/O.
    // Leave unlocks only; it must not release this hold.
    bool (*holdOwner)(void*);
    bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t); // False may have reached hardware.
    // Actual serialized RREG32_PCIE/SMN transaction, not a cached flag. The
    // adapter must itself check revocation before each selector/data access.
    bool (*readSmn32)(void*,uint32_t,uint32_t&);
    bool (*nowUs)(void*,uint64_t&); // Monotonic, bounded CPU-only clock read.
    void (*delayUs)(void*,uint32_t);
};
struct Result {
    Status status; Stage stage;
    uint32_t reads,smnReads,writes,polls,lastRegister,lastValue,message,argument,response;
    uint32_t firmwareFlags,smuVersion,interfaceVersion;
    uint8_t attemptedDomains,acknowledgedDomains; // VCN0,VCN1,JPEG0 bits0..2.
    bool ownerHeld,mailboxTouched,indirectReadAttempted;
    bootstrap::Result posted; coldaux::Result auxiliary;
};
class State {
public:
    State():phase_(Phase::Fresh),layout_{},binding_{},context_(nullptr),ownerHeld_(false),
        touched_(false),indirect_(false),attempted_(0),acknowledged_(0),smuVersion_(0),interfaceVersion_(0) {}
    State(const State&)=delete; State& operator=(const State&)=delete;
    State(State&&)=delete; State& operator=(State&&)=delete;
    Phase phase() const { return phase_; }
    bool requiresOwnerRetention() const { return ownerHeld_; }
    uint8_t attemptedDomains() const { return attempted_; }
private:
    Phase phase_; Layout layout_; Binding binding_; void* context_;
    bool ownerHeld_,touched_,indirect_; uint8_t attempted_,acknowledged_;
    uint32_t smuVersion_,interfaceVersion_;
    friend struct Access;
};
Status buildLayout(const uint8_t* discovery,size_t bytes,uint64_t barBytes,
                   uint32_t biosScratchRegisterDword,Layout&);
// BIOS-running SMU bridge before PSP loading. This pre-PSP ordering is port
// policy: Linux's media start normally follows PSP. Neither the shared gate,
// PCI open, zero sOS nor BIOS scratch proves inherited memory is preserved.
// A native pre-PSP adapter has NOT been implemented and must not admit this
// operation until it enforces host DMA exclusion and preserves GPU-local and
// firmware-owned memory across a possible wake. BME-off alone does not stop
// internal VRAM accesses or preserve host memory mappings/queued requests.
// Check posted cold state and
// MP1_FIRMWARE_FLAGS.INTERRUPTS_ENABLED, then perform real V1 mailbox version
// queries and PowerUpVcn(0), PowerUpVcn(1<<16), PowerUpJpeg(0). Linux's first
// SMU_FW_INIT query skips the pre-poll once; all later commands require the
// previous successful response. Every reply and the complete call is bounded.
// Finally require the unchanged full coldaux stop/drain observation. Neither
// a power-command ACK nor an unreadable PGFSM is admission or DMA retirement.
//
// This does not issue static tile power requests, reset VCN/JPEG, resume rings,
// upload firmware, claim memory, or replace translation. An inherited active
// engine still fails the stop check. A possibly reached command is never
// silently rolled back. Failure seals State and preserves any acquired owner
// hold (preflight/hold-acquisition failures do not claim a nonexistent hold);
// recovery belongs to the enclosing verified reset/containment controller.
// Success retains the same owner while media remain unused/stopped. Replacing
// SMU firmware/resetting invalidates this state; no general runtime power API.
Status prepare(const Layout&,const Binding&,const Io&,const Limits&,State&,Result&);
// Fresh observation in the same powered/reset epoch and gate. SMN selector
// access still occurs, but no firmware power/mailbox command is sent.
// No claim survives an intervening SMU reload, reset, PM revocation or owner
// change. Only the native owner can transfer/drop its hold after real verified
// retirement; destroying this CPU state does not release hardware ownership.
Status verify(const Io&,const Limits&,State&,Result&);
const char* statusString(Status);
} }
#endif
