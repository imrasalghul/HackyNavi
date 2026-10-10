// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hackynavi { namespace gfx11product {
// Gfx1100LeafBufferV1. The native caller must copy/own these bytes before
// invoking validate, then retain the unchanged copy through GPU retirement.
constexpr size_t kMaxCodeBytes = 16384;
constexpr uint32_t kMaxBindings = 32, kDescriptorBytes = 16, kPushBytes = 48;
constexpr uint32_t kWaveSize = 32, kMaxLocalThreads = 1024;
enum class Status : uint32_t {
    Ok, InvalidArgument, InvalidLaunch, Truncated, UnsupportedInstruction,
    ReservedBits, InvalidOperand, UninitializedRegister, PendingLoad,
    RegisterLimit, InvalidProvenance, OutOfBounds, InvalidClause,
    InvalidWait, MissingEnd, InvalidPadding
};
struct Request {
    const uint8_t* code;
    size_t codeBytes, executableBytes;
    uint32_t local[3];
    uint32_t waveSize, numSgprs, numVgprs;
};
// Pure diagnostics, never an authority token. On failure only status, byteOffset
// and instructionWord are meaningful; all derived launch/access fields are zero.
// No caller-provided resource masks, descriptors, RSRC values or code identities
// enter the validator. Native publication must itself revalidate owned bytes.
struct Report {
    Status status;
    uint32_t byteOffset, instructionWord;
    uint32_t instructionCount, requiredSgprs, requiredVgprs;
    uint32_t readMask, writeMask;
    uint32_t rsrc1, rsrc2, rsrc3;
};
Status validate(const Request&, Report&);
const char* statusString(Status);
} }
