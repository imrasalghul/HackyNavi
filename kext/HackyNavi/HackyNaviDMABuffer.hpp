// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DMA_BUFFER_HPP
#define HACKYNAVI_DMA_BUFFER_HPP

#include <IOKit/IOReturn.h>
#include <stdint.h>

class IOService;
class IOPCIDevice;
class IOMapper;
class IODMACommand;
class IOBufferMemoryDescriptor;

// Owned, wired kernel memory for future Navi31 page tables/rings. This helper
// does not open the device, enable DMA, program GPU registers, or submit work.
// All calls and CPU accesses must be serialized by the owner in a sleepable
// client context, outside an IOWorkLoop gate/interrupt/spinlock. Allocation and
// mapper resolution may block. No method is internally thread-safe.
class HackyNaviDMABuffer final {
public:
    enum : uint32_t {
        kPageBytes = 4096,
        kMaxBytes = 16U * 1024U * 1024U,
        kMaxAlignment = 2U * 1024U * 1024U,
        kMaxSegments = 256
    };
    struct Options {
        uint64_t bytes;       // 4KiB multiple, 4KiB..16MiB; never rounded silently.
        uint32_t alignment;   // Power of two, 4KiB..2MiB; CPU and first DMA byte.
        uint8_t addressBits;  // 32..44: Linux GMC11 system-DMA policy, not GPU VA.
        uint32_t maxSegments; // 1..256. Use 1 for a contiguous DMA ring/table.
    };
    struct Segment {
        uint64_t dmaAddress;  // Device-visible I/O bus address, NOT CPU physical.
        uint64_t length;      // Concatenate segments in order to cover the buffer.
    };
    enum class MappingKind { Unallocated, DeviceMapper, SystemIdentity };

    HackyNaviDMABuffer();
    ~HackyNaviDMABuffer();
    HackyNaviDMABuffer(const HackyNaviDMABuffer&) = delete;
    HackyNaviDMABuffer& operator=(const HackyNaviDMABuffer&) = delete;

    // openOwner must already own device->open(openOwner). It must keep the
    // device open/alive and its DMA domain stable until reset() finishes.
    // A second allocate on an allocated object returns Busy without changing it.
    // Failure unwinds; CPU/DMA addresses are exposed only after full validation.
    IOReturn allocate(IOPCIDevice* device, IOService* openOwner, const Options& options);

    // Caller MUST first stop every GPU/DMA use and remove references from GPU
    // page tables/rings. Destruction has the same precondition. Teardown errors
    // require treating the device as unusable; no DMA restart is safe to assume.
    IOReturn reset();

    void* cpuAddress() const { return ready_ ? cpuAddress_ : nullptr; }
    uint64_t size() const { return ready_ ? bytes_ : 0; }
    uint32_t segmentCount() const { return ready_ ? segmentCount_ : 0; }
    const Segment* segment(uint32_t index) const;
    MappingKind mappingKind() const { return ready_ ? mappingKind_ : MappingKind::Unallocated; }

    // Supports coherent, snooped x86 PCI DMA with normal cached kernel memory.
    // These order CPU accesses and call IODMACommand::synchronize. They do NOT
    // wait for GPU completion, flush GPU caches, invalidate GPU TLBs, or replace
    // ring/doorbell barriers. The owner must arrange CPU/GPU ownership and use
    // appropriate atomic/volatile accesses to any concurrently observed words.
    // No non-snooped DMA, write-combining mapping, or bounce buffer is supported.
    IOReturn synchronizeForDevice();
    IOReturn synchronizeForCPU();

private:
    IOReturn selectMapper();
    IOReturn enumerateSegments(const Options& options);
    bool ownerIsOpen() const;

    IOPCIDevice* device_;
    IOService* openOwner_; // Borrowed; owner must outlive this object.
    IOMapper* mapper_;
    IOBufferMemoryDescriptor* memory_;
    IODMACommand* command_;
    Segment* segments_;
    void* cpuAddress_;
    uint64_t bytes_;
    uint32_t segmentCapacity_;
    uint32_t segmentCount_;
    MappingKind mappingKind_;
    bool memoryPrepared_;
    bool ready_;
};

#endif
