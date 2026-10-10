// SPDX-License-Identifier: MIT
// Independently implemented storage/lifetime management using the public
// HackyNaviDMABuffer and GPUVM encoders. Linux policy references at commit
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// amdgpu_gart.c:amdgpu_gart_unbind -- dummy address with flags=0 (SYSTEM clear).
// gmc_v11_0.c:gmc_v11_0_gart_init -- MTYPE_UC and EXECUTABLE for live GART PTEs.
// amdgpu_ttm.c:amdgpu_ttm_tt_pde_flags/pte_flags -- TT is VALID|SYSTEM,
// cached CPU storage is SNOOPED, and normal owned storage is readable/writable.
// This layer deliberately makes EXECUTABLE an explicit per-binding choice.
// Initial invalid fill adopts the unbind policy; it does not reproduce Linux's
// separate memset_io used when allocating a VRAM table.

#include "HackyNaviGARTStorage.hpp"
#include "hackynavi/gpu_vm.hpp"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

namespace {
using hackynavi::Status;
using namespace hackynavi::gmc11;

bool validBufferSize(uint64_t bytes) {
    return bytes >= kPageBytes && bytes <= HackyNaviDMABuffer::kMaxBytes &&
        (bytes & (kPageBytes - 1)) == 0;
}

// Binding metadata is private and always bounded to the allocated table.
bool guardsOverlap(size_t guardFirst, size_t guardEnd,
                   const HackyNaviGARTStorage::Binding& other) {
    return other.bound && guardFirst < other.firstPage + other.pageCount + 1 &&
        other.firstPage - 1 < guardEnd;
}
} // namespace

HackyNaviGARTStorage::HackyNaviGARTStorage()
    : table_(nullptr), tableBytes_(0), pageCount_(0), apertureBytes_(0),
      dummyDmaAddress_(0), dataBinding_{}, writebackBinding_{}, displayBinding_{}, ready_(false) {}

HackyNaviGARTStorage::~HackyNaviGARTStorage() { (void)reset(); }

IOReturn HackyNaviGARTStorage::allocate(IOPCIDevice* device, IOService* openOwner,
                                      const Options& options) {
    if (ready_ || table_) return kIOReturnBusy;
    if (!device || !openOwner || options.apertureBytes < kMinApertureBytes ||
        options.apertureBytes > kMaxApertureBytes ||
        options.apertureBytes % kMinApertureBytes ||
        !validBufferSize(options.dataBytes) || !validBufferSize(options.writebackBytes) ||
        !validBufferSize(options.displayBytes))
        return kIOReturnBadArgument;
    const size_t pages = static_cast<size_t>(options.apertureBytes / kPageBytes);
    // Reserve separate leading/trailing guards for all three buffers. These
    // sums are bounded by the per-buffer 16MiB limits checked above.
    if ((options.dataBytes + options.writebackBytes + options.displayBytes) / kPageBytes + 6 > pages)
        return kIOReturnNoSpace;

    pageCount_ = pages;
    apertureBytes_ = options.apertureBytes;
    tableBytes_ = pages * kEntryBytes;
    table_ = static_cast<uint8_t*>(IOMalloc(tableBytes_));
    if (!table_) { (void)reset(); return kIOReturnNoMemory; }

    HackyNaviDMABuffer::Options dma = {kPageBytes, kPageBytes, 44, 1};
    IOReturn result = dummy_.allocate(device, openOwner, dma);
    if (result == kIOReturnSuccess) {
        dma.bytes = options.dataBytes;
        dma.maxSegments = HackyNaviDMABuffer::kMaxSegments;
        result = data_.allocate(device, openOwner, dma);
    }
    if (result == kIOReturnSuccess) {
        dma.bytes = options.writebackBytes;
        result = writeback_.allocate(device, openOwner, dma);
    }
    if (result == kIOReturnSuccess) {
        dma.bytes = options.displayBytes;
        result = display_.allocate(device, openOwner, dma);
    }
    if (result != kIOReturnSuccess) { (void)reset(); return result; }

    const HackyNaviDMABuffer::Segment* dummy = dummy_.segment(0);
    if (dummy_.segmentCount() != 1 || !dummy || dummy->length != kPageBytes) {
        (void)reset();
        return kIOReturnUnsupported;
    }
    dummyDmaAddress_ = dummy->dmaAddress;
    if (writeInvalidGartPtes({table_, tableBytes_}, 0, dummyDmaAddress_, pageCount_) != Status::Ok) {
        (void)reset();
        return kIOReturnUnsupported;
    }
    result = dummy_.synchronizeForDevice();
    if (result == kIOReturnSuccess) result = data_.synchronizeForDevice();
    if (result == kIOReturnSuccess) result = writeback_.synchronizeForDevice();
    if (result == kIOReturnSuccess) result = display_.synchronizeForDevice();
    if (result != kIOReturnSuccess) { (void)reset(); return result; }
    ready_ = true;
    return kIOReturnSuccess;
}

IOReturn HackyNaviGARTStorage::bind(Region region, size_t firstPage, bool executable) {
    if (!ready_) return kIOReturnNotReady;
    HackyNaviDMABuffer* owned = mutableBuffer(region);
    Binding* selected = mutableBinding(region);
    if (!owned || !selected) return kIOReturnBadArgument;
    if (selected->bound) return kIOReturnBusy;
    const size_t pages = static_cast<size_t>(owned->size() / kPageBytes);
    // A preceding and following guard must fit. Subtract only after testing
    // firstPage, so SIZE_MAX and a last-slot request cannot wrap.
    if (!pages || !firstPage || firstPage >= pageCount_ ||
        pages > pageCount_ - firstPage - 1) return kIOReturnNoSpace;
    const size_t guardFirst = firstPage - 1;
    const size_t guardEnd = firstPage + pages + 1;
    if (guardsOverlap(guardFirst, guardEnd, dataBinding_) ||
        guardsOverlap(guardFirst, guardEnd, writebackBinding_) ||
        guardsOverlap(guardFirst, guardEnd, displayBinding_)) return kIOReturnBusy;

    // At most 16MiB of buffer pages plus two guards: a 32,784-byte temporary.
    // Segment validation never partially changes the caller-visible image.
    const size_t temporaryBytes = (pages + 2) * kEntryBytes;
    uint8_t* temporary = static_cast<uint8_t*>(IOMalloc(temporaryBytes));
    if (!temporary) return kIOReturnNoMemory;
    const TableBuffer staging = {temporary, temporaryBytes};
    Status encoded = writeInvalidGartPtes(staging, 0, dummyDmaAddress_, pages + 2);
    const LeafAttributes attributes = {MemoryDomain::SystemDma, MemoryType::Uncached,
                                      true, true, true, executable, false};
    size_t covered = 0;
    for (uint32_t index = 0; encoded == Status::Ok && index < owned->segmentCount(); ++index) {
        const HackyNaviDMABuffer::Segment* segment = owned->segment(index);
        if (!segment || !segment->length || segment->length % kPageBytes ||
            segment->length / kPageBytes > pages - covered) {
            encoded = Status::InvalidArgument;
            break;
        }
        const size_t count = static_cast<size_t>(segment->length / kPageBytes);
        encoded = writeContiguousPtes(staging, 1 + covered, segment->dmaAddress,
                                     count, attributes);
        if (encoded == Status::Ok) covered += count;
    }
    IOReturn result = encoded == Status::Ok && covered == pages
        ? owned->synchronizeForDevice() : kIOReturnUnsupported;
    if (result == kIOReturnSuccess) {
        bcopy(temporary, table_ + guardFirst * kEntryBytes, temporaryBytes);
        *selected = {true, firstPage, pages, executable};
    }
    IOFree(temporary, temporaryBytes);
    return result;
}

IOReturn HackyNaviGARTStorage::unbind(Region region) {
    if (!ready_) return kIOReturnNotReady;
    Binding* selected = mutableBinding(region);
    if (!selected) return kIOReturnBadArgument;
    if (!selected->bound) return kIOReturnSuccess;
    const Status result = writeInvalidGartPtes({table_, tableBytes_}, selected->firstPage - 1,
                                              dummyDmaAddress_, selected->pageCount + 2);
    if (result != Status::Ok) return kIOReturnInternalError;
    *selected = {};
    return kIOReturnSuccess;
}

IOReturn HackyNaviGARTStorage::reset() {
    ready_ = false;
    dataBinding_ = {};
    writebackBinding_ = {};
    displayBinding_ = {};
    if (table_) { IOFree(table_, tableBytes_); table_ = nullptr; }
    tableBytes_ = 0;
    pageCount_ = 0;
    apertureBytes_ = 0;
    dummyDmaAddress_ = 0;
    IOReturn result = display_.reset();
    const IOReturn writebackResult = writeback_.reset();
    const IOReturn dataResult = data_.reset();
    const IOReturn dummyResult = dummy_.reset();
    if (result == kIOReturnSuccess) result = writebackResult;
    if (result == kIOReturnSuccess) result = dataResult;
    if (result == kIOReturnSuccess) result = dummyResult;
    return result;
}

HackyNaviDMABuffer* HackyNaviGARTStorage::mutableBuffer(Region region) {
    switch (region) {
    case Region::Data: return &data_;
    case Region::Writeback: return &writeback_;
    case Region::Display: return &display_;
    }
    return nullptr;
}

HackyNaviGARTStorage::Binding* HackyNaviGARTStorage::mutableBinding(Region region) {
    switch (region) {
    case Region::Data: return &dataBinding_;
    case Region::Writeback: return &writebackBinding_;
    case Region::Display: return &displayBinding_;
    }
    return nullptr;
}

const HackyNaviDMABuffer* HackyNaviGARTStorage::buffer(Region region) const {
    if (!ready_) return nullptr;
    switch (region) {
    case Region::Data: return &data_;
    case Region::Writeback: return &writeback_;
    case Region::Display: return &display_;
    }
    return nullptr;
}

const HackyNaviGARTStorage::Binding* HackyNaviGARTStorage::binding(Region region) const {
    if (!ready_) return nullptr;
    switch (region) {
    case Region::Data: return &dataBinding_;
    case Region::Writeback: return &writebackBinding_;
    case Region::Display: return &displayBinding_;
    }
    return nullptr;
}

IOReturn HackyNaviGARTStorage::synchronizeForDevice(Region region) {
    if (!ready_) return kIOReturnNotReady;
    HackyNaviDMABuffer* owned = mutableBuffer(region);
    return owned ? owned->synchronizeForDevice() : kIOReturnBadArgument;
}

IOReturn HackyNaviGARTStorage::synchronizeForCPU(Region region) {
    if (!ready_) return kIOReturnNotReady;
    HackyNaviDMABuffer* owned = mutableBuffer(region);
    return owned ? owned->synchronizeForCPU() : kIOReturnBadArgument;
}
