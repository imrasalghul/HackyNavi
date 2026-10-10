// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_GFX_QUEUE_HPP
#define HACKYNAVI_GFX_QUEUE_HPP
#include "hackynavi/gfx_init.hpp"
namespace hackynavi {
namespace gfxqueue {
enum class Status { Ok, InvalidArgument, InvalidState, UnsupportedLayout, OutsideBar,
    ActiveEngine, ReadFailed, WriteFailed, DeviceUnavailable, ReadbackMismatch,
    SyncFailed, PointerCorrupt, PendingSubmission, CounterExhausted, FenceTimeout, IdleTimeout };
enum class Phase { Fresh, Running, Faulted, Stopped };
enum class Stage { Preflight, Guard, Initialize, Program, Enable, Encode, Publish, FenceWait, Halt, Complete };
struct Config { uint32_t ringWords; uint64_t ringGpu,rptrGpu,wptrGpu,fenceGpu; };
struct Memory { uint32_t* ring; size_t capacityWords; volatile uint32_t* rptr;
    volatile uint64_t* wptr; volatile uint32_t* fence; };
struct Io { void* context; bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t);
    bool (*syncForDeviceAndHdp)(void*); bool (*syncForCpu)(void*);
    void (*delayUs)(void*,uint32_t); };
struct PollOptions { uint32_t maxPolls,delayUs; }; //1..1M,1..10; product<=1s
struct Result { Status status; Stage stage; uint32_t reads,writes,polls,lastRegister,lastValue,
    producerWords,observedRptr,fenceValue; bool retentionRequired,submissionMayHaveOccurred; };
struct Access;
class State {
public:
    State():phase_(Phase::Fresh),config_{},memory_{},regs_{},producer_(0),sequence_(0),pending_(false),retain_(false),startAttempted_(false) {}
    State(const State&)=delete; State& operator=(const State&)=delete;
    Phase phase() const { return phase_; } bool pending() const { return pending_; }
    bool needsRetention() const { return retain_; } uint32_t producerWords() const { return producer_; }
    // True only before any start hardware callback, with no producer, fence,
    // pending submission or retained queue bindings. Failed preflight leaves
    // this true; an attempted start never does. Read under the owner lock.
    // This is no CSIB/RLC, VM, hardware-idle or allocation-release authority.
    bool neverPublished() const;
    // Nonmutating preflight before publishing another VM. Requires the same
    // owner lock as submission; success grants no hardware/retirement proof.
    // The submission guard repeats this check before touching the real ring.
    Status submissionCapacity() const;
private:
    Phase phase_; Config config_; Memory memory_; uint32_t regs_[32],producer_,sequence_;
    bool pending_,retain_,startAttempted_; friend struct Access;
};
// Actual legacy GC11 pipe0 VMID0 queue, MMIO WPTR and no doorbell/interrupt.
// Requires prepared GC/CSB plus disabled-GFXOFF SMU on the same reset epoch.
// Kernel owner supplies actual pinned coherent x86 DMA mappings with active
// GART translations, CPU/GPU bindings matching Config, PCI BME and power held.
// Ring2048..16384DWORDs, power-of-two,256B GPU aligned. WB fields are disjoint,
// CPU aligned and GPU4/8/4B aligned. No allocation or CPU fallback exists here.
// Caller holds one sleepable lock and excludes all other GC selectors/queues,
// reset/PM/removal. CPU buffers and all API objects remain disjoint and alive.
// Device sync publishes ALL modified ring/WB/IB memory plus realHDP; CPU sync
// observes GPU fences/readbacks. Keep those bindings pinned until stop proves
// queue retirement or a separately verified reset retires them.
// start programs RB controls/writebacks/VMID/base, activates pipe0, unhalts
// ME/PFP, submits complete AMD clear-state PM4 + RELEASE_MEM fence, and waits
// for GPU-written fence plusRPTR before reporting Running. MEC stays halted.
Status start(const Navi31IpLayout&,uint64_t barBytes,const gfxinit::State&,
    const smu::State&,const Config&,const Memory&,const Io&,const PollOptions&,State&,Result&);
// Trusted, resident kernel-owned VMID0 IB only; not a userspace PM4 validator.
// Caller validates commands and every referenced allocation. Dword-aligned,
// 1..1048575DWORDs; does not set compute-only IB VALID on this graphics path.
Status submitIndirect(const Io&,State&,uint64_t ibGpu,uint32_t words,Result&);
// Trusted native capture path only: immutable validated IB in an owned VMID1..7
// page tree on both hubs, all other producers excluded. This primitive does
// NOT validate PM4 or establish residency; HackyNaviGraphicsQueue admits only
// its private canonical capture. Appends CS drain/PFP sync before real fence.
Status submitIsolatedIndirect(const Io&,State&,uint8_t vmid,uint64_t ibGpu,uint32_t words,Result&);
// Writes one owned GPU word using real CP WRITE_DATA, then cache-release fence.
Status submitWrite(const Io&,State&,uint64_t destinationGpu,uint32_t value,Result&);
// Single outstanding submission. Errors fault and retain; observation may be
// retried to retire a pending command, but never reopens a faulted queue.
Status waitFence(const Io&,State&,const PollOptions&,Result&);
// Observe the CURRENT final fence/RPTR, exact ring bindings, exclusive legacy
// producer controls and bounded CP idle. Performs no submission or halt. The
// native owner must also prove immutable per-VM sole-producer provenance; this
// observation alone never authorizes freeing a VM. Accepts Running or Stopped,
// never Faulted/pending. Rechecks DMA writeback after the hardware idle check.
Status checkIdleCompletion(const Io&,State&,const PollOptions&,Result&);
// Drain, halt ME/PFP, wait CP idle, deactivate RB0 and disable its doorbell.
// This retires queue buffers only; RLC CSB/TMR/firmware remain independently live.
Status stop(const Io&,State&,const PollOptions&,Result&);
const char* statusString(Status);
} // namespace gfxqueue
} // namespace hackynavi
#endif
