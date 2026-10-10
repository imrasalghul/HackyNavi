// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hackynavi { namespace gfx11graphics {
// Gfx1100PositionConstantColorV1. This is shader admission, not draw admission.
// The caller must first copy the entire image to its own stable CPU storage,
// serialize access, and retain identical GPU RX bytes until actual retirement.
// Neither a pointer nor a successful Report proves BO ownership/residency.
constexpr size_t kMaxStageBytes = 1024, kCodeAlignment = 256, kImageAlignment = 4096;
constexpr uint32_t kWaveSize = 32, kAllocatedVgprs = 24, kHardwareSgprs = 108;
enum class Status : uint32_t {
    Ok, InvalidArgument, InvalidLayout, InvalidPadding, InvalidControlFlow,
    Truncated, UnsupportedInstruction, ReservedBits, InvalidOperand,
    UninitializedRegister, RegisterLimit, InvalidExport
};
enum class Stage : uint32_t { Image, Vertex, Fragment };
struct Layout {
    size_t vertexBytes, fragmentOffset, fragmentBytes, imageBytes;
};
// Vertex starts at zero; PS starts at the next 256-byte boundary; the image
// ends at the next 4096-byte boundary. All bytes outside code must be zero.
// Each stage includes exactly five ACO s_code_end words after executable code.
// On failure, layout() leaves output unchanged.
Status layout(size_t vertexBytes, size_t fragmentBytes, Layout& output);
struct Request {
    const uint8_t* image;
    size_t imageBytes, vertexBytes, fragmentBytes;
};
struct Program {
    uint32_t executableBytes, instructionCount, requiredVgprs;
    uint32_t allocatedVgprs, hardwareSgprs, waveSize, rsrc1, rsrc2;
};
// Diagnostic/derived data only, never a transferable authority token. On
// failure every field except status/stage/byteOffset/instructionWord is zero.
// Offsets are relative to the complete image, including for fragment errors.
// No caller-provided hardware register values or compiler metadata are trusted.
// Exception: an output overlapping the claimed image is rejected unchanged,
// because publishing diagnostics would itself modify the immutable input.
struct Report {
    Status status;
    Stage stage;
    uint32_t byteOffset, instructionWord;
    Layout image;
    Program vertex, fragment;
    uint32_t vgtShaderStagesEn, spiShaderPosFormat;
    uint32_t spiPsInputEna, spiPsInputAddr, spiPsInControl;
    uint32_t spiShaderColFormat, spiShaderZFormat, cbShaderMask;
    uint32_t fragmentColorBits[4];
};
// The only admitted ABI is resource-free Position-only passthrough NGG and
// constant RGBA32 MRT0 PS: wave32/CU, zero LDS/scratch/attributes/parameters.
// NGG system s3/v0/v5 and base-vertex s8 must be supplied by the eventual
// trusted draw builder. That builder must also own RT/NGG state, barriers,
// topology, code BO mappings, submission and reset/retirement protocols.
Status validate(const Request&, Report&);
// Additive Gfx1100PositionFragmentBufferV1 admission. The unchanged vertex
// contract is paired with four 32-bit reads of byte offsets 0,4,8,12 from the
// single immutable raw descriptor supplied in PS s0..s3. Every component must
// reach its matching RGBA export after vmcnt(0). No other memory operation,
// scalar write, address arithmetic or descriptor source is admitted. The native
// draw owner must derive and retain the exact read-only 16-byte resource.
// fragmentColorBits is zero on success because the color is GPU memory data,
// not a constant known by admission. The old validate() still rejects loads.
Status validateFragmentBuffer(const Request&, Report&);
// Additive vertex-buffer forms. NGG s8..s11 contains a trusted raw48 read-only
// descriptor; s12 is baseVertex0. The only address is (s12+VertexID)*16, with
// four DWORD loads at 0,4,8,12 reaching Position XYZW after vmcnt(0).
// The native draw must restrict IDs to 0..2 and retain the exact 48-byte span.
// Old profiles continue to reject this ABI. No shader hash/metadata is trusted.
Status validateVertexBuffer(const Request&, Report&);
Status validateVertexFragmentBuffers(const Request&, Report&);
const char* statusString(Status);
} }
