// SPDX-License-Identifier: MIT
#include "HackyNaviGraphicsQueue.hpp"
#include "HackyNaviVMContext.hpp"
#include "HackyNaviVMCompletion.hpp"
#include <IOKit/IOLib.h>

#if !defined(__x86_64__)
#error HackyNaviGraphicsQueue requires coherent x86_64 DMA and uncached MMIO
#endif
OSDefineMetaClassAndStructors(HackyNaviGraphicsQueue, OSObject)
using namespace hackynavi;
namespace {
using Region = HackyNaviGARTStorage::Region;
constexpr uint64_t kRingBytes = 16384, kIbBytes = 16384, kDataStart = kRingBytes + kIbBytes;
constexpr size_t kWorkspaceBytes = 4096;
bool validPoll(const gfxqueue::PollOptions& p) {
    return p.maxPolls && p.maxPolls <= 1000000 && p.delayUs && p.delayUs <= 10 &&
        uint64_t(p.maxPolls) * p.delayUs <= 1000000;
}
}
bool HackyNaviGraphicsQueue::init() {
    lock_ = nullptr; firmware_ = nullptr; gart_ = nullptr;
    workspace_ = dataCpu_ = nullptr; dataBytes_ = dataGpu_ = 0;
    storageLease_ = hardwareHold_ = neverStartedRetirement_ = false; state_ = State::Empty;
    rawIndirectUsed_ = isolatedClaimed_ = false;
    clearState_ = VramAllocation{}; initResult_ = gfxinit::Result{};
    shader_ = VramAllocation{}; shaderGpu_ = 0; shaderReady_ = false;
    computeStatus_ = gfxcompute::Status::InvalidArgument;
    config_ = gfxqueue::Config{}; memory_ = gfxqueue::Memory{}; queueResult_ = gfxqueue::Result{};
    if (!OSObject::init()) return false;
    lock_ = IOLockAlloc();
    return lock_ != nullptr;
}
HackyNaviGraphicsQueue* HackyNaviGraphicsQueue::withFirmware(
    HackyNaviFirmwareController* firmware, IOReturn& result) {
    result = kIOReturnNoMemory;
    auto* self = new HackyNaviGraphicsQueue;
    if (!self) return nullptr;
    if (!self->init()) { self->release(); return nullptr; }
    result = self->initialize(firmware);
    if (result != kIOReturnSuccess) { self->release(); return nullptr; }
    return self;
}
IOReturn HackyNaviGraphicsQueue::initialize(HackyNaviFirmwareController* firmware) {
    if (!firmware || !firmware->gart_) return kIOReturnBadArgument;
    if (!firmware->gart_->normalAccessReady()) return kIOReturnOffline;
    firmware_ = firmware; firmware_->retain();
    gart_ = firmware_->gart_; gart_->retain();
    workspace_ = static_cast<uint8_t*>(IOMalloc(kWorkspaceBytes));
    if (!workspace_) return kIOReturnNoMemory;
    IOLockLock(lock_); IOLockLock(firmware_->lock_);
    // Firmware mutex precedes acquireQueueStorage's internal GART mutex.
    const auto acquired = gart_->acquireQueueStorage(this);
    IOLockLock(gart_->lock_);
    if (acquired != kIOReturnSuccess) return finish(acquired);
    storageLease_ = true;
    if (!providerReady()) return finish(kIOReturnNotReady);
    const auto* data = gart_->storage_.buffer(Region::Data);
    const auto* wb = gart_->storage_.buffer(Region::Writeback);
    uint64_t wbGpu = 0;
    if (!data || !wb || data->size() < kDataStart + 4096 || wb->size() < 4096 ||
        !data->cpuAddress() || !wb->cpuAddress() ||
        (reinterpret_cast<uintptr_t>(data->cpuAddress()) & 4095) ||
        (reinterpret_cast<uintptr_t>(wb->cpuAddress()) & 4095) ||
        !gart_->gpuAddress(Region::Data, dataGpu_) || !gart_->gpuAddress(Region::Writeback, wbGpu))
        return finish(kIOReturnBadArgument);
    if (gart_->allocator_.allocate(4096, 4096, clearState_) != VramAllocatorStatus::Ok)
        return finish(kIOReturnNoSpace);
    uint64_t shaderPhysical = 0;
    if (gart_->allocator_.allocate(gfxcompute::kShaderImageBytes, 4096, shader_) != VramAllocatorStatus::Ok ||
        gart_->allocator_.addresses(shader_.handle, shaderGpu_, shaderPhysical) != VramAllocatorStatus::Ok)
        return finish(kIOReturnNoSpace);
    dataCpu_ = static_cast<uint8_t*>(data->cpuAddress()); dataBytes_ = data->size();
    auto* wbCpu = static_cast<uint8_t*>(wb->cpuAddress());
    config_ = {uint32_t(kRingBytes / 4), dataGpu_, wbGpu, wbGpu + 8, wbGpu + 16};
    memory_ = {reinterpret_cast<uint32_t*>(dataCpu_), size_t(kRingBytes / 4),
        reinterpret_cast<volatile uint32_t*>(wbCpu), reinterpret_cast<volatile uint64_t*>(wbCpu + 8),
        reinterpret_cast<volatile uint32_t*>(wbCpu + 16)};
    state_ = State::Allocated;
    return finish(gart_->normalAccessReady()?kIOReturnSuccess:kIOReturnOffline);
}
void HackyNaviGraphicsQueue::lockOwners() {
    IOLockLock(lock_); IOLockLock(firmware_->lock_); IOLockLock(gart_->lock_);
}
IOReturn HackyNaviGraphicsQueue::finish(IOReturn result) {
    gart_->mmioActive_ = false;
    IOLockUnlock(gart_->lock_); IOLockUnlock(firmware_->lock_); IOLockUnlock(lock_);
    return result;
}
bool HackyNaviGraphicsQueue::providerReady() const {
    return gart_->accessReady() && storageLease_ && gart_->queueOwner_ == this && gart_->firmwareOwner_ == firmware_ &&
        gart_->firmwareReady_ && gart_->state_ == HackyNaviGARTController::State::BothEnabled &&
        firmware_->state_ == HackyNaviFirmwareController::State::FirmwareLoaded &&
        firmware_->graphics_.autoloadComplete() && firmware_->smu_.awake() && firmware_->providerReady();
}
bool HackyNaviGraphicsQueue::neverStartedRetirementReadyLocked() const {
    if (!hardwareHold_ || !storageLease_ || !shaderReady_ || rawIndirectUsed_ || isolatedClaimed_ ||
        !queue_.neverPublished() || !initState_.prepared() || !initState_.needsClearStateRetention() ||
        gart_->queueOwner_ != this || gart_->vmRetirementActive_ ||
        gart_->vmRetirementFailure_ != kIOReturnSuccess || gart_->hasVmOwners()) return false;
    const auto owned = initState_.clearStateAllocation();
    VramRange range{};
    return owned.owner == &gart_->allocator_ && owned.owner == clearState_.handle.owner &&
        owned.serial == clearState_.handle.serial && owned.slot == clearState_.handle.slot &&
        gart_->allocator_.lookup(owned, range) == VramAllocatorStatus::Ok && range.bytes >= 4096;
}
void HackyNaviGraphicsQueue::holdHardware() {
    if (!hardwareHold_) { retain(); hardwareHold_ = true; }
}
IOReturn HackyNaviGraphicsQueue::fault() {
    state_ = State::FaultedRetained;
    IOLog("HackyNavi: graphics failure init=%s queue=%s stage=%u reg=0x%x value=0x%x; allocations retained\n",
        gfxinit::statusString(initResult_.status), gfxqueue::statusString(queueResult_.status),
        static_cast<unsigned>(queueResult_.stage), queueResult_.lastRegister, queueResult_.lastValue);
    return kIOReturnIOError;
}
bool HackyNaviGraphicsQueue::read32(void* context, uint32_t offset, uint32_t& value) {
    auto* self = static_cast<HackyNaviGraphicsQueue*>(context);
    return HackyNaviGARTController::read32(self->gart_, offset, value);
}
bool HackyNaviGraphicsQueue::write32(void* context, uint32_t offset, uint32_t value) {
    auto* self = static_cast<HackyNaviGraphicsQueue*>(context);
    return HackyNaviGARTController::write32(self->gart_, offset, value);
}
bool HackyNaviGraphicsQueue::publish(void* context) {
    auto* self = static_cast<HackyNaviGraphicsQueue*>(context);
    return self->gart_->synchronizeDeviceLocked(Region::Data) == kIOReturnSuccess &&
        self->gart_->synchronizeDeviceLocked(Region::Writeback) == kIOReturnSuccess &&
        HackyNaviGARTController::hdpFlush(self->gart_);
}
bool HackyNaviGraphicsQueue::syncCpu(void* context) {
    auto* self = static_cast<HackyNaviGraphicsQueue*>(context);
    return self->gart_->synchronizeCpuLocked(Region::Writeback) == kIOReturnSuccess &&
        self->gart_->synchronizeCpuLocked(Region::Data) == kIOReturnSuccess;
}
void HackyNaviGraphicsQueue::delayUs(void*, uint32_t us) {
    if (us >= 1000) IOSleep((us + 999) / 1000); else IODelay(us);
}
gfxqueue::Io HackyNaviGraphicsQueue::io() { return {this, read32, write32, publish, syncCpu, delayUs}; }
IOReturn HackyNaviGraphicsQueue::prepare() {
    if (!gart_ || !gart_->normalAccessReady()) return kIOReturnOffline;
    lockOwners();
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    if (state_ != State::Allocated) return finish(kIOReturnNotReady);
    if (!providerReady()) return finish(kIOReturnOffline);
    holdHardware(); gart_->mmioActive_ = true;
    const gfxinit::Io gcIo = {this, read32, write32};
    if (gfxinit::prepare(gart_->ipLayout_, firmware_->graphicsFiles_, firmware_->graphics_, firmware_->smu_,
        gart_->allocator_, clearState_.handle, workspace_, kWorkspaceBytes, gart_->transferIo(), gcIo,
        gart_->hdpIo(), gart_->hdpState_, initState_, initResult_) != gfxinit::Status::Ok) {
        if (!initState_.needsClearStateRetention()) {
            // No CSIB address has been published; the VRAM upload and halted
            // register programming cannot introduce a reference to this owner.
            state_ = State::FailedUnpublished; hardwareHold_ = false;
            finish(kIOReturnIOError); release(); return kIOReturnIOError;
        }
        return finish(fault());
    }
    computeStatus_ = gfxcompute::shaderImage(workspace_, kWorkspaceBytes);
    if (computeStatus_ != gfxcompute::Status::Ok ||
        uploadVram(gart_->allocator_, shader_.handle, 0, workspace_, gfxcompute::kShaderImageBytes,
            gart_->transferIo(), gart_->transferResult_) != VramTransferStatus::Ok ||
        !HackyNaviGARTController::hdpFlush(gart_)) return finish(fault());
    shaderReady_ = true;
    state_ = State::Prepared;
    IOLog("HackyNavi: GC11 RS64 start PCs and owned RLC clear state prepared\n");
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviGraphicsQueue::start(const gfxqueue::PollOptions& poll) {
    if (!gart_ || !gart_->normalAccessReady()) return kIOReturnOffline;
    if (!validPoll(poll)) return kIOReturnBadArgument;
    lockOwners();
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    if (state_ != State::Prepared) return finish(kIOReturnNotReady);
    if (!providerReady()) return finish(fault());
    gart_->mmioActive_ = true;
    if (gfxqueue::start(gart_->ipLayout_, gart_->registerBytes_, initState_, firmware_->smu_, config_, memory_,
        io(), poll, queue_, queueResult_) != gfxqueue::Status::Ok) return finish(fault());
    state_ = State::Running;
    IOLog("HackyNavi: GC11 startup fence=%u RPTR=%u observed from GPU writeback\n",
        queueResult_.fenceValue, queueResult_.observedRptr);
    return finish(kIOReturnSuccess);
}
bool HackyNaviGraphicsQueue::dataRange(uint64_t offset, uint64_t bytes) const {
    return bytes && offset >= kDataStart && offset < dataBytes_ && bytes <= dataBytes_ - offset;
}
IOReturn HackyNaviGraphicsQueue::writeLocked(uint64_t offset, uint32_t value, const gfxqueue::PollOptions& poll) {
    if (!gart_ || !gart_->normalAccessReady()) return kIOReturnOffline;
    if (state_ != State::Running) return kIOReturnNotReady;
    if (!validPoll(poll) || (offset & 3) || !dataRange(offset, 4)) return kIOReturnBadArgument;
    if (!providerReady()) return fault();
    gart_->mmioActive_ = true;
    if (gfxqueue::submitWrite(io(), queue_, dataGpu_ + offset, value, queueResult_) != gfxqueue::Status::Ok ||
        gfxqueue::waitFence(io(), queue_, poll, queueResult_) != gfxqueue::Status::Ok) return fault();
    return kIOReturnSuccess;
}
IOReturn HackyNaviGraphicsQueue::writeData(uint64_t offset, uint32_t value, const gfxqueue::PollOptions& poll) {
    lockOwners();
    return finish(writeLocked(offset, value, poll));
}
IOReturn HackyNaviGraphicsQueue::submitIndirect(const uint32_t* commands, uint32_t words,
    const gfxqueue::PollOptions& poll) {
    if (!gart_ || !gart_->normalAccessReady()) return kIOReturnOffline;
    if (!commands || !words || words > kIbBytes / 4 || !validPoll(poll)) return kIOReturnBadArgument;
    lockOwners();
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    if (state_ != State::Running) return finish(kIOReturnNotReady);
    if (isolatedClaimed_) return finish(kIOReturnNotPermitted);
    if (!providerReady()) return finish(fault());
    // An arbitrary trusted-kernel stream can alter VMID/producer controls.
    // Never subsequently claim exclusive isolated-VM ownership in this epoch.
    rawIndirectUsed_ = true;
    // No job is in flight: every successful public submission waits under the
    // same lock, and a failed wait seals this owner. Commands may overlap IB
    // source storage supplied by a trusted caller, so use memmove semantics.
    memmove(dataCpu_ + kRingBytes, commands, size_t(words) * 4);
    gart_->mmioActive_ = true;
    if (gfxqueue::submitIndirect(io(), queue_, dataGpu_ + kRingBytes, words, queueResult_) != gfxqueue::Status::Ok ||
        gfxqueue::waitFence(io(), queue_, poll, queueResult_) != gfxqueue::Status::Ok) return finish(fault());
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviGraphicsQueue::submitIsolated(HackyNaviVMContext* context,
    const gfxqueue::PollOptions& poll, HackyNaviVMCompletion*& completion) {
    completion = nullptr;
    if (!gart_ || !gart_->normalAccessReady()) return kIOReturnOffline;
    if (!context || !validPoll(poll)) return kIOReturnBadArgument;
    lockOwners(); // Queue -> Firmware -> GART excludes all queue producers.
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    if (state_ != State::Running || queue_.pending()) return finish(kIOReturnNotReady);
    // Foreign owners, unmapped resources, mutable captures and altered PM4
    // fail before context-register writes or any ring publication.
    if (rawIndirectUsed_ || context->gart_ != gart_ || !context->captureValidLocked() ||
        context->state_ != HackyNaviVMContext::State::Captured || context->hardwareHold_ ||
        context->submittedQueue_ || !context->captureGeneration_ || context->submittedCaptureGeneration_)
        return finish(kIOReturnNotPermitted);
    // Exhaustion is not a GPU fault and must not expose a fresh/reused VM.
    // No producer can advance under these locks; the ring guard repeats it.
    const auto capacity = queue_.submissionCapacity();
    if (capacity != gfxqueue::Status::Ok)
        return finish(capacity == gfxqueue::Status::CounterExhausted ? kIOReturnNoResources : kIOReturnNotReady);
    if (!providerReady()) return finish(fault());
    auto* observed = HackyNaviVMCompletion::withOwners(this, context);
    if (!observed) return finish(kIOReturnNoMemory);
    IOReturn status = kIOReturnSuccess;
    // One immutable producer claim precedes both VM publication and WPTR.
    // Failed exposure/submission never gets a completed sequence or a retry.
    isolatedClaimed_ = true; context->submittedQueue_ = this;
    context->submittedCaptureGeneration_ = context->captureGeneration_;
    const uint32_t maximum = 100000 / poll.delayUs;
    const vmcontext::Poll vmPoll = {poll.maxPolls < maximum ? poll.maxPolls : maximum, poll.delayUs};
    status = context->activateLocked(vmPoll);
    if (status == kIOReturnSuccess) {
        gart_->mmioActive_ = true;
        // Actual IODMACommand publication of private tables, code, data and IB,
        // then HDP flush. The ring's own publication follows this operation.
        if (!HackyNaviVMContext::publish(context) ||
            gfxqueue::submitIsolatedIndirect(io(), queue_, context->vmid_, context->capturedIndirectGpu_,
                context->capturedWords_, queueResult_) != gfxqueue::Status::Ok ||
            gfxqueue::waitFence(io(), queue_, poll, queueResult_) != gfxqueue::Status::Ok)
            status = kIOReturnIOError;
    }
    if (status != kIOReturnSuccess) {
        if (context->hardwareHold_) context->state_ = HackyNaviVMContext::State::FaultedRetained;
        fault(); finish(status); observed->release(); return status;
    }
    const auto& result = context->capturedResult_;
    observed->epoch_ = context->epoch_; observed->vmid_ = context->vmid_;
    observed->captureGeneration_ = context->submittedCaptureGeneration_;
    observed->id_ = result.buffer.id; observed->generation_ = result.buffer.generation;
    observed->offset_ = result.offset; observed->bytes_ = result.bytes;
    // waitFence accepts only the actual DMA-synchronized GPU-written sequence
    // AND consumed ring pointer. No CPU-written flag can issue this object.
    observed->sequence_ = queueResult_.fenceValue; observed->producer_ = queueResult_.observedRptr;
    context->submittedSequence_ = observed->sequence_; context->submittedProducer_ = observed->producer_;
    observed->valid_ = true;
    finish(kIOReturnSuccess); completion = observed; return kIOReturnSuccess;
}
IOReturn HackyNaviGraphicsQueue::readCompletion(const HackyNaviVMCompletion& observed,
    uint64_t offset, void* output, size_t bytes) {
    if (!gart_ || !gart_->normalAccessReady()) return kIOReturnOffline;
    if (!observed.valid_ || observed.queue_ != this || !observed.context_ || !output || !bytes ||
        offset > observed.bytes_ || bytes > observed.bytes_ - offset ||
        bytes - 1 > UINTPTR_MAX - reinterpret_cast<uintptr_t>(output)) return kIOReturnBadArgument;
    lockOwners();
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    auto* context = observed.context_;
    // No new queue operation may overlap readback. A later producer advance
    // revokes this observation even if that later job has already completed.
    if (!completionMatchesLocked(observed)) return finish(kIOReturnNotReady);
    if (!providerReady() || !syncCpu(this)) return finish(fault());
    if (*memory_.fence != observed.sequence_ || *memory_.rptr != observed.producer_) return finish(fault());
    auto* buffer = context->find({observed.id_, observed.generation_});
    if (!buffer || observed.offset_ > buffer->dma->size() ||
        observed.bytes_ > buffer->dma->size() - observed.offset_) return finish(kIOReturnNotReady);
    const auto status = gart_->synchronizeCpuBuffer(*buffer->dma);
    if (status != kIOReturnSuccess) return finish(fault());
    memmove(output, static_cast<const uint8_t*>(buffer->dma->cpuAddress()) + observed.offset_ + offset, bytes);
    return finish(kIOReturnSuccess);
}
bool HackyNaviGraphicsQueue::completedContextLocked(const HackyNaviVMContext* context,
    uint32_t sequence, uint32_t producer) const {
    // Pinned Linux amdgpu_ids.c waits every VMID's active job fences before
    // reuse. Here each sealed capture has exactly ONE IB, issued by this queue.
    // The successful waitFence record cannot be supplied or edited by callers.
    return isolatedClaimed_ && !rawIndirectUsed_ && context && context->gart_ == gart_ &&
        context->submittedQueue_ == this && context->submittedSequence_ && context->submittedProducer_ &&
        context->captureGeneration_ && context->submittedCaptureGeneration_ == context->captureGeneration_ &&
        context->submittedSequence_ <= sequence && context->submittedProducer_ <= producer &&
        context->state_ == HackyNaviVMContext::State::Active && context->hardwareHold_ && context->registryHold_ &&
        context->mm_.phase() == vmcontext::Phase::Enabled && context->gfx_.phase() == vmcontext::Phase::Enabled &&
        context->captureValidLocked();
}
bool HackyNaviGraphicsQueue::completionMatchesLocked(const HackyNaviVMCompletion& observed) const {
    const auto* context = observed.context_;
    if (!observed.valid_ || observed.queue_ != this || !context || state_ != State::Running || queue_.pending() ||
        queue_.producerWords() != observed.producer_ || context->epoch_ != observed.epoch_ ||
        context->vmid_ != observed.vmid_ || context->submittedSequence_ != observed.sequence_ ||
        !observed.captureGeneration_ || context->captureGeneration_ != observed.captureGeneration_ ||
        context->submittedProducer_ != observed.producer_ ||
        !completedContextLocked(context, observed.sequence_, observed.producer_)) return false;
    const auto& result = context->capturedResult_;
    return observed.id_ == result.buffer.id && observed.generation_ == result.buffer.generation &&
        observed.offset_ == result.offset && observed.bytes_ == result.bytes;
}
IOReturn HackyNaviGraphicsQueue::retireContextLocked(HackyNaviVMContext* context,
    const vmcontext::Poll& poll, unsigned& drops) {
    drops = 0;
    const auto detached = context->detachLocked(poll);
    if (detached != kIOReturnSuccess) return detached;
    // The separate queue observation proved work/cache writeback complete;
    // detach now proves both hubs cannot issue new page walks for this VMID.
    // Disable/ACK without that producer evidence never reaches this release.
    const auto result = context->releaseStorageLocked();
    drops = unsigned(context->hardwareHold_) + unsigned(context->registryHold_);
    context->hardwareHold_ = context->registryHold_ = false;
    return result;
}
IOReturn HackyNaviGraphicsQueue::retireCompletion(HackyNaviVMCompletion& observed,
    const vmcontext::Poll& poll) {
    if (!gart_ || !gart_->accessReady()) return kIOReturnOffline;
    if (!poll.iterations || !poll.delayUs || poll.delayUs > 10 ||
        uint64_t(poll.iterations) * poll.delayUs > 100000) return kIOReturnBadArgument;
    lockOwners();
    if (!gart_->accessReady()) return finish(kIOReturnOffline);
    if (!completionMatchesLocked(observed)) return finish(kIOReturnNotReady);
    if (!providerReady()) return finish(fault());
    gart_->mmioActive_ = true;
    const gfxqueue::PollOptions queuePoll = {poll.iterations, poll.delayUs};
    if (gfxqueue::checkIdleCompletion(io(), queue_, queuePoll, queueResult_) != gfxqueue::Status::Ok ||
        queueResult_.fenceValue != observed.sequence_ || queueResult_.observedRptr != observed.producer_)
        return finish(fault());
    auto* context = observed.context_;
    observed.valid_ = false; // revoke readback BEFORE the first detach write
    unsigned drops = 0;
    const auto result = retireContextLocked(context, poll, drops);
    if (result != kIOReturnSuccess) fault();
    finish(result);
    // Completion retains this context and queue. Registry/publication holds
    // are dropped only after all three owner locks are released.
    for (unsigned n = 0; n < drops; ++n) context->release();
    return result;
}
IOReturn HackyNaviGraphicsQueue::consumeCompletion(HackyNaviVMCompletion& observed,
    const vmcontext::Poll& poll) {
    // A recovery scope may tear down a context but must never reopen a mutable
    // arena. Normal authority remains installed through readback and this call.
    if (!gart_ || !gart_->normalAccessReady()) return kIOReturnOffline;
    if (!poll.iterations || !poll.delayUs || poll.delayUs > 10 ||
        uint64_t(poll.iterations) * poll.delayUs > 100000) return kIOReturnBadArgument;
    lockOwners();
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    if (gart_->vmRetirementFailure_ != kIOReturnSuccess) return finish(gart_->vmRetirementFailure_);
    if (!completionMatchesLocked(observed)) return finish(kIOReturnNotReady);
    if (!providerReady()) return finish(fault());
    gart_->mmioActive_ = true;
    const gfxqueue::PollOptions queuePoll = {poll.iterations, poll.delayUs};
    if (gfxqueue::checkIdleCompletion(io(), queue_, queuePoll, queueResult_) != gfxqueue::Status::Ok ||
        queueResult_.fenceValue != observed.sequence_ || queueResult_.observedRptr != observed.producer_)
        return finish(fault());
    auto* context = observed.context_;
    // Tokens never survive even a partially applied detach. Neither a later
    // job nor a repeated call may use the old observation to free new storage.
    observed.valid_ = false;
    IOReturn result = context->detachLocked(poll);
    if (result == kIOReturnSuccess) result = context->consumeCapturedStorageLocked();
    const bool drop = result == kIOReturnSuccess;
    if (drop) {
        // Only the queue's actual idle/fence and both hub detaches reached the
        // private cleanup. Data owners/root/registry stay; publication ends.
        context->hardwareHold_ = false;
    } else {
        context->state_ = HackyNaviVMContext::State::FaultedRetained;
        fault();
    }
    finish(result);
    // The completion keeps context and queue alive. Drop the hardware hold
    // outside Queue -> Firmware -> GART; registry ownership is unchanged.
    if (drop) context->release();
    return result;
}
IOReturn HackyNaviGraphicsQueue::retireCompletedContexts(const vmcontext::Poll& poll) {
    if (!gart_ || !gart_->accessReady()) return kIOReturnOffline;
    if (!poll.iterations || !poll.delayUs || poll.delayUs > 10 ||
        uint64_t(poll.iterations) * poll.delayUs > 100000) return kIOReturnBadArgument;
    lockOwners();
    if (!gart_->accessReady()) return finish(kIOReturnOffline);
    if (gart_->vmRetirementFailure_ != kIOReturnSuccess) return finish(gart_->vmRetirementFailure_);
    if (neverStartedRetirement_) {
        if (state_ != State::QuiescedRetained || !providerReady() ||
            !neverStartedRetirementReadyLocked()) return finish(fault());
        // No queue start callback occurred and no native VM exists. There is
        // no completion to observe. Keep the real CSIB pinned for RLC drain.
        return finish(kIOReturnSuccess);
    }
    if (state_ != State::QuiescedRetained || queue_.phase() != gfxqueue::Phase::Stopped ||
        queue_.pending() || queue_.needsRetention()) return finish(kIOReturnNotReady);
    if (!providerReady()) return finish(fault());
    gart_->mmioActive_ = true;
    const gfxqueue::PollOptions queuePoll = {poll.iterations, poll.delayUs};
    if (gfxqueue::checkIdleCompletion(io(), queue_, queuePoll, queueResult_) != gfxqueue::Status::Ok)
        return finish(fault());
    HackyNaviVMContext* retired[7] = {}; unsigned drops[7] = {};
    IOReturn result = kIOReturnSuccess;
    for (unsigned slot = 0; slot < 7; ++slot) {
        auto* context = gart_->vmOwners_[slot];
        if (!context) continue;
        if (!context->hardwareHold_) {
            result = context->releaseStorageLocked();
            drops[slot] = unsigned(context->registryHold_); context->registryHold_ = false;
        } else if (!completedContextLocked(context, queueResult_.fenceValue, queueResult_.observedRptr)) {
            result = kIOReturnNotPermitted;
        } else {
            result = retireContextLocked(context, poll, drops[slot]);
        }
        retired[slot] = context;
        if (result != kIOReturnSuccess) { fault(); break; }
    }
    finish(result);
    for (unsigned slot = 0; slot < 7; ++slot)
        for (unsigned n = 0; n < drops[slot]; ++n) retired[slot]->release();
    return result;
}
IOReturn HackyNaviGraphicsQueue::runWriteTest(const gfxqueue::PollOptions& poll, WriteTestResult& out) {
    if (!gart_ || !gart_->normalAccessReady()) return kIOReturnOffline;
    out = WriteTestResult{}; out.firstMismatch = UINT32_MAX;
    if (!validPoll(poll)) return kIOReturnBadArgument;
    lockOwners();
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    if (state_ != State::Running || !dataRange(kDataStart, 4096)) return finish(kIOReturnNotReady);
    auto* data = reinterpret_cast<volatile uint32_t*>(dataCpu_ + kDataStart);
    // Publish a canary page; three separated CP writes must change only their
    // target words. This verifies GPU execution and catches address/stride bugs.
    for (uint32_t i = 0; i < 1024; ++i) data[i] = 0x6a93e500U ^ (i * 0x01010101U);
    const uint32_t indices[] = {1, 511, 1022}, values[] = {0x71c539ab, 0x029ef164, 0xc8065da2};
    for (unsigned i = 0; i < 3; ++i) {
        const auto status = writeLocked(kDataStart + indices[i] * 4, values[i], poll);
        if (status != kIOReturnSuccess) return finish(status);
    }
    if (!syncCpu(this)) return finish(fault());
    for (uint32_t i = 0; i < 1024; ++i) {
        uint32_t expected = 0x6a93e500U ^ (i * 0x01010101U);
        for (unsigned j = 0; j < 3; ++j) if (i == indices[j]) expected = values[j];
        const uint32_t observed = data[i]; ++out.wordsCompared;
        if (observed != expected) {
            out.firstMismatch = i; out.expected = expected; out.observed = observed;
            return finish(fault());
        }
    }
    out.completed = true;
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviGraphicsQueue::computeLocked(gfxcompute::Operation operation,
    uint64_t source, uint64_t destination, uint32_t bytes, const uint32_t* pattern,
    const gfxqueue::PollOptions& poll) {
    if (!gart_ || !gart_->normalAccessReady()) return kIOReturnOffline;
    if (state_ != State::Running || !shaderReady_) return kIOReturnNotReady;
    if (!validPoll(poll) || !bytes || (bytes & 15) || (destination & 15) ||
        !dataRange(destination, bytes) ||
        (operation == gfxcompute::Operation::Copy && ((source & 15) || !dataRange(source, bytes))) ||
        (operation == gfxcompute::Operation::Clear && !pattern)) return kIOReturnBadArgument;
    if (!providerReady()) return fault();
    gfxcompute::Request request = {};
    request.operation = operation; request.shader = {shaderGpu_, gfxcompute::kShaderImageBytes};
    request.destination = {dataGpu_ + destination, bytes}; request.bytes = bytes;
    if (operation == gfxcompute::Operation::Copy) request.source = {dataGpu_ + source, bytes};
    else for (unsigned i = 0; i < 4; ++i) request.pattern[i] = pattern[i];
    uint32_t commands[gfxcompute::kMaxCommandDwords] = {}; size_t words = 0;
    computeStatus_ = gfxcompute::encode(request, commands, gfxcompute::kMaxCommandDwords, words);
    if (computeStatus_ != gfxcompute::Status::Ok) return kIOReturnBadArgument;
    memcpy(dataCpu_ + kRingBytes, commands, words * sizeof(uint32_t));
    gart_->mmioActive_ = true;
    // submitIndirect publishes all modified Data/WB + HDP, then the actual IB.
    // Its RELEASE_MEM cache-writeback fence follows this IB's CS drain. Every
    // command, shader and data allocation remains pinned through observation.
    if (gfxqueue::submitIndirect(io(), queue_, dataGpu_ + kRingBytes, uint32_t(words), queueResult_) != gfxqueue::Status::Ok ||
        gfxqueue::waitFence(io(), queue_, poll, queueResult_) != gfxqueue::Status::Ok) return fault();
    return kIOReturnSuccess;
}
IOReturn HackyNaviGraphicsQueue::clearBuffer(uint64_t offset, uint32_t bytes,
    const uint32_t (&pattern)[4], const gfxqueue::PollOptions& poll) {
    lockOwners(); return finish(computeLocked(gfxcompute::Operation::Clear, 0, offset, bytes, pattern, poll));
}
IOReturn HackyNaviGraphicsQueue::copyBuffer(uint64_t source, uint64_t destination, uint32_t bytes,
    const gfxqueue::PollOptions& poll) {
    lockOwners(); return finish(computeLocked(gfxcompute::Operation::Copy, source, destination, bytes, nullptr, poll));
}
IOReturn HackyNaviGraphicsQueue::runShaderTest(const gfxqueue::PollOptions& poll, ShaderTestResult& out) {
    if (!gart_ || !gart_->normalAccessReady()) return kIOReturnOffline;
    out = ShaderTestResult{}; out.firstMismatch = UINT32_MAX;
    if (!validPoll(poll)) return kIOReturnBadArgument;
    lockOwners();
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    constexpr uint64_t source = kDataStart + 4096, destination = kDataStart + 3 * 4096;
    constexpr uint32_t bytes = 4096, count = 5 * 4096 / 4;
    if (state_ != State::Running || !shaderReady_ || !dataRange(kDataStart, uint64_t(count) * 4))
        return finish(kIOReturnNotReady);
    if (!providerReady() || !syncCpu(this)) return finish(fault());
    auto* data = reinterpret_cast<volatile uint32_t*>(dataCpu_ + kDataStart);
    const uint32_t pattern[4] = {0x1bf7ac83, 0xe136ac04, 0x326891df, 0xbc057ea2};
    for (uint32_t i = 0; i < count; ++i) data[i] = 0x642ae703U ^ (i * 0x01030d07U);
    auto result = computeLocked(gfxcompute::Operation::Clear, 0, destination, bytes, pattern, poll);
    if (result != kIOReturnSuccess) return finish(result);
    ++out.dispatchesCompleted;
    if (!syncCpu(this)) return finish(fault());
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t expected = 0x642ae703U ^ (i * 0x01030d07U);
        if (i >= 3 * 1024 && i < 4 * 1024) expected = pattern[i & 3];
        const uint32_t observed = data[i]; ++out.wordsCompared;
        if (observed != expected) {
            out.firstMismatch = i; out.expected = expected; out.observed = observed; return finish(fault());
        }
    }
    result = computeLocked(gfxcompute::Operation::Copy, source, destination, bytes, nullptr, poll);
    if (result != kIOReturnSuccess) return finish(result);
    ++out.dispatchesCompleted;
    if (!syncCpu(this)) return finish(fault());
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t reference = i >= 3 * 1024 && i < 4 * 1024 ? i - 2 * 1024 : i;
        const uint32_t expected = 0x642ae703U ^ (reference * 0x01030d07U);
        const uint32_t observed = data[i]; ++out.wordsCompared;
        if (observed != expected) {
            out.firstMismatch = i; out.expected = expected; out.observed = observed; return finish(fault());
        }
    }
    out.completed = true;
    IOLog("HackyNavi: GFX11 shader clear/copy completed; %u words compared\n", out.wordsCompared);
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviGraphicsQueue::stop(const gfxqueue::PollOptions& poll) {
    if (!validPoll(poll)) return kIOReturnBadArgument;
    lockOwners();
    if (state_ == State::Retired || state_ == State::QuiescedRetained) return finish(kIOReturnSuccess);
    if (!hardwareHold_) {
        const auto result = releaseAllocationsLocked();
        if (result == kIOReturnSuccess) state_ = State::Retired;
        return finish(result);
    }
    if (!providerReady()) return finish(fault());
    if (state_ == State::Prepared) {
        if (!neverStartedRetirementReadyLocked()) return finish(fault());
        neverStartedRetirement_ = true;
        state_ = State::QuiescedRetained;
        return finish(kIOReturnSuccess);
    }
    gart_->mmioActive_ = true;
    if (queue_.needsRetention() && gfxqueue::stop(io(), queue_, poll, queueResult_) != gfxqueue::Status::Ok)
        return finish(fault());
    // CSIB is independent of the CP ring. Do not release its owner merely
    // because the CP halted: RLC/SRM retirement or full reset must follow.
    state_ = State::QuiescedRetained;
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviGraphicsQueue::retireAfterQuiescence(const gfxinit::RetirementPollOptions& poll) {
    if (!poll.iterations || poll.iterations > 1000000 || !poll.intervalUs || poll.intervalUs > 1000 ||
        uint64_t(poll.iterations) * poll.intervalUs > 10000000) return kIOReturnBadArgument;
    lockOwners();
    if (state_ == State::Retired) return finish(kIOReturnSuccess);
    if (state_ != State::QuiescedRetained || queue_.needsRetention() || queue_.pending())
        return finish(kIOReturnNotReady);
    if (!providerReady()) return finish(fault());
    if (neverStartedRetirement_ && !neverStartedRetirementReadyLocked()) return finish(fault());
    // Retirement stops RLC and is terminal for this epoch. Revoke readiness
    // BEFORE any I/O, including on failure; freeing the queue lease must not
    // permit a new queue to reuse the old autoload/SMU CPU state.
    gart_->firmwareReady_ = false;
    firmware_->state_ = HackyNaviFirmwareController::State::Quiesced;
    gart_->mmioActive_ = true;
    const gfxinit::RetirementIo gcIo = {this, read32, write32, delayUs};
    if (gfxinit::retireClearState(gart_->ipLayout_, gart_->registerBytes_, firmware_->smu_, gart_->allocator_,
        gcIo, poll, initState_, initResult_) != gfxinit::Status::Ok || initState_.needsClearStateRetention())
        return finish(fault());
    firmware_->rlcRetired_ = true;
    const auto result = releaseAllocationsLocked();
    if (result != kIOReturnSuccess) return finish(fault());
    state_ = State::Retired;
    const bool dropHold = hardwareHold_; hardwareHold_ = false;
    IOLog("HackyNavi: graphics queue and RLC clear-state references retired\n");
    finish(kIOReturnSuccess);
    if (dropHold) release();
    return kIOReturnSuccess;
}
IOReturn HackyNaviGraphicsQueue::releaseAllocationsLocked() {
    if (shader_.handle.owner) {
        if (gart_->allocator_.release(shader_.handle) != VramAllocatorStatus::Ok) return kIOReturnInternalError;
        shader_ = VramAllocation{}; shaderGpu_ = 0; shaderReady_ = false;
    }
    if (clearState_.handle.owner) {
        if (gart_->allocator_.release(clearState_.handle) != VramAllocatorStatus::Ok) return kIOReturnInternalError;
        clearState_ = VramAllocation{};
    }
    if (storageLease_) {
        if (gart_->queueOwner_ != this) return kIOReturnInternalError;
        gart_->queueOwner_ = nullptr; storageLease_ = false;
    }
    return kIOReturnSuccess;
}
IOReturn HackyNaviGraphicsQueue::releaseAfterDeviceReset() {
    lockOwners();
    if (hardwareHold_ && !gart_->accessReady()) return finish(kIOReturnOffline);
    const auto result = releaseAllocationsLocked();
    if (result != kIOReturnSuccess) return finish(result);
    state_ = State::Retired;
    const bool dropHold = hardwareHold_; hardwareHold_ = false;
    finish(kIOReturnSuccess);
    if (dropHold) release();
    return kIOReturnSuccess;
}
void HackyNaviGraphicsQueue::free() {
    // Published references pin the object, so ordinary free is reachable only
    // before CSIB publication or after verified hardware retirement.
    if (gart_) {
        IOLockLock(gart_->lock_); (void)releaseAllocationsLocked(); IOLockUnlock(gart_->lock_);
    }
    if (workspace_) { IOFree(workspace_, kWorkspaceBytes); workspace_ = nullptr; }
    if (gart_) { gart_->release(); gart_ = nullptr; }
    if (firmware_) { firmware_->release(); firmware_ = nullptr; }
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    OSObject::free();
}
