// SPDX-License-Identifier: MIT
#pragma once
#include "hackynavi/native_abi.hpp"
#include <mutex>

namespace hackynavi { namespace transport {
enum class Status { Ok, Invalid, NotOpen, Unsupported, NotFound, AmbiguousDevice,
                    OutOfMemory, Busy, Timeout, DeviceLost, Protocol, Transport };
struct Result { Status status; int32_t iokit; explicit operator bool() const { return status == Status::Ok; } };
struct Buffer { nativeabi::Handle handle{}; uint64_t epoch = 0; };
struct Mapping { void* address = nullptr; uint64_t bytes = 0; };
struct Fence { uint64_t sequence = 0, epoch = 0; };
struct ProductVariant {
    Buffer code;
    uint64_t offset;
    uint32_t codeBytes, executableBytes, local[3], waveSize, numSgprs, numVgprs;
};
struct ProductResource { Buffer buffer; uint64_t offset, bytes; uint32_t binding; };
struct ProductDispatch {
    const ProductVariant* variants;
    uint32_t variantCount;
    const ProductResource* resources;
    uint32_t resourceCount;
    uint32_t grid[3], nominalLocal[3];
    Buffer result;
    uint64_t resultOffset;
    uint32_t resultBytes;
};
struct ProductDraw {
    Buffer code, target;
    uint64_t codeOffset, targetOffset;
    uint32_t vertexBytes, fragmentBytes;
    HNColorLayout targetLayout;
};
struct BufferedProductDraw { ProductDraw draw; Buffer fragment; uint64_t fragmentOffset; };
struct VertexBufferedProductDraw { ProductDraw draw; Buffer vertex; uint64_t vertexOffset; };
struct VertexFragmentBufferedProductDraw {
    ProductDraw draw; Buffer vertex,fragment; uint64_t vertexOffset,fragmentOffset;
};

// Real IOKit transport only. No alternate/software backend exists here.
// Connections represent one isolated VM and one in-order graphics queue.
// Methods serialize the connection; caller must still synchronize buffer data.
class Client final {
public:
    Client() = default;
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Result open(uint64_t registryId = 0);
    Result openBuiltIn(uint64_t registryId = 0);
    Result openCompiler(uint64_t registryId = 0);
    Result openProduct(uint64_t registryId = 0);
    Result openReusableProduct(uint64_t registryId = 0);
    Result openGraphicsProduct(uint64_t registryId = 0);
    Result openBufferedGraphicsProduct(uint64_t registryId = 0);
    Result openVertexBufferedGraphicsProduct(uint64_t registryId = 0);
    // Opens this exact retained IOAccelerator port once. Hello continues to
    // identify its immediate native owner, not the distinct accelerator ID.
    // No direct-owner enumeration or fallback follows a failed open.
    Result openBufferedGraphicsAccelerator(uint32_t acceleratorPort,
        uint64_t acceleratorRegistryId,uint64_t ownerRegistryId);
    Result openVertexBufferedGraphicsAccelerator(uint32_t acceleratorPort,
        uint64_t acceleratorRegistryId,uint64_t ownerRegistryId);
    Result close(); // Explicit Close RPC surfaces native retirement errors.
    Result info(nativeabi::HelloReply&);
    Result allocate(uint64_t bytes, uint64_t alignment, nativeabi::Domain, uint32_t flags, Buffer&);
    Result release(Buffer&); // Busy while CPU-mapped; kernel also enforces GPU retirement.
    Result mapCpu(const Buffer&, Mapping&);
    Result unmapCpu(const Buffer&);
    Result mapVa(const Buffer&, uint64_t va, uint64_t offset, uint64_t bytes, uint32_t access);
    Result unmapVa(const Buffer&, uint64_t va, uint64_t offset, uint64_t bytes);
    Result submit(const nativeabi::Ib*, uint32_t ibCount,
                  const nativeabi::Resource*, uint32_t resourceCount, Fence&);
    Result wait(Fence, uint64_t timeoutNs);
    Result write(const Buffer&, uint64_t offset, const void*, size_t bytes);
    Result read(const Buffer&, uint64_t offset, void*, size_t bytes);
    Result clear(const Buffer&, uint64_t offset, uint32_t bytes, const uint32_t pattern[4], Fence&);
    Result copy(const Buffer& source, uint64_t sourceOffset, const Buffer& destination,
                uint64_t destinationOffset, uint32_t bytes, Fence&);
    // Executes the pinned AppleAddOneGfx1100V1 product. Offsets are backing-BO
    // offsets; each source/destination extent must contain kCompilerResultBytes.
    Result compilerAddOne(const Buffer& source, uint64_t sourceOffset, const Buffer& destination,
                          uint64_t destinationOffset, Fence&);
    // Code is staged with allocate/write in unmapped BOs. Data BOs have exact
    // R/RW mappings; the kernel derives access from independently admitted ISA.
    // v1.3 seals after the first attempted dispatch. v1.4 reopens only after
    // actual completion/readback and native job consumption; failures never
    // permit retry. Up to eight results remain readable until explicitly freed.
    Result product(const ProductDispatch&, Fence&);
    // v1.5 only. Numeric inputs for the real Mesa layout calculator; neither
    // they nor its output are accepted as hardware ownership by the kernel.
    Result graphicsInformation(HNColorTopology&);
    // One triangle / one instance, resource-free Position and constant RGBA.
    // Result contains the target's exact linear slice, including pitch padding.
    Result graphicsProduct(const ProductDraw&, Fence&);
    // v1.6 adds one actual read-only16-byte fragment buffer. Offset refers to
    // its backing BO; the native owner resolves the current mapping itself.
    Result graphicsProduct(const BufferedProductDraw&, Fence&);
    Result graphicsProduct(const VertexBufferedProductDraw&, Fence&);
    Result graphicsProduct(const VertexFragmentBufferedProductDraw&, Fence&);
    Result readResult(Fence, uint64_t relativeOffset, void*, size_t bytes);
    // v1.4 CPU history release; clears the supplied fence only on confirmed
    // success. Does not free a BO or confer any GPU retirement permission.
    Result releaseResult(Fence&);
private:
    struct Record {
        nativeabi::AllocateReply allocation{};
        Mapping mapping{};
    };
    static constexpr uint32_t kMaxBuffers = 4096;
    Result exchange(nativeabi::Selector, const void*, size_t, void*, size_t);
    Result openProfile(uint64_t, uint64_t requiredCapabilities, uint16_t minor);
    Result openServiceLocked(uint32_t servicePort,uint64_t ownerRegistryId,
        uint64_t requiredCapabilities,uint16_t minor);
    nativeabi::Header header(size_t bytes) const;
    Result bufferIo(const Buffer&, uint64_t, void*, size_t, bool write);
    Result compute(nativeabi::ComputeRequest&, Fence&);
    Result graphicsProduct(const ProductDraw&,const Buffer* fragment,uint64_t fragmentOffset,
        const Buffer* vertex,uint64_t vertexOffset,Fence&);
    Result va(const Buffer&, uint64_t, uint64_t, uint64_t, uint32_t, bool);
    Record* find(const Buffer&);
    struct HistoricalResult { uint64_t sequence = 0, bytes = 0; };
    HistoricalResult* findResult(Fence);
    HistoricalResult* freeResult();
    bool reusable() const { return nativeabi::reusableProfile(hello_.header.minor); }
    HistoricalResult results_[nativeabi::kMaxRetainedResults]{};
    Result closeLocked();
    std::mutex mutex_;
    uint32_t connection_ = 0;
    nativeabi::HelloReply hello_{};
    Record* records_ = nullptr;
    uint64_t submitted_ = 0;
    uint64_t resultBytes_ = 0;
    bool lost_ = false;
    bool sealed_ = false;
};
} }
