// SPDX-License-Identifier: MIT
#include "HackyNaviVMContext.hpp"
#include "HackyNaviGARTController.hpp"
#include "HackyNaviPciAccess.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/libkern.h>

#if !defined(__x86_64__)
#error HackyNaviVMContext requires coherent x86_64 DMA
#endif
OSDefineMetaClassAndStructors(HackyNaviVMContext, OSObject)
using namespace hackynavi;
namespace {
bool validPoll(const vmcontext::Poll& poll) {
    return poll.iterations && poll.iterations <= 100000 && poll.delayUs &&
        poll.delayUs <= 10 && uint64_t(poll.iterations) * poll.delayUs <= 100000;
}
bool range(uint64_t offset, uint64_t bytes, uint64_t total) {
    return bytes && offset <= total && bytes <= total - offset;
}
bool sameConfig(const vmcontext::Config& a, const vmcontext::Config& b) {
    return a.vmid == b.vmid && a.epoch == b.epoch && a.vaBegin == b.vaBegin && a.vaEnd == b.vaEnd &&
        a.root.address == b.root.address && a.root.bytes == b.root.bytes &&
        a.root.domain == b.root.domain && a.root.snooped == b.root.snooped;
}
}
bool HackyNaviVMContext::init() {
    gart_ = nullptr; pages_ = nullptr; vmid_ = 0; epoch_ = residentBytes_ = 0;
    hardwareHold_ = registryHold_ = reusableDetached_ = false;
    state_ = State::Empty; allocationResult_ = kIOReturnSuccess;
    submittedQueue_ = nullptr; submittedSequence_ = submittedProducer_ = 0;
    captureGeneration_ = submittedCaptureGeneration_ = 0;
    for (auto& buffer : buffers_) buffer = Buffer{};
    for (auto& mapping : mappings_) mapping = Mapping{};
    mmResult_ = vmcontext::Result{}; gfxResult_ = vmcontext::Result{};
    capturedClass_ = ShaderClass::None; captured_ = gfxisolated::Request{};
    capturedCompiler_ = gfxaco::Request{}; capturedProduct_ = nullptr; capturedGraphics_ = nullptr;
    capturedResult_ = CapturedResult{}; capturedIndirectGpu_ = 0;
    capturedShader_ = capturedIndirect_ = capturedParameters_ = Handle{}; capturedWords_ = 0;
    capturedAttributes_ = Handle{};
    return OSObject::init();
}
HackyNaviVMContext* HackyNaviVMContext::withGART(HackyNaviGARTController* gart,
    uint8_t vmid, uint64_t epoch, IOReturn& result) {
    result = kIOReturnNoMemory;
    auto* context = new HackyNaviVMContext;
    if (!context) return nullptr;
    if (!context->init()) { context->release(); return nullptr; }
    result = context->initialize(gart, vmid, epoch);
    if (result != kIOReturnSuccess) { context->release(); return nullptr; }
    return context;
}
IOReturn HackyNaviVMContext::initialize(HackyNaviGARTController* gart, uint8_t vmid, uint64_t epoch) {
    if (!gart || !gart->lock_ || vmid < 1 || vmid > 7 || !epoch) return kIOReturnBadArgument;
    if (!gart->normalAccessReady()) return kIOReturnOffline;
    gart_ = gart; gart_->retain(); vmid_ = vmid; epoch_ = epoch;
    IOLockLock(gart_->lock_);
    if (gart_->vmAdmissionClosed_ || gart_->state_ == HackyNaviGARTController::State::Empty ||
        gart_->state_ == HackyNaviGARTController::State::FaultedRetained ||
        gart_->state_ == HackyNaviGARTController::State::Retired || !providerReady(false))
        return finish(kIOReturnNotReady);
    if (gart_->vmOwners_[vmid - 1]) return finish(kIOReturnBusy);
    pages_ = static_cast<vmpt::Page*>(IOMalloc(sizeof(vmpt::Page) * vmpt::kMaxPages));
    if (!pages_) return finish(kIOReturnNoMemory);
    const auto status = tree_.initialize(pages_, vmpt::kMaxPages, {this, allocatePage, releasePage});
    if (status != vmpt::Status::Ok) return finish(treeStatus(status));
    // Publish a fully initialized strong registry entry while the VMID mutex
    // is still held. Factory failure has no registry reference to unwind.
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    retain(); registryHold_ = true; gart_->vmOwners_[vmid - 1] = this;
    state_ = State::Prepared;
    return finish(kIOReturnSuccess);
}
bool HackyNaviVMContext::leased() const {
    return gart_ && vmid_ >= 1 && vmid_ <= 7 && gart_->vmOwners_[vmid_ - 1] == this;
}
bool HackyNaviVMContext::providerReady(bool dma) const {
    return gart_ && gart_->accessReady() && nativepci::ready(gart_->device_, gart_->openOwner_,
        dma ? nativepci::BusMaster::Enabled : nativepci::BusMaster::Any) && gart_->accessReady();
}
IOReturn HackyNaviVMContext::mutableState() const {
    if (!gart_->normalAccessReady()) return kIOReturnOffline;
    if (gart_->vmAdmissionClosed_) return kIOReturnNotPermitted;
    if (hardwareHold_ || state_ == State::Captured) return kIOReturnNotPermitted;
    if (!leased() || state_ != State::Prepared || !tree_.ready()) return kIOReturnNotReady;
    if (gart_->state_ == HackyNaviGARTController::State::FaultedRetained ||
        gart_->state_ == HackyNaviGARTController::State::Retired || !providerReady(false))
        return kIOReturnOffline;
    return kIOReturnSuccess;
}
HackyNaviVMContext::Buffer* HackyNaviVMContext::find(Handle handle) {
    if (!handle.id || handle.id > kBufferSlots || !handle.generation) return nullptr;
    Buffer& buffer = buffers_[handle.id - 1];
    return buffer.dma && !buffer.internal && buffer.generation == handle.generation ? &buffer : nullptr;
}
IOReturn HackyNaviVMContext::finish(IOReturn result) {
    gart_->mmioActive_ = false; IOLockUnlock(gart_->lock_); return result;
}
IOReturn HackyNaviVMContext::treeStatus(vmpt::Status status) {
    switch (status) {
    case vmpt::Status::Ok: return kIOReturnSuccess;
    case vmpt::Status::InvalidArgument: return kIOReturnBadArgument;
    case vmpt::Status::InvalidState: return kIOReturnNotReady;
    case vmpt::Status::NoMemory:
        return allocationResult_ == kIOReturnSuccess ? kIOReturnNoMemory : allocationResult_;
    case vmpt::Status::NoSpace: return kIOReturnNoSpace;
    case vmpt::Status::Overlap: return kIOReturnExclusiveAccess;
    case vmpt::Status::MissingMapping: return kIOReturnNotFound;
    case vmpt::Status::CorruptTree:
    case vmpt::Status::ReleaseFailed:
        state_ = hardwareHold_ ? State::FaultedRetained : State::FailedUnpublished;
        return kIOReturnIOError;
    }
    return kIOReturnInternalError;
}
bool HackyNaviVMContext::allocatePage(void* opaque, vmpt::Storage& storage) {
    auto* self = static_cast<HackyNaviVMContext*>(opaque);
    if (!self->gart_->normalAccessReady()) {self->allocationResult_=kIOReturnOffline;return false;}
    auto* dma = new HackyNaviDMABuffer;
    if (!dma) { self->allocationResult_ = kIOReturnNoMemory; return false; }
    self->allocationResult_ = dma->allocate(self->gart_->device_, self->gart_->openOwner_,
        {vmpt::kPageBytes, vmpt::kPageBytes, 44, 1});
    if (self->allocationResult_ != kIOReturnSuccess) {
        self->gart_->latchVmStorageFailureLocked(dma->teardownFailure());
        delete dma; return false;
    }
    if (!self->gart_->normalAccessReady()) {
        const auto cleanup=dma->reset();delete dma;
        self->gart_->latchVmStorageFailureLocked(cleanup);
        self->allocationResult_=cleanup==kIOReturnSuccess?kIOReturnOffline:cleanup;return false;
    }
    const auto* segment = dma->segment(0);
    if (dma->segmentCount() != 1 || !segment || segment->length != vmpt::kPageBytes) {
        const auto result = dma->reset(); delete dma;
        self->gart_->latchVmStorageFailureLocked(result);
        self->allocationResult_ = result == kIOReturnSuccess ? kIOReturnInternalError : result;
        return false;
    }
    storage = {static_cast<uint8_t*>(dma->cpuAddress()), segment->dmaAddress, dma};
    return true;
}
bool HackyNaviVMContext::releasePage(void* opaque, vmpt::Storage& storage) {
    auto* self = static_cast<HackyNaviVMContext*>(opaque);
    auto* dma = static_cast<HackyNaviDMABuffer*>(storage.owner);
    if (!dma) { self->gart_->latchVmStorageFailureLocked(kIOReturnIOError); return false; }
    const IOReturn result = dma->reset(); delete dma; storage = vmpt::Storage{};
    // Also covers partial page-tree allocation rollback and factory cleanup.
    self->gart_->latchVmStorageFailureLocked(result);
    return result == kIOReturnSuccess;
}
IOReturn HackyNaviVMContext::allocateBuffer(uint64_t bytes, uint32_t alignment, Handle& handle) {
    handle = Handle{};
    if (!gart_) return kIOReturnNotReady;
    IOLockLock(gart_->lock_);
    return finish(allocateBufferLocked(bytes, alignment, handle));
}
IOReturn HackyNaviVMContext::allocateBufferLocked(uint64_t bytes, uint32_t alignment, Handle& handle) {
    handle = Handle{};
    const auto ready = mutableState(); if (ready != kIOReturnSuccess) return (ready);
    if (!bytes || (bytes & 4095) || bytes > kMaxBufferBytes || alignment < 4096 ||
        alignment > HackyNaviDMABuffer::kMaxAlignment || (alignment & (alignment - 1)))
        return (kIOReturnBadArgument);
    if (bytes > kResidentBudget - residentBytes_) return (kIOReturnNoSpace);
    uint32_t slot = 0;
    while (slot < kBufferSlots && (buffers_[slot].dma || buffers_[slot].generation == UINT32_MAX)) ++slot;
    if (slot == kBufferSlots) return (kIOReturnNoSpace); // no generation wrap/reuse
    auto* dma = new HackyNaviDMABuffer;
    if (!dma) return (kIOReturnNoMemory);
    const IOReturn result = dma->allocate(gart_->device_, gart_->openOwner_,
        {bytes, alignment, 44, HackyNaviDMABuffer::kMaxSegments});
    if (result != kIOReturnSuccess) {
        gart_->latchVmStorageFailureLocked(dma->teardownFailure());
        delete dma; return (result);
    }
    if (!gart_->normalAccessReady()) {
        const auto cleanup=dma->reset();delete dma;gart_->latchVmStorageFailureLocked(cleanup);
        return cleanup==kIOReturnSuccess?kIOReturnOffline:cleanup;
    }
    Buffer& buffer = buffers_[slot];
    ++buffer.generation; buffer.dma = dma; buffer.mappings = 0; buffer.internal = false; residentBytes_ += bytes;
    handle = {slot + 1, buffer.generation};
    return (kIOReturnSuccess);
}
IOReturn HackyNaviVMContext::freeBuffer(Handle handle) {
    if (!gart_) return kIOReturnNotReady;
    IOLockLock(gart_->lock_);
    const auto ready = mutableState(); if (ready != kIOReturnSuccess) return finish(ready);
    auto* buffer = find(handle); if (!buffer) return finish(kIOReturnNotFound);
    if (buffer->mappings) return finish(kIOReturnBusy);
    residentBytes_ -= buffer->dma->size();
    const auto result = buffer->dma->reset(); delete buffer->dma; buffer->dma = nullptr;
    gart_->latchVmStorageFailureLocked(result);
    if (result != kIOReturnSuccess) state_ = State::FailedUnpublished;
    return finish(result);
}
bool HackyNaviVMContext::copyPages(const Buffer& buffer, uint64_t offset, uint64_t bytes, uint64_t* pages) {
    uint64_t remaining = bytes; size_t n = 0;
    for (uint32_t s = 0; s < buffer.dma->segmentCount() && remaining; ++s) {
        const auto* segment = buffer.dma->segment(s);
        if (!segment || !segment->length || (segment->length & 4095) || (segment->dmaAddress & 4095) ||
            segment->dmaAddress >= gmc11::kSystemDmaAddressLimit ||
            segment->length > gmc11::kSystemDmaAddressLimit - segment->dmaAddress) return false;
        if (offset >= segment->length) { offset -= segment->length; continue; }
        const uint64_t available = segment->length - offset;
        const uint64_t copied = remaining < available ? remaining : available;
        for (uint64_t p = 0; p < copied; p += 4096) pages[n++] = segment->dmaAddress + offset + p;
        remaining -= copied; offset = 0;
    }
    return remaining == 0;
}
IOReturn HackyNaviVMContext::map(Handle handle, uint64_t offset, uint64_t va,
    uint64_t bytes, uint32_t access) {
    if (!gart_) return kIOReturnNotReady;
    IOLockLock(gart_->lock_);
    return finish(mapLocked(handle, offset, va, bytes, access));
}
IOReturn HackyNaviVMContext::mapLocked(Handle handle, uint64_t offset, uint64_t va,
    uint64_t bytes, uint32_t access) {
    const auto ready = mutableState(); if (ready != kIOReturnSuccess) return (ready);
    auto* buffer = find(handle); if (!buffer) return (kIOReturnNotFound);
    if (!range(offset, bytes, buffer->dma->size()) || ((offset | va | bytes) & 4095) ||
        va < vmpt::kVaBegin || va >= vmpt::kVaEnd || bytes > vmpt::kVaEnd - va ||
        !(access & Read) || (access & ~(Read | Write | Execute))) return (kIOReturnBadArgument);
    uint32_t slot = kMappingSlots;
    for (uint32_t m = 0; m < kMappingSlots; ++m) {
        const Mapping& other = mappings_[m];
        if (!other.bytes) { if (slot == kMappingSlots) slot = m; continue; }
        if (va < other.va + other.bytes && other.va < va + bytes) return (kIOReturnExclusiveAccess);
    }
    if (slot == kMappingSlots) return (kIOReturnNoSpace);
    const size_t count = static_cast<size_t>(bytes / 4096), addressBytes = count * sizeof(uint64_t);
    auto* addresses = static_cast<uint64_t*>(IOMalloc(addressBytes));
    if (!addresses) return (kIOReturnNoMemory);
    if (!copyPages(*buffer, offset, bytes, addresses)) {
        IOFree(addresses, addressBytes); state_ = State::FailedUnpublished; return (kIOReturnIOError);
    }
    const gmc11::LeafAttributes attributes = {gmc11::MemoryDomain::SystemDma,
        gmc11::MemoryType::Uncached, true, true, bool(access & Write), bool(access & Execute), false};
    const auto status = tree_.map(va, addresses, count, attributes);
    IOFree(addresses, addressBytes);
    if (status != vmpt::Status::Ok) return (treeStatus(status));
    mappings_[slot] = {handle, va, bytes, offset, access}; ++buffer->mappings;
    return (kIOReturnSuccess);
}
IOReturn HackyNaviVMContext::unmap(Handle handle, uint64_t va, uint64_t bytes) {
    if (!gart_) return kIOReturnNotReady;
    IOLockLock(gart_->lock_);
    const auto ready = mutableState(); if (ready != kIOReturnSuccess) return finish(ready);
    auto* buffer = find(handle); if (!buffer) return finish(kIOReturnNotFound);
    for (auto& mapping : mappings_) {
        if (mapping.bytes && mapping.buffer.id == handle.id && mapping.buffer.generation == handle.generation &&
            mapping.va == va && mapping.bytes == bytes) {
            const auto status = tree_.unmap(va, static_cast<size_t>(bytes / 4096));
            if (status != vmpt::Status::Ok) return finish(treeStatus(status));
            mapping = Mapping{}; --buffer->mappings; return finish(kIOReturnSuccess);
        }
    }
    return finish(kIOReturnNotFound);
}
IOReturn HackyNaviVMContext::writeBuffer(Handle handle, uint64_t offset, const void* data, size_t bytes) {
    if (!gart_) return kIOReturnNotReady;
    IOLockLock(gart_->lock_);
    const auto ready = mutableState(); if (ready != kIOReturnSuccess) return finish(ready);
    auto* buffer = find(handle); if (!buffer) return finish(kIOReturnNotFound);
    if (!data || !range(offset, bytes, buffer->dma->size()) ||
        bytes - 1 > UINTPTR_MAX - reinterpret_cast<uintptr_t>(data)) return finish(kIOReturnBadArgument);
    bcopy(data, static_cast<uint8_t*>(buffer->dma->cpuAddress()) + offset, bytes);
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviVMContext::readBuffer(Handle handle, uint64_t offset, void* data, size_t bytes) {
    if (!gart_) return kIOReturnNotReady;
    IOLockLock(gart_->lock_);
    const auto ready = mutableState(); if (ready != kIOReturnSuccess) return finish(ready);
    auto* buffer = find(handle); if (!buffer) return finish(kIOReturnNotFound);
    if (!data || !range(offset, bytes, buffer->dma->size()) ||
        bytes - 1 > UINTPTR_MAX - reinterpret_cast<uintptr_t>(data)) return finish(kIOReturnBadArgument);
    const auto result = gart_->synchronizeCpuBuffer(*buffer->dma);
    if (result != kIOReturnSuccess) return finish(result);
    bcopy(static_cast<const uint8_t*>(buffer->dma->cpuAddress()) + offset, data, bytes);
    return finish(kIOReturnSuccess);
}
bool HackyNaviVMContext::bindingLocked(Handle handle, uint64_t offset, uint64_t bytes, uint32_t access,
    gfxisolated::Binding& binding, uint64_t& gpu) {
    auto* buffer = find(handle);
    if (!buffer || !range(offset, bytes, buffer->dma->size())) return false;
    for (const auto& mapping : mappings_) {
        if (mapping.buffer.id != handle.id || mapping.buffer.generation != handle.generation ||
            !mapping.bytes || (mapping.access & access) != access || offset < mapping.offset ||
            offset - mapping.offset >= mapping.bytes || bytes > mapping.bytes - (offset - mapping.offset)) continue;
        gpu = mapping.va + (offset - mapping.offset);
        binding = {reinterpret_cast<uintptr_t>(this), epoch_, handle.id, handle.generation,
            mapping.va, mapping.bytes, mapping.offset, buffer->dma->size(), mapping.access};
        return true;
    }
    return false;
}
bool HackyNaviVMContext::controlVaLocked(uint64_t& va, uint64_t bytes) const {
    if (!bytes || (bytes & 4095) || bytes > kMaxBufferBytes) return false;
    va = vmpt::kVaEnd - bytes;
    // Each conflict strictly lowers the candidate below an existing mapping.
    // Private controls therefore need no fixed/global identity aperture.
    for (uint32_t pass = 0; pass <= kMappingSlots; ++pass) {
        bool conflict = false;
        for (const auto& mapping : mappings_) {
            if (!mapping.bytes || va + bytes <= mapping.va || mapping.va + mapping.bytes <= va) continue;
            if (mapping.va < vmpt::kVaBegin + bytes) return false;
            va = mapping.va - bytes; conflict = true; break;
        }
        if (!conflict) return true;
    }
    return false;
}
bool HackyNaviVMContext::captureValidLocked() const {
    return (state_ == State::Captured || state_ == State::Active) && captureStorageValidLocked();
}
bool HackyNaviVMContext::captureStorageValidLocked() const {
    if (gart_->vmAdmissionClosed_ || !leased() || !tree_.ready() || !captureGeneration_ ||
        !capturedWords_ || !capturedShader_.id || !capturedIndirect_.id) return false;
    if (capturedClass_ == ShaderClass::Gfx1100LeafBufferV1) return productCaptureValidLocked();
    if (capturedClass_ == ShaderClass::Gfx1100PositionConstantColorV1 ||
        capturedClass_ == ShaderClass::Gfx1100PositionFragmentBufferV1 ||
        capturedClass_ == ShaderClass::Gfx1100PositionVertexBufferV1 ||
        capturedClass_ == ShaderClass::Gfx1100PositionVertexFragmentBuffersV1) return graphicsCaptureValidLocked();
    if (capturedProduct_ || capturedGraphics_ || capturedAttributes_.id || capturedAttributes_.generation) return false;
    const gfxisolated::Binding* bindings[5] = {};
    const gfxisolated::Binding* destination = nullptr;
    unsigned count = 0, controls = 0;
    const uint64_t owner = reinterpret_cast<uintptr_t>(this);
    switch (capturedClass_) {
    case ShaderClass::BuiltInClearCopy:
        if (capturedWords_ > gfxcompute::kMaxCommandDwords || captured_.owner != owner ||
            captured_.epoch != epoch_ || captured_.vmid != vmid_ || capturedParameters_.id ||
            capturedParameters_.generation) return false;
        bindings[0] = &captured_.shader; bindings[1] = &captured_.indirect;
        bindings[2] = &captured_.destination; bindings[3] = &captured_.source;
        destination = &captured_.destination;
        count = captured_.dispatch.operation == gfxcompute::Operation::Copy ? 4 : 3; controls = 2;
        break;
    case ShaderClass::AppleAddOneGfx1100V1:
        if (capturedWords_ > gfxaco::kMaxCommandDwords || capturedCompiler_.owner != owner ||
            capturedCompiler_.epoch != epoch_ || capturedCompiler_.vmid != vmid_ ||
            !capturedParameters_.id || capturedParameters_.id != capturedCompiler_.parameters.id ||
            capturedParameters_.generation != capturedCompiler_.parameters.generation) return false;
        bindings[0] = &capturedCompiler_.shader; bindings[1] = &capturedCompiler_.indirect;
        bindings[2] = &capturedCompiler_.parameters; bindings[3] = &capturedCompiler_.destination;
        bindings[4] = &capturedCompiler_.source; destination = &capturedCompiler_.destination;
        count = 5; controls = 3;
        break;
    default: return false;
    }
    if (capturedShader_.id != bindings[0]->id || capturedShader_.generation != bindings[0]->generation ||
        capturedIndirect_.id != bindings[1]->id || capturedIndirect_.generation != bindings[1]->generation ||
        capturedIndirectGpu_ != bindings[1]->gpu || capturedResult_.buffer.id != destination->id ||
        capturedResult_.buffer.generation != destination->generation) return false;
    for (unsigned n = 0; n < count; ++n) {
        const auto& binding = *bindings[n];
        if (!binding.id || binding.id > kBufferSlots || binding.owner != owner ||
            binding.epoch != epoch_) return false;
        const auto& buffer = buffers_[binding.id - 1];
        if (!buffer.dma || !buffer.dma->cpuAddress() || buffer.generation != binding.generation ||
            buffer.dma->size() != binding.allocationBytes || buffer.internal != (n < controls) ||
            (n < controls && (buffer.dma->size() != 4096 || buffer.mappings != 1))) return false;
        bool mapped = false;
        for (const auto& mapping : mappings_) {
            if (mapping.buffer.id == binding.id && mapping.buffer.generation == binding.generation &&
                mapping.va == binding.gpu && mapping.bytes == binding.bytes &&
                mapping.offset == binding.bufferOffset && mapping.access == binding.access) mapped = true;
        }
        if (!mapped) return false;
    }
    // The mapping ledger/tree and all byte-copy entry points have been sealed
    // since capture. Private controls are never exported or writable by a GPU
    // alias; recheck the retained class and canonical stream before publication.
    const auto* commands = static_cast<const uint32_t*>(buffers_[capturedIndirect_.id - 1].dma->cpuAddress());
    if (capturedClass_ == ShaderClass::BuiltInClearCopy) {
        if (gfxisolated::validate(captured_, commands, capturedWords_) != gfxisolated::Status::Ok) return false;
        return capturedResult_.offset == destination->bufferOffset +
            (captured_.dispatch.destination.gpu - destination->gpu) &&
            capturedResult_.bytes == captured_.dispatch.bytes;
    }
    const auto* shader = static_cast<const uint8_t*>(buffers_[capturedShader_.id - 1].dma->cpuAddress());
    const auto* parameters = static_cast<const uint8_t*>(buffers_[capturedParameters_.id - 1].dma->cpuAddress());
    return gfxaco::validate(capturedCompiler_, shader, gfxaco::kControlBytes, parameters,
        gfxaco::kControlBytes, commands, capturedWords_) == gfxaco::Status::Ok &&
        compilerStorageDistinctLocked(capturedCompiler_) &&
        capturedResult_.offset == destination->bufferOffset + capturedCompiler_.destinationOffset &&
        capturedResult_.bytes == gfxaco::kDataBytes;
}
bool HackyNaviVMContext::compilerStorageDistinctLocked(const gfxaco::Request& captured) const {
    const gfxisolated::Binding* bindings[] = {&captured.shader, &captured.indirect,
        &captured.parameters, &captured.source, &captured.destination};
    // Every BO here comes from a fresh owned IOBufferMemoryDescriptor; this
    // class cannot import or alias someone else's wired storage. In addition
    // to the exact generation ledger, reject duplicate DMA owners, overlapping
    // CPU allocations and overlapping device-visible I/O ranges. I/O bus
    // addresses are not claimed to be CPU physical addresses.
    for (unsigned n = 0; n < 5; ++n) {
        const auto& b = *bindings[n];
        if (!b.id || b.id > kBufferSlots) return false;
        const auto& buffer = buffers_[b.id - 1];
        auto* dma = buffer.dma;
        if (!dma || buffer.generation != b.generation || !dma->cpuAddress() || !dma->size() ||
            !dma->segmentCount() || dma->segmentCount() > HackyNaviDMABuffer::kMaxSegments) return false;
        const uintptr_t cpu = reinterpret_cast<uintptr_t>(dma->cpuAddress());
        if (dma->size() - 1 > UINTPTR_MAX - cpu) return false;
        for (uint32_t s = 0; s < dma->segmentCount(); ++s) {
            const auto* segment = dma->segment(s);
            if (!segment || !segment->length || segment->dmaAddress >= gmc11::kSystemDmaAddressLimit ||
                segment->length > gmc11::kSystemDmaAddressLimit - segment->dmaAddress) return false;
        }
        for (unsigned previous = 0; previous < n; ++previous) {
            auto* other = buffers_[bindings[previous]->id - 1].dma;
            const uintptr_t otherCpu = reinterpret_cast<uintptr_t>(other->cpuAddress());
            if (b.id == bindings[previous]->id || dma == other ||
                (cpu <= otherCpu ? otherCpu - cpu < dma->size() : cpu - otherCpu < other->size())) return false;
            for (uint32_t s = 0; s < dma->segmentCount(); ++s)
                for (uint32_t p = 0; p < other->segmentCount(); ++p) {
                    const auto* a = dma->segment(s); const auto* o = other->segment(p);
                    if (a->dmaAddress < o->dmaAddress + o->length &&
                        o->dmaAddress < a->dmaAddress + a->length) return false;
                }
        }
    }
    return true;
}
IOReturn HackyNaviVMContext::beginCaptureLocked() {
    // Called before even the first private control allocation. A previous
    // capture must have been completely consumed; partial or stale metadata
    // is never adopted by a later job, and exhausted identities never wrap.
    if (capturedClass_ != ShaderClass::None || capturedProduct_ || capturedGraphics_ || capturedWords_ ||
        capturedAttributes_.id || capturedAttributes_.generation ||
        capturedShader_.id || capturedShader_.generation || capturedIndirect_.id || capturedIndirect_.generation ||
        capturedParameters_.id || capturedParameters_.generation || submittedQueue_ ||
        submittedSequence_ || submittedProducer_ || submittedCaptureGeneration_)
        return kIOReturnNotPermitted;
    if (captureGeneration_ == UINT64_MAX) return kIOReturnNoResources;
    ++captureGeneration_;
    return kIOReturnSuccess;
}
IOReturn HackyNaviVMContext::captureCompute(const ComputeRequest& request) {
    if (!gart_) return kIOReturnNotReady;
    IOLockLock(gart_->lock_);
    const auto ready = mutableState(); if (ready != kIOReturnSuccess) return finish(ready);
    const auto generation = beginCaptureLocked(); if (generation != kIOReturnSuccess) return finish(generation);
    const bool copy = request.operation == gfxcompute::Operation::Copy;
    if ((!copy && request.operation != gfxcompute::Operation::Clear) || !request.bytes ||
        (request.bytes & 15) || (request.destinationOffset & 15) ||
        (copy && (request.sourceOffset & 15)) || (!copy &&
        (request.source.id || request.source.generation || request.sourceOffset))) return finish(kIOReturnBadArgument);
    gfxisolated::Request captured{};
    captured.owner = reinterpret_cast<uintptr_t>(this); captured.epoch = epoch_; captured.vmid = vmid_;
    uint64_t sourceGpu = 0, destinationGpu = 0;
    if (!bindingLocked(request.destination, request.destinationOffset, request.bytes, Read | Write,
            captured.destination, destinationGpu) || (copy &&
        !bindingLocked(request.source, request.sourceOffset, request.bytes, Read, captured.source, sourceGpu)))
        return finish(kIOReturnNotFound);
    if (copy && request.source.id == request.destination.id &&
        request.sourceOffset < request.destinationOffset + request.bytes &&
        request.destinationOffset < request.sourceOffset + request.bytes) return finish(kIOReturnBadArgument);
    // From here any allocation/encoding failure seals the unpublished context.
    // It can be released safely, but partially built control storage is never
    // exposed as a normal user BO or silently reused by a later capture.
    Handle shader{}, indirect{}; uint64_t shaderVa = 0, indirectVa = 0;
    auto status = allocateBufferLocked(4096, 4096, shader);
    if (status == kIOReturnSuccess && !controlVaLocked(shaderVa)) status = kIOReturnNoSpace;
    if (status == kIOReturnSuccess) status = mapLocked(shader, 0, shaderVa, 4096, Read | Execute);
    if (status == kIOReturnSuccess) status = allocateBufferLocked(4096, 4096, indirect);
    if (status == kIOReturnSuccess && !controlVaLocked(indirectVa)) status = kIOReturnNoSpace;
    if (status == kIOReturnSuccess) status = mapLocked(indirect, 0, indirectVa, 4096, Read | Execute);
    if (status == kIOReturnSuccess &&
        (!bindingLocked(shader, 0, 4096, Read | Execute, captured.shader, shaderVa) ||
         !bindingLocked(indirect, 0, 4096, Read | Execute, captured.indirect, indirectVa))) status = kIOReturnInternalError;
    if (status != kIOReturnSuccess) {
        if (shader.id) buffers_[shader.id - 1].internal = true;
        if (indirect.id) buffers_[indirect.id - 1].internal = true;
        state_ = State::FailedUnpublished; return finish(status);
    }
    auto& shaderBuffer = buffers_[shader.id - 1]; auto& indirectBuffer = buffers_[indirect.id - 1];
    shaderBuffer.internal = indirectBuffer.internal = true;
    captured.dispatch = {request.operation, {shaderVa, 4096},
        {sourceGpu, copy ? uint64_t(request.bytes) : 0}, {destinationGpu, request.bytes},
        request.bytes, {request.pattern[0], request.pattern[1], request.pattern[2], request.pattern[3]}};
    auto* commands = static_cast<uint32_t*>(indirectBuffer.dma->cpuAddress()); size_t words = 0;
    if (gfxcompute::shaderImage(static_cast<uint8_t*>(shaderBuffer.dma->cpuAddress()), 4096) != gfxcompute::Status::Ok ||
        gfxcompute::encode(captured.dispatch, commands, 4096 / 4, words) != gfxcompute::Status::Ok ||
        gfxisolated::validate(captured, commands, words) != gfxisolated::Status::Ok) {
        state_ = State::FailedUnpublished; return finish(kIOReturnBadArgument);
    }
    // These private controls have exactly one RX mapping and no writable CPU
    // export. All public mutators now reject State::Captured, before activation.
    captured_ = captured; capturedShader_ = shader; capturedIndirect_ = indirect;
    capturedClass_ = ShaderClass::BuiltInClearCopy; capturedIndirectGpu_ = captured.indirect.gpu;
    capturedResult_ = {request.destination, request.destinationOffset, request.bytes};
    capturedWords_ = static_cast<uint32_t>(words); state_ = State::Captured;
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviVMContext::captureCompilerCompute(const CompilerComputeRequest& request) {
    if (!gart_) return kIOReturnNotReady;
    IOLockLock(gart_->lock_);
    const auto ready = mutableState(); if (ready != kIOReturnSuccess) return finish(ready);
    const auto generation = beginCaptureLocked(); if (generation != kIOReturnSuccess) return finish(generation);
    if ((request.sourceOffset & 3) || (request.destinationOffset & 3) ||
        request.source.id == request.destination.id) return finish(kIOReturnBadArgument);
    gfxaco::Request captured{};
    captured.owner = reinterpret_cast<uintptr_t>(this); captured.epoch = epoch_; captured.vmid = vmid_;
    uint64_t sourceGpu = 0, destinationGpu = 0;
    if (!bindingLocked(request.source, request.sourceOffset, gfxaco::kDataBytes, Read, captured.source, sourceGpu) ||
        !bindingLocked(request.destination, request.destinationOffset, gfxaco::kDataBytes, Read | Write,
            captured.destination, destinationGpu)) return finish(kIOReturnNotFound);
    if (captured.source.access != Read || captured.destination.access != (Read | Write))
        return finish(kIOReturnNotPermitted);
    captured.sourceOffset = sourceGpu - captured.source.gpu;
    captured.destinationOffset = destinationGpu - captured.destination.gpu;
    // All partial controls remain owned but private on failure. No page root,
    // register or queue has been published; releaseBeforeExposure can unwind
    // the entire context, including any page-table allocation made by map.
    Handle controls[3] = {};
    gfxisolated::Binding* bindings[] = {&captured.shader, &captured.indirect, &captured.parameters};
    const uint32_t permissions[] = {Read | Execute, Read | Execute, Read};
    IOReturn status = kIOReturnSuccess;
    for (unsigned n = 0; n < 3 && status == kIOReturnSuccess; ++n) {
        uint64_t va = 0;
        status = allocateBufferLocked(gfxaco::kControlBytes, 4096, controls[n]);
        if (status == kIOReturnSuccess && !controlVaLocked(va)) status = kIOReturnNoSpace;
        if (status == kIOReturnSuccess) status = mapLocked(controls[n], 0, va, gfxaco::kControlBytes, permissions[n]);
        if (status == kIOReturnSuccess && !bindingLocked(controls[n], 0, gfxaco::kControlBytes,
                permissions[n], *bindings[n], va)) status = kIOReturnInternalError;
    }
    for (const auto& handle : controls) if (handle.id) buffers_[handle.id - 1].internal = true;
    if (status != kIOReturnSuccess) { state_ = State::FailedUnpublished; return finish(status); }
    auto* shader = static_cast<uint8_t*>(buffers_[controls[0].id - 1].dma->cpuAddress());
    auto* commands = static_cast<uint32_t*>(buffers_[controls[1].id - 1].dma->cpuAddress());
    auto* parameters = static_cast<uint8_t*>(buffers_[controls[2].id - 1].dma->cpuAddress());
    size_t words = 0;
    if (!compilerStorageDistinctLocked(captured) ||
        gfxaco::shaderImage(shader, gfxaco::kControlBytes) != gfxaco::Status::Ok ||
        gfxaco::parameterImage(captured, parameters, gfxaco::kControlBytes) != gfxaco::Status::Ok ||
        gfxaco::encode(captured, commands, gfxaco::kControlBytes / 4, words) != gfxaco::Status::Ok ||
        gfxaco::validate(captured, shader, gfxaco::kControlBytes, parameters, gfxaco::kControlBytes,
            commands, words) != gfxaco::Status::Ok) {
        state_ = State::FailedUnpublished; return finish(kIOReturnBadArgument);
    }
    capturedClass_ = ShaderClass::AppleAddOneGfx1100V1; capturedCompiler_ = captured;
    capturedShader_ = controls[0]; capturedIndirect_ = controls[1]; capturedParameters_ = controls[2];
    capturedIndirectGpu_ = captured.indirect.gpu;
    capturedResult_ = {request.destination, request.destinationOffset, gfxaco::kDataBytes};
    capturedWords_ = static_cast<uint32_t>(words); state_ = State::Captured;
    return finish(kIOReturnSuccess);
}
bool HackyNaviVMContext::read32(void* opaque, uint32_t offset, uint32_t& value) {
    auto* self = static_cast<HackyNaviVMContext*>(opaque);
    return HackyNaviGARTController::read32(self->gart_, offset, value);
}
bool HackyNaviVMContext::write32(void* opaque, uint32_t offset, uint32_t value) {
    auto* self = static_cast<HackyNaviVMContext*>(opaque);
    return HackyNaviGARTController::write32(self->gart_, offset, value);
}
void HackyNaviVMContext::delayUs(void*, uint32_t us) { IODelay(us); }
bool HackyNaviVMContext::publish(void* opaque) {
    auto* self = static_cast<HackyNaviVMContext*>(opaque);
    // Both hubs consume the SAME retained tree. Tables and initialized BO data
    // must be visible before root publication and before each TLB invalidation.
    for (uint16_t p = 0; p < self->tree_.pageCount(); ++p) {
        auto* dma = static_cast<HackyNaviDMABuffer*>(self->tree_.page(p)->storage.owner);
        if (!dma || self->gart_->synchronizeDeviceBuffer(*dma) != kIOReturnSuccess) return false;
    }
    for (auto& buffer : self->buffers_)
        if (buffer.dma && self->gart_->synchronizeDeviceBuffer(*buffer.dma) != kIOReturnSuccess) return false;
    return HackyNaviGARTController::hdpFlush(self->gart_);
}
vmcontext::Io HackyNaviVMContext::io() { return {this, read32, write32, delayUs, publish}; }
IOReturn HackyNaviVMContext::activate(const vmcontext::Poll& poll) {
    if (!gart_) return kIOReturnNotReady;
    IOLockLock(gart_->lock_);
    // Captured jobs can only be exposed by their sole submitting queue. A
    // direct activation must never acquire completion-retirement provenance.
    if (state_ == State::Captured || submittedQueue_) return finish(kIOReturnNotPermitted);
    return finish(activateLocked(poll));
}
IOReturn HackyNaviVMContext::activateLocked(const vmcontext::Poll& poll) {
    if (!gart_->normalAccessReady()) return kIOReturnOffline;
    if (gart_->vmAdmissionClosed_ || hardwareHold_ || !leased() || !tree_.ready() ||
        (state_ != State::Prepared && state_ != State::Captured)) return kIOReturnNotReady;
    if (!validPoll(poll)) return (kIOReturnBadArgument);
    if (gart_->state_ != HackyNaviGARTController::State::BothEnabled || !gart_->firmwareOwner_ ||
        !gart_->firmwareReady_ || !providerReady(true)) return (kIOReturnNotReady);
    const auto* root = tree_.page(0); if (!root) return (kIOReturnInternalError);
    const vmcontext::Config config = {vmid_, {root->storage.dmaAddress, 4096,
        gmc11::MemoryDomain::SystemDma, true}, vmpt::kVaBegin, vmpt::kVaEnd, epoch_};
    const bool reactivate = reusableDetached_;
    if (reactivate) {
        if (state_ != State::Captured || !submittedQueue_ || submittedSequence_ || submittedProducer_ ||
            !captureGeneration_ || submittedCaptureGeneration_ != captureGeneration_ ||
            mm_.phase() != vmcontext::Phase::Detached || gfx_.phase() != vmcontext::Phase::Detached ||
            !mm_.requiresHardwareRetirement() || !gfx_.requiresHardwareRetirement() ||
            !sameConfig(mm_.config(), config) || !sameConfig(gfx_.config(), config)) return kIOReturnNotPermitted;
    } else if (mm_.phase() != vmcontext::Phase::Fresh || gfx_.phase() != vmcontext::Phase::Fresh ||
        mm_.requiresHardwareRetirement() || gfx_.requiresHardwareRetirement()) return kIOReturnNotPermitted;
    // Even preflight failure now preserves storage: no caller may infer whether
    // a callback's failed write or DMA publication reached the device.
    retain(); hardwareHold_ = true; gart_->mmioActive_ = true;
    reusableDetached_ = false;
    const auto mmStatus = reactivate ? vmcontext::reactivate(mm_, io(), poll, mmResult_) :
        vmcontext::enable(gart_->ipLayout_, gart_->registerBytes_, vmcontext::Hub::Mm,
            config, io(), poll, mm_, mmResult_);
    if (mmStatus != vmcontext::Status::Ok) {
        state_ = State::FaultedRetained; return kIOReturnIOError;
    }
    const auto gfxStatus = reactivate ? vmcontext::reactivate(gfx_, io(), poll, gfxResult_) :
        vmcontext::enable(gart_->ipLayout_, gart_->registerBytes_, vmcontext::Hub::Gfx,
            config, io(), poll, gfx_, gfxResult_);
    if (gfxStatus != vmcontext::Status::Ok) {
        state_ = State::FaultedRetained; return (kIOReturnIOError);
    }
    state_ = State::Active; return (kIOReturnSuccess);
}
IOReturn HackyNaviVMContext::detach(const vmcontext::Poll& poll) {
    if (!gart_) return kIOReturnNotReady;
    IOLockLock(gart_->lock_);
    if (submittedQueue_) return finish(kIOReturnNotPermitted);
    return finish(detachLocked(poll));
}
IOReturn HackyNaviVMContext::detachLocked(const vmcontext::Poll& poll) {
    if (gart_->vmAdmissionClosed_) return kIOReturnNotPermitted;
    if (!gart_->accessReady()) return kIOReturnOffline;
    if (!leased() || state_ != State::Active) return kIOReturnNotReady;
    if (!validPoll(poll)) return kIOReturnBadArgument;
    if (gart_->state_ != HackyNaviGARTController::State::BothEnabled || !providerReady(true)) {
        state_ = State::FaultedRetained; return kIOReturnOffline;
    }
    gart_->mmioActive_ = true;
    if (vmcontext::detach(gfx_, io(), poll, gfxResult_) != vmcontext::Status::Ok ||
        vmcontext::detach(mm_, io(), poll, mmResult_) != vmcontext::Status::Ok) {
        state_ = State::FaultedRetained; return kIOReturnIOError;
    }
    // Keep the VMID lease and ALL DMA mappings. ACK/disable is not retirement.
    state_ = State::DetachedRetained; return kIOReturnSuccess;
}
IOReturn HackyNaviVMContext::consumeCapturedStorageLocked() {
    // The queue has already revoked its completed-job token, proved idle and
    // detached both hubs while holding the outer operation/queue/GART locks.
    // These checks bind its cleanup to this exact retained capture; they are
    // not an independent DMA-retirement certificate.
    const auto failed = [&](IOReturn status) -> IOReturn {
        state_ = State::FaultedRetained;
        gart_->latchVmStorageFailureLocked(status);
        return status;
    };
    if (!gart_->normalAccessReady()) return failed(kIOReturnOffline);
    if (gart_->vmAdmissionClosed_ || gart_->vmRetirementFailure_ != kIOReturnSuccess ||
        state_ != State::DetachedRetained || !hardwareHold_ || !registryHold_ || reusableDetached_ ||
        !leased() || !tree_.ready() || !submittedQueue_ || !submittedSequence_ || !submittedProducer_ ||
        !captureGeneration_ || submittedCaptureGeneration_ != captureGeneration_ ||
        mm_.phase() != vmcontext::Phase::Detached || gfx_.phase() != vmcontext::Phase::Detached ||
        !mm_.requiresHardwareRetirement() || !gfx_.requiresHardwareRetirement())
        return failed(kIOReturnNotPermitted);
    const auto* root = tree_.page(0);
    if (!root || !providerReady(true)) return failed(kIOReturnOffline);
    const vmcontext::Config config = {vmid_, {root->storage.dmaAddress, 4096,
        gmc11::MemoryDomain::SystemDma, true}, vmpt::kVaBegin, vmpt::kVaEnd, epoch_};
    uint64_t controls = 0;
    if (!sameConfig(mm_.config(), config) || !sameConfig(gfx_.config(), config) ||
        !captureStorageValidLocked() || !capturedControlMaskLocked(controls)) return failed(kIOReturnNotPermitted);

    // Preflight the complete ledger before changing any PTE or releasing any
    // DMA owner. Every private control has exactly one whole-allocation R/RX
    // mapping, except the exact graphics capture's private RW attribute ring.
    // Data mappings and all BO generations remain unchanged.
    uint32_t mappingCounts[kBufferSlots] = {};
    uint64_t resident = 0, mappedControls = 0;
    for (uint32_t n = 0; n < kBufferSlots; ++n) {
        const auto& b = buffers_[n];
        if (!b.dma) {
            if (b.mappings || b.internal) return failed(kIOReturnInternalError);
            continue;
        }
        const uint64_t bytes = b.dma->size();
        if (!b.generation || !bytes || (bytes & 4095) || bytes > kMaxBufferBytes ||
            bytes > kResidentBudget - resident) return failed(kIOReturnInternalError);
        resident += bytes;
    }
    if (resident != residentBytes_) return failed(kIOReturnInternalError);
    for (const auto& m : mappings_) if (m.bytes) {
        if (!m.buffer.id || m.buffer.id > kBufferSlots) return failed(kIOReturnInternalError);
        const auto& b = buffers_[m.buffer.id - 1];
        if (!b.dma || m.buffer.generation != b.generation || !range(m.offset, m.bytes, b.dma->size()) ||
            ((m.offset | m.bytes | m.va) & 4095) || m.va < vmpt::kVaBegin || m.va >= vmpt::kVaEnd ||
            m.bytes > vmpt::kVaEnd - m.va || !(m.access & Read) || (m.access & ~(Read | Write | Execute)))
            return failed(kIOReturnInternalError);
        ++mappingCounts[m.buffer.id - 1];
        const uint64_t bit = UINT64_C(1) << (m.buffer.id - 1);
        if (controls & bit) {
            if ((mappedControls & bit) || m.offset || m.bytes != b.dma->size() ||
                ((m.access & Write) && !graphicsWritableControlLocked(m)))
                return failed(kIOReturnInternalError);
            mappedControls |= bit;
        }
    }
    if (mappedControls != controls) return failed(kIOReturnInternalError);
    for (uint32_t n = 0; n < kBufferSlots; ++n)
        if (mappingCounts[n] != buffers_[n].mappings) return failed(kIOReturnInternalError);

    for (auto& m : mappings_) if (m.bytes && (controls & (UINT64_C(1) << (m.buffer.id - 1)))) {
        if (!gart_->normalAccessReady()) return failed(kIOReturnOffline);
        const auto status = tree_.unmap(m.va, static_cast<size_t>(m.bytes / 4096));
        if (status != vmpt::Status::Ok) return failed(treeStatus(status));
        --buffers_[m.buffer.id - 1].mappings;
        m = Mapping{};
    }
    // Both contexts are still disabled. No page-tree storage is released and
    // no table publication is attempted here; reactivation publishes all PTE
    // changes only after the same stored root and disabled controls match.
    for (uint32_t n = 0; n < kBufferSlots; ++n) if (controls & (UINT64_C(1) << n)) {
        if (!gart_->normalAccessReady()) return failed(kIOReturnOffline);
        auto& b = buffers_[n];
        const uint64_t bytes = b.dma->size();
        const auto status = b.dma->reset();
        delete b.dma; b.dma = nullptr; b.internal = false; residentBytes_ -= bytes;
        if (status != kIOReturnSuccess) return failed(status);
    }
    if (!gart_->normalAccessReady()) return failed(kIOReturnOffline);
    releaseProductCaptureLocked();
    releaseGraphicsCaptureLocked(); capturedAttributes_ = Handle{};
    capturedClass_ = ShaderClass::None; captured_ = gfxisolated::Request{};
    capturedCompiler_ = gfxaco::Request{}; capturedResult_ = CapturedResult{}; capturedIndirectGpu_ = 0;
    capturedShader_ = capturedIndirect_ = capturedParameters_ = Handle{}; capturedWords_ = 0;
    submittedQueue_ = nullptr; submittedSequence_ = submittedProducer_ = 0; submittedCaptureGeneration_ = 0;
    reusableDetached_ = true; state_ = State::Prepared;
    // hardwareHold_ intentionally remains true until the queue clears it
    // under the same locks and drops the corresponding reference outside.
    return kIOReturnSuccess;
}
IOReturn HackyNaviVMContext::releaseStorageLocked() {
    IOReturn result = tree_.release() == vmpt::Status::Ok ? kIOReturnSuccess : kIOReturnIOError;
    for (auto& buffer : buffers_) {
        if (buffer.dma) {
            const auto status = buffer.dma->reset();
            gart_->latchVmStorageFailureLocked(status);
            if (result == kIOReturnSuccess) result = status;
            delete buffer.dma; buffer.dma = nullptr; buffer.mappings = 0;
        }
    }
    for (auto& mapping : mappings_) mapping = Mapping{};
    releaseProductCaptureLocked();
    releaseGraphicsCaptureLocked(); capturedAttributes_ = Handle{};
    capturedClass_ = ShaderClass::None; captured_ = gfxisolated::Request{};
    capturedCompiler_ = gfxaco::Request{}; capturedResult_ = CapturedResult{}; capturedIndirectGpu_ = 0;
    capturedShader_ = capturedIndirect_ = capturedParameters_ = Handle{}; capturedWords_ = 0;
    submittedQueue_ = nullptr; submittedSequence_ = submittedProducer_ = 0;
    submittedCaptureGeneration_ = 0; reusableDetached_ = false;
    residentBytes_ = 0;
    if (leased()) gart_->vmOwners_[vmid_ - 1] = nullptr;
    state_ = State::Retired;
    // Direct pre-exposure/reset release and free() use this same path. The
    // first exact DMA error (including page-tree callbacks) survives loss of
    // the registry entry; repeated release of empty storage returns it too.
    gart_->latchVmStorageFailureLocked(result);
    return gart_->vmRetirementFailure_;
}
IOReturn HackyNaviVMContext::releaseBeforeExposure() {
    if (!gart_) return kIOReturnNotReady;
    IOLockLock(gart_->lock_);
    if (hardwareHold_) return finish(kIOReturnNotPermitted);
    const auto result = releaseStorageLocked();
    const bool drop = registryHold_; registryHold_ = false;
    finish(result);
    if (drop) release(); // drop the registry hold outside the GART mutex
    return result;
}
IOReturn HackyNaviVMContext::releaseAfterVerifiedReset(const HackyNaviResetController::RetirementProof& proof) {
    if (!gart_) return kIOReturnNotReady;
    if (!gart_->accessReady()) return kIOReturnOffline;
    // The caller holds the OUTER device epoch lock through verification, every
    // owner's release and finishRetirementTransfer. Do not invert Reset->GART
    // locking: validate the typed proof before taking the shared GART mutex.
    const auto verified = proof.validateFor(gart_->device_, gart_->openOwner_, gart_->registerMap_);
    if (verified != kIOReturnSuccess) return verified;
    if (!gart_->accessReady()) return kIOReturnOffline;
    IOLockLock(gart_->lock_);
    const auto result = releaseStorageLocked();
    const unsigned drops = unsigned(hardwareHold_) + unsigned(registryHold_);
    hardwareHold_ = registryHold_ = false;
    finish(result);
    // Both publication and registry holds may be present. A registry snapshot
    // or caller keeps this object alive until the final release; no member is
    // accessed after dropping these references.
    for (unsigned i = 0; i < drops; ++i) release();
    return result;
}
void HackyNaviVMContext::snapshot(Snapshot& out) {
    out = Snapshot{};
    if (!gart_) { out.state = state_; return; }
    IOLockLock(gart_->lock_);
    out.state = state_; out.vmid = vmid_; out.epoch = epoch_; out.residentBytes = residentBytes_;
    out.tablePages = tree_.pageCount(); out.requiresHardwareRetirement = hardwareHold_;
    out.mm = mmResult_; out.gfx = gfxResult_;
    for (const auto& buffer : buffers_) if (buffer.dma) ++out.buffers;
    for (const auto& mapping : mappings_) if (mapping.bytes) ++out.mappings;
    finish(kIOReturnSuccess);
}
void HackyNaviVMContext::free() {
    // A strong registry entry and publication hold prevent destruction while
    // leased/exposed. Only factory failure or explicit retirement reaches here.
    if (gart_) {
        IOLockLock(gart_->lock_); (void)releaseStorageLocked(); IOLockUnlock(gart_->lock_);
        gart_->release(); gart_ = nullptr;
    }
    if (pages_) { IOFree(pages_, sizeof(vmpt::Page) * vmpt::kMaxPages); pages_ = nullptr; }
    OSObject::free();
}
