// SPDX-License-Identifier: MIT
#include "command_lifecycle.hpp"

namespace hackynavi { namespace compute { namespace command {
Scheduler::Scheduler(uint32_t maxSlots) noexcept
    : capacity_(maxSlots && maxSlots <= kMaxSlots ? maxSlots : 0) {}
const Scheduler::Slot* Scheduler::find(Ticket value) const noexcept {
    if (value.owner_ != this || value.slot >= capacity_ || !value.generation) return nullptr;
    const auto& slot = slots_[value.slot];
    return slot.lifecycle && slot.generation == value.generation ? &slot : nullptr;
}
Scheduler::Slot* Scheduler::find(Ticket value) noexcept {
    return const_cast<Slot*>(static_cast<const Scheduler*>(this)->find(value));
}
bool Scheduler::contains(Ticket value) const noexcept { return find(value) != nullptr; }
Ticket Scheduler::ticket(uint32_t index) const noexcept {
    Ticket result; result.slot = index; result.generation = slots_[index].generation;
    result.owner_ = this; return result;
}
bool Scheduler::reserve(Lifecycle& state, Ticket& output) noexcept {
    if (!capacity_ || used_ == capacity_ || !nextGeneration_ || state.bound_ || state.owner_ ||
        state.phase_ != Phase::NotEnqueued) return false;
    uint32_t index = 0;
    while (index < capacity_ && slots_[index].lifecycle) ++index;
    if (index == capacity_) return false;
    auto& slot = slots_[index];
    // Unsigned wrap to zero permanently closes admission after the last
    // possible generation; a stale generation can never become valid again.
    slot.generation = nextGeneration_++;
    slot.order = 0; slot.lifecycle = &state; state.bound_ = true; state.owner_ = this; ++used_;
    output = ticket(index); return true;
}
bool Scheduler::enqueue(Ticket value) noexcept {
    auto* slot = find(value);
    if (!slot || slot->lifecycle->phase_ != Phase::NotEnqueued || !nextOrder_) return false;
    slot->order = nextOrder_++; slot->lifecycle->phase_ = Phase::Enqueued; return true;
}
bool Scheduler::commit(Ticket value) noexcept {
    auto* slot = find(value);
    if (!slot || !slot->lifecycle->mutableEncoding()) return false;
    if (slot->lifecycle->phase_ == Phase::NotEnqueued) {
        if (!nextOrder_) return false;
        slot->order = nextOrder_++;
    }
    slot->lifecycle->phase_ = Phase::Committed; return true;
}
bool Scheduler::takeReady(Ticket& output) noexcept {
    if (active_ != kMaxSlots) return false;
    uint32_t first = kMaxSlots;
    for (uint32_t index = 0; index < capacity_; ++index) {
        const auto& slot = slots_[index];
        if (slot.lifecycle && slot.order &&
            (first == kMaxSlots || slot.order < slots_[first].order)) first = index;
    }
    if (first == kMaxSlots || slots_[first].lifecycle->phase_ != Phase::Committed) return false;
    active_ = first; slots_[first].lifecycle->active_ = true;
    output = ticket(first); return true;
}
bool Scheduler::markScheduled(Ticket value) noexcept {
    auto* slot = find(value);
    if (!slot || active_ != value.slot || slot->lifecycle->phase_ != Phase::Committed) return false;
    slot->lifecycle->phase_ = Phase::Scheduled; return true;
}
bool Scheduler::finish(Ticket value, bool success) noexcept {
    auto* slot = find(value);
    if (!slot || active_ != value.slot) return false;
    auto& state = *slot->lifecycle;
    if (success ? state.phase_ != Phase::Scheduled :
        (state.phase_ != Phase::Committed && state.phase_ != Phase::Scheduled)) return false;
    state.phase_ = success ? Phase::Completed : Phase::Error;
    state.active_ = false; slot->order = 0; active_ = kMaxSlots; return true;
}
bool Scheduler::markScheduledCallbacksDone(Lifecycle& state) noexcept {
    if (state.owner_ != this || state.scheduledCallbacksDone_ ||
        (state.phase_ != Phase::Scheduled && state.phase_ != Phase::Completed)) return false;
    state.scheduledCallbacksDone_ = true; return true;
}
bool Scheduler::markCallbacksDone(Ticket value) noexcept {
    auto* slot = find(value);
    return slot && markCallbacksDone(*slot->lifecycle);
}
bool Scheduler::markCallbacksDone(Lifecycle& state) noexcept {
    if (state.owner_ != this || !state.terminal() || state.callbacksDone_ ||
        (state.phase_ == Phase::Completed && !state.scheduledCallbacksDone_)) return false;
    state.callbacksDone_ = true; return true;
}
void Scheduler::releaseSlot(Slot& slot) noexcept {
    slot.lifecycle->bound_ = false; slot.lifecycle = nullptr; slot.order = 0; --used_;
}
bool Scheduler::release(Ticket value) noexcept {
    auto* slot = find(value);
    if (!slot || !slot->lifecycle->terminal()) return false;
    releaseSlot(*slot); return true;
}
bool Scheduler::cancel(Ticket value) noexcept {
    auto* slot = find(value);
    if (!slot || !slot->lifecycle->mutableEncoding()) return false;
    slot->lifecycle->phase_ = Phase::Error;
    slot->lifecycle->callbacksDone_ = true;
    releaseSlot(*slot); return true;
}
} } }
