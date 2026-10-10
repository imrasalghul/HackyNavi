// SPDX-License-Identifier: MIT
// Native immutable GFX1100LeafBufferV1 capture. Numeric decoder reports and
// dispatch bindings never substitute for this retained VM/DMA owner ledger.
#include "HackyNaviVMContext.hpp"
#include "HackyNaviGARTController.hpp"
#include "hackynavi/gfx11_dispatch_product.hpp"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

using namespace hackynavi;
namespace {
struct Interval { uint64_t first, bytes; };
constexpr size_t kMaximumIntervals = HackyNaviVMContext::kBufferSlots *
    HackyNaviDMABuffer::kMaxSegments + vmpt::kMaxPages + 2;
static_assert(gfx11dispatch::kMaxVariants + gfx11dispatch::kMaxBindings + 2 < 64,
    "exact mapping bitset cannot represent the admitted binding limit");
bool pointer(const void* p, size_t bytes, size_t alignment = 1) {
    return p && bytes && !(reinterpret_cast<uintptr_t>(p) % alignment) &&
        bytes - 1 <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(p);
}
bool range(uint64_t offset, uint64_t bytes, uint64_t total) {
    return bytes && offset <= total && bytes <= total-offset;
}
bool sameHandle(HackyNaviVMContext::Handle a, HackyNaviVMContext::Handle b) {
    return a.id == b.id && a.generation == b.generation;
}
void swap(Interval& a, Interval& b) { const Interval t = a; a = b; b = t; }
void sift(Interval* intervals, size_t count, size_t root) {
    while (root < count/2) {
        size_t child = root*2 + 1;
        if (child + 1 < count && intervals[child].first < intervals[child+1].first) ++child;
        if (intervals[root].first >= intervals[child].first) return;
        swap(intervals[root], intervals[child]); root = child;
    }
    // An even-sized heap has its last parent at count/2-1; the loop above
    // includes it. No recursion or attacker-dependent stack allocation.
}
bool disjoint(Interval* intervals, size_t count) {
    if (!count) return false;
    for (size_t n = count/2; n; --n) sift(intervals, count, n-1);
    for (size_t n = count; n > 1; --n) {
        swap(intervals[0], intervals[n-1]); sift(intervals, n-1, 0);
    }
    for (size_t n = 1; n < count; ++n)
        if (intervals[n].first - intervals[n-1].first < intervals[n-1].bytes) return false;
    return true;
}
bool ownerGeometry(const HackyNaviDMABuffer* dma, size_t& segments) {
    if (!dma || !dma->cpuAddress() || !dma->size() || dma->size() > HackyNaviDMABuffer::kMaxBytes ||
        (dma->size() & 4095) || !pointer(dma->cpuAddress(), size_t(dma->size()), 4096) ||
        !dma->segmentCount() || dma->segmentCount() > HackyNaviDMABuffer::kMaxSegments ||
        dma->mappingKind() == HackyNaviDMABuffer::MappingKind::Unallocated) return false;
    uint64_t bytes = 0;
    for (uint32_t n = 0; n < dma->segmentCount(); ++n) {
        const auto* s = dma->segment(n);
        if (!s || !s->length || ((s->dmaAddress | s->length) & 4095) ||
            s->dmaAddress >= gmc11::kSystemDmaAddressLimit ||
            s->length > gmc11::kSystemDmaAddressLimit - s->dmaAddress ||
            s->length > dma->size()-bytes) return false;
        bytes += s->length;
    }
    if (bytes != dma->size() || dma->segmentCount() > kMaximumIntervals-segments) return false;
    segments += dma->segmentCount(); return true;
}
} // namespace

struct HackyNaviVMContext::ProductCapture {
    gfx11dispatch::Request dispatch;
    gfx11dispatch::Variant variants[gfx11dispatch::kMaxVariants];
    gfx11dispatch::Resource resources[gfx11dispatch::kMaxBindings];
    // Borrowed input pointers exist only while copying the request. No queue
    // path consults them; they are zeroed after capture into private DMA pages.
    gfx11product::Request inputPrograms[gfx11dispatch::kMaxVariants];
    ProductResource inputResources[gfx11dispatch::kMaxBindings];
    CapturedResult result;
    gfx11dispatch::Workspace workspace;
    Interval* intervals;
    size_t intervalCapacity;
    bool sealed;

    size_t bindingCount() const { return dispatch.variantCount + dispatch.resourceCount + 2; }
    const gfxisolated::Binding& binding(size_t n) const {
        if (n < dispatch.variantCount) return variants[n].shader;
        n -= dispatch.variantCount;
        if (n < dispatch.resourceCount) return resources[n].mapping;
        return n == dispatch.resourceCount ? dispatch.indirect : dispatch.parameters;
    }
    bool control(size_t n) const {
        return n < dispatch.variantCount || n >= dispatch.variantCount + dispatch.resourceCount;
    }
};

// Checks every retained BO, including unmapped unused allocations, and every
// actual page-table DMA owner. Pairwise segment scans could require hundreds
// of millions of comparisons; bounded retained heap scratch + heapsort makes
// this O(N log N), with N <= 64*256 + 1024. Device-visible I/O bus addresses
// are not claimed to be CPU physical addresses. All allocations here are
// fresh owned IOBufferMemoryDescriptors; imported memory is not supported.
bool HackyNaviVMContext::productStorageDistinctLocked() const {
    ProductCapture* p = capturedProduct_;
    if (!p || !p->intervals || !p->intervalCapacity || p->intervalCapacity > kMaximumIntervals ||
        !pointer(p->intervals, p->intervalCapacity*sizeof(Interval), alignof(Interval))) return false;
    size_t required = 2, cpuCount = 0;
    for (const auto& b : buffers_) if (b.dma) {
        if (!ownerGeometry(b.dma, required) || required > p->intervalCapacity) return false;
        p->intervals[cpuCount++] = {reinterpret_cast<uintptr_t>(b.dma->cpuAddress()), b.dma->size()};
    }
    if (!tree_.pageCount() || tree_.pageCount() > vmpt::kMaxPages) return false;
    for (uint16_t n = 0; n < tree_.pageCount(); ++n) {
        const auto* page = tree_.page(n);
        const auto* dma = page ? static_cast<const HackyNaviDMABuffer*>(page->storage.owner) : nullptr;
        if (!ownerGeometry(dma, required) || required > p->intervalCapacity ||
            dma->size() != 4096 || dma->segmentCount() != 1 ||
            page->storage.bytes != dma->cpuAddress() || page->storage.dmaAddress != dma->segment(0)->dmaAddress)
            return false;
        p->intervals[cpuCount++] = {reinterpret_cast<uintptr_t>(dma->cpuAddress()), dma->size()};
    }
    if (required != p->intervalCapacity) return false;
    p->intervals[cpuCount++] = {reinterpret_cast<uintptr_t>(p), sizeof(ProductCapture)};
    p->intervals[cpuCount++] = {reinterpret_cast<uintptr_t>(p->intervals), p->intervalCapacity*sizeof(Interval)};
    if (!disjoint(p->intervals, cpuCount)) return false;
    size_t ioCount = 0;
    for (const auto& b : buffers_) if (b.dma)
        for (uint32_t n = 0; n < b.dma->segmentCount(); ++n) {
            const auto* segment = b.dma->segment(n);
            p->intervals[ioCount++] = {segment->dmaAddress, segment->length};
        }
    for (uint16_t n = 0; n < tree_.pageCount(); ++n) {
        const auto* dma = static_cast<const HackyNaviDMABuffer*>(tree_.page(n)->storage.owner);
        const auto* segment = dma->segment(0);
        p->intervals[ioCount++] = {segment->dmaAddress, segment->length};
    }
    return ioCount + 2 == p->intervalCapacity && disjoint(p->intervals, ioCount);
}

bool HackyNaviVMContext::productCaptureValidLocked() const {
    ProductCapture* p = capturedProduct_;
    if (!p || !p->sealed || capturedGraphics_ || capturedAttributes_.id || capturedAttributes_.generation ||
        !(hardwareHold_ ? gart_->accessReady() : gart_->normalAccessReady()) ||
        !providerReady(hardwareHold_) ||
        capturedClass_ != ShaderClass::Gfx1100LeafBufferV1 || !capturedWords_ ||
        capturedWords_ > gfx11dispatch::kMaxCommandDwords ||
        !p->dispatch.variantCount || p->dispatch.variantCount > gfx11dispatch::kMaxVariants ||
        p->dispatch.resourceCount > gfx11dispatch::kMaxBindings ||
        p->dispatch.variants != p->variants || p->dispatch.resources != p->resources ||
        p->dispatch.owner != reinterpret_cast<uintptr_t>(this) || p->dispatch.epoch != epoch_ ||
        p->dispatch.vmid != vmid_ ||
        !sameHandle(capturedShader_, {p->variants[0].shader.id, p->variants[0].shader.generation}) ||
        !sameHandle(capturedIndirect_, {p->dispatch.indirect.id, p->dispatch.indirect.generation}) ||
        !sameHandle(capturedParameters_, {p->dispatch.parameters.id, p->dispatch.parameters.generation}) ||
        capturedIndirectGpu_ != p->dispatch.indirect.gpu ||
        !sameHandle(capturedResult_.buffer, p->result.buffer) ||
        capturedResult_.offset != p->result.offset || capturedResult_.bytes != p->result.bytes)
        return false;
    const size_t count = p->bindingCount();
    for (size_t n = 0; n < count; ++n) {
        const auto& b = p->binding(n);
        if (!b.id || b.id > kBufferSlots || b.owner != p->dispatch.owner || b.epoch != epoch_) return false;
        const Buffer& buffer = buffers_[b.id-1];
        if (!buffer.dma || !buffer.dma->cpuAddress() || buffer.generation != b.generation ||
            buffer.dma->size() != b.allocationBytes || buffer.internal != p->control(n) || buffer.mappings != 1)
            return false;
        if (n < p->dispatch.variantCount &&
            p->variants[n].program.code != static_cast<const uint8_t*>(buffer.dma->cpuAddress())) return false;
    }
    // Every GPU mapping must belong to exactly one admitted binding. Reject
    // all extra aliases/permissions, including aliases outside the requested
    // BO subrange or an otherwise unused resident BO. No hidden executable or
    // writable mapping survives sealing.
    uint64_t seen = 0;
    for (const auto& mapping : mappings_) if (mapping.bytes) {
        size_t n = 0;
        for (; n < count; ++n) {
            const auto& b = p->binding(n);
            if (mapping.buffer.id == b.id && mapping.buffer.generation == b.generation &&
                mapping.va == b.gpu && mapping.bytes == b.bytes && mapping.offset == b.bufferOffset &&
                mapping.access == b.access) break;
        }
        if (n == count || (seen & (UINT64_C(1)<<n))) return false;
        seen |= UINT64_C(1)<<n;
    }
    if (seen != (UINT64_C(1)<<count)-1 || !productStorageDistinctLocked()) return false;
    const auto* parameters = static_cast<const uint8_t*>(buffers_[capturedParameters_.id-1].dma->cpuAddress());
    const auto* commands = static_cast<const uint32_t*>(buffers_[capturedIndirect_.id-1].dma->cpuAddress());
    if (gfx11dispatch::validate(p->dispatch, p->workspace, parameters, gfx11dispatch::kControlBytes,
            commands, capturedWords_) != gfx11dispatch::Status::Ok) return false;
    // The unused IB page is also private and zeroed; an altered trailing word
    // is rejected even though the queue would not execute past capturedWords_.
    for (size_t n = capturedWords_; n < gfx11dispatch::kMaxCommandDwords; ++n)
        if (commands[n]) return false;
    bool resultBound = false;
    for (size_t n = 0; n < p->dispatch.resourceCount; ++n) {
        const auto& resource = p->resources[n];
        const auto& b = resource.mapping;
        if (sameHandle(p->result.buffer, {b.id,b.generation}) &&
            (p->workspace.writeMask & (uint32_t(1)<<resource.binding))) {
            const uint64_t offset = b.bufferOffset + resource.offset;
            resultBound = p->result.offset >= offset &&
                range(p->result.offset-offset, p->result.bytes, resource.bytes);
        }
    }
    return resultBound && !(p->result.offset & 3) && !(p->result.bytes & 3) &&
        p->result.bytes <= kMaxBufferBytes &&
        (hardwareHold_ ? gart_->accessReady() : gart_->normalAccessReady());
}

IOReturn HackyNaviVMContext::captureProductCompute(const ProductComputeRequest& request) {
    if (!gart_) return kIOReturnNotReady;
    IOLockLock(gart_->lock_);
    const auto ready = mutableState(); if (ready != kIOReturnSuccess) return finish(ready);
    const auto generation = beginCaptureLocked(); if (generation != kIOReturnSuccess) return finish(generation);
    const ProductComputeRequest incoming = request;
    if (capturedProduct_ || capturedClass_ != ShaderClass::None ||
        !incoming.variantCount || incoming.variantCount > gfx11dispatch::kMaxVariants ||
        !incoming.resourceCount || incoming.resourceCount > gfx11dispatch::kMaxBindings ||
        !pointer(incoming.variants, incoming.variantCount*sizeof(gfx11product::Request), alignof(gfx11product::Request)) ||
        !pointer(incoming.resources, incoming.resourceCount*sizeof(ProductResource), alignof(ProductResource)) ||
        !incoming.result.id || !incoming.result.generation || !incoming.resultBytes ||
        ((incoming.resultOffset | incoming.resultBytes) & 3) || incoming.resultBytes > kMaxBufferBytes)
        return finish(kIOReturnBadArgument);
    auto* p = static_cast<ProductCapture*>(IOMalloc(sizeof(ProductCapture)));
    if (!p) return finish(kIOReturnNoMemory);
    bzero(p, sizeof(*p)); capturedProduct_ = p;
    // From this point every failure makes the context FailedUnpublished.
    // All partial DMA controls and metadata remain in this owner's ledger;
    // only releaseBeforeExposure or actual retirement can consume them.
    p->dispatch.owner = reinterpret_cast<uintptr_t>(this); p->dispatch.epoch = epoch_; p->dispatch.vmid = vmid_;
    p->dispatch.variants = p->variants; p->dispatch.variantCount = incoming.variantCount;
    p->dispatch.resources = p->resources; p->dispatch.resourceCount = incoming.resourceCount;
    for (unsigned d = 0; d < 3; ++d) {
        p->dispatch.grid[d] = incoming.grid[d]; p->dispatch.nominalLocal[d] = incoming.nominalLocal[d];
    }
    p->result = {incoming.result,incoming.resultOffset,incoming.resultBytes};
    bcopy(incoming.variants, p->inputPrograms, incoming.variantCount*sizeof(gfx11product::Request));
    bcopy(incoming.resources, p->inputResources, incoming.resourceCount*sizeof(ProductResource));
    IOReturn status = kIOReturnSuccess;
    uint32_t reads = 0, writes = 0;
    for (size_t n = 0; n < incoming.variantCount && status == kIOReturnSuccess; ++n) {
        const auto& input = p->inputPrograms[n];
        if (!input.codeBytes || input.codeBytes > gfx11product::kMaxCodeBytes ||
            !pointer(input.code, input.codeBytes)) { status = kIOReturnBadArgument; break; }
        const size_t bytes = (input.codeBytes+4095)&~size_t(4095);
        Handle handle{}; uint64_t va = 0;
        status = allocateBufferLocked(bytes, 4096, handle);
        if (status != kIOReturnSuccess) break;
        Buffer& buffer = buffers_[handle.id-1];
        auto* code = static_cast<uint8_t*>(buffer.dma->cpuAddress());
        bzero(code, bytes); bcopy(input.code, code, input.codeBytes); // only read of caller code
        auto& variant = p->variants[n]; variant.program = input; variant.program.code = code;
        if (gfx11product::validate(variant.program, p->workspace.variants[n]) != gfx11product::Status::Ok)
            status = kIOReturnBadArgument;
        if (status == kIOReturnSuccess && !controlVaLocked(va, bytes)) status = kIOReturnNoSpace;
        if (status == kIOReturnSuccess) status = mapLocked(handle, 0, va, bytes, Read|Execute);
        if (status == kIOReturnSuccess && !bindingLocked(handle, 0, bytes, Read|Execute, variant.shader, va))
            status = kIOReturnInternalError;
        // Private helpers use find(), which excludes internal handles. This
        // lock-held allocate/map sequence has no public interleaving, and its
        // sole exit marks even failed/partial controls internal permanently.
        buffer.internal = true;
        if (status == kIOReturnSuccess) {
            reads |= p->workspace.variants[n].readMask; writes |= p->workspace.variants[n].writeMask;
        }
    }
    bzero(p->inputPrograms, sizeof(p->inputPrograms));
    uint32_t supplied = 0;
    for (size_t n = 0; n < incoming.resourceCount && status == kIOReturnSuccess; ++n) {
        const auto& input = p->inputResources[n];
        if (input.binding >= gfx11dispatch::kMaxBindings || !input.bytes || (input.offset&3) || (input.bytes&3)) {
            status = kIOReturnBadArgument; break;
        }
        const uint32_t bit = uint32_t(1)<<input.binding;
        if ((supplied&bit) || !((reads|writes)&bit)) { status = kIOReturnBadArgument; break; }
        supplied |= bit;
        auto& resource = p->resources[n]; resource.binding = input.binding; resource.bytes = input.bytes;
        uint64_t gpu = 0;
        const uint32_t access = Read | ((writes&bit) ? Write : 0);
        if (!bindingLocked(input.buffer, input.offset, input.bytes, access, resource.mapping, gpu)) {
            status = kIOReturnNotFound; break;
        }
        if (resource.mapping.access != access) { status = kIOReturnNotPermitted; break; }
        resource.offset = gpu-resource.mapping.gpu;
    }
    bzero(p->inputResources, sizeof(p->inputResources));
    if (status == kIOReturnSuccess && supplied != (reads|writes)) status = kIOReturnBadArgument;
    gfxisolated::Binding* controls[] = {&p->dispatch.indirect,&p->dispatch.parameters};
    const uint32_t permissions[] = {Read|Execute,Read};
    for (unsigned n = 0; n < 2 && status == kIOReturnSuccess; ++n) {
        Handle handle{}; uint64_t va = 0;
        status = allocateBufferLocked(gfx11dispatch::kControlBytes, 4096, handle);
        if (status != kIOReturnSuccess) break;
        if (!controlVaLocked(va)) status = kIOReturnNoSpace;
        if (status == kIOReturnSuccess) status = mapLocked(handle, 0, va, gfx11dispatch::kControlBytes, permissions[n]);
        if (status == kIOReturnSuccess && !bindingLocked(handle, 0, gfx11dispatch::kControlBytes,
                permissions[n], *controls[n], va)) status = kIOReturnInternalError;
        buffers_[handle.id-1].internal = true;
    }
    // Allocate exactly the current immutable set's segment scratch, not a
    // maximum-sized kernel stack array. No page/BO allocation follows this.
    size_t capacity = 2;
    if (status == kIOReturnSuccess) {
        for (const auto& buffer : buffers_) if (buffer.dma && !ownerGeometry(buffer.dma, capacity)) {
            status = kIOReturnBadArgument; break;
        }
        for (uint16_t n = 0; n < tree_.pageCount() && status == kIOReturnSuccess; ++n) {
            const auto* page = tree_.page(n);
            if (!page || !ownerGeometry(static_cast<const HackyNaviDMABuffer*>(page->storage.owner), capacity))
                status = kIOReturnBadArgument;
        }
    }
    if (status == kIOReturnSuccess) {
        p->intervals = static_cast<Interval*>(IOMalloc(capacity*sizeof(Interval)));
        if (!p->intervals) status = kIOReturnNoMemory;
        else p->intervalCapacity = capacity;
    }
    if (status != kIOReturnSuccess) { state_ = State::FailedUnpublished; return finish(status); }
    auto* parameters = static_cast<uint8_t*>(buffers_[p->dispatch.parameters.id-1].dma->cpuAddress());
    auto* commands = static_cast<uint32_t*>(buffers_[p->dispatch.indirect.id-1].dma->cpuAddress());
    bzero(parameters, gfx11dispatch::kControlBytes); bzero(commands, gfx11dispatch::kControlBytes);
    size_t words = 0;
    if (!productStorageDistinctLocked() ||
        gfx11dispatch::parameterImage(p->dispatch, p->workspace, parameters, gfx11dispatch::kControlBytes) != gfx11dispatch::Status::Ok ||
        gfx11dispatch::encode(p->dispatch, p->workspace, commands, gfx11dispatch::kMaxCommandDwords, words) != gfx11dispatch::Status::Ok) {
        state_ = State::FailedUnpublished; return finish(kIOReturnBadArgument);
    }
    capturedClass_ = ShaderClass::Gfx1100LeafBufferV1;
    capturedShader_ = {p->variants[0].shader.id,p->variants[0].shader.generation};
    capturedIndirect_ = {p->dispatch.indirect.id,p->dispatch.indirect.generation};
    capturedParameters_ = {p->dispatch.parameters.id,p->dispatch.parameters.generation};
    capturedIndirectGpu_ = p->dispatch.indirect.gpu; capturedWords_ = static_cast<uint32_t>(words);
    capturedResult_ = p->result;
    p->sealed = true; state_ = State::Captured;
    // This exact private DMA image is revalidated now and by the existing queue
    // before its real VM publication. Only that queue issues completion after
    // genuine fence/RPTR observation; metadata grants no execution authority.
    if (!captureValidLocked()) { state_ = State::FailedUnpublished; return finish(kIOReturnNotPermitted); }
    return finish(kIOReturnSuccess);
}

bool HackyNaviVMContext::capturedControlMaskLocked(uint64_t& mask) const {
    mask = 0;
    if (capturedClass_ == ShaderClass::Gfx1100PositionConstantColorV1 ||
        capturedClass_ == ShaderClass::Gfx1100PositionFragmentBufferV1 ||
        capturedClass_ == ShaderClass::Gfx1100PositionVertexBufferV1 ||
        capturedClass_ == ShaderClass::Gfx1100PositionVertexFragmentBuffersV1)
        return graphicsControlMaskLocked(mask);
    if (capturedGraphics_ || capturedAttributes_.id || capturedAttributes_.generation) return false;
    const auto add = [&](Handle handle) -> bool {
        if (!handle.id || handle.id > kBufferSlots || !handle.generation) return false;
        const uint64_t bit = UINT64_C(1) << (handle.id - 1);
        const auto& b = buffers_[handle.id - 1];
        if ((mask & bit) || !b.dma || !b.internal || b.generation != handle.generation || b.mappings != 1)
            return false;
        mask |= bit; return true;
    };
    switch (capturedClass_) {
    case ShaderClass::BuiltInClearCopy:
        if (capturedProduct_ || !add(capturedShader_) || !add(capturedIndirect_) ||
            capturedParameters_.id || capturedParameters_.generation) return false;
        break;
    case ShaderClass::AppleAddOneGfx1100V1:
        if (capturedProduct_ || !add(capturedShader_) || !add(capturedIndirect_) || !add(capturedParameters_))
            return false;
        break;
    case ShaderClass::Gfx1100LeafBufferV1: {
        const ProductCapture* p = capturedProduct_;
        if (!p || !p->sealed || !p->dispatch.variantCount || p->dispatch.variantCount > gfx11dispatch::kMaxVariants)
            return false;
        for (size_t n = 0; n < p->dispatch.variantCount; ++n)
            if (!add({p->variants[n].shader.id, p->variants[n].shader.generation})) return false;
        if (!add(capturedIndirect_) || !add(capturedParameters_)) return false;
        break;
    }
    default: return false;
    }
    // Every private DMA owner must be in this exact capture, including all
    // product variants. Never release data merely because a handle was stale
    // or because only the first variant happened to be recorded as shader.
    for (uint32_t n = 0; n < kBufferSlots; ++n)
        if (buffers_[n].internal != bool(mask & (UINT64_C(1) << n))) return false;
    return mask != 0;
}

void HackyNaviVMContext::releaseProductCaptureLocked() {
    if (!capturedProduct_) return;
    ProductCapture* p = capturedProduct_; capturedProduct_ = nullptr;
    if (p->intervals) {
        IOFree(p->intervals, p->intervalCapacity*sizeof(Interval)); p->intervals = nullptr;
    }
    bzero(p, sizeof(*p)); IOFree(p, sizeof(ProductCapture));
}
