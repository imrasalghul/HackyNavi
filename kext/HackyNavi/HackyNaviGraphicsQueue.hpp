// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_KERNEL_GRAPHICS_QUEUE_HPP
#define HACKYNAVI_KERNEL_GRAPHICS_QUEUE_HPP
#include "HackyNaviFirmwareController.hpp"
#include "hackynavi/gfx_queue.hpp"
#include "hackynavi/gfx_compute.hpp"

// Actual GC11 pipe0 kernel queue. This owner connects the completed PSP/SMU
// state to the same GART's real DMA mappings and owned RLC clear-state VRAM.
// Only trusted kernel PM4 may enter submitIndirect; this is not a user client.
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
    IOReturn initialize(HackyNaviFirmwareController*);
    // Queue -> firmware -> GART, held for each whole hardware protocol. All
    // callbacks use already-locked storage and never re-enter these mutexes.
    void lockOwners();
    IOReturn finish(IOReturn);
    bool providerReady() const;
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
