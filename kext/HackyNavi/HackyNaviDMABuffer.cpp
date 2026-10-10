// SPDX-License-Identifier: MIT
// Independently implemented from public IOKit contracts; no CPU-physical to
// DMA-address assumption. API declarations compiled against MacKernelSDK commit
// 3f750085caa17ec3a7880f11c11bf4f48cd6a164.
// Relevant Apple implementation evidence (read, not copied):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/iokit/Kernel/IOMapper.cpp
// https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/iokit/Kernel/IODMACommand.cpp
// Public IOKit KPI exports, including IOMapper::gSystem/copyMapperForDevice and
// IODMACommand::getIOMemoryDescriptor/getPreparedOffsetAndLength:
// https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/config/IOKit.exports
// GMC11 distinguishes 48-bit MC/PTE addresses from 44-bit system DMA:
// https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/gmc_v11_0.c#L823-L832

#include "HackyNaviDMABuffer.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMapper.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/libkern.h>

#if !defined(__x86_64__)
#error HackyNaviDMABuffer currently supports coherent Intel x86_64 DMA only
#endif

namespace {
bool powerOfTwo(uint64_t value) { return value && !(value & (value - 1)); }

// A missing direct iommu-parent is insufficient evidence for identity DMA.
// Reject inherited declarations too. The iterator retains entries and detects
// registry changes. The traversal is bounded and fails closed on exhaustion.
IOReturn checkNoMapperDeclaration(IOPCIDevice* device) {
    IORegistryIterator* iterator = IORegistryIterator::iterateOver(device,
        gIOServicePlane, kIORegistryIterateParents | kIORegistryIterateRecursively);
    if (!iterator) return kIOReturnNoMemory;
    IOReturn result = kIOReturnSuccess;
    IORegistryEntry* entry = iterator->getCurrentEntry();
    unsigned visited = 0;
    while (entry) {
        if (++visited > 128) { result = kIOReturnUnsupported; break; }
        OSObject* declaration = entry->copyProperty("iommu-parent");
        if (declaration) {
            declaration->release();
            result = kIOReturnUnsupported;
            break;
        }
        entry = OSDynamicCast(IORegistryEntry, iterator->getNextObject());
    }
    if (!iterator->isValid()) result = kIOReturnNotReady;
    iterator->release();
    return result;
}

void cpuFence() {
    // A CPU ordering fence, not GPU completion or GPU cache maintenance.
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
} // namespace

HackyNaviDMABuffer::HackyNaviDMABuffer()
    : device_(nullptr), openOwner_(nullptr), mapper_(nullptr), memory_(nullptr),
      command_(nullptr), segments_(nullptr), cpuAddress_(nullptr), bytes_(0),
      segmentCapacity_(0), segmentCount_(0), mappingKind_(MappingKind::Unallocated),
      memoryPrepared_(false), ready_(false), teardownFailure_(kIOReturnSuccess) {}

HackyNaviDMABuffer::~HackyNaviDMABuffer() { (void)reset(); }

bool HackyNaviDMABuffer::ownerIsOpen() const {
    return device_ && openOwner_ && !device_->isInactive() && device_->isOpen(openOwner_);
}

IOReturn HackyNaviDMABuffer::selectMapper() {
    OSObject* declaration = device_->copyProperty("iommu-parent");
    if (declaration) {
        declaration->release();
        // Returns a retained device-associated mapper, resolving its declared
        // registry ID if necessary. This may block; never call while gated.
        mapper_ = IOMapper::copyMapperForDevice(device_);
        if (!mapper_ || mapper_->isInactive()) return kIOReturnUnsupported;
        const uint64_t pageSize = mapper_->getPageSize();
        if (!powerOfTwo(pageSize) || pageSize > kMaxAlignment)
            return kIOReturnUnsupported;
        mappingKind_ = MappingKind::DeviceMapper;
        return kIOReturnSuccess;
    }
    const IOReturn result = checkNoMapperDeclaration(device_);
    if (result != kIOReturnSuccess) return result;
    // Apple IOMapper declares resolved kNoMapper as nullptr; unknown/required
    // states and real system mappers are all non-null. Do not cast/dereference
    // the sentinels, wait indefinitely for them, or treat them as mapper-none.
    if (IOMapper::gSystem != nullptr) return kIOReturnUnsupported;
    mappingKind_ = MappingKind::SystemIdentity;
    return kIOReturnSuccess;
}

IOReturn HackyNaviDMABuffer::allocate(IOPCIDevice* device, IOService* openOwner,
                                    const Options& options) {
    if (teardownFailure_ != kIOReturnSuccess) return teardownFailure_;
    if (device_ || memory_ || command_ || ready_) return kIOReturnBusy;
    if (!device || !openOwner || options.bytes < kPageBytes || options.bytes > kMaxBytes ||
        (options.bytes & (kPageBytes - 1)) || !powerOfTwo(options.alignment) ||
        options.alignment < kPageBytes || options.alignment > kMaxAlignment ||
        options.addressBits < 32 || options.addressBits > 44 ||
        !options.maxSegments || options.maxSegments > kMaxSegments)
        return kIOReturnBadArgument;
    if (device->isInactive() || !device->isOpen(openOwner)) return kIOReturnNotOpen;
    device_ = device;
    device_->retain();
    openOwner_ = openOwner;
    bytes_ = options.bytes;
    IOReturn result = selectMapper();
    if (result != kIOReturnSuccess) { (void)reset(); return result; }

    segmentCapacity_ = options.maxSegments;
    segments_ = static_cast<Segment*>(IOMalloc(sizeof(Segment) * segmentCapacity_));
    // Default kernel allocation is wired and cached. No physical-contiguity or
    // mapper-bypass option is requested; getBytesNoCopy is the owned CPU view.
    memory_ = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut, static_cast<vm_size_t>(bytes_), options.alignment);
    if (!segments_ || !memory_) { (void)reset(); return kIOReturnNoMemory; }
    cpuAddress_ = memory_->getBytesNoCopy();
    if (!cpuAddress_ || (reinterpret_cast<uintptr_t>(cpuAddress_) & (options.alignment - 1)) ||
        memory_->getLength() != bytes_) { (void)reset(); return kIOReturnNotAligned; }
    bzero(cpuAddress_, static_cast<size_t>(bytes_));
    result = memory_->prepare(kIODirectionInOut);
    if (result != kIOReturnSuccess) { (void)reset(); return result; }
    memoryPrepared_ = true;

    IODMACommand::SegmentOptions specification = {};
    specification.fStructSize = sizeof(specification);
    // Without a mapper, a narrow command can silently substitute individual
    // bounce pages, which getIOMemoryDescriptor() cannot distinguish. Use 64
    // bits for enumeration in the OS-declared identity case and enforce the
    // requested hardware width ourselves BEFORE exposing any DMA address.
    specification.fNumAddressBits = mapper_ ? options.addressBits : 64;
    specification.fMaxSegmentSize = bytes_;
    specification.fMaxTransferSize = bytes_;
    specification.fAlignment = options.alignment;
    specification.fAlignmentLength = kPageBytes;
    specification.fAlignmentInternalSegments = kPageBytes;
    command_ = IODMACommand::withSpecification(kIODMACommandOutputHost64,
        &specification, IODMACommand::kMapped, mapper_, nullptr);
    if (!command_) { (void)reset(); return kIOReturnNoMemory; }
    result = command_->setMemoryDescriptor(memory_, false);
    if (result == kIOReturnSuccess) result = command_->prepare(0, bytes_);
    if (result != kIOReturnSuccess) { (void)reset(); return result; }
    // Full bounce buffers can be created for alignment even in mapped mode.
    // Persistent rings/page tables require the original CPU and DMA storage.
    if (command_->getIOMemoryDescriptor() != memory_) {
        (void)reset();
        return kIOReturnUnsupported;
    }
    result = enumerateSegments(options);
    if (result == kIOReturnSuccess && !ownerIsOpen()) result = kIOReturnNotOpen;
    if (result == kIOReturnSuccess && !mapper_ && IOMapper::gSystem != nullptr)
        result = kIOReturnUnsupported;
    if (result != kIOReturnSuccess) { (void)reset(); return result; }
    cpuFence();
    ready_ = true;
    return kIOReturnSuccess;
}

IOReturn HackyNaviDMABuffer::enumerateSegments(const Options& options) {
    const UInt64 addressMask = (UInt64(1) << options.addressBits) - 1;
    UInt64 offset = 0;
    UInt64 covered = 0;
    while (offset < bytes_) {
        if (segmentCount_ == segmentCapacity_) return kIOReturnNoSpace;
        IODMACommand::Segment64 generated = {};
        UInt32 count = 1;
        const IOReturn result = command_->gen64IOVMSegments(&offset, &generated, &count);
        if (result != kIOReturnSuccess) return result;
        const UInt64 address = generated.fIOVMAddr;
        const UInt64 length = generated.fLength;
        const UInt64 alignment = segmentCount_ ? kPageBytes : options.alignment;
        if (count != 1 || !length || length > bytes_ - covered ||
            (length & (kPageBytes - 1)) || (address & (alignment - 1)) ||
            address > addressMask || length - 1 > addressMask - address ||
            offset != covered + length)
            return kIOReturnUnsupported;
        // Repeated/overlapping DMA storage cannot represent distinct owned
        // buffer bytes. Both endpoints are safe after the width checks above.
        for (uint32_t previous = 0; previous < segmentCount_; ++previous) {
            const Segment& other = segments_[previous];
            if (address < other.dmaAddress + other.length &&
                other.dmaAddress < address + length) return kIOReturnUnsupported;
        }
        segments_[segmentCount_++] = {address, length};
        covered += length;
    }
    return covered == bytes_ && segmentCount_ ? kIOReturnSuccess : kIOReturnUnderrun;
}

const HackyNaviDMABuffer::Segment* HackyNaviDMABuffer::segment(uint32_t index) const {
    return ready_ && index < segmentCount_ ? &segments_[index] : nullptr;
}

IOReturn HackyNaviDMABuffer::synchronizeForDevice() {
    if (!ready_) return kIOReturnNotReady;
    if (!ownerIsOpen()) return kIOReturnNotOpen;
    cpuFence();
    const IOReturn result = command_->synchronize(kIODirectionOut);
    cpuFence();
    return result;
}

IOReturn HackyNaviDMABuffer::synchronizeForCPU() {
    if (!ready_) return kIOReturnNotReady;
    if (!ownerIsOpen()) return kIOReturnNotOpen;
    // GPU completion/ownership must have been established by the caller.
    const IOReturn result = command_->synchronize(kIODirectionIn);
    cpuFence();
    return result;
}

IOReturn HackyNaviDMABuffer::reset() {
    ready_ = false;
    cpuAddress_ = nullptr;
    segmentCount_ = 0;
    IOReturn result = kIOReturnSuccess;
    if (command_) {
        // prepare() can increment its active count before returning an error.
        // Inspect that count through this public API, so failed prepares also
        // get paired with complete(). Always detach the descriptor explicitly.
        if (command_->getPreparedOffsetAndLength(nullptr, nullptr) == kIOReturnSuccess)
            result = command_->complete();
        const IOReturn cleared = command_->clearMemoryDescriptor(true);
        if (result == kIOReturnSuccess) result = cleared;
        command_->release();
        command_ = nullptr;
    }
    if (memory_) {
        if (memoryPrepared_) {
            const IOReturn completed = memory_->complete(kIODirectionInOut);
            if (result == kIOReturnSuccess) result = completed;
        }
        memory_->release();
        memory_ = nullptr;
    }
    memoryPrepared_ = false;
    if (segments_) {
        IOFree(segments_, sizeof(Segment) * segmentCapacity_);
        segments_ = nullptr;
    }
    if (mapper_) { mapper_->release(); mapper_ = nullptr; }
    if (device_) { device_->release(); device_ = nullptr; }
    openOwner_ = nullptr;
    bytes_ = 0;
    segmentCapacity_ = 0;
    mappingKind_ = MappingKind::Unallocated;
    if (result != kIOReturnSuccess && teardownFailure_ == kIOReturnSuccess) {
        teardownFailure_ = result;
        IOLog("HackyNaviDMABuffer: DMA teardown failed (0x%x); device must remain quiesced\n", result);
    }
    return teardownFailure_;
}
