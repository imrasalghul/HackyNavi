// SPDX-License-Identifier: MIT
// Bounded ATOM command interpreter; all hardware operations are caller supplied.
#ifndef HACKYNAVI_ATOM_VM_HPP
#define HACKYNAVI_ATOM_VM_HPP
#include <stddef.h>
#include <stdint.h>
namespace hackynavi { namespace atomvm {
enum class Status : uint8_t {
    Ok, InvalidArgument, BadImage, BadTable, Truncated, UnsupportedVersion,
    UnsupportedOpcode, UnsupportedIo, BadOperand, BadBranch, BadIndirectIo,
    MissingTable, ParameterBounds, WorkspaceBounds, ScratchBounds,
    InstructionLimit, NestingLimit, DelayLimit, IoFailed
};
enum class Space : uint8_t { Mmio, Pll, MemoryController };
enum class Event : uint8_t { PostCard, Beep, Debug };
struct Io {
    void* context;
    // Register addresses are DWORD indices, including reg_block. The adapter
    // performs byte conversion/indirect MMIO access and serializes shared ports.
    bool (*read)(void*, Space, uint32_t index, uint32_t& value);
    bool (*write)(void*, Space, uint32_t index, uint32_t value);
    bool (*delayMicroseconds)(void*, uint32_t microseconds);
    // Optional firmware diagnostic events; never used as a hardware operation.
    void (*event)(void*, Event, uint8_t value);
};
struct Limits {
    uint32_t instructions; // Main ISA and indirect-I/O instructions share this.
    uint64_t delayMicroseconds; // Cumulative requested delay, across all calls.
    uint8_t nesting; // 1..32; root command is depth one.
};
enum : size_t {
    kMaxDepth = 32, kWorkspaceWords = 255, kAddressSpace = 65536,
    kPendingInstructions = 8192, kAsicInitParameterBytes = 72
};
struct Program {
    // ROM must remain immutable and alive throughout analyze/execute. These
    // structures are not trust tokens; execute re-opens the supplied image.
    const uint8_t* image;
    size_t imageBytes;
    uint16_t commands, data, commandCount, dataCount;
    uint16_t indirectStart[256];
    uint32_t indirectEnd[256]; // Exclusive end may be exactly 65536.
};
struct Frame {
    uint32_t words[kWorkspaceWords];
    size_t parameterBase;
    uint32_t pc, start, end;
    uint8_t index, workspaceWords, parameterShift;
};
// Allocate this in owner-managed heap storage, never on a kernel stack. No
// dynamic allocation or native C++ recursion occurs in the interpreter.
struct Workspace {
    Frame frames[kMaxDepth];
    uint8_t byteKind[kAddressSpace];
    uint16_t pending[kPendingInstructions];
    uint8_t tables[256];
};
struct Result {
    Status status;
    uint16_t tableOffset, instructionOffset;
    uint8_t tableIndex, opcode, depth, maximumDepth;
    uint32_t instructions, analyzedInstructions, analyzedTables;
    uint32_t opcodes[4], calledTables[8];
    uint64_t delayedMicroseconds;
    uint32_t ioOperations;
    bool writeAttempted; // Set BEFORE invoking a potentially mutating callback.
};
Status open(const void* image, size_t bytes, Program& out);
// Structural control-flow validation follows BOTH sides of branches, all cases,
// and CALLTABLE edges. It executes no instructions and performs no hardware I/O.
Status analyze(const Program&, uint8_t tableIndex, Workspace&, Result&);
// Parameters/scratch are little-endian byte arrays, not host-endian u32 arrays.
// FB operands address CPU scratch exactly as Linux atom.c; they are NOT GPU VRAM.
// Scratch is persistent caller storage and is not cleared on entry. Parameters
// contain nested table parameter space too (CALLTABLE shifts by header PS/4).
// Every call starts with zeroed global VM state and new zeroed frame workspaces.
// Unsupported instructions, malformed control flow, failed I/O and exhausted
// limits return an error, with all previous writes reported in Result.
Status execute(const Program&, uint8_t tableIndex, void* parameters,
               size_t parameterBytes, void* scratch, size_t scratchBytes,
               const Io&, const Limits&, Workspace&, Result&);
// Builds the real v2.1 ASIC_Init 8-byte parameters + 64-byte reserved call space
// from FirmwareInfo v3.x boot clocks. fbReset=true is Linux's post-MODE1 path.
Status prepareAsicInit(const Program&, bool fbReset,
                       uint8_t (&parameters)[kAsicInitParameterBytes]);
const char* statusName(Status);
} }
#endif
