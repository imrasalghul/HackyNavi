// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_GART_HPP
#define HACKYNAVI_GART_HPP
#include "hackynavi/ip_layout.hpp"
#include "hackynavi/memory_layout.hpp"

namespace hackynavi {
namespace gart {

enum class Hub { Gfx, Mm };
enum class Status {
    Ok, InvalidArgument, UnsupportedLayout, OutsideBar, PlanCapacityExceeded,
    ReadFailed, DeviceUnavailable, WriteFailed, HdpFlushFailed,
    SemaphoreTimeout, InvalidateTimeout, ReadbackMismatch, PlanConsumed
};
enum class Stage {
    Preflight, Registers, HdpVisibility, SemaphoreAcquire, InvalidateRequest,
    InvalidateAck, SemaphoreRelease, PrivateInvalidate, PostingRead, Complete
};
enum class StepKind { Write, FlushHdp, Invalidate, PostingRead };
struct Step {
    StepKind kind;
    uint32_t byteOffset, value;
    uint32_t readMask; // PostingRead compares only these bits; ignored for writes.
};
constexpr size_t kMaxSteps = 128;
constexpr uint64_t kDefaultApertureBytes = 512ULL << 20;
constexpr uint64_t kMaxApertureBytes = 4ULL << 30;

struct Io {
    void* context;
    // Ordered, native 32-bit MMIO at BAR BYTE offsets. false may still mean
    // the access reached hardware. UINT32_MAX reads are treated as unavailable.
    bool (*read32)(void*, uint32_t byteOffset, uint32_t& value);
    bool (*write32)(void*, uint32_t byteOffset, uint32_t value);
    void (*delayUs)(void*, uint32_t microseconds);
    // REQUIRED: perform the initialized Navi31 HDP flush including its posting
    // read, and report completion/failure. A CPU fence alone is insufficient.
    // The owner must also finish CPU table upload/DMA synchronization beforehand.
    bool (*flushHdp)(void*);
};
struct PollOptions { uint32_t maxPolls, delayUs; }; // 1..100000 and 1..10 us.
struct Config {
    MemoryLayout memory;
    uint64_t tablePhysicalAddress; // Owned contiguous VRAM leaf table, final PA.
    uint64_t tableBytes;           // 4 KiB multiple, covers aperturePages*8.
    uint64_t apertureStart;        // GPU MC/VMID0 address, 4 GiB aligned.
    uint64_t apertureBytes;        // 4 KiB multiple, 4 KiB..4 GiB.
    uint64_t scratchPhysicalAddress; // Separate owned 4 KiB VRAM default page.
    uint64_t dummyDmaAddress;        // Owned snooped system page, 44-bit DMA PA.
    bool tableSnooped;               // Match the VRAM table's actual cache policy.
};
struct Result {
    Status status;
    Stage stage;
    uint32_t lastOffset, lastValue;
    uint32_t readAttempts, writeAttempts, semaphorePolls, ackPolls;
    bool writesMayHaveOccurred; // Includes possible writes inside flushHdp.
    bool semaphoreMayBeHeld;   // A failed semaphore read can have acquired it.
};

struct Builder;
class Plan {
public:
    Plan();
    Plan(const Plan&) = delete;
    Plan& operator=(const Plan&) = delete;
    Plan(Plan&&) = delete;
    Plan& operator=(Plan&&) = delete;
    bool valid() const { return valid_; }
    bool consumed() const { return consumed_; }
    size_t stepCount() const { return count_; }
    const Step* step(size_t index) const { return index < count_ ? &steps_[index] : nullptr; }
    Hub hub() const { return hub_; }
private:
    friend struct Builder;
    friend Status executePlan(Plan&, const Io&, const PollOptions&, Result&);
    void reset();
    Step steps_[kMaxSteps];
    size_t count_;
    Hub hub_;
    uint32_t semaphore_, request_, ack_, privateInvalidate_, privateValue_;
    bool valid_, consumed_;
};

// Preparation performs bounded ordinary register reads only; every address and
// requested value is validated and all RMW values are captured before writes.
// On failure plan is invalid/empty. Source memory, plan and result must not alias.
// The same owner must keep the register state stable from preparation through
// execution: these plans are not durable snapshots or a security boundary.
//
// This is the bare-metal Navi31 GC11.0.0/MMHUB3.0.0, VMID0 flat-4KiB-table path.
// It explicitly disables contexts0..15 before enable, then enables only0. AGP
// and identity apertures are disabled, translate-further is off. The aperture
// stays below Linux's 48-bit VA hole and outside the framebuffer window. Faults
// redirect to the owned dummy/default pages; no interrupt handler is installed.
//
// Caller prerequisites: exclusive device/register/invalidation-engine17 access;
// all GPU clients quiesced; bare metal (no SR-IOV/XGMI); powered GFX held awake;
// no KIQ/MES/GFXOFF; coherent initialized memory controller/HDP/firmware; owned,
// resident and uploaded table/scratch/dummy allocations. The entire prepare +
// execute interval must be serialized and must permit the delay callback.
// PCI open alone does not establish these hardware prerequisites. MMHUB is
// normally enabled during GMC init; GFXHUB is enabled later during GFX init.
Status prepareEnable(const Navi31IpLayout&, uint64_t barBytes, Hub,
                     const Config&, const Io&, Plan&, Result&);
Status prepareDisable(const Navi31IpLayout&, uint64_t barBytes, Hub,
                      const Io&, Plan&, Result&);
// Flush/disable explicitly restore engine17's full invalidation range. Disable
// invalidates while caches still operate, then disables contexts and L1/L2.
// It verifies all16 contexts are zero, L1 enable/advanced-model and L2 enable
// bits are clear, and L2_CNTL3 is zero before returning Ok.
Status prepareFlush(const Navi31IpLayout&, uint64_t barBytes, Hub,
                    const Io&, Plan&, Result&);

// A validated plan is single-use. The first execution consumes it even if a
// callback fails. This is NOT an atomic hardware transaction: failure requires
// the owner to keep resources resident and quiesce/reset, not free/reuse them or
// assume rollback. Known-held semaphores get a best-effort release on failure;
// uncertainty is reported separately. Ok confirms this register protocol only,
// not working DMA, graphics, compute, ownership or correct table contents.
Status executePlan(Plan&, const Io&, const PollOptions&, Result&);
const char* statusString(Status);

} // namespace gart
} // namespace hackynavi
#endif
