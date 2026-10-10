// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_GART_STORAGE_HPP
#define HACKYNAVI_GART_STORAGE_HPP

#include "HackyNaviDMABuffer.hpp"
#include <stddef.h>

// Owned system-memory buffers plus a CPU-only flat GMC11 GART PTE image.
// Nothing here allocates VRAM, uploads the table, chooses its GPU address,
// programs a hub, invalidates a TLB, enables DMA, or submits GPU work.
// Caller serializes every method/access in a sleepable context and keeps the
// device open and its DMA domain stable under the DMABuffer ownership contract.
class HackyNaviGARTStorage final {
public:
    enum : uint64_t {
        kDefaultApertureBytes = 512ULL * 1024 * 1024,
        kMinApertureBytes = 2ULL * 1024 * 1024,
        kMaxApertureBytes = 1024ULL * 1024 * 1024
    };
    struct Options {
        uint64_t apertureBytes;  // 2MiB multiple, 2MiB..1GiB; default 512MiB.
        uint64_t dataBytes;      // 4KiB multiple, 4KiB..DMABuffer::kMaxBytes.
        uint64_t writebackBytes; // Same bounds; independently owned allocation.
        uint64_t displayBytes;   // Dedicated DMCUB runtime BO, separately owned.
    };
    enum class Region { Data, Writeback, Display };
    struct Binding {
        bool bound;
        size_t firstPage; // Index in this flat image, not an absolute GPU VA.
        size_t pageCount;
        bool executable;
    };

    HackyNaviGARTStorage();
    ~HackyNaviGARTStorage();
    HackyNaviGARTStorage(const HackyNaviGARTStorage&) = delete;
    HackyNaviGARTStorage& operator=(const HackyNaviGARTStorage&) = delete;

    static Options defaultOptions() {
        return {kDefaultApertureBytes, 64 * 1024, 4096, 512 * 1024};
    }

    // Allocates one prepared dummy DMA page and three distinct DMA buffers.
    // Display holds DMCUB VBIOS/mailbox/trace/state independently of queues.
    // Initial entries retain the dummy DMA address with ALL flags clear:
    // VALID=0 and SYSTEM=0, matching Linux's Vega10+ unbind representation.
    // The CPU image is 1MiB for the default aperture, at most 2MiB. It is not
    // itself a DMA allocation or a usable hardware page-table base.
    // Failure unwinds everything. Allocating an existing object returns Busy.
    IOReturn allocate(IOPCIDevice* device, IOService* openOwner, const Options& options);

    // Binds the WHOLE selected owned buffer. Each binding reserves an invalid
    // guard page on either side; guards cannot overlap another binding/guard.
    // Only SYSTEM, VALID, SNOOPED, READABLE, WRITEABLE, MTYPE_UC and optionally
    // EXECUTABLE are set. No imported address or CPU-physical address is accepted.
    // An already-bound region must be unbound before moving/replacing it.
    // Validation and a temporary PTE image precede the single CPU-image commit;
    // any error leaves image and binding metadata unchanged.
    IOReturn bind(Region region, size_t firstPage, bool executable = false);

    // Restores invalid dummy entries, including both guards; idempotent when
    // already unbound. This edits ONLY the CPU image and keeps all DMA owners.
    // A future uploader must separately publish changes and invalidate TLBs.
    IOReturn unbind(Region region);

    // Before reset/destruction, caller MUST quiesce all GPU/DMA users and remove
    // every hardware reference to these buffers/table. Calling unbind alone
    // does not establish that condition. Teardown errors leave the device
    // unusable; all owners are unwound and the first error is returned.
    IOReturn reset();

    bool isReady() const { return ready_; }
    const uint8_t* tableBytes() const { return ready_ ? table_ : nullptr; }
    size_t tableByteCount() const { return ready_ ? tableBytes_ : 0; }
    size_t pageCount() const { return ready_ ? pageCount_ : 0; }
    uint64_t apertureBytes() const { return ready_ ? apertureBytes_ : 0; }
    // AMD calls this a "dummy" page: it is a real, owned, prepared 4KiB DMA
    // allocation used for fault/default handling, not a mock or sentinel.
    // This getter returns its actual device-visible DMA address.
    // Returns false and zero before allocation/after reset. Zero itself is a
    // valid DMA address, so callers must check the boolean, not the address.
    bool dummyDmaAddress(uint64_t& out) const {
        out = ready_ ? dummyDmaAddress_ : 0;
        return ready_;
    }
    const Binding* binding(Region region) const;

    // Read-only owner access exposes CPU storage/segment diagnostics but cannot
    // reset/reallocate a mapped region. Returned views last until reset().
    // CPU data/writeback access must follow DMABuffer's coherent DMA ownership
    // rules. Synchronization never waits for GPU completion or updates the PTE
    // image on hardware. The image itself is ordinary serialized CPU memory.
    const HackyNaviDMABuffer* buffer(Region region) const;
    IOReturn synchronizeForDevice(Region region);
    IOReturn synchronizeForCPU(Region region);

private:
    HackyNaviDMABuffer* mutableBuffer(Region region);
    Binding* mutableBinding(Region region);

    HackyNaviDMABuffer dummy_;
    HackyNaviDMABuffer data_;
    HackyNaviDMABuffer writeback_;
    HackyNaviDMABuffer display_;
    uint8_t* table_;
    size_t tableBytes_;
    size_t pageCount_;
    uint64_t apertureBytes_;
    uint64_t dummyDmaAddress_;
    Binding dataBinding_;
    Binding writebackBinding_;
    Binding displayBinding_;
    bool ready_;
};

#endif // HACKYNAVI_GART_STORAGE_HPP
