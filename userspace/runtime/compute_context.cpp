// SPDX-License-Identifier: MIT
#include "compute_context.hpp"
#include "graphics_snapshot.hpp"
#include "graphics_buffer_snapshot.hpp"
#include <cstring>
#include <new>

namespace hackynavi { namespace compute {
namespace {
Result ok() { return {Status::Ok, 0}; }
Result fail(Status status) { return {status, 0}; }
bool range(uint64_t offset, size_t bytes, uint64_t total) { return offset <= total && bytes <= total - offset; }
bool span(const void* pointer, size_t bytes) {
    return !bytes || (pointer && bytes <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(pointer));
}
// The native v1.4 owner has 64 total BO slots, including private code/IB/
// descriptor controls. Reserve before mutation instead of discovering a full
// arena after product capture has made the native context terminal.
constexpr uint32_t kNativeBufferSlots = 64;
// Actual worst-case six-SE private graphics controls plus one unmapped upload
// BO. CPU history is separately bounded by the native 16MiB result contract.
constexpr uint64_t kGraphicsAttributeBytes = 6 * 22 * 65536;
constexpr uint64_t kGraphicsExtraBytes = kGraphicsAttributeBytes + 4096 + 8192 + 4096;
}
struct State {
    std::mutex mutex;
    transport::Client client;
    nativeabi::HelloReply info{};
    HNColorTopology graphicsTopology{};
    bool open = false;
    uint32_t buffers = 0;
    uint64_t bytes = 0;
    uint64_t vaStride = 0;
    const Buffer* reservations[kNativeBufferSlots]{};
    Result closeResult{Status::Ok, 0};
    Result closeLocked() {
        if (open) { open = false; closeResult = client.close(); }
        return closeResult;
    }
    // A failed mutating RPC or submission makes this owner terminal. The
    // kernel's close/recovery path decides when exposed storage is releasable.
    Result terminate(Result reason) { closeLocked(); return reason; }
    ~State() { client.close(); }
};
Result Context::open(uint64_t registry, std::shared_ptr<Context>& out) {
    return openProfile(registry, nativeabi::kReusableMinor, out);
}
Result Context::openGraphics(uint64_t registry, std::shared_ptr<Context>& out) {
    return openProfile(registry, nativeabi::kGraphicsMinor, out);
}
Result Context::openBufferedGraphics(uint64_t registry, std::shared_ptr<Context>& out) {
    return openProfile(registry, nativeabi::kBufferedGraphicsMinor, out);
}
Result Context::openBufferedGraphicsAccelerator(uint32_t acceleratorPort,
    uint64_t acceleratorID,uint64_t ownerID,std::shared_ptr<Context>& out) {
    if(!acceleratorPort||!acceleratorID||!ownerID||acceleratorID==ownerID)return fail(Status::Invalid);
    return openProfile(ownerID,nativeabi::kBufferedGraphicsMinor,out,acceleratorPort,acceleratorID);
}
Result Context::openVertexBufferedGraphics(uint64_t registry,std::shared_ptr<Context>& out) {
    return openProfile(registry,nativeabi::kVertexBufferedGraphicsMinor,out);
}
Result Context::openVertexBufferedGraphicsAccelerator(uint32_t acceleratorPort,
    uint64_t acceleratorID,uint64_t ownerID,std::shared_ptr<Context>& out) {
    if(!acceleratorPort||!acceleratorID||!ownerID||acceleratorID==ownerID)return fail(Status::Invalid);
    return openProfile(ownerID,nativeabi::kVertexBufferedGraphicsMinor,out,acceleratorPort,acceleratorID);
}
Result Context::openProfile(uint64_t registry, uint16_t minor, std::shared_ptr<Context>& out,
    uint32_t acceleratorPort,uint64_t acceleratorID) {
    if (out) return fail(Status::Invalid);
    const bool graphics=nativeabi::graphicsProfile(minor);
    try {
        auto owner = std::make_shared<State>();
        auto result = minor==nativeabi::kVertexBufferedGraphicsMinor?
            (acceleratorPort?owner->client.openVertexBufferedGraphicsAccelerator(acceleratorPort,acceleratorID,registry):
                owner->client.openVertexBufferedGraphicsProduct(registry)):
            acceleratorPort?owner->client.openBufferedGraphicsAccelerator(acceleratorPort,acceleratorID,registry):
            minor==nativeabi::kBufferedGraphicsMinor?owner->client.openBufferedGraphicsProduct(registry):
            graphics ? owner->client.openGraphicsProduct(registry) : owner->client.openReusableProduct(registry);
        if (!result) return result;
        owner->open = true;
        result = owner->client.info(owner->info);
        if (!result) return owner->terminate(result);
        if (owner->info.pageBytes != 4096 || owner->info.header.minor != minor)
            return owner->terminate(fail(Status::Unsupported));
        if (graphics) {
            result = owner->client.graphicsInformation(owner->graphicsTopology);
            if (!result) return owner->terminate(result);
            const HNColorRequest dimensions = {1, 1};
            if (hn_color_target_validate_inputs(&owner->graphicsTopology, &dimensions) != HN_COLOR_OK)
                return owner->terminate(fail(Status::Unsupported));
        }
        // Reserve disjoint intervals per live data object, leaving room above
        // this arena for the kernel's separately owned control mappings.
        const uint64_t controlHeadroom = graphics ? kGraphicsAttributeBytes + 4096 + 8192 + 65536 :
            nativeabi::kMaxProductVariants * nativeabi::kMaxProductCodeBytes + 8192;
        if (!owner->info.maxAllocationBytes || owner->info.maxAllocationBytes > UINT64_MAX - 4095 ||
            (owner->info.vaBegin & 4095) || owner->info.vaEnd <= owner->info.vaBegin ||
            owner->info.vaEnd - owner->info.vaBegin <= controlHeadroom)
            return owner->terminate(fail(Status::Unsupported));
        owner->vaStride = (owner->info.maxAllocationBytes + 4095) & ~uint64_t(4095);
        if (owner->vaStride > (owner->info.vaEnd - owner->info.vaBegin - controlHeadroom) / kNativeBufferSlots)
            return owner->terminate(fail(Status::Unsupported));
        out = std::shared_ptr<Context>(new Context(std::move(owner))); return ok();
    } catch (const std::bad_alloc&) { return fail(Status::OutOfMemory); }
}
Result Context::information(nativeabi::HelloReply& out) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->open) return fail(Status::NotOpen);
    out = state_->info; return ok();
}
Result Context::graphicsInformation(HNColorTopology& out) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->open) return fail(Status::NotOpen);
    if (!nativeabi::graphicsProfile(state_->info.header.minor)) return fail(Status::Unsupported);
    out = state_->graphicsTopology; return ok();
}
Result Context::allocatedSize(uint64_t& out) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->open) return fail(Status::NotOpen);
    out = state_->bytes; return ok();
}
Result Context::close() {
    std::lock_guard<std::mutex> lock(state_->mutex); return state_->closeLocked();
}
Result Context::allocate(uint64_t bytes, std::shared_ptr<Buffer>& out) {
    if (out || !bytes || bytes > UINT64_MAX - 4095) return fail(Status::Invalid);
    try {
        // Construct the shared owner before taking its mutex: an allocation
        // failure in shared_ptr construction must not run a Buffer destructor
        // while the same nonrecursive mutex is held.
        auto buffer = std::shared_ptr<Buffer>(new Buffer(state_));
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->open) return fail(Status::NotOpen);
        const uint64_t allocation = (bytes + 4095) & ~uint64_t(4095);
        if (allocation > state_->info.maxAllocationBytes) return fail(Status::Unsupported);
        if (state_->buffers >= kNativeBufferSlots - 4 || state_->bytes > state_->info.systemBudget ||
            allocation > state_->info.systemBudget - state_->bytes) return fail(Status::OutOfMemory);
        uint32_t slot = 0;
        while (slot < kNativeBufferSlots && state_->reservations[slot]) ++slot;
        if (slot == kNativeBufferSlots) return fail(Status::OutOfMemory);
        const auto result = state_->client.allocate(allocation, 4096, nativeabi::Domain::System, 0, buffer->native_);
        if (!result) return state_->terminate(result);
        buffer->length_ = bytes; buffer->allocationBytes_ = allocation;
        buffer->gpuAddress_ = state_->info.vaBegin + state_->vaStride * slot;
        buffer->reservationSlot_ = slot; state_->reservations[slot] = buffer.get();
        ++state_->buffers; state_->bytes += allocation;
        out = std::move(buffer); return ok();
    } catch (const std::bad_alloc&) { return fail(Status::OutOfMemory); }
}
Buffer::~Buffer() {
    (void)release();
}
Result Buffer::release() {
    std::lock_guard<std::mutex> lock(owner_->mutex);
    if (!native_.handle.id) return ok();
    if (reservationSlot_ >= kNativeBufferSlots || owner_->reservations[reservationSlot_] != this)
        return owner_->terminate(fail(Status::Protocol));
    if (owner_->open) {
        const auto result = owner_->client.release(native_);
        if (!result) return owner_->terminate(result);
        --owner_->buffers; owner_->bytes -= allocationBytes_;
    } else {
        owner_->reservations[reservationSlot_] = nullptr;
        native_ = {}; return owner_->closeResult;
    }
    owner_->reservations[reservationSlot_] = nullptr;
    native_ = {}; return ok();
}
Result Buffer::upload(uint64_t offset, const void* input, size_t bytes) {
    if (!range(offset, bytes, length_) || !span(input, bytes)) return fail(Status::Invalid);
    std::lock_guard<std::mutex> lock(owner_->mutex);
    if (!owner_->open) return fail(Status::NotOpen);
    if (!native_.handle.id) return fail(Status::NotFound);
    if (!bytes) return ok();
    const auto result = owner_->client.write(native_, offset, input, bytes);
    return result ? result : owner_->terminate(result);
}
Result Buffer::download(uint64_t offset, void* output, size_t bytes) {
    if (!range(offset, bytes, length_) || !span(output, bytes)) return fail(Status::Invalid);
    try {
        std::vector<uint8_t> copy(bytes);
        std::lock_guard<std::mutex> lock(owner_->mutex);
        if (!owner_->open) return fail(Status::NotOpen);
        if (!native_.handle.id) return fail(Status::NotFound);
        if (!bytes) return ok();
        const auto result = owner_->client.read(native_, offset, copy.data(), bytes);
        if (!result) return owner_->terminate(result);
        std::memcpy(output, copy.data(), bytes); return ok();
    } catch (const std::bad_alloc&) { return fail(Status::OutOfMemory); }
}
Result Context::dispatch(std::shared_ptr<const Pipeline> pipeline, const Binding* input,
                         size_t count, Completion& completion) {
    completion = {};
    if (!pipeline || !input || !count || count > nativeabi::kMaxProductResources || count != pipeline->resources_.size() ||
        !span(input, count * sizeof(Binding)))
        return fail(Status::Invalid);
    // Pin all actual buffers before taking the lock; destruction happens after
    // the mutex releases even when submission or cleanup fails.
    std::array<Binding, nativeabi::kMaxProductResources> bindings{};
    for (size_t n = 0; n < count; ++n) bindings[n] = input[n];
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->open) return fail(Status::NotOpen);
    std::array<transport::ProductResource, nativeabi::kMaxProductResources> resources{};
    std::array<uint32_t, nativeabi::kMaxProductResources> access{};
    std::array<uint64_t, nativeabi::kMaxProductResources> addresses{};
    size_t resultIndex = count;
    for (size_t n = 0; n < count; ++n) {
        const auto& b = bindings[n];
        if (!b.buffer || b.buffer->owner_ != state_ || !b.buffer->native_.handle.id || !b.bytes || ((b.offset | b.bytes) & 3) ||
            b.bytes > UINT32_MAX || !range(b.offset, size_t(b.bytes), b.buffer->length_))
            return fail(Status::Invalid);
        for (size_t p = 0; p < n; ++p)
            if (bindings[p].binding == b.binding || bindings[p].buffer == b.buffer) return fail(Status::Invalid);
        const HNPipelineResource* metadata = nullptr;
        for (const auto& r : pipeline->resources_) if (r.binding == b.binding) { metadata = &r; break; }
        if (!metadata) return fail(Status::Invalid);
        access[n] = nativeabi::Read | ((metadata->access & 2) ? nativeabi::Write : 0U);
        if ((metadata->access & 2) && resultIndex == count) resultIndex = n;
        resources[n] = {b.buffer->native_, b.offset, b.bytes, b.binding};
        addresses[n] = b.buffer->gpuAddress_;
    }
    if (resultIndex == count) return fail(Status::Unsupported);
    uint64_t extraBytes = 2 * 4096; // actual private IB and parameter controls
    for (uint32_t n = 0; n < pipeline->variantCount_; ++n)
        extraBytes += 2 * ((uint64_t(pipeline->variants_[n].metadata.code_bytes) + 4095) & ~uint64_t(4095));
    // Each variant needs both an unmapped staging BO and the actual native RX
    // copy. Published data buffers remain resident throughout the dispatch.
    if (state_->buffers > kNativeBufferSlots - (2 * pipeline->variantCount_ + 2) ||
        state_->bytes > state_->info.systemBudget || extraBytes > state_->info.systemBudget - state_->bytes)
        return fail(Status::OutOfMemory);
    std::array<transport::ProductVariant, nativeabi::kMaxProductVariants> variants{};
    // Any RPC failure closes this exact context. It never frees guessed DMA
    // ownership, attempts another dispatch, or reuses a partial map/code stage.
    for (uint32_t n = 0; n < pipeline->variantCount_; ++n) {
        const auto& source = pipeline->variants_[n]; const auto& metadata = source.metadata; auto& v = variants[n];
        const uint64_t bytes = (uint64_t(metadata.code_bytes) + 4095) & ~uint64_t(4095);
        auto result = state_->client.allocate(bytes, 4096, nativeabi::Domain::System, 0, v.code);
        if (!result) return state_->terminate(result);
        result = state_->client.write(v.code, 0, source.code.data(), source.code.size());
        if (!result) return state_->terminate(result);
        v.codeBytes = metadata.code_bytes; v.executableBytes = metadata.executable_bytes;
        std::memcpy(v.local, metadata.local_size, sizeof(v.local));
        v.waveSize = metadata.wave_size; v.numSgprs = metadata.num_sgprs; v.numVgprs = metadata.num_vgprs;
    }
    for (size_t n = 0; n < count; ++n) {
        const auto& b = *bindings[n].buffer;
        const auto result = state_->client.mapVa(b.native_, addresses[n], 0, b.allocationBytes_, access[n]);
        if (!result) return state_->terminate(result);
    }
    transport::ProductDispatch dispatch{};
    dispatch.variants = variants.data(); dispatch.variantCount = pipeline->variantCount_;
    dispatch.resources = resources.data(); dispatch.resourceCount = uint32_t(count);
    std::memcpy(dispatch.grid, pipeline->grid_, sizeof(dispatch.grid));
    std::memcpy(dispatch.nominalLocal, pipeline->local_, sizeof(dispatch.nominalLocal));
    dispatch.result = resources[resultIndex].buffer;
    dispatch.resultOffset = resources[resultIndex].offset; dispatch.resultBytes = uint32_t(resources[resultIndex].bytes);
    transport::Fence fence{};
    auto result = state_->client.product(dispatch, fence);
    if (!result) return state_->terminate(result);
    result = state_->client.wait(fence, nativeabi::kMaxWaitNs);
    if (!result) return state_->terminate(result);
    const Completion observed{fence.epoch, fence.sequence};
    // The native call has already copied and consumed the completed job. This
    // layer needs the persistent BOs, so release its CPU-only history promptly.
    result = state_->client.releaseResult(fence);
    if (!result) return state_->terminate(result);
    for (size_t n = 0; n < count; ++n) {
        const auto& b = *bindings[n].buffer;
        result = state_->client.unmapVa(b.native_, addresses[n], 0, b.allocationBytes_);
        if (!result) return state_->terminate(result);
    }
    for (uint32_t n = 0; n < pipeline->variantCount_; ++n) {
        result = state_->client.release(variants[n].code);
        if (!result) return state_->terminate(result);
    }
    completion = observed; return ok();
}
Result Context::draw(std::shared_ptr<const GraphicsPipeline> pipeline, std::shared_ptr<Buffer> target,
    uint64_t targetOffset, const HNColorLayout& suppliedLayout, Completion& completion) {
    return draw(pipeline.get(),nullptr,nullptr,nullptr,std::move(target),targetOffset,suppliedLayout,nullptr,0,nullptr,0,completion);
}
Result Context::draw(std::shared_ptr<const FragmentBufferGraphicsPipeline> pipeline,std::shared_ptr<Buffer> target,
    uint64_t targetOffset,const HNColorLayout& suppliedLayout,std::shared_ptr<Buffer> fragment,
    uint64_t fragmentOffset,Completion& completion) {
    if(!pipeline||!fragment)return fail(Status::Invalid);
    return draw(nullptr,pipeline.get(),nullptr,nullptr,std::move(target),targetOffset,suppliedLayout,
        nullptr,0,std::move(fragment),fragmentOffset,completion);
}
Result Context::draw(std::shared_ptr<const VertexBufferGraphicsPipeline> pipeline,std::shared_ptr<Buffer> target,
    uint64_t targetOffset,const HNColorLayout& layout,std::shared_ptr<Buffer> vertex,uint64_t vertexOffset,Completion& completion) {
    if(!pipeline||!vertex)return fail(Status::Invalid);
    return draw(nullptr,nullptr,pipeline.get(),nullptr,std::move(target),targetOffset,layout,std::move(vertex),vertexOffset,
        nullptr,0,completion);
}
Result Context::draw(std::shared_ptr<const VertexFragmentBufferGraphicsPipeline> pipeline,std::shared_ptr<Buffer> target,
    uint64_t targetOffset,const HNColorLayout& layout,std::shared_ptr<Buffer> vertex,uint64_t vertexOffset,
    std::shared_ptr<Buffer> fragment,uint64_t fragmentOffset,Completion& completion) {
    if(!pipeline||!vertex||!fragment)return fail(Status::Invalid);
    return draw(nullptr,nullptr,nullptr,pipeline.get(),std::move(target),targetOffset,layout,std::move(vertex),vertexOffset,
        std::move(fragment),fragmentOffset,completion);
}
Result Context::draw(const GraphicsPipeline* pipeline,const FragmentBufferGraphicsPipeline* buffered,
    const VertexBufferGraphicsPipeline* vertexPipeline,const VertexFragmentBufferGraphicsPipeline* combined,
    std::shared_ptr<Buffer> target,uint64_t targetOffset,const HNColorLayout& suppliedLayout,
    std::shared_ptr<Buffer> vertex,uint64_t vertexOffset,std::shared_ptr<Buffer> fragment,uint64_t fragmentOffset,Completion& completion) {
    // Hold independent immutable/resource references until after the mutex is
    // released. A caller's Buffer destruction cannot race native ownership.
    if (unsigned(bool(pipeline))+unsigned(bool(buffered))+unsigned(bool(vertexPipeline))+unsigned(bool(combined))!=1||!target)
        return fail(Status::Invalid);
    const bool needsVertex=vertexPipeline||combined,needsFragment=buffered||combined;
    if(bool(vertex)!=needsVertex||bool(fragment)!=needsFragment)return fail(Status::Invalid);
    const HNColorLayout layout = suppliedLayout;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->open) return fail(Status::NotOpen);
    if (!nativeabi::graphicsProfile(state_->info.header.minor)||
        (needsFragment&&!nativeabi::bufferedGraphicsProfile(state_->info.header.minor))||
        (needsVertex&&state_->info.header.minor!=nativeabi::kVertexBufferedGraphicsMinor))return fail(Status::Unsupported);
    if (target->owner_ != state_ || !nativeabi::validHandle(target->native_.handle) ||
        target->native_.epoch != state_->info.header.epoch || target->reservationSlot_ >= kNativeBufferSlots ||
        state_->reservations[target->reservationSlot_] != target.get()) return fail(Status::Invalid);
    const auto resourceValid=[&](const std::shared_ptr<Buffer>& resource,uint64_t offset,size_t bytes) {
        return resource&&resource!=target&&resource->owner_==state_&&
            nativeabi::validHandle(resource->native_.handle)&&resource->native_.epoch==state_->info.header.epoch&&
            resource->reservationSlot_<kNativeBufferSlots&&state_->reservations[resource->reservationSlot_]==resource.get()&&
            resource->native_.handle.id!=target->native_.handle.id&&!(offset&15)&&
            range(offset,bytes,resource->length_)&&resource->gpuAddress_>=state_->info.vaBegin&&
            resource->gpuAddress_<state_->info.vaEnd&&resource->allocationBytes_<=state_->info.vaEnd-resource->gpuAddress_;
    };
    if((needsVertex&&!resourceValid(vertex,vertexOffset,48))||(needsFragment&&!resourceValid(fragment,fragmentOffset,16)))
        return fail(Status::Invalid);
    if(needsVertex&&needsFragment&&(vertex==fragment||vertex->native_.handle.id==fragment->native_.handle.id||
        (vertex->gpuAddress_<fragment->gpuAddress_+fragment->allocationBytes_&&
         fragment->gpuAddress_<vertex->gpuAddress_+vertex->allocationBytes_)))return fail(Status::Invalid);
    uint64_t footprint = 0;
    auto result = graphicsTargetFootprint(state_->graphicsTopology, layout, target->length_,
                                          target->allocationBytes_, targetOffset, footprint);
    if (!result) return result;
    if (target->gpuAddress_ < state_->info.vaBegin || target->gpuAddress_ >= state_->info.vaEnd ||
        targetOffset > state_->info.vaEnd - target->gpuAddress_ ||
        footprint > state_->info.vaEnd - target->gpuAddress_ - targetOffset)
        return fail(Status::Invalid);
    const uint64_t address = target->gpuAddress_ + targetOffset;
    for(const auto* resource:{vertex.get(),fragment.get()})
        if(resource&&address<resource->gpuAddress_+resource->allocationBytes_&&resource->gpuAddress_<address+footprint)
            return fail(Status::Invalid);
    // No mutation precedes full data/control slot and resident budget checks.
    // Native code, attribute ring and private IB consume three slots; the
    // unmapped copied-code staging object consumes the fourth.
    if (state_->buffers > kNativeBufferSlots - 4 || state_->bytes > state_->info.systemBudget ||
        kGraphicsExtraBytes > state_->info.systemBudget - state_->bytes)
        return fail(Status::OutOfMemory);
    std::array<uint8_t,nativeabi::kGraphicsImageBytes> image{};
    result=combined?combined->copyImage(image.data(),image.size()):vertexPipeline?vertexPipeline->copyImage(image.data(),image.size()):
        buffered?buffered->copyImage(image.data(),image.size()):pipeline->copyImage(image.data(),image.size());
    if(!result)return result;
    transport::Buffer upload{};
    result = state_->client.allocate(nativeabi::kGraphicsImageBytes, 4096, nativeabi::Domain::System, 0, upload);
    if (!result) return state_->terminate(result);
    result = state_->client.write(upload, 0, image.data(), image.size());
    if (!result) return state_->terminate(result);
    result = state_->client.mapVa(target->native_, address, targetOffset, footprint, nativeabi::Read | nativeabi::Write);
    if (!result) return state_->terminate(result);
    if(needsVertex) {
        result=state_->client.mapVa(vertex->native_,vertex->gpuAddress_,0,vertex->allocationBytes_,nativeabi::Read);
        if(!result)return state_->terminate(result);
    }
    if(needsFragment) {
        result=state_->client.mapVa(fragment->native_,fragment->gpuAddress_,0,fragment->allocationBytes_,nativeabi::Read);
        if(!result)return state_->terminate(result);
    }
    transport::ProductDraw draw{};
    draw.code = upload; draw.target = target->native_; draw.targetOffset = targetOffset;
    draw.vertexBytes = combined?combined->vertexBytes():vertexPipeline?vertexPipeline->vertexBytes():buffered?buffered->vertexBytes():pipeline->vertexBytes();
    draw.fragmentBytes = combined?combined->fragmentBytes():vertexPipeline?vertexPipeline->fragmentBytes():buffered?buffered->fragmentBytes():pipeline->fragmentBytes();
    draw.targetLayout = layout;
    transport::Fence fence{};
    if(combined) {
        const transport::VertexFragmentBufferedProductDraw withBuffers={draw,vertex->native_,fragment->native_,vertexOffset,fragmentOffset};
        result=state_->client.graphicsProduct(withBuffers,fence);
    } else if(vertexPipeline) {
        const transport::VertexBufferedProductDraw withVertex={draw,vertex->native_,vertexOffset};
        result=state_->client.graphicsProduct(withVertex,fence);
    } else if(buffered) {
        const transport::BufferedProductDraw withBuffer={draw,fragment->native_,fragmentOffset};
        result=state_->client.graphicsProduct(withBuffer,fence);
    } else result = state_->client.graphicsProduct(draw, fence);
    if (!result) return state_->terminate(result);
    result = state_->client.wait(fence, nativeabi::kMaxWaitNs);
    if (!result) return state_->terminate(result);
    const Completion observed{fence.epoch, fence.sequence};
    result = state_->client.releaseResult(fence);
    if (!result) return state_->terminate(result);
    result = state_->client.unmapVa(target->native_, address, targetOffset, footprint);
    if (!result) return state_->terminate(result);
    if(needsVertex) {
        result=state_->client.unmapVa(vertex->native_,vertex->gpuAddress_,0,vertex->allocationBytes_);
        if(!result)return state_->terminate(result);
    }
    if(needsFragment) {
        result=state_->client.unmapVa(fragment->native_,fragment->gpuAddress_,0,fragment->allocationBytes_);
        if(!result)return state_->terminate(result);
    }
    result = state_->client.release(upload);
    if (!result) return state_->terminate(result);
    completion = observed; return ok();
}
} }
