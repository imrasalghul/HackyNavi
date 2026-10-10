// SPDX-License-Identifier: MIT
// Shared little-endian x86_64 user-client ABI. No pointers, physical addresses,
// register selectors, task ports, or user-writable completion words cross it.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hackynavi/gfx11_color_target.h"

namespace hackynavi { namespace nativeabi {
constexpr uint32_t kMagic = 0x314e5648; // HVN1
constexpr uint16_t kMajor = 1, kMinor = 1, kGenericMinor = 0;
// v1.1 clear/copy remains a separate negotiated profile. v1.2 admits only the
// exact compiler-produced class below. v1.3 copies staged Gfx1100LeafBufferV1
// products into private storage and independently admits their actual ISA.
// v1.4 adds serialized reuse and eight explicitly retained historical results;
// it never permits caller-writable command buffers or CPU mappings.
// v1.5 adds the bounded Position/constant-color graphics product to v1.4.
// It never accepts caller PM4, hardware topology authority or completion data.
constexpr uint16_t kCompilerMinor = 2, kProductMinor = 3, kReusableMinor = 4,
    kGraphicsMinor = 5, kBufferedGraphicsMinor = 6, kVertexBufferedGraphicsMinor = 7,
    kMaxMinor = kVertexBufferedGraphicsMinor;
constexpr bool bufferedGraphicsProfile(uint16_t minor) {
    return minor == kBufferedGraphicsMinor || minor == kVertexBufferedGraphicsMinor;
}
constexpr bool graphicsProfile(uint16_t minor) { return minor == kGraphicsMinor || bufferedGraphicsProfile(minor); }
constexpr bool reusableProfile(uint16_t minor) { return minor == kReusableMinor || graphicsProfile(minor); }
constexpr uint32_t kUserClientType = 0x484e5631; // HNV1
constexpr uint32_t kMaxMessage = 4096, kMaxIbs = 32, kMaxResources = 64;
constexpr uint32_t kMaxIbBytes = 1024 * 1024;
constexpr uint64_t kMaxWaitNs = 2000000000ULL;
constexpr uint32_t kMaxCopyBytes = 2048;
constexpr const char* kServiceClass = "HackyNaviDevice";

enum class Selector : uint32_t { Hello, Allocate, Free, MapVa, UnmapVa, Submit, Wait,
    WriteBuffer, ReadBuffer, Compute, ReadResult, Close, CompilerCompute, ProductCompute, ReleaseResult,
    GraphicsInformation, ProductGraphics, ProductBufferedGraphics,
    ProductVertexBufferedGraphics, ProductVertexFragmentBufferedGraphics, Count };
enum Capability : uint64_t {
    IsolatedGpuVa = 1ULL << 0,
    CapturedValidatedIb = 1ULL << 1,
    GpuWrittenFence = 1ULL << 2,
    ZeroedAllocation = 1ULL << 3,
    CpuMapping = 1ULL << 4,
    PinnedSubmissionResources = 1ULL << 5,
    ResetRetirement = 1ULL << 6,
    BuiltInCompute = 1ULL << 7,
    CopiedBufferIO = 1ULL << 8,
    SealedSingleJob = 1ULL << 9,
    CompilerFixtureCompute = 1ULL << 10,
    ProductLeafCompute = 1ULL << 11,
    ReusableCompute = 1ULL << 12,
    PositionConstantColorGraphics = 1ULL << 13,
    PositionFragmentBufferGraphics = 1ULL << 14,
    PositionVertexBufferGraphics = 1ULL << 15,
    PositionVertexFragmentBufferGraphics = 1ULL << 16
};
constexpr uint64_t kRequiredCapabilities = (1ULL << 7) - 1;
constexpr uint64_t kBuiltInCapabilities = IsolatedGpuVa | GpuWrittenFence | ZeroedAllocation |
    PinnedSubmissionResources | ResetRetirement | BuiltInCompute | CopiedBufferIO | SealedSingleJob;
constexpr uint64_t kCompilerCapabilities = kBuiltInCapabilities | CompilerFixtureCompute;
constexpr uint64_t kProductCapabilities = kBuiltInCapabilities | ProductLeafCompute;
// v1.4 preserves the actual VM/data BOs after queue-proven completed-job
// consumption. Historical result slots have CPU ownership only.
constexpr uint64_t kReusableCapabilities = (kProductCapabilities & ~SealedSingleJob) | ReusableCompute;
constexpr uint64_t kGraphicsCapabilities = kReusableCapabilities | PositionConstantColorGraphics;
constexpr uint64_t kBufferedGraphicsCapabilities = kGraphicsCapabilities | PositionFragmentBufferGraphics;
constexpr uint64_t kVertexBufferedGraphicsCapabilities = kBufferedGraphicsCapabilities |
    PositionVertexBufferGraphics | PositionVertexFragmentBufferGraphics;
constexpr uint64_t graphicsCapabilities(uint16_t minor) {
    return minor == kVertexBufferedGraphicsMinor ? kVertexBufferedGraphicsCapabilities :
        minor == kBufferedGraphicsMinor ? kBufferedGraphicsCapabilities : kGraphicsCapabilities;
}
constexpr uint32_t kMaxRetainedResults = 8;
constexpr uint64_t kKnownCapabilities = (1ULL << 17) - 1;
enum class Domain : uint32_t { Vram = 1, System = 2 };
enum AllocationFlag : uint32_t { CpuVisible = 1 };
enum Access : uint32_t { Read = 1, Write = 2, Execute = 4 };
constexpr uint32_t kAccessMask = Read | Write | Execute;
enum class FenceState : uint32_t { Pending = 0, Complete = 1, DeviceLost = 2 };

struct Header {
    uint32_t magic;
    uint16_t major, minor;
    uint32_t bytes, flags;
    uint64_t epoch; // kernel-assigned connection generation; changes after reset
};
struct Handle { uint32_t id, generation; };
struct HelloRequest { Header header; uint64_t requiredCapabilities, reserved; };
struct HelloReply {
    Header header;
    uint64_t capabilities, registryId, vramBytes, cpuVisibleVramBytes, systemBudget;
    uint64_t vaBegin, vaEnd, maxAllocationBytes; // vaEnd is exclusive
    uint16_t vendorId, deviceId;
    uint32_t gcVersion, pageBytes, maxIbs, maxResources, maxIbBytes;
    uint64_t reserved[2];
};
struct AllocateRequest {
    Header header;
    uint64_t bytes, alignment;
    uint32_t domain, flags;
    uint64_t reserved;
};
struct AllocateReply {
    Header header;
    Handle handle;
    uint64_t bytes;
    uint32_t memoryType, domain, flags, reserved;
};
struct FreeRequest { Header header; Handle handle; uint64_t reserved; };
struct VaRequest {
    Header header;
    Handle handle;
    uint64_t va, offset, bytes;
    uint32_t access, reserved;
};
struct Ib { Handle handle; uint64_t offset; uint32_t words, reserved; };
struct Resource { Handle handle; uint32_t access, reserved; };
struct SubmitRequest {
    Header header;
    uint32_t ibCount, resourceCount;
    uint64_t reserved;
    // ibCount Ib records followed by resourceCount Resource records.
    // Kernel must copy this message once, resolve connection-owned handles,
    // pin mappings and BOs, COPY all command buffers into kernel-owned storage,
    // validate every packet/operand and capture or reject nested IBs, then
    // submit in an isolated VM. Caller-writable IBs must never be executed.
};
struct SubmitReply { Header header; uint64_t sequence, reserved; };
struct WaitRequest { Header header; uint64_t sequence, timeoutNs; };
struct WaitReply {
    Header header;
    uint64_t sequence, completedSequence;
    uint32_t state, reserved;
    // Complete means the GPU-written fence retired with cache visibility.
    // Reset, cancellation, timeout, transport success alone are not completion.
};
// v1.1 copied I/O: WriteBuffer appends exactly bytes payload bytes. ReadBuffer
// and ReadResult return DataReply followed by exactly bytes payload bytes.
// ReadBuffer is pre-capture only; ReadResult uses an actual GPU completion.
struct BufferIoRequest { Header header; Handle handle; uint64_t offset; uint32_t bytes, reserved; };
struct DataReply { Header header; uint32_t bytes, reserved; };
enum class ComputeOperation : uint32_t { Clear = 1, Copy = 2 };
struct ComputeRequest {
    Header header;
    Handle source, destination;
    uint64_t sourceOffset, destinationOffset;
    uint32_t bytes, operation, pattern[4];
    uint64_t reserved;
};
struct ReadResultRequest { Header header; uint64_t sequence, offset; uint32_t bytes, reserved; };
// v1.4 only. Exact connection epoch + actual nonwrapping queue sequence.
// Releases a historical CPU copy; this is never hardware retirement authority.
struct ReleaseResultRequest { Header header; uint64_t sequence, reserved; };
static_assert(sizeof(ReleaseResultRequest) == 40, "historical result release layout");
enum class CompilerShaderClass : uint32_t { AppleAddOneGfx1100V1 = 1 };
constexpr uint32_t kCompilerElements = 4096, kCompilerGuardWords = 64;
constexpr uint32_t kCompilerResultBytes = (kCompilerElements + kCompilerGuardWords) * 4;
struct CompilerComputeRequest {
    Header header;
    Handle source, destination;
    uint64_t sourceOffset, destinationOffset;
    uint32_t shaderClass, reserved;
};
// v1.3 Gfx1100LeafBufferV1. Code BOs hold staging bytes, never executable user
// mappings. Kernel copies code and metadata into private ownership and derives
// descriptor access and RSRC values by decoding actual ISA. These records are
// transport data, never evidence of successful shader admission or completion.
constexpr uint32_t kMaxProductVariants = 8, kMaxProductResources = 32;
constexpr uint32_t kMaxProductCodeBytes = 16384, kMaxProductLocalThreads = 1024;
struct ProductComputeRequest {
    Header header;
    uint32_t variantCount, resourceCount;
    uint32_t grid[3], nominalLocal[3];
    Handle result;
    uint64_t resultOffset;
    uint32_t resultBytes, reserved;
    // variantCount ProductVariant records, then resourceCount ProductResource.
};
struct ProductVariant {
    Handle code;
    uint64_t offset;
    uint32_t codeBytes, executableBytes, local[3], waveSize, numSgprs, numVgprs;
    uint32_t reserved[2];
};
struct ProductResource {
    uint32_t binding, reserved;
    Handle buffer;
    uint64_t offset, bytes;
};
struct CapturedProductCompute {
    ProductComputeRequest request;
    ProductVariant variants[kMaxProductVariants];
    ProductResource resources[kMaxProductResources];
};
// GraphicsInformation takes a Header and returns address-free numeric layout
// inputs from the retained kernel owner. These bytes grant no hardware lease.
struct GraphicsInformationReply { Header header; HNColorTopology topology; uint64_t reserved; };
constexpr uint32_t kGraphicsImageBytes = 4096, kGraphicsStageBytes = 1024;
constexpr uint64_t kMaxGraphicsTargetBytes = 16ULL * 1024 * 1024;
struct ProductGraphicsRequest {
    Header header;
    Handle code; // Unmapped upload BO, copied before actual ISA admission.
    uint64_t codeOffset;
    uint32_t vertexBytes, fragmentBytes;
    Handle target;
    uint64_t targetOffset;
    HNColorLayout targetLayout; // Calculated by Mesa, revalidated in kernel.
    uint64_t reserved;
};
static_assert(sizeof(GraphicsInformationReply) == 40 && sizeof(ProductGraphicsRequest) == 136,
              "graphics wire layout");
// v1.6 additive selector. The embedded draw header describes this entire
// 160-byte message; all v1.5 record sizes/selectors remain unchanged. Exactly
// one buffer at Metal index0 is read-only over16bytes. No GPU VA/descriptor.
struct ProductBufferedGraphicsRequest {
    ProductGraphicsRequest draw;
    Handle fragment;
    uint64_t fragmentOffset, fragmentBytes;
};
static_assert(sizeof(ProductBufferedGraphicsRequest) == 160 &&
              offsetof(ProductBufferedGraphicsRequest, fragment) == 136,
              "fragment buffer graphics wire layout");
// v1.7 preserves earlier selectors and adds two explicit resource contracts.
// Vertex buffer0 supplies exactly three float4 positions (48 bytes); optional
// fragment buffer0 supplies16 bytes. Offsets are BO offsets, never GPU VAs.
// Stage byte counts remain draw.vertexBytes/draw.fragmentBytes. Resource byte
// counts use distinct names to avoid confusing shader size with buffer bounds.
struct ProductVertexBufferedGraphicsRequest {
    ProductGraphicsRequest draw;
    Handle vertex;
    uint64_t vertexOffset, vertexBufferBytes;
};
struct ProductVertexFragmentBufferedGraphicsRequest {
    ProductVertexBufferedGraphicsRequest vertexDraw;
    Handle fragment;
    uint64_t fragmentOffset, fragmentBufferBytes;
};
static_assert(sizeof(ProductVertexBufferedGraphicsRequest) == 160 &&
              offsetof(ProductVertexBufferedGraphicsRequest, vertex) == 136,
              "vertex buffer graphics wire layout");
static_assert(sizeof(ProductVertexFragmentBufferedGraphicsRequest) == 184 &&
              offsetof(ProductVertexFragmentBufferedGraphicsRequest, fragment) == 160,
              "vertex and fragment buffer graphics wire layout");
static_assert(offsetof(ProductGraphicsRequest, codeOffset) == 32 &&
              offsetof(ProductGraphicsRequest, targetLayout) == 64, "graphics wire offsets");
static_assert(sizeof(ProductComputeRequest) == 80 && sizeof(ProductVariant) == 56 &&
              sizeof(ProductResource) == 32 && sizeof(CapturedProductCompute) == 1552, "product layout");
static_assert(offsetof(ProductComputeRequest, resultOffset) == 64 &&
              offsetof(ProductVariant, offset) == 8 && offsetof(ProductResource, offset) == 16,
              "product offsets");
static_assert(sizeof(CapturedProductCompute) <= kMaxMessage, "product inline message bound");
static_assert(sizeof(CompilerComputeRequest) == 64, "compiler class layout");
static_assert(sizeof(Header) == 24 && sizeof(Handle) == 8, "ABI scalar layout");
static_assert(sizeof(HelloRequest) == 40 && sizeof(HelloReply) == 128, "hello layout");
static_assert(sizeof(AllocateRequest) == 56 && sizeof(AllocateReply) == 56, "allocation layout");
static_assert(sizeof(FreeRequest) == 40 && sizeof(VaRequest) == 64, "BO operation layout");
static_assert(sizeof(Ib) == 24 && sizeof(Resource) == 16, "submission descriptor layout");
static_assert(sizeof(SubmitRequest) == 40 && sizeof(SubmitReply) == 40, "submission layout");
static_assert(sizeof(WaitRequest) == 40 && sizeof(WaitReply) == 48, "fence layout");
static_assert(sizeof(BufferIoRequest) == 48 && sizeof(DataReply) == 32 &&
              sizeof(ComputeRequest) == 88 && sizeof(ReadResultRequest) == 48, "built-in layout");
static_assert(sizeof(BufferIoRequest) + kMaxCopyBytes <= kMaxMessage &&
              sizeof(DataReply) + kMaxCopyBytes <= kMaxMessage, "inline copy bound");
static_assert(offsetof(Header, epoch) == 16 && offsetof(HelloReply, vaBegin) == 64, "ABI offsets");
static_assert(sizeof(SubmitRequest) + kMaxIbs * sizeof(Ib) + kMaxResources * sizeof(Resource) <= kMaxMessage,
              "submission must fit inline IOKit message");

enum class Validation { Ok, Size, Header, Version, Epoch, Reserved, Bounds, Handle, Access, Unsupported };
struct CapturedSubmit {
    SubmitRequest request;
    Ib ibs[kMaxIbs];
    Resource resources[kMaxResources];
};
Header makeHeader(size_t bytes, uint64_t epoch, uint16_t minor = kMinor);
bool validHandle(Handle);
bool sameHandle(Handle, Handle);
Validation validateHeader(const Header&, size_t bytes, uint64_t epoch, uint16_t minor = kMinor);
Validation validateHelloRequest(const HelloRequest&, uint64_t supportedCapabilities);
Validation validateHello(const HelloReply&, uint64_t expectedRegistryId = 0,
                         uint64_t requiredCapabilities = kRequiredCapabilities);
Validation validateAllocate(const AllocateRequest&, const HelloReply&);
Validation validateVa(const VaRequest&, const HelloReply&, uint64_t allocationBytes, bool unmap);
// Validation of the transport envelope only, NOT a PM4/shader security parser.
// Kernel additionally validates its own current handle table, sizes, VM mappings,
// and captured GPU commands before setting CapturedValidatedIb capability.
Validation decodeSubmit(const void*, size_t, const HelloReply&, CapturedSubmit&);
Validation validateWait(const WaitReply&, uint64_t epoch, uint64_t requestedSequence,
                        uint16_t minor = kMinor);
Validation validateBufferIo(const BufferIoRequest&, size_t messageBytes, uint64_t epoch, bool write,
                            uint16_t minor = kMinor);
Validation validateCompute(const ComputeRequest&, uint64_t epoch, uint16_t minor = kMinor);
Validation validateReadResult(const ReadResultRequest&, uint64_t epoch, uint16_t minor = kMinor);
Validation validateCompilerCompute(const CompilerComputeRequest&, const HelloReply&);
// Parses an unaligned inline message, clears out on every failure. Kernel must
// still resolve current connection handles, copy the actual BO code bytes,
// admit ISA, resolve exact data mappings and prove native GPU retirement.
Validation decodeProductCompute(const void*, size_t, const HelloReply&, CapturedProductCompute&);
Validation validateProductCompute(const CapturedProductCompute&, uint64_t epoch,
                                  uint16_t minor = kProductMinor);
Validation validateReleaseResult(const ReleaseResultRequest&, const HelloReply&);
Validation validateGraphicsInformation(const GraphicsInformationReply&, const HelloReply&);
// Envelope checks only. Native ownership, actual topology/layout, private ISA,
// captured commands and real GPU retirement are independently enforced later.
Validation validateProductGraphics(const ProductGraphicsRequest&, uint64_t epoch,
                                   uint16_t minor = kGraphicsMinor);
Validation decodeProductGraphics(const void*, size_t, const HelloReply&, ProductGraphicsRequest&);
Validation validateProductBufferedGraphics(const ProductBufferedGraphicsRequest&, uint64_t epoch,
                                           uint16_t minor = kBufferedGraphicsMinor);
Validation decodeProductBufferedGraphics(const void*, size_t, const HelloReply&, ProductBufferedGraphicsRequest&);
Validation validateProductVertexBufferedGraphics(const ProductVertexBufferedGraphicsRequest&, uint64_t epoch,
                                                 uint16_t minor = kVertexBufferedGraphicsMinor);
Validation decodeProductVertexBufferedGraphics(const void*, size_t, const HelloReply&, ProductVertexBufferedGraphicsRequest&);
Validation validateProductVertexFragmentBufferedGraphics(const ProductVertexFragmentBufferedGraphicsRequest&, uint64_t epoch,
                                                         uint16_t minor = kVertexBufferedGraphicsMinor);
Validation decodeProductVertexFragmentBufferedGraphics(const void*, size_t, const HelloReply&, ProductVertexFragmentBufferedGraphicsRequest&);
} }
