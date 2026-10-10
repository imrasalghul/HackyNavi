// SPDX-License-Identifier: MIT
#pragma once
#include "hackynavi/gfx11_compute_product.hpp"
#include "hackynavi/isolated_compute.hpp"
namespace hackynavi { namespace gfx11dispatch {
constexpr size_t kMaxVariants=8,kMaxRegions=8,kMaxBindings=32;
constexpr size_t kControlBytes=4096,kDescriptorBytes=512,kPushBytes=48;
constexpr size_t kMaxCommandDwords=kControlBytes/4;
enum class Status { Ok,InvalidArgument,ForeignContext,InvalidBinding,Permission,
    AliasedStorage,Overlap,InvalidShader,InvalidDimensions,MissingVariant,
    UnusedVariant,InvalidResources,BufferTooSmall,InvalidParameters,InvalidStream };
struct Variant {
    gfx11product::Request program;
    // One private RX allocation, exactly round_up(program.codeBytes,4096).
    // program.code points to this FULL readable CPU image; all page padding
    // after codeBytes must be zero. No writable/CPU-user alias is permitted.
    gfxisolated::Binding shader;
};
struct Resource {
    uint32_t binding;
    gfxisolated::Binding mapping;
    uint64_t offset,bytes; // mapping-relative byte bounds, never a raw descriptor
};
struct Request {
    uint64_t owner,epoch;
    uint8_t vmid;
    // Exact-thread mode. Nonzero dimensions, nominal local product<=1024.
    // All Cartesian full/tail regions are generated here, in mask order0..7.
    uint32_t grid[3],nominalLocal[3];
    const Variant* variants;size_t variantCount;
    const Resource* resources;size_t resourceCount;
    gfxisolated::Binding indirect,parameters; // private4KiB RX and R respectively
};
struct Region {
    uint32_t variant,local[3],groups[3],push[12];
};
// Caller-owned scratch; allocate outside the kernel stack. Contents are pure
// diagnostics, never authority. Every entry point reconstructs this workspace
// from the actual bytes and bindings; no report supplied by a caller is trusted.
struct Workspace {
    gfx11product::Report variants[kMaxVariants];
    Region regions[kMaxRegions];
    size_t regionCount,commandWords;
    uint32_t readMask,writeMask;
};
// All CPU input objects/images must remain stable, readable, externally
// serialized and disjoint from workspace/output for each call. Numeric POD
// bindings cannot establish residency: native capture must resolve exact owned
// generation slots/mappings and actual independent DMA owners, then seal edits.
// Every BO ID and mapped VA extent must be distinct, including all data BOs.
// Data permissions are exactly R or RW derived from decoded ISA effects;
// unused descriptor slots are zero. Shader/IB RX, parameters R, never writable.
Status prepare(const Request&,Workspace&);
// Zeroes the entire private parameter page, creates bounded raw descriptors
// at16*binding and each region's real48-byte dispatch payload at512+48*region.
// Output bytes and words remain unchanged on failure (workspace may change).
Status parameterImage(const Request&,Workspace&,uint8_t*,size_t capacityBytes);
Status encode(const Request&,Workspace&,uint32_t*,size_t capacityWords,size_t& words);
// Full parameter page and exact canonical IB comparison, after revalidating
// every actual code image. No caller PM4/RSRC/push constants are admitted.
Status validate(const Request&,Workspace&,const uint8_t* parameters,size_t parameterBytes,
    const uint32_t* commands,size_t words);
// Each region waits CS_PARTIAL_FLUSH then ACQUIRE_MEM at PFP with instruction,
// scalar/vector/L1 invalidation and L2/metadata writeback+invalidation before
// subsequent state/dispatch. Native queue STILL owns the final RELEASE_MEM
// fence, consumed RPTR, CPU visibility and retained VM retirement proof.
const char* statusString(Status);
} }
