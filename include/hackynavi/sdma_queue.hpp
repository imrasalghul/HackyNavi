// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_SDMA_QUEUE_HPP
#define HACKYNAVI_SDMA_QUEUE_HPP
#include "hackynavi/ip_layout.hpp"

namespace hackynavi {
namespace sdmaqueue {
enum class Status {
    Ok, InvalidArgument, InvalidState, UnsupportedLayout, OutsideBar,
    AliasedRegisters, ReadFailed, WriteFailed, DeviceUnavailable,
    EngineNotStopped, OtherQueueActive, UnsupportedQueueMode,
    ReadbackMismatch, SyncFailed, PendingSubmission, PointerCorrupt,
    CounterExhausted, FenceTimeout, IdleTimeout
};
enum class Stage {
    Preflight, Guards, InitializeMemory, ProgramRing, VerifyRing,
    Enable, Encode, Publish, FenceWait, Drain, Stop, VerifyStop, Complete
};
enum class Phase { Empty, Running, Faulted, Stopped };
struct Config {
    uint32_t engine; // 0 or 1, queue0 only
    uint32_t ringWords; // power of two, 256..16384 DWORDs
    uint32_t watchdog100ms; // 1..255, Linux QUEUE_HANG_COUNT units
    uint64_t ringGpuAddress, rptrGpuAddress, wptrGpuAddress, fenceGpuAddress;
};
struct Memory {
    uint32_t* ring;
    size_t capacityWords;
    volatile uint64_t* rptr;
    volatile uint64_t* wptr;
    volatile uint32_t* fence;
};
struct Io {
    void* context;
    bool (*read32)(void*, uint32_t byteOffset, uint32_t&);
    bool (*write32)(void*, uint32_t byteOffset, uint32_t);
    // Synchronize ALL CPU-modified ring/writeback/transfer/IB DMA allocations
    // for the device, issue the required CPU release/full fence, then actual
    // HDP publication. Called before initial enable and each WPTR publication.
    bool (*syncForDeviceAndHdp)(void*);
    // Synchronize DMA writebacks AND transfer destinations for CPU access and
    // provide acquire ordering before the following volatile aligned loads.
    bool (*syncForCpu)(void*);
    void (*delayUs)(void*, uint32_t);
};
struct PollOptions { uint32_t maxPolls, delayUs; }; // 1..1000000,1..10; product<=1000000us
struct Result {
    Status status; Stage stage;
    uint32_t lastOffset, lastValue, reads, writes, polls, fenceValue;
    uint64_t producerBytes, observedRptrBytes;
    bool hardwareMayHaveChanged, submissionMayHaveOccurred, retentionRequired;
};
struct Access;
class State {
public:
    State() : phase_(Phase::Empty), config_{}, memory_{}, regs_{},
        producerBytes_(0), lastRptrBytes_(0), fenceSequence_(0), pending_(false), retain_(false) {}
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    Phase phase() const { return phase_; }
    bool pending() const { return pending_; }
    bool needsRetention() const { return retain_; }
    uint64_t producerBytes() const { return producerBytes_; }
    uint32_t fenceSequence() const { return fenceSequence_; }
    // CPU bookkeeping only, after independent actual device reset/retirement.
    // It does not halt hardware or provide proof that allocations can be freed.
    void clearAfterVerifiedReset();
private:
    Phase phase_; Config config_; Memory memory_;
    uint32_t regs_[40];
    uint64_t producerBytes_, lastRptrBytes_;
    uint32_t fenceSequence_;
    bool pending_, retain_;
    friend struct Access;
};

// Real SDMA6 queue0 protocol, bare-metal Navi31/GC11.0.0 only. Caller owns the
// selected ENGINE (including queues1..7), excludes all submissions/doorbells/
// reset/power/removal/MMIO races, and has actually loaded compatible SDMA
// firmware. Maintain powered GC/SDMA and active VMID0 GART/TLB mappings for all
// supplied GPU virtual addresses, PCI memory decode+BME, and actual DMA memory
// ownership. GPU addresses are NOT CPU physical addresses or raw DMA addresses.
// The immutable layout and these declarations alone establish none of this.
//
// Memory uses little-endian coherent x86_64 mappings: aligned 64-bit volatile
// RPTR/WPTR accesses are single-copy atomic. CPU ring and writeback ranges must
// be disjoint and agree with the GPU mappings. Keep them alive and immutable
// as bindings until stop succeeds or independent reset proves retirement.
// Io/Config/Memory/State/Result must not overlap these allocations or each other.
// All calls require one external sleepable serialization lock.
//
// start requires halted/idle engine, disabled queue0 RB+IB, queues1..7 disabled,
// and no inherited AQL/preempt/freeze mode. It zeroes the real ring and shadows,
// programs Linux's BASE/size/writeback/cache/watchdog controls, disables this
// queue's doorbell, unhalts firmware, and enables RB then IB with readback.
// MMIO WPTR mode is implemented; no NBIO route, interrupt, scheduling/preemption
// or firmware upload is claimed. WPTR shadow remains zero in this mode, matching
// Linux's non-doorbell submission path. Successful start still needs a real
// submitted fence/copy test to establish functioning firmware/execution.
Status start(const Navi31IpLayout&, uint64_t barBytes, const Config&,
             const Memory&, const Io&, State&, Result&);

// One outstanding submission. Callers must own/pin/map every source/destination
// range until completion; numeric address validation cannot prove residency.
// Reject control-buffer overlap. Each batch appends Linux's whole-cache GCR,
// an uncached 32-bit fence (no trap), and NOPs to a16-DWORD fetch boundary.
// CPU uploads/HDP precede MMIO WPTR low then high. Counter wrap is refused.
Status submitCopy(const Io&, State&, uint64_t sourceGpu, uint64_t destinationGpu,
                  uint32_t bytes, Result&);
Status submitFill(const Io&, State&, uint64_t destinationGpu, uint32_t value,
                  uint32_t bytes, Result&);
// Trusted, owned VMID0 IB only:32-byte base alignment,8-DWORD padded length,
// 8..65536DWORDs. Caller validates its commands and every referenced allocation;
// this is not a public user-command validator. CSA is zero as Linux uses for
// VMID0. The6-DWORD INDIRECT packet is aligned to end on an8-DWORD boundary.
Status submitIndirect(const Io&, State&, uint64_t ibGpu, uint32_t lengthWords, Result&);
// Polls actual CPU-visible fence and64-bit RPTR after DMA synchronization. Both
// must reach this submission before it is complete; no simulated interrupt.
// Timeout/error faults state and retains memory; wait/stop can retry draining.
Status waitFence(const Io&, State&, const PollOptions&, Result&);
// Drains pending fence/RPTR first, checks all other queues still disabled,
// disables queue0 RB then IB, disables its doorbell and halts the engine;
// verifies controls and IDLE. Only success clears needsRetention(). Failures
// leave partial hardware state and retain every allocation; never auto-resume.
Status stop(const Io&, State&, const PollOptions&, Result&);
const char* statusString(Status);
} // namespace sdmaqueue
} // namespace hackynavi
#endif
