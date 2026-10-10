// SPDX-License-Identifier: MIT
#pragma once
#include "hackynavi/gfx11_graphics_product.hpp"
#include "hackynavi/gfx11_color_target.h"
#include "hackynavi/isolated_compute.hpp"
namespace hackynavi { namespace gfx11draw {
constexpr size_t kCodeBytes=4096,kIndirectBytes=8192,kMaxCommandDwords=kIndirectBytes/4;
constexpr uint32_t kAttributeBytesPerSe=22*65536;
// Private native observations, NEVER a public wire structure or transferable
// token. Existing color topology alone is insufficient: GC discovery and
// harvested per-SA CU observations must be captured by the same live owner.
struct Topology {
    HNColorTopology color;
    uint32_t maxShaderEngines,maxRenderBackends,minGoodCuPerSa;
};
enum class Status { Ok,InvalidArgument,UnsupportedTopology,ForeignContext,
    InvalidBinding,Permission,AliasedStorage,Overlap,InvalidShader,InvalidTarget,
    BufferTooSmall,InvalidStream };
struct Request {
    uint64_t owner,epoch;
    uint8_t vmid;
    Topology topology;
    gfx11graphics::Request image;
    HNColorLayout targetLayout;
    gfxisolated::Binding code,target,attributes,indirect;
};
// Caller-owned CPU scratch, outside the kernel stack. Reconstructed from code,
// topology and actual bindings on EVERY entry point; never admission authority.
struct Workspace {
    gfx11graphics::Report program;
    uint32_t address32Hi,psCuMask,lateAlloc,nggCuMask;
    uint32_t vertexPrefetch,fragmentPrefetch;
    size_t commandWords;
};
// Additive internal draw form. This is not a userspace wire ABI. The buffer is
// a retained read-only page mapping from the same native context; offset is
// relative to that mapping, 16-byte aligned. The descriptor exposes exactly
// 16 bytes, never the page-rounded extent. CPU/kernel mapping provenance,
// generation, real DMA independence and lifetime remain native obligations.
struct BufferedRequest { Request draw; gfxisolated::Binding fragment; uint64_t offset; };
struct BufferedWorkspace { Workspace draw; uint32_t descriptor[4]; };
// The vertex span is exactly three float4 positions (48 bytes), aligned16.
// Separate types select the shader ABI; there is no caller-supplied descriptor
// or public flag which could enable a shader without its required resource.
struct VertexBufferedRequest { Request draw; gfxisolated::Binding vertex; uint64_t offset; };
struct VertexFragmentBufferedRequest {
    Request draw; gfxisolated::Binding vertex,fragment; uint64_t vertexOffset,fragmentOffset;
};
struct VertexBufferedWorkspace { Workspace draw; uint32_t vertexDescriptor[4],fragmentDescriptor[4]; };
// Exactly one triangle (3 vertices, one instance, baseVertex0), one linear
// RGBA8 target, full-target Metal viewport/scissor (negative hardware Y scale,
// unchanged clip Z=0..1), one sample, no memory-reading
// shaders/depth/stencil/blend/query/streamout/texture/VRS/indirect operation.
// Code/private IB RX; attributes private RW with 64KiB VA alignment, exact
// per-SE extent and BIG_PAGE=0; target RW. Code+attributes share a4GiB VA window.
// The whole immutable code page is mapped, including gfx11 prefetch distance3.
// All storage IDs/VA ranges are distinct. Actual residency, DMA independence,
// topology provenance, no CPU/user aliases, stable generation, hardware
// exclusion and lifetime remain native-owner obligations. No startup callsite.
// CPU inputs remain readable/stable/serialized. Workspace/output must not alias
// request or code. Workspace may change on failure, except alias rejection;
// output stream and word-count remain unchanged on every failure.
Status prepare(const Request&,Workspace&);
Status encode(const Request&,Workspace&,uint32_t*,size_t capacityWords,size_t& words);
// Exact canonical IB comparison after revalidation, not arbitrary PM4 parsing.
Status validate(const Request&,Workspace&,const uint32_t*,size_t exactWords);
// Same triangle/target/state/retirement contract, with the independently
// admitted fragment-buffer shader. All four old bindings and the fragment
// mapping must have distinct storage IDs and disjoint entire VA extents.
// Descriptor words are reconstructed on each call, never accepted as input.
Status prepare(const BufferedRequest&,BufferedWorkspace&);
Status encode(const BufferedRequest&,BufferedWorkspace&,uint32_t*,size_t capacityWords,size_t& words);
Status validate(const BufferedRequest&,BufferedWorkspace&,const uint32_t*,size_t exactWords);
// Same ownership/retirement obligations, with a read-only vertex mapping and
// optional independently admitted fragment mapping. Every full binding extent
// and storage ID is distinct, including vertex versus fragment.
Status prepare(const VertexBufferedRequest&,VertexBufferedWorkspace&);
Status encode(const VertexBufferedRequest&,VertexBufferedWorkspace&,uint32_t*,size_t capacityWords,size_t& words);
Status validate(const VertexBufferedRequest&,VertexBufferedWorkspace&,const uint32_t*,size_t exactWords);
Status prepare(const VertexFragmentBufferedRequest&,VertexBufferedWorkspace&);
Status encode(const VertexFragmentBufferedRequest&,VertexBufferedWorkspace&,uint32_t*,size_t capacityWords,size_t& words);
Status validate(const VertexFragmentBufferedRequest&,VertexBufferedWorkspace&,const uint32_t*,size_t exactWords);
const char* statusString(Status);
} }
