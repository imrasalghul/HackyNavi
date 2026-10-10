// SPDX-License-Identifier: MIT
#pragma once
#include "../transport/iokit_client.hpp"
#include "../metal/include/hackynavi_pipeline.h"
#include <array>
#include <memory>
#include <vector>

namespace hackynavi { namespace compute {
using Result = transport::Result;
using Status = transport::Status;
struct State;
class Context;
class GraphicsPipeline;
class FragmentBufferGraphicsPipeline;
class VertexBufferGraphicsPipeline;
class VertexFragmentBufferGraphicsPipeline;

// CPU-owned immutable copy of an actual successful compiler product. Capture
// does not open a GPU and is not kernel ISA admission. The original compiler
// result may be freed after capture returns. Exact-thread products only.
class Pipeline final {
public:
    static Result capture(const HNPipelineResult*, const uint32_t grid[3],
                          const uint32_t local[3], std::shared_ptr<const Pipeline>&);
    const std::vector<HNPipelineResource>& resources() const { return resources_; }
    uint32_t variantCount() const { return variantCount_; }
    Result variantInfo(uint32_t index, HNPipelineVariant&) const;
    Result copyCode(uint32_t index, void* output, size_t exactBytes) const;
private:
    friend class Context;
    Pipeline() = default;
    struct Variant { HNPipelineVariant metadata{}; std::vector<uint8_t> code; };
    std::array<Variant, nativeabi::kMaxProductVariants> variants_;
    std::vector<HNPipelineResource> resources_;
    uint32_t variantCount_ = 0, grid_[3]{}, local_[3]{};
};

// Real native allocation with a persistent connection-owned BO generation.
// No CPU pointer is exposed: Shared/Managed Metal storage is not implemented
// by returning a disconnected heap mirror. Copied I/O is explicit.
class Buffer final {
public:
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    uint64_t length() const { return length_; }
    uint64_t allocatedSize() const { return allocationBytes_; }
    // Stable reservation in this connection's isolated VA space. Native pages
    // are mapped there only while the buffer participates in a dispatch.
    // This conveys no permission to submit unchecked pointers or packets.
    uint64_t gpuAddress() const { return gpuAddress_; }
    Result upload(uint64_t offset, const void*, size_t bytes);
    // Failure leaves the destination unchanged, including a late RPC failure.
    Result download(uint64_t offset, void*, size_t bytes);
    // Invalidates every shared reference to this Buffer. length stays immutable;
    // later I/O and binding attempts fail. A failed native release closes the
    // context and returns that failure; close() exposes cleanup status too.
    Result release();
private:
    friend class Context;
    explicit Buffer(std::shared_ptr<State> owner) : owner_(std::move(owner)) {}
    std::shared_ptr<State> owner_;
    transport::Buffer native_{};
    uint64_t length_ = 0, allocationBytes_ = 0;
    uint64_t gpuAddress_ = 0;
    uint32_t reservationSlot_ = 0;
};

struct Binding { uint32_t binding = 0; std::shared_ptr<Buffer> buffer; uint64_t offset = 0, bytes = 0; };
// Historical observation only. It is not a transport fence or retirement token.
struct Completion { uint64_t epoch = 0, sequence = 0; };

// Serialized synchronous native compute backend for the Metal object layer.
// There is no alternate backend. All GPU results come from the actual native
// user client; no automatic retry follows a potentially exposed submission.
class Context final {
public:
    static Result open(uint64_t registryId, std::shared_ptr<Context>&);
    // Explicit v1.5 graphics+reusable-compute profile. open() stays v1.4.
    static Result openGraphics(uint64_t registryId, std::shared_ptr<Context>&);
    static Result openBufferedGraphics(uint64_t registryId, std::shared_ptr<Context>&);
    static Result openBufferedGraphicsAccelerator(uint32_t acceleratorPort,
        uint64_t acceleratorRegistryId,uint64_t ownerRegistryId,std::shared_ptr<Context>&);
    static Result openVertexBufferedGraphics(uint64_t registryId,std::shared_ptr<Context>&);
    static Result openVertexBufferedGraphicsAccelerator(uint32_t acceleratorPort,
        uint64_t acceleratorRegistryId,uint64_t ownerRegistryId,std::shared_ptr<Context>&);
    Result information(nativeabi::HelloReply&) const;
    // Returns actual numeric observations retained from this native owner's
    // graphicsInformation RPC. They confer no topology/lease authority.
    Result graphicsInformation(HNColorTopology&) const;
    // Current persistent native BO bytes, including their actual page padding.
    // Serialized with allocation/release/dispatch. A lost or closed owner does
    // not report zero: hardware retirement may still retain its allocations.
    Result allocatedSize(uint64_t&) const;
    Result allocate(uint64_t length, std::shared_ptr<Buffer>&);
    Result dispatch(std::shared_ptr<const Pipeline>, const Binding*, size_t count, Completion&);
    // One resource-free triangle into the exact validated linear RGBA8 target.
    // The same real Buffer survives completion. No implicit CPU upload/copy;
    // callers explicitly download when needed. Outputs stay unchanged on
    // failure, including cleanup failure after real hardware execution.
    Result draw(std::shared_ptr<const GraphicsPipeline>, std::shared_ptr<Buffer> target,
                uint64_t targetOffset, const HNColorLayout&, Completion&);
    Result draw(std::shared_ptr<const FragmentBufferGraphicsPipeline>, std::shared_ptr<Buffer> target,
                uint64_t targetOffset, const HNColorLayout&, std::shared_ptr<Buffer> fragment,
                uint64_t fragmentOffset, Completion&);
    Result draw(std::shared_ptr<const VertexBufferGraphicsPipeline>,std::shared_ptr<Buffer> target,
        uint64_t targetOffset,const HNColorLayout&,std::shared_ptr<Buffer> vertex,uint64_t vertexOffset,Completion&);
    Result draw(std::shared_ptr<const VertexFragmentBufferGraphicsPipeline>,std::shared_ptr<Buffer> target,
        uint64_t targetOffset,const HNColorLayout&,std::shared_ptr<Buffer> vertex,uint64_t vertexOffset,
        std::shared_ptr<Buffer> fragment,uint64_t fragmentOffset,Completion&);
    // Explicit close returns native retirement status and invalidates all live
    // Buffer objects. Without it, buffers retain the connection's actual owner.
    Result close();
private:
    static Result openProfile(uint64_t registryId, uint16_t minor, std::shared_ptr<Context>&,
        uint32_t acceleratorPort=0,uint64_t acceleratorRegistryId=0);
    Result draw(const GraphicsPipeline*,const FragmentBufferGraphicsPipeline*,const VertexBufferGraphicsPipeline*,
        const VertexFragmentBufferGraphicsPipeline*,std::shared_ptr<Buffer>,uint64_t,const HNColorLayout&,
        std::shared_ptr<Buffer> vertex,uint64_t vertexOffset,std::shared_ptr<Buffer> fragment,uint64_t fragmentOffset,Completion&);
    explicit Context(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
};
} }
