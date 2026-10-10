// SPDX-License-Identifier: MIT
#include "HackyNaviClientMemory.hpp"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/task.h>
#include <libkern/libkern.h>
#include <mach/vm_param.h>

static_assert(PAGE_SIZE == HackyNaviClientMemory::kPageBytes,
              "This client ABI targets x86_64 macOS pages");

HackyNaviClientMemory::HackyNaviClientMemory()
    : task_(nullptr), memory_(nullptr), mapping_(nullptr), bytes_(nullptr),
      logicalBytes_(0), allocatedBytes_(0), clientAddress_(0) {}
HackyNaviClientMemory::~HackyNaviClientMemory() { reset(); }

IOReturn HackyNaviClientMemory::allocate(task_t task, size_t logicalBytes,
    Access access, const void* initial, size_t initialLength) {
    if (task_ || memory_ || mapping_) return kIOReturnBusy;
    if (!task || task == kernel_task || task != current_task()) return kIOReturnNotPermitted;
    if (!logicalBytes || logicalBytes > kMaxBytes || initialLength > logicalBytes ||
        (initialLength && !initial) || (!initialLength && initial) ||
        (access != Access::ReadOnly && access != Access::ReadWrite))
        return kIOReturnBadArgument;
    // kMaxBytes is page aligned and the addition cannot overflow.
    const size_t allocation = (logicalBytes + kPageBytes - 1) & ~(kPageBytes - 1);
    task_ = task;
    task_reference(task_);
    memory_ = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIOMemoryKernelUserShared | kIODirectionInOut, allocation, kPageBytes);
    if (!memory_) { reset(); return kIOReturnNoMemory; }
    bytes_ = static_cast<uint8_t*>(memory_->getBytesNoCopy());
    if (!bytes_ || memory_->getLength() != allocation ||
        (reinterpret_cast<uintptr_t>(bytes_) & (kPageBytes - 1))) {
        reset(); return kIOReturnError;
    }
    bzero(bytes_, allocation);
    if (initialLength) bcopy(initial, bytes_, initialLength);
    IOOptionBits options = kIOMapAnywhere | kIOMapUnique;
    if (access == Access::ReadOnly) options |= kIOMapReadOnly;
    mapping_ = memory_->createMappingInTask(task_, 0, options, 0, allocation);
    if (!mapping_) { reset(); return kIOReturnNoMemory; }
    const uint64_t address = mapping_->getAddress();
    if (!address || (address & (kPageBytes - 1)) ||
        mapping_->getLength() != allocation || address > UINT64_MAX - allocation) {
        reset(); return kIOReturnError;
    }
    logicalBytes_ = logicalBytes;
    allocatedBytes_ = allocation;
    clientAddress_ = address;
    return kIOReturnSuccess;
}

void HackyNaviClientMemory::reset() {
    // The unique map has no shared caller references. Its final release
    // destroys the task mapping before its separately owned backing memory.
    if (mapping_) { mapping_->release(); mapping_ = nullptr; }
    if (memory_) { memory_->release(); memory_ = nullptr; }
    if (task_) { task_deallocate(task_); task_ = nullptr; }
    bytes_ = nullptr;
    logicalBytes_ = allocatedBytes_ = 0;
    clientAddress_ = 0;
}

IOReturn HackyNaviClientMemory::clientAddress(task_t caller, uint64_t& address) const {
    if (!ready()) return kIOReturnNotReady;
    if (!caller || caller != task_ || caller != current_task()) return kIOReturnNotPermitted;
    address = clientAddress_;
    return kIOReturnSuccess;
}
bool HackyNaviClientMemory::contains(size_t offset, size_t count) const {
    return ready() && offset <= logicalBytes_ && count <= logicalBytes_ - offset;
}
IOReturn HackyNaviClientMemory::copyFromClient(size_t offset, void* output, size_t count) const {
    if (!ready()) return kIOReturnNotReady;
    if (!contains(offset, count) || (count && !output)) return kIOReturnBadArgument;
    if (count) bcopy(bytes_ + offset, output, count);
    return kIOReturnSuccess;
}
IOReturn HackyNaviClientMemory::copyToClient(size_t offset, const void* input, size_t count) {
    if (!ready()) return kIOReturnNotReady;
    if (!contains(offset, count) || (count && !input)) return kIOReturnBadArgument;
    if (count) bcopy(input, bytes_ + offset, count);
    return kIOReturnSuccess;
}
IOReturn HackyNaviClientMemory::load32(size_t offset, uint32_t& value) const {
    if (!ready()) return kIOReturnNotReady;
    if ((offset & 3U) || !contains(offset, sizeof(value))) return kIOReturnBadArgument;
    value = __atomic_load_n(reinterpret_cast<const uint32_t*>(bytes_ + offset), __ATOMIC_ACQUIRE);
    return kIOReturnSuccess;
}
IOReturn HackyNaviClientMemory::store32(size_t offset, uint32_t value) {
    if (!ready()) return kIOReturnNotReady;
    if ((offset & 3U) || !contains(offset, sizeof(value))) return kIOReturnBadArgument;
    __atomic_store_n(reinterpret_cast<uint32_t*>(bytes_ + offset), value, __ATOMIC_RELEASE);
    return kIOReturnSuccess;
}
