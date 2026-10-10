// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_KERNEL_GRAPHICS_QUEUE_HPP
#define HACKYNAVI_KERNEL_GRAPHICS_QUEUE_HPP
#include "HackyNaviFirmwareController.hpp"
#include "hackynavi/gfx_queue.hpp"
#include "hackynavi/gfx_compute.hpp"
#include "hackynavi/vm_context.hpp"
class HackyNaviVMContext;
class HackyNaviVMCompletion;

// Actual GC11 pipe0 kernel queue. This owner connects the completed PSP/SMU
// state to the same GART's real DMA mappings and owned RLC clear-state VRAM.
// Only trusted kernel PM4 may enter submitIndirect; this is not a user client.
// Hardware/allocation/submission entry points require the caller's live
// HackyNaviGARTController::Access, installed before descendant locks. Normal
// work requires Operation authority; cleanup accepts RetirementScope. Only
// CPU-only unpublished release is permitted without live hardware authority.

class HackyNaviGraphicsQueue : public OSObject {
    OSDeclareDefaultStructors(HackyNaviGraphicsQueue)
public:
    enum class State { Empty, Allocated, Prepared, Running, QuiescedRetained,
                       FailedUnpublished, FaultedRetained, Retired };
    struct WriteTestResult {
        bool completed;
        uint32_t wordsCompared, firstMismatch, expected, observed;
    };
    struct ShaderTestResult {
        bool completed;
        uint32_t dispatchesCompleted, wordsCompared, firstMismatch, expected, observed;
    };
    enum class ReuseStep : uint32_t { None, Allocate, Upload, MapFirst, CaptureFirst,
        IdentityFirst, SubmitFirst, ReadFirst, ConsumeFirst, IdentityConsumed,
        StaleFirst, RemapSecond, CaptureSecond, IdentitySecond, StaleSecond,
        SubmitSecond, ReadSecond, RetireSecond, Complete, StaleActiveSecond, VerifyReleased };
    struct ReuseTestResult {
        bool completed, dataOwnersPreserved, rootPreserved, retainedOnFailure;
        ReuseStep step;
        IOReturn result, cleanupResult;
        uint32_t jobsCompleted, wordsCompared, guardWordsCompared, staleRejections,
            controlsReused, firstMismatch, expected, observed;
        uint32_t firstFence, secondFence, firstRptr, secondRptr;
        uint64_t firstCapture, secondCapture;
        uint8_t vmid;
    };
    // Actual Apple-compiled add_one: A->B, consume, then the SAME GPU-written
    // B->C without CPU reupload. Caller holds this provider's normal Operation
    // and lexical GART::Access throughout; no userspace/test authority flags.
    // Uses real completions, retained DMA identity and final VM retirement.
    IOReturn runReuseTest(const HackyNaviFunctionGroup::Operation&,
        const hackynavi::gfxqueue::PollOptions&, ReuseTestResult&);
    static const char* reuseStepString(ReuseStep);
    // Requires completed firmware/SMU and enabled MMHUB+GFXHUB. Allocates a
    // distinct 4KiB CSB in the shared VRAM allocator; leases all Data/Writeback
    // storage exclusively. No GPU programming occurs in the factory.
    static HackyNaviGraphicsQueue* withFirmware(HackyNaviFirmwareController*, IOReturn&);
    IOReturn prepare();
    // Success requires the real GPU-written startup fence and consumed RPTR.
    IOReturn start(const hackynavi::gfxqueue::PollOptions&);
    // Offset is relative to the owned GART Data allocation, >=32KiB. Ring
    // [0,16KiB) and IB [16KiB,32KiB) are never writable through this method.
    IOReturn writeData(uint64_t offset, uint32_t value, const hackynavi::gfxqueue::PollOptions&);
    // Copies <=4096 DWORDs into pinned IB storage, publishes it and waits for
    // execution. Caller must validate EVERY packet and referenced allocation;
    // no untrusted/user input, imported address or asynchronous pointer escape.
    IOReturn submitIndirect(const uint32_t* commands, uint32_t words,
                            const hackynavi::gfxqueue::PollOptions&);
    // Only the native owner's sealed clear/copy or recorded Apple add_one
    // GFX1100 compiler capture is admitted, with its class-specific validator.
    // Uses real VMID1..7 IB VA, kernel-owned drain/cache-release fence and GPU
    // RPTR observation. Returns a retained readback/retirement completion on
    // success. Each immutable capture is exposed and submitted exactly once.
    // A consumed job permits a new capture in the same retained data arena;
    // no prior completion can authorize that capture or its retirement.
    // No userspace/raw PM4 endpoint, generic ACO shader or RADV admission.
    IOReturn submitIsolated(HackyNaviVMContext*, const hackynavi::gfxqueue::PollOptions&,
                           HackyNaviVMCompletion*&);
    IOReturn runWriteTest(const hackynavi::gfxqueue::PollOptions&, WriteTestResult&);
    // Actual GFX11 shader dispatches over owned Data ranges >=32KiB. Byte
    // sizes/offsets are16B multiples; Copy requires disjoint source/destination.
    // The immutable shaders reside in separately owned, verified-upload VRAM.
    IOReturn clearBuffer(uint64_t offset, uint32_t bytes, const uint32_t (&pattern)[4],
                         const hackynavi::gfxqueue::PollOptions&);
    IOReturn copyBuffer(uint64_t sourceOffset, uint64_t destinationOffset, uint32_t bytes,
                        const hackynavi::gfxqueue::PollOptions&);
    IOReturn runShaderTest(const hackynavi::gfxqueue::PollOptions&, ShaderTestResult&);
    hackynavi::gfxcompute::Status lastComputeStatus() const { return computeStatus_; }
    // Drains and halts this queue. RLC may still reference the CSB, so this
    // leaves all allocations and the firmware/GART lease pinned.
    IOReturn stop(const hackynavi::gfxqueue::PollOptions&);
    // After stop(), before retireAfterQuiescence(): reclaim unpublished VMs
    // and this queue's immutable, genuinely completed one-job captures. Old
    // completion tokens are unnecessary; private per-context provenance and
    // current stopped-ring fence/RPTR/idle observations are mandatory. An
    // incomplete/foreign/ambiguous exposed VM fails and remains retained.
    // A Prepared queue stopped before any start callback instead requires
    // the private never-started stop latch and an empty native VM registry;
    // its portable queue remains Fresh and no fence is manufactured.
    IOReturn retireCompletedContexts(const hackynavi::vmcontext::Poll&);
    // Requires stop() success. Performs terminal RLC safe-mode/stop and live
    // memory-transaction/SRM/FIFO drain, clears CSIB, then releases this owner.
    // Firmware readiness is revoked before teardown; a new queue requires a
    // new firmware/reset epoch. Ambiguous retirement retains all allocations.
    IOReturn retireAfterQuiescence(const hackynavi::gfxinit::RetirementPollOptions&);
    // Caller supplies separately verified device reset/power removal. This
    // method performs no reset and does not infer it from a stopped queue.
    IOReturn releaseAfterDeviceReset();
    State state() const { return state_; }
    bool requiresHardwareRetirement() const { return hardwareHold_; }
    const hackynavi::gfxinit::Result& lastInitResult() const { return initResult_; }
    const hackynavi::gfxqueue::Result& lastQueueResult() const { return queueResult_; }
protected:
    bool init() override;
    void free() override;
private:
    friend class HackyNaviVMCompletion;
    IOReturn readCompletion(const HackyNaviVMCompletion&, uint64_t relativeOffset,
                           void* kernelOutput, size_t bytes);
    IOReturn retireCompletion(HackyNaviVMCompletion&, const hackynavi::vmcontext::Poll&);
    IOReturn consumeCompletion(HackyNaviVMCompletion&, const hackynavi::vmcontext::Poll&);
    bool completionMatchesLocked(const HackyNaviVMCompletion&) const;
    bool completedContextLocked(const HackyNaviVMContext*, uint32_t, uint32_t) const;
    IOReturn retireContextLocked(HackyNaviVMContext*, const hackynavi::vmcontext::Poll&, unsigned&);
    IOReturn initialize(HackyNaviFirmwareController*);
    // Queue -> firmware -> GART, held for each whole hardware protocol. All
    // callbacks use already-locked storage and never re-enter these mutexes.
    void lockOwners();
    IOReturn finish(IOReturn);
    bool providerReady() const;
    // Caller holds Queue -> Firmware -> GART. Checks actual untouched queue,
    // owned prepared CSIB and empty healthy VM registry, not caller flags.
    bool neverStartedRetirementReadyLocked() const;
    void holdHardware();
    IOReturn fault();
    IOReturn releaseAllocationsLocked();
    IOReturn writeLocked(uint64_t, uint32_t, const hackynavi::gfxqueue::PollOptions&);
    IOReturn computeLocked(hackynavi::gfxcompute::Operation, uint64_t, uint64_t,
        uint32_t, const uint32_t*, const hackynavi::gfxqueue::PollOptions&);
    bool dataRange(uint64_t, uint64_t) const;
    static bool read32(void*, uint32_t, uint32_t&);
    static bool write32(void*, uint32_t, uint32_t);
    static bool publish(void*);
    static bool syncCpu(void*);
    static void delayUs(void*, uint32_t);
    hackynavi::gfxqueue::Io io();

    IOLock* lock_;
    HackyNaviFirmwareController* firmware_;
    HackyNaviGARTController* gart_;
    uint8_t* workspace_;
    uint8_t* dataCpu_;
    uint64_t dataBytes_, dataGpu_;
    bool storageLease_, hardwareHold_;
    // Set only by stop() from actual Prepared after all private guards pass.
    // This selects the no-submission cleanup path; it does not stop RLC.
    bool neverStartedRetirement_;
    // Generic kernel PM4 and isolated VM ownership cannot coexist in an epoch:
    // arbitrary PM4 can name VMIDs/engine controls. Canonical helpers remain
    // eligible because their exact grammar never admits caller commands.
    bool rawIndirectUsed_, isolatedClaimed_;
    State state_;
    hackynavi::VramAllocation clearState_;
    hackynavi::VramAllocation shader_;
    uint64_t shaderGpu_;
    bool shaderReady_;
    hackynavi::gfxcompute::Status computeStatus_;
    hackynavi::gfxinit::State initState_;
    hackynavi::gfxinit::Result initResult_;
    hackynavi::gfxqueue::Config config_;
    hackynavi::gfxqueue::Memory memory_;
    hackynavi::gfxqueue::State queue_;
    hackynavi::gfxqueue::Result queueResult_;
};
#endif
