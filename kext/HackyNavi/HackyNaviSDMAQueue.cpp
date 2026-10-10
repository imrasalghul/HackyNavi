// SPDX-License-Identifier: MIT
#include "HackyNaviSDMAQueue.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>

#if !defined(__x86_64__)
#error HackyNaviSDMAQueue requires coherent x86_64 DMA and uncached MMIO
#endif

OSDefineMetaClassAndStructors(HackyNaviSDMAQueue, OSObject)
namespace sq = hackynavi::sdmaqueue;
namespace {
constexpr uint64_t kRingBytes = 4096;
constexpr uint32_t kTestBytes = 4096;
using Region = HackyNaviGARTStorage::Region;
bool validPoll(const sq::PollOptions& p) {
    return p.maxPolls && p.maxPolls <= 1000000 && p.delayUs && p.delayUs <= 10 &&
        uint64_t(p.maxPolls) * p.delayUs <= 1000000;
}
}

bool HackyNaviSDMAQueue::init() {
    lock_ = nullptr; gart_ = nullptr; dataCpu_ = nullptr;
    dataBytes_ = dataGpu_ = 0;
    storageLease_ = hardwareHold_ = mmioActive_ = false;
    state_ = State::Empty;
    config_ = sq::Config{}; memory_ = sq::Memory{}; result_ = sq::Result{};
    if (!OSObject::init()) return false;
    lock_ = IOLockAlloc();
    return lock_ != nullptr;
}
HackyNaviSDMAQueue* HackyNaviSDMAQueue::withController(
    HackyNaviGARTController* gart, uint32_t engine, IOReturn& result) {
    result = kIOReturnNoMemory;
    auto* queue = new HackyNaviSDMAQueue;
    if (!queue) return nullptr;
    if (!queue->init()) { queue->release(); return nullptr; }
    result = queue->initialize(gart, engine);
    if (result != kIOReturnSuccess) { queue->release(); return nullptr; }
    return queue;
}
IOReturn HackyNaviSDMAQueue::initialize(HackyNaviGARTController* gart, uint32_t engine) {
    if (!gart || engine > 1) return kIOReturnBadArgument;
    gart_ = gart; gart_->retain();
    IOReturn result = gart_->acquireQueueStorage(this);
    if (result != kIOReturnSuccess) return result;
    storageLease_ = true;
    const auto* data = gart_->buffer(Region::Data);
    const auto* wb = gart_->buffer(Region::Writeback);
    uint64_t wbGpu = 0;
    if (!data || !wb || data->size() < 3 * kRingBytes || wb->size() < 4096 ||
        !data->cpuAddress() || !wb->cpuAddress() ||
        (reinterpret_cast<uintptr_t>(data->cpuAddress()) & 4095) ||
        (reinterpret_cast<uintptr_t>(wb->cpuAddress()) & 4095) ||
        !gart_->gpuAddress(Region::Data, dataGpu_) ||
        !gart_->gpuAddress(Region::Writeback, wbGpu)) return kIOReturnBadArgument;
    dataCpu_ = static_cast<uint8_t*>(data->cpuAddress());
    dataBytes_ = data->size();
    auto* wbCpu = static_cast<uint8_t*>(wb->cpuAddress());
    config_.engine = engine;
    config_.ringWords = kRingBytes / 4;
    config_.watchdog100ms = 10;
    config_.ringGpuAddress = dataGpu_;
    config_.rptrGpuAddress = wbGpu;
    config_.wptrGpuAddress = wbGpu + 8;
    config_.fenceGpuAddress = wbGpu + 16;
    memory_.ring = reinterpret_cast<uint32_t*>(dataCpu_);
    memory_.capacityWords = config_.ringWords;
    memory_.rptr = reinterpret_cast<volatile uint64_t*>(wbCpu);
    memory_.wptr = reinterpret_cast<volatile uint64_t*>(wbCpu + 8);
    memory_.fence = reinterpret_cast<volatile uint32_t*>(wbCpu + 16);
    state_ = State::Prepared;
    return kIOReturnSuccess;
}
bool HackyNaviSDMAQueue::providerReady() const {
    if (!gart_ || !storageLease_ || gart_->state_ != HackyNaviGARTController::State::BothEnabled ||
        !gart_->device_ || !gart_->openOwner_ || gart_->device_->isInactive() ||
        !gart_->device_->isOpen(gart_->openOwner_) ||
        gart_->device_->configRead32(0) != 0x744c1002) return false;
    const UInt16 command = gart_->device_->configRead16(kIOPCIConfigCommand);
    if (command == 0xffff || (command & 6) != 6) return false;
    UInt8 pm = 0;
    if (gart_->device_->findPCICapability(1, &pm)) {
        if (pm < 0x40 || pm > 0xf8 || (pm & 3)) return false;
        const UInt16 power = gart_->device_->configRead16(pm + 4);
        if (power == 0xffff || (power & 3)) return false;
    }
    return true;
}
bool HackyNaviSDMAQueue::accessAllowed(uint32_t offset) const {
    return mmioActive_ && gart_ && storageLease_ && gart_->registers_ &&
        !gart_->device_->isInactive() && gart_->device_->isOpen(gart_->openOwner_) &&
        !(offset & 3) && gart_->registerBytes_ >= 4 && offset <= gart_->registerBytes_ - 4;
}
bool HackyNaviSDMAQueue::read32(void* context, uint32_t offset, uint32_t& value) {
    auto* self = static_cast<HackyNaviSDMAQueue*>(context);
    if (!self->accessAllowed(offset)) return false;
    __asm__ __volatile__("" ::: "memory");
    value = self->gart_->registers_[offset / 4];
    OSSynchronizeIO();
    __asm__ __volatile__("" ::: "memory");
    return true;
}
bool HackyNaviSDMAQueue::write32(void* context, uint32_t offset, uint32_t value) {
    auto* self = static_cast<HackyNaviSDMAQueue*>(context);
    if (!self->accessAllowed(offset)) return false;
    __asm__ __volatile__("" ::: "memory");
    self->gart_->registers_[offset / 4] = value;
    OSSynchronizeIO();
    __asm__ __volatile__("" ::: "memory");
    return true;
}
bool HackyNaviSDMAQueue::publish(void* context) {
    auto* self = static_cast<HackyNaviSDMAQueue*>(context);
    return self->gart_ && self->gart_->publishQueueWrites(self) == kIOReturnSuccess;
}
bool HackyNaviSDMAQueue::syncCpu(void* context) {
    auto* self = static_cast<HackyNaviSDMAQueue*>(context);
    return self->gart_ &&
        self->gart_->synchronizeForCPU(Region::Writeback) == kIOReturnSuccess &&
        self->gart_->synchronizeForCPU(Region::Data) == kIOReturnSuccess;
}
void HackyNaviSDMAQueue::delayUs(void*, uint32_t microseconds) {
    if (microseconds >= 1000) IOSleep((microseconds + 999) / 1000);
    else IODelay(microseconds);
}
sq::Io HackyNaviSDMAQueue::io() { return {this, read32, write32, publish, syncCpu, delayUs}; }
IOReturn HackyNaviSDMAQueue::endOperation(IOReturn result) {
    mmioActive_ = false;
    IOLockUnlock(lock_);
    return result;
}
void HackyNaviSDMAQueue::holdHardware() {
    if (!hardwareHold_) { retain(); hardwareHold_ = true; }
}
IOReturn HackyNaviSDMAQueue::faultLocked() {
    state_ = State::FaultedRetained;
    return kIOReturnIOError;
}
IOReturn HackyNaviSDMAQueue::start() {
    IOLockLock(lock_);
    if (state_ != State::Prepared) return endOperation(kIOReturnNotReady);
    if (!providerReady()) return endOperation(kIOReturnOffline);
    holdHardware(); // Before the first possible hardware publication.
    mmioActive_ = true;
    if (sq::start(gart_->ipLayout_, gart_->registerBytes_, config_, memory_, io(), queue_, result_) != sq::Status::Ok) {
        if (!queue_.needsRetention()) {
            // The portable start has not attempted a hardware write. Leave
            // the lease prepared so ordinary stop/free can retire it; no
            // device-reset assertion is needed for a read-only rejection.
            hardwareHold_ = false;
            endOperation(kIOReturnNotReady);
            release();
            return kIOReturnNotReady;
        }
        return endOperation(faultLocked());
    }
    state_ = State::Running;
    return endOperation(kIOReturnSuccess);
}
bool HackyNaviSDMAQueue::dataRange(uint64_t offset, uint32_t bytes) const {
    return bytes && offset >= kRingBytes && offset < dataBytes_ && bytes <= dataBytes_ - offset;
}
IOReturn HackyNaviSDMAQueue::copyLocked(uint64_t source, uint64_t destination, uint32_t bytes,
                                       const sq::PollOptions& poll) {
    if (state_ != State::Running) return kIOReturnNotReady;
    if (!validPoll(poll) || !dataRange(source, bytes) || !dataRange(destination, bytes) ||
        (source < destination + bytes && destination < source + bytes)) return kIOReturnBadArgument;
    if (!providerReady()) return faultLocked();
    mmioActive_ = true;
    if (sq::submitCopy(io(), queue_, dataGpu_ + source, dataGpu_ + destination, bytes, result_) != sq::Status::Ok ||
        sq::waitFence(io(), queue_, poll, result_) != sq::Status::Ok) return faultLocked();
    return kIOReturnSuccess;
}
IOReturn HackyNaviSDMAQueue::copyData(uint64_t source, uint64_t destination,
                                    uint32_t bytes, const sq::PollOptions& poll) {
    IOLockLock(lock_);
    return endOperation(copyLocked(source, destination, bytes, poll));
}
IOReturn HackyNaviSDMAQueue::fillData(uint64_t destination, uint32_t value,
                                    uint32_t bytes, const sq::PollOptions& poll) {
    IOLockLock(lock_);
    if (state_ != State::Running) return endOperation(kIOReturnNotReady);
    if (!validPoll(poll) || !dataRange(destination, bytes) || ((destination | bytes) & 3))
        return endOperation(kIOReturnBadArgument);
    if (!providerReady()) return endOperation(faultLocked());
    mmioActive_ = true;
    if (sq::submitFill(io(), queue_, dataGpu_ + destination, value, bytes, result_) != sq::Status::Ok ||
        sq::waitFence(io(), queue_, poll, result_) != sq::Status::Ok) return endOperation(faultLocked());
    return endOperation(kIOReturnSuccess);
}
IOReturn HackyNaviSDMAQueue::runCopyTest(const sq::PollOptions& poll, CopyTestResult& out) {
    out = CopyTestResult{};
    out.firstMismatch = UINT32_MAX;
    IOLockLock(lock_);
    if (!validPoll(poll)) return endOperation(kIOReturnBadArgument);
    if (state_ != State::Running || !dataRange(kRingBytes, 2 * kTestBytes))
        return endOperation(kIOReturnNotReady);
    // No prior submission is in flight: public copy/fill methods wait under
    // the same mutex, and a timeout faults the queue before this can run again.
    auto* source = dataCpu_ + kRingBytes;
    auto* destination = source + kTestBytes;
    for (uint32_t i = 0; i < kTestBytes; ++i) {
        source[i] = static_cast<uint8_t>(i * 131U + (i >> 3) + 0x5a);
        destination[i] = static_cast<uint8_t>(~source[i]);
    }
    const IOReturn result = copyLocked(kRingBytes, 2 * kRingBytes, kTestBytes, poll);
    if (result != kIOReturnSuccess) return endOperation(result);
    if (!syncCpu(this)) return endOperation(faultLocked());
    for (uint32_t i = 0; i < kTestBytes; ++i) {
        ++out.bytesCompared;
        // Compare independently generated expected bytes, not merely two
        // buffers which could both have been corrupted by bad GPU addressing.
        const uint8_t expected = static_cast<uint8_t>(i * 131U + (i >> 3) + 0x5a);
        const uint8_t observed = static_cast<volatile uint8_t*>(destination)[i];
        const uint8_t sourceObserved = static_cast<volatile uint8_t*>(source)[i];
        if (observed != expected || sourceObserved != expected) {
            out.firstMismatch = i; out.expected = expected;
            out.observed = observed != expected ? observed : sourceObserved;
            return endOperation(faultLocked());
        }
    }
    out.completed = true;
    return endOperation(kIOReturnSuccess);
}
IOReturn HackyNaviSDMAQueue::stop(const sq::PollOptions& poll) {
    IOLockLock(lock_);
    if (!validPoll(poll)) return endOperation(kIOReturnBadArgument);
    if (state_ == State::Stopped) return endOperation(kIOReturnSuccess);
    if (state_ == State::Empty) return endOperation(kIOReturnNotReady);
    if (hardwareHold_) {
        if (!providerReady()) return endOperation(faultLocked());
        mmioActive_ = true;
        if (sq::stop(io(), queue_, poll, result_) != sq::Status::Ok)
            return endOperation(faultLocked());
    }
    const IOReturn result = storageLease_ ? gart_->releaseQueueStorage(this) : kIOReturnSuccess;
    if (result != kIOReturnSuccess) return endOperation(faultLocked());
    storageLease_ = false; state_ = State::Stopped;
    const bool dropHold = hardwareHold_; hardwareHold_ = false;
    endOperation(kIOReturnSuccess);
    if (dropHold) release();
    return kIOReturnSuccess;
}
IOReturn HackyNaviSDMAQueue::releaseAfterDeviceReset() {
    IOLockLock(lock_);
    const IOReturn result = storageLease_ ? gart_->releaseQueueStorage(this) : kIOReturnSuccess;
    if (result != kIOReturnSuccess) return endOperation(result);
    queue_.clearAfterVerifiedReset();
    storageLease_ = false; state_ = State::Stopped;
    const bool dropHold = hardwareHold_; hardwareHold_ = false;
    endOperation(kIOReturnSuccess);
    if (dropHold) release();
    return kIOReturnSuccess;
}
void HackyNaviSDMAQueue::free() {
    // Active/faulted hardware references carry the self-hold, so free is only
    // reachable before publication or after explicit verified retirement.
    if (storageLease_ && gart_) (void)gart_->releaseQueueStorage(this);
    storageLease_ = false;
    dataCpu_ = nullptr; dataBytes_ = dataGpu_ = 0;
    if (gart_) { gart_->release(); gart_ = nullptr; }
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    OSObject::free();
}
