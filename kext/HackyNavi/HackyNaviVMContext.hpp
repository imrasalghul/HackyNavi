// SPDX-License-Identifier: MIT
#pragma once
#include "HackyNaviDMABuffer.hpp"
#include "HackyNaviResetController.hpp"
#include "hackynavi/vm_context.hpp"
#include "hackynavi/vm_page_tree.hpp"
#include "hackynavi/isolated_compute.hpp"
#include "hackynavi/gfx_aco_fixture.hpp"
#include "hackynavi/gfx11_compute_product.hpp"
#include "hackynavi/gfx11_color_target.h"
#include <libkern/c++/OSObject.h>
class HackyNaviGARTController;
class HackyNaviGraphicsQueue;

// Owns real wired/zeroed IODMACommand storage, a sparse GMC11 page tree and one
// VMID1..7 lease on both hubs. All operations use the GART controller's mutex,
// including allocation and shared invalidate engine17. No second VM lock.
// Sleepable context only; caller holds the outer device epoch/admission lock,
// excludes PM/reset/GFXOFF and every producer of this VMID through enable and
// detach, and keeps PCI open/the DMA domain stable. Native queue admission is
// limited to sealed clear/copy, the recorded Apple add_one ACO fixture and
// copied Gfx1100LeafBufferV1 products and the fixed position/constant-color
// graphics profile independently admitted from actual ISA and owned bindings.
// Arbitrary shader/PM4 input and a userspace scheduler are not implemented.
// Hardware/allocation/submission entry points require the caller's live
// HackyNaviGARTController::Access, installed before descendant locks. Normal
// work requires Operation authority; cleanup accepts RetirementScope. Only
// CPU-only unpublished release is permitted without live hardware authority.

class HackyNaviVMContext : public OSObject {
    OSDeclareDefaultStructors(HackyNaviVMContext)
public:
    enum : uint32_t { kBufferSlots = 64, kMappingSlots = 128,
        kMaxBufferBytes = HackyNaviDMABuffer::kMaxBytes,
        kResidentBudget = 256U * 1024U * 1024U };
    struct Handle { uint32_t id, generation; };
    enum Access : uint32_t { Read = 1, Write = 2, Execute = 4 };
    enum class State { Empty, Prepared, Captured, Active, DetachedRetained,
                       FailedUnpublished, FaultedRetained, Retired };
    struct ComputeRequest {
        hackynavi::gfxcompute::Operation operation;
        Handle source, destination;
        uint64_t sourceOffset, destinationOffset;
        uint32_t bytes, pattern[4];
    };
    struct CompilerComputeRequest {
        Handle source, destination;
        uint64_t sourceOffset, destinationOffset; // backing BO byte offsets
    };
    struct ProductResource {
        uint32_t binding;
        Handle buffer;
        uint64_t offset, bytes; // exact BO range, never a GPU address
    };
    struct ProductComputeRequest {
        const hackynavi::gfx11product::Request* variants;
        size_t variantCount;
        const ProductResource* resources;
        size_t resourceCount;
        uint32_t grid[3], nominalLocal[3];
        Handle result;
        uint64_t resultOffset;
        uint32_t resultBytes;
    };
    struct ProductGraphicsRequest {
        const uint8_t* image;
        size_t imageBytes, vertexBytes, fragmentBytes;
        Handle target;
        uint64_t targetOffset;
        HNColorLayout targetLayout;
    };
    struct ProductBufferedGraphicsRequest {
        ProductGraphicsRequest draw;
        Handle fragment;
        uint64_t fragmentOffset; // actual BO offset, exactly 16 bytes read
    };
    struct ProductVertexBufferedGraphicsRequest {
        ProductGraphicsRequest draw;
        Handle vertex;
        uint64_t vertexOffset; // actual BO offset, exactly48 bytes read
    };
    struct ProductVertexFragmentBufferedGraphicsRequest {
        ProductGraphicsRequest draw;
        Handle vertex,fragment;
        uint64_t vertexOffset,fragmentOffset;
    };
    struct Snapshot {
        State state;
        uint8_t vmid;
        uint64_t epoch, residentBytes;
        uint32_t buffers, mappings, tablePages;
        bool requiresHardwareRetirement;
        hackynavi::vmcontext::Result mm, gfx;
    };
    // Leases a VMID immediately, retaining GART. Allocates a real root page;
    // factory does not program hardware. The registry retains a successful
    // context until explicit releaseBeforeExposure(), queue-proven completed
    // job retirement, or typed reset retirement;
    // merely dropping a client reference does not remove its registry lease.
    // epoch is the kernel owner's actual
    // reset/resource epoch, never a userspace-provided readiness assertion.
    static HackyNaviVMContext* withGART(HackyNaviGARTController*, uint8_t vmid,
                                      uint64_t epoch, IOReturn&);
    // Initial implementation is system-DMA only: 4KiB-multiple allocations,
    // maximum16MiB per BO, 256MiB/context, 64 generation slots, 128 mappings,
    // at most1024 table pages. This is not a claim of general RADV BO support.
    IOReturn allocateBuffer(uint64_t bytes, uint32_t alignment, Handle&);
    IOReturn freeBuffer(Handle);
    IOReturn map(Handle, uint64_t bufferOffset, uint64_t va, uint64_t bytes, uint32_t access);
    IOReturn unmap(Handle, uint64_t va, uint64_t bytes); // exact mapping only
    // Kernel-owned byte copies; no raw address/descriptor export. These do
    // not implement IOUserClient CPU mappings or synchronized GPU readback.
    IOReturn writeBuffer(Handle, uint64_t offset, const void*, size_t bytes);
    IOReturn readBuffer(Handle, uint64_t offset, void*, size_t bytes);
    // Captures the canonical built-in clear/copy kernel and command stream in
    // new private read/execute BOs in THIS context. Data bindings must already
    // exist. Success seals every CPU write/mapping edit until the queue proves
    // completion and consumes this job. No arbitrary ACO/RADV PM4.
    IOReturn captureCompute(const ComputeRequest&);
    // Captures the exact recorded GFX1100 Apple add_one compiler product.
    // Executes 4096 uint32 elements; the retained result also includes64 guard
    // words (16640 bytes total). Source/destination must be separate BOs with
    // existing exact R/RW mappings. A third private R page holds descriptors
    // and the translator dispatch payload. No caller code or PM4 is accepted.
    IOReturn captureCompilerCompute(const CompilerComputeRequest&);
    // Internal kernel interface: all pointers must name live kernel memory
    // for this call. Copies code and metadata into private ownership before
    // validation. Only the reviewed Gfx1100LeafBufferV1 ISA is admitted; native
    // handles resolve actual mappings and the kernel creates descriptors/PM4.
    // No IOUserClient selector accepts this pointer-bearing structure.
    IOReturn captureProductCompute(const ProductComputeRequest&);
    // Copies a canonical owned position/constant-color image. Target must have
    // one exact RW page mapping covering its validated RGBA8 footprint. Other
    // non-internal data BOs may retain one R/RW mapping; aliases/Execute fail.
    // The
    // actual same-generation GART topology supplies every hardware fact; code,
    // IB and GE attribute-ring storage are newly allocated private controls.
    // No numeric topology, GPU address, admission report or PM4 is accepted.
    IOReturn captureProductGraphics(HackyNaviFunctionGroup::Operation&, const ProductGraphicsRequest&);
    // Additive internal buffer profile. Resolves a live generation handle and
    // its one read-only mapping, derives the exact 16-byte descriptor, and
    // seals/retains the real buffer with all prior graphics resources through
    // queue completion/consumption or reset. No caller descriptor or GPU VA.
    IOReturn captureProductGraphics(HackyNaviFunctionGroup::Operation&, const ProductBufferedGraphicsRequest&);
    IOReturn captureProductGraphics(HackyNaviFunctionGroup::Operation&, const ProductVertexBufferedGraphicsRequest&);
    IOReturn captureProductGraphics(HackyNaviFunctionGroup::Operation&, const ProductVertexFragmentBufferedGraphicsRequest&);

    // All BO/tree mutation and CPU access stop at activation.
    // Without a real per-VM queue completion/drain proof, live map/unmap/free
    // or CPU reuse must return NotPermitted. Disable/invalidate ACK is NOT a
    // DMA-retirement proof. A self-hold retains this owner and every allocation
    // before first publication, including ambiguous write/publication failure.
    IOReturn activate(const hackynavi::vmcontext::Poll&);
    IOReturn detach(const hackynavi::vmcontext::Poll&);
    IOReturn releaseBeforeExposure();
    IOReturn releaseAfterVerifiedReset(const HackyNaviResetController::RetirementProof&);
    void snapshot(Snapshot&);
protected:
    bool init() override;
    void free() override;
private:
    friend class HackyNaviGraphicsQueue;
    struct Buffer { HackyNaviDMABuffer* dma; uint32_t generation, mappings; bool internal; };
    struct Mapping { Handle buffer; uint64_t va, bytes, offset; uint32_t access; };
    IOReturn initialize(HackyNaviGARTController*, uint8_t, uint64_t);
    bool leased() const;
    bool providerReady(bool dma) const;
    IOReturn mutableState() const;
    IOReturn allocateBufferLocked(uint64_t, uint32_t, Handle&);
    IOReturn mapLocked(Handle, uint64_t, uint64_t, uint64_t, uint32_t);
    IOReturn activateLocked(const hackynavi::vmcontext::Poll&);
    IOReturn detachLocked(const hackynavi::vmcontext::Poll&);
    // Only the queue's actual completed-job owner may call this after both
    // hubs detach. Releases private job controls, preserving all data/root/
    // VMID ownership. The queue drops exactly one hardware hold after success
    // and unlocking; this method never drops that reference or proves idle.
    IOReturn consumeCapturedStorageLocked();
    IOReturn beginCaptureLocked();
    bool bindingLocked(Handle, uint64_t offset, uint64_t bytes, uint32_t access,
                       hackynavi::gfxisolated::Binding&, uint64_t& gpu);
    bool controlVaLocked(uint64_t&, uint64_t bytes = 4096) const;
    bool captureValidLocked() const;
    bool captureStorageValidLocked() const;
    bool capturedControlMaskLocked(uint64_t&) const;
    bool compilerStorageDistinctLocked(const hackynavi::gfxaco::Request&) const;
    struct ProductCapture;
    bool productCaptureValidLocked() const;
    bool productStorageDistinctLocked() const;
    void releaseProductCaptureLocked();
    struct GraphicsCapture;
    IOReturn captureGraphicsProduct(HackyNaviFunctionGroup::Operation&,const ProductGraphicsRequest&,
        Handle fragment,uint64_t fragmentOffset,Handle vertex,uint64_t vertexOffset);
    bool graphicsCaptureValidLocked() const;
    bool graphicsStorageDistinctLocked() const;
    bool graphicsMappingsValidLocked(const hackynavi::gfxisolated::Binding&, bool controls) const;
    bool graphicsControlMaskLocked(uint64_t&) const;
    bool graphicsWritableControlLocked(const Mapping&) const;
    void releaseGraphicsCaptureLocked();
    Buffer* find(Handle);
    IOReturn finish(IOReturn);
    IOReturn treeStatus(hackynavi::vmpt::Status);
    IOReturn releaseStorageLocked();
    bool copyPages(const Buffer&, uint64_t offset, uint64_t bytes, uint64_t* pages);
    static bool allocatePage(void*, hackynavi::vmpt::Storage&);
    static bool releasePage(void*, hackynavi::vmpt::Storage&);
    static bool read32(void*, uint32_t, uint32_t&);
    static bool write32(void*, uint32_t, uint32_t);
    static void delayUs(void*, uint32_t);
    static bool publish(void*);
    hackynavi::vmcontext::Io io();
    HackyNaviGARTController* gart_;
    hackynavi::vmpt::Page* pages_;
    hackynavi::vmpt::Tree tree_;
    Buffer buffers_[kBufferSlots];
    Mapping mappings_[kMappingSlots];
    hackynavi::vmcontext::Context mm_, gfx_;
    hackynavi::vmcontext::Result mmResult_, gfxResult_;
    IOReturn allocationResult_;
    State state_;
    uint8_t vmid_;
    uint64_t epoch_, residentBytes_;
    bool hardwareHold_, registryHold_, reusableDetached_;
    enum class ShaderClass { None, BuiltInClearCopy, AppleAddOneGfx1100V1, Gfx1100LeafBufferV1,
                             Gfx1100PositionConstantColorV1, Gfx1100PositionFragmentBufferV1,
                             Gfx1100PositionVertexBufferV1, Gfx1100PositionVertexFragmentBuffersV1 };
    struct CapturedResult { Handle buffer; uint64_t offset, bytes; };
    ShaderClass capturedClass_;
    hackynavi::gfxisolated::Request captured_;
    hackynavi::gfxaco::Request capturedCompiler_;
    ProductCapture* capturedProduct_;
    GraphicsCapture* capturedGraphics_;
    CapturedResult capturedResult_;
    uint64_t capturedIndirectGpu_;
    Handle capturedShader_, capturedIndirect_, capturedParameters_;
    Handle capturedAttributes_;
    uint32_t capturedWords_;
    // Issued for each capture by the actual queue BEFORE exposure; never set by public
    // activate(). Compare-only binding: the exposed queue self-hold and each
    // completion keep its issuer alive. No pointer dereference/retain cycle.
    HackyNaviGraphicsQueue* submittedQueue_;
    uint32_t submittedSequence_, submittedProducer_;
    // Monotonic per-owner identity, independent of recycled BO slots or queue
    // counters. Failed capture attempts may consume a generation; no wrap.
    uint64_t captureGeneration_, submittedCaptureGeneration_;
};
