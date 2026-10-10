// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_GFX_COMPUTE_HPP
#define HACKYNAVI_GFX_COMPUTE_HPP
#include <stddef.h>
#include <stdint.h>
namespace hackynavi { namespace gfxcompute {
constexpr size_t kShaderImageBytes = 4096;
constexpr uint64_t kCopyShaderOffset = 1024;
constexpr size_t kMaxCommandDwords = 128;
constexpr uint32_t kMaxTransferBytes = 256U * 1024U * 1024U;
enum class Operation { Clear, Copy };
enum class Status { Ok, InvalidArgument, InvalidRange, OverlappingMemory, BufferTooSmall };
struct Buffer { uint64_t gpu, bytes; };
struct Request {
    Operation operation;
    Buffer shader, source, destination;
    uint32_t bytes;
    uint32_t pattern[4];
};
// Exact AMD GFX11 buffer-clear/copy machine code, including instruction
// prefetch padding, in a fixed 4KiB image. Caller uploads and verifies this
// image in owned GPU-executable VRAM before referencing it. No GPU work here.
Status shaderImage(uint8_t* destination, size_t capacity);
// Trusted GC11 graphics-ring dispatch only, not a userspace PM4 validator.
// Requires running GC11/GFXHUB, immutable resident shaderImage(), exclusive
// graphics state and owned resident VMID0 buffers. Addresses use the lower
// 47-bit GPU VA region. Buffers must be disjoint; transfer size is a nonzero
// multiple of16, up to256MiB, and must fit both descriptors for Copy.
// Clear's source descriptor must be zero. Shader/data CPU edits must be
// published with actual DMA synchronization/HDP before submitting this IB.
// The stream invalidates shader/data caches, sets source-derived CS state,
// dispatches 64-thread groups, waits CS_PARTIAL_FLUSH and synchronizes PFP.
// The queue owner MUST append its real RELEASE_MEM cache-writeback fence and
// observe that fence before CPU readback, memory reuse or retirement.
// No hardware callbacks, simulated execution, memory allocation or fallback.
// All failures leave destination and words unchanged. Input/output storage
// must be disjoint, little-endian, stable and externally serialized.
Status encode(const Request&, uint32_t* destination, size_t capacityWords, size_t& words);
const char* statusString(Status);
} }
#endif
