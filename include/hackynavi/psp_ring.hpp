// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_PSP_RING_HPP
#define HACKYNAVI_PSP_RING_HPP
#include "hackynavi/psp.hpp"
#include "hackynavi/psp_ta.hpp"
namespace hackynavi {
namespace pspdisplayta { class State; struct Access; }
namespace pspring {
constexpr size_t kRingBytes=4096, kCommandBytes=1024, kScratchBytes=4096;
enum class Status { Ok, InvalidArgument, InvalidState, UnsupportedLayout, OutsideBar,
    InvalidAllocation, AliasedAllocation, TransferFailed, VisibilityFailed,
    ReadFailed, WriteFailed, DeviceUnavailable, SosUnavailable, MailboxTimeout,
    MailboxRejected, PointerCorrupt, PendingCommand, CounterExhausted,
    FenceTimeout, FenceCorrupt, CommandRejected, ResponseCorrupt, InvalidTmrSize };
enum class Stage { Preflight, Guard, Initialize, Publish, Create, MailboxWait,
    Encode, UploadCommand, UploadFrame, Submit, FenceWait, ReadResponse, Stop, Complete };
enum class Phase { Fresh, Running, Faulted, Stopped };
enum class TaPhase { Fresh, Loading, Loaded, Invoking, Unloading, FaultedRetained, Retired };
struct TaInformation {
    TaPhase phase; uint32_t sessionId,invocations;
    bool sessionValid,retained,loadAttempted,loadAcknowledged,unloadAttempted,unloadAcknowledged;
};
struct Layout {
    bool valid; uint64_t barBytes;
    uint32_t control,wptr,rptr,ringLo,ringHi,ringSize,heartbeat;
};
struct Buffers {
    VramAllocationHandle ring,command,fence; // Distinct owned >=4KiB slots.
    uint8_t* scratch; size_t scratchBytes; // >=4KiB writable CPU workspace.
};
struct Io {
    void* context;
    bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t); // False may have landed.
    bool (*flushHdp)(void*); // Actual prepared NBIO4.3 publication+posting read.
    // Supply acquire ordering before ordered uncached MM_INDEX reads. HDP6
    // has no Linux invalidate_hdp hook; do not invent an invalidation register.
    // Adapter must ensure this transport observes PSP writes, not cached bytes.
    bool (*beforeReadback)(void*);
    void (*delayUs)(void*,uint32_t);
    void (*delayMs)(void*,uint32_t);
};
struct PollOptions { uint32_t maxPolls,delayUs; }; // 1..1M,1..1000; product<=10s.
struct Result {
    Status status; Stage stage;
    uint32_t reads,writes,polls,lastRegister,lastValue;
    uint32_t commandId,fenceSequence,observedFence,responseStatus;
    uint32_t tmrBytes,bootConfig; uint64_t firmwareAddress;
    bool mailboxWritesMayHaveOccurred,submissionMayHaveOccurred;
    bool ringRetentionRequired,tmrRetentionRequired,taRetentionRequired;
    VramTransferResult transfer;
};
struct Access;
class State {
public:
    State();
    State(const State&)=delete; State& operator=(const State&)=delete;
    State(State&&)=delete; State& operator=(State&&)=delete;
    Phase phase() const { return phase_; }
    bool pending() const { return pending_; }
    bool needsRingRetention() const { return ringRetain_; }
    bool needsTmrRetention() const { return tmrRetain_; }
    bool tmrReady() const { return tmrReady_; }
    uint32_t requiredTmrBytes() const { return requiredTmr_; }
    uint32_t fenceSequence() const { return sequence_; }
    bool needsTaRetention() const;
    // Diagnostic copy only; no imported session ID or transferable authority.
    bool taInformation(pspta::Kind,TaInformation&) const;
private:
    struct TaRecord {
        TaInformation info;
        VramAllocationHandle shared,firmware;
        uint64_t sharedMc,firmwareMc,firmwareOffset;
        uint32_t firmwareBytes,commandId;
        const pspdisplayta::State* displayOwner;
    };
    Phase phase_; Layout layout_; const VramAllocator* allocator_; Buffers buffers_;
    uint64_t mc_[3]; uint32_t wptr_,sequence_,pendingId_,requiredTmr_;
    VramAllocationHandle tmr_,pendingInput_;
    bool pending_,ringRetain_,tmrRetain_,tmrReady_;
    TaRecord ta_[2]; uint8_t pendingTa_;
    friend struct Access;
    friend struct pspdisplayta::Access;
    // Typed display protocol alone can invoke while it owns the TA leases.
    Status submitDisplayInvoke(const Io&,pspta::Kind,uint32_t,const pspdisplayta::State&,Result&);
    bool displayBindingsValid() const;
};
Status buildLayout(const Navi31IpLayout&,uint64_t barBytes,Layout&);
// Requires this driver's successfully loaded cold SOS State, the same reset
// epoch/device and exclusive PSP rings/mailboxes/MM-index window. All three
// live allocations must be separate, GPU MC page-aligned and accessible to
// PSP VMID0. Uses actual owned VRAM transfers, no CPU-address-as-GPU assumption.
// Keeps bindings/workspace until verified ring destruction or independent reset.
Status create(const Layout&,const psp::State& loadedSos,const VramAllocator&,
              const Buffers&,const Io&,const PollOptions&,State&,Result&);
// Pure preflight for staging BEFORE modifying VRAM: same live allocator,
// page-aligned source, bounded range and no control/TMR alias. Requires Running
// with no pending command. The caller still owns publication and immutability.
Status validateInputAllocation(const State&,VramAllocationHandle,uint64_t offset,uint32_t bytes);
// Single-flight typed commands; zero1024-byte ABI with buf_size/version zero
// exactly as Linux GPCOM acquire/prep does, frame64bytes (cmd_buf_size also0).
// Input MC addresses are derived from live owned handles. LOAD inputs must
// already be authenticated-family-selected, uploaded/readback-verified and HDP
// visible; preserve bytes until wait returns Ok. These APIs do not stage input.
Status submitBootConfig(const Io&,State&,Result&); // BOOT_CFG id0x22, GET2.
Status submitLoadToc(const Io&,State&,VramAllocationHandle,uint64_t offset,uint32_t bytes,Result&);
// LOAD_TOC must have completed successfully with bounded nonzero TMR size.
// TMR allocation>=returnedsize,4KiB size,1MiB MC+GPU-physical aligned,<=64MiB.
// Both GPU MC and physical-page domains are encoded; never CPU PCI BAR address.
Status submitSetupTmr(const Io&,State&,VramAllocationHandle,Result&);
Status submitLoadIp(const Io&,State&,VramAllocationHandle,uint64_t offset,uint32_t bytes,
                    uint32_t firmwareType,Result&);
Status submitAutoloadRlc(const Io&,State&,Result&);
// Exact persistent-shared-buffer PSP LOAD_TA/INVOKE_CMD/UNLOAD_TA ABI. The
// caller must parse/select the real HDCP/DTM image, upload/readback-verify it,
// initialize the distinct 16KiB shared allocation and publish both before load.
// Firmware offset is page aligned (stage the parsed entry at such an offset).
// The shared allocation stays live and exclusive through confirmed unload;
// firmware staging also stays live until successful LOAD completion. These
// calls never adopt a caller-supplied session ID. Session ID zero is preserved.
// INVOKE's application command/shared input and returned application status
// need their own HDCP/DTM protocol validation. PSP response success alone does
// not prove application-command success, authentication or inherited exclusion.
Status submitLoadTa(const Io&,State&,pspta::Kind,VramAllocationHandle firmware,
    uint64_t offset,uint32_t bytes,VramAllocationHandle shared,Result&);
Status submitInvokeTa(const Io&,State&,pspta::Kind,uint32_t applicationCommand,Result&);
// Caller quiesces every user of this owned TA/shared buffer first. No new
// sessions after unload: this bounded ring owner loads each kind at most once.
Status submitUnloadTa(const Io&,State&,pspta::Kind,Result&);
// Caller must first quiesce every GPU user of TMR/firmware. A successful PSP
// DESTROY_TMR response alone does not establish those external users retired.
// Rejected while either owned TA is retained, including ambiguous failures.
Status submitDestroyTmr(const Io&,State&,Result&);
// Polls actual VRAM fence, reads command response, checkszero status. Unlike
// Linux's warning-only bare-metal path, a nonzero response is a hard failure.
// Timeout/error seals submission; wait may retry observation, never resubmit.
// Completed rejected commands remain faulted and retain TMR conservatively.
// Nonzero LOAD_IP_FW response addresses must lie inside the owned TMR. Zero is
// preserved (some firmware commands have no executable address); consumers
// requiring a code address must reject zero before programming an engine.
Status wait(const Io&,State&,const PollOptions&,Result&);
// Drain pending command first; then real DESTROY_RINGS0x30000+20ms+ACK. It
// retires ring/cmd/fence bindings only. Existing TMR/loaded engines stay live!
// Does not free allocations or clear TMR retention, even after successful stop.
// Cannot destroy the ring while any TA/shared-buffer ownership remains.
Status stop(const Io&,State&,const PollOptions&,Result&);
// All calls serialized by caller; device powered/posted, HDP prepared, reset/
// suspend/removal and all other PSP/MM-window actors excluded throughout.
// Objects, workspace and source allocations do not alias. No callback mutates
// bindings/allocator. Errors may leave hardware writes; retain resources for
// explicit recovery. Reconstruct State only after verified new reset epoch.
const char* statusString(Status);
} // namespace pspring
} // namespace hackynavi
#endif
