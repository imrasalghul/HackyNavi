// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

namespace hackynavi { namespace compute { namespace command {
// Values match the public MTLCommandBufferStatus enumeration. These are host
// object states, never GPU completion or native retirement authority.
enum class Phase : uint32_t {
    NotEnqueued = 0, Enqueued = 1, Committed = 2,
    Scheduled = 3, Completed = 4, Error = 5
};
class Scheduler;
struct Ticket {
    uint32_t slot = UINT32_MAX;
    uint64_t generation = 0;
    bool operator==(const Ticket& other) const noexcept {
        return slot == other.slot && generation == other.generation && owner_ == other.owner_;
    }
    bool operator!=(const Ticket& other) const noexcept { return !(*this == other); }
private:
    friend class Scheduler;
    const Scheduler* owner_ = nullptr;
};

// The actual command buffer owns this at a stable address through callback
// completion, retaining the original Scheduler for that lifetime. Final state
// remains inspectable after the execution slot is released or canceled.
// All reads and writes, including these getters, require the queue's mutex.
class Lifecycle final {
public:
    Lifecycle() = default;
    Lifecycle(const Lifecycle&) = delete;
    Lifecycle& operator=(const Lifecycle&) = delete;
    Phase phase() const noexcept { return phase_; }
    bool mutableEncoding() const noexcept { return phase_ == Phase::NotEnqueued || phase_ == Phase::Enqueued; }
    bool terminal() const noexcept { return phase_ == Phase::Completed || phase_ == Phase::Error; }
    bool scheduledReady() const noexcept {
        return phase_ == Phase::Error ||
            ((phase_ == Phase::Scheduled || phase_ == Phase::Completed) && scheduledCallbacksDone_);
    }
    bool completedReady() const noexcept { return terminal() && callbacksDone_; }
    bool active() const noexcept { return active_; }
private:
    friend class Scheduler;
    Phase phase_ = Phase::NotEnqueued;
    bool bound_ = false, active_ = false, scheduledCallbacksDone_ = false, callbacksDone_ = false;
    const Scheduler* owner_ = nullptr;
};

// Allocation-free, bounded host admission/order bookkeeping used by the real
// ObjC/GCD queue. It has no executor, backend, mutex, callbacks or GPU results.
// The caller holds one queue mutex for EVERY operation/getter and retains the
// Scheduler and registered Lifecycle objects through callback completion (or
// cancellation). Callbacks run outside that mutex and the serial GPU worker.
class Scheduler final {
public:
    static constexpr uint32_t kMaxSlots = 64;
    explicit Scheduler(uint32_t maxSlots = kMaxSlots) noexcept;
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    bool valid() const noexcept { return capacity_ != 0; }
    uint32_t capacity() const noexcept { return capacity_; }
    uint32_t usedSlots() const noexcept { return used_; }
    bool contains(Ticket) const noexcept;
    // Failed operations leave outputs and all state unchanged. A lifecycle
    // belongs to at most one scheduler and cannot be reserved again after use.
    bool reserve(Lifecycle&, Ticket& output) noexcept;
    bool enqueue(Ticket) noexcept;
    bool commit(Ticket) noexcept; // NotEnqueued implicitly enqueues atomically.
    // Only the oldest enqueued command may begin; an earlier uncommitted
    // command blocks later committed commands. Reserved-only slots do not.
    bool takeReady(Ticket& output) noexcept;
    // Called ONLY after the actual worker has established all dependencies
    // and successful native dispatches. The synchronous v1.4 RPC exposes this
    // fact late, after GPU completion. Never call merely before an RPC.
    bool markScheduled(Ticket) noexcept;
    // Success requires Scheduled; failure permits Committed or Scheduled.
    // Clears active/FIFO ownership immediately. The caller then releases the
    // execution slot before handlers run; a capacity-one handler can create
    // another command while its own Lifecycle still waits for callbacks.
    bool finish(Ticket, bool success) noexcept;
    // Scheduled waits include all scheduled handlers, independently of the
    // later completion handlers. Mark even an empty successful handler list.
    // Error never invents scheduling and directly unblocks scheduled waits.
    bool markScheduledCallbacksDone(Lifecycle&) noexcept;
    bool markCallbacksDone(Ticket) noexcept;
    // Lifecycle remains owned by the command object after its ticket/slot is
    // released. This overload checks the original queue identity, terminal
    // phase and single completion, without granting any stale ticket access.
    bool markCallbacksDone(Lifecycle&) noexcept;
    bool release(Ticket) noexcept; // Terminal required; callbacks are independent.
    // Only for an abandoned, uncommitted object. No execution/callback duty
    // was committed. Removes its FIFO entry and releases the reserved slot.
    bool cancel(Ticket) noexcept;
private:
    struct Slot { Lifecycle* lifecycle = nullptr; uint64_t generation = 0, order = 0; };
    Slot slots_[kMaxSlots]{};
    uint32_t capacity_ = 0, used_ = 0, active_ = kMaxSlots;
    uint64_t nextGeneration_ = 1, nextOrder_ = 1;
    Slot* find(Ticket) noexcept;
    const Slot* find(Ticket) const noexcept;
    Ticket ticket(uint32_t slot) const noexcept;
    void releaseSlot(Slot&) noexcept;
};
} } }
