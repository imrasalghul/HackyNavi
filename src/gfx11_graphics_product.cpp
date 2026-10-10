// SPDX-License-Identifier: MIT
// Narrow gfx1100 graphics ISA admission, not a compiler identity allowlist.
// Source proof and the exact lane-control contract: docs/gfx11-graphics-product.md.
#include "hackynavi/gfx11_graphics_product.hpp"

namespace hackynavi { namespace gfx11graphics { namespace {
constexpr uint32_t kPadding = 0xbf9f0000, kEnd = 0xbfb00000;
uint32_t word(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
        (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
bool span(const void* p, size_t n) {
    return p && n && reinterpret_cast<uintptr_t>(p) <= UINTPTR_MAX - (n - 1);
}
bool overlap(const void* a, size_t an, const void* b, size_t bn) {
    const uintptr_t av = reinterpret_cast<uintptr_t>(a), bv = reinterpret_cast<uintptr_t>(b);
    return av <= bv ? bv - av < an : av - bv < bn;
}
bool inlineValue(unsigned src, uint32_t& value) {
    if (src >= 128 && src <= 192) { value = src - 128; return true; }
    if (src >= 193 && src <= 208) { value = 192u - src; return true; }
    constexpr uint32_t floats[] = {0x3f000000, 0xbf000000, 0x3f800000,
        0xbf800000, 0x40000000, 0xc0000000, 0x40800000, 0xc0800000, 0x3e22f983};
    if (src >= 240 && src <= 248) { value = floats[src - 240]; return true; }
    return false;
}
struct Value { uint32_t bits; bool known; };
struct State {
    Value values[kAllocatedVgprs];
    uint32_t defined;
    unsigned required, instructions;
    bool vertex, mask1, vcc;
    explicit State(bool isVertex) : values{}, defined(isVertex ? 1u << 5 : 0),
        required(isVertex ? 6 : 1), instructions(0), vertex(isVertex), mask1(false), vcc(false) {}
    Status read(unsigned src, uint32_t literal, Value& out) {
        out = {};
        if (inlineValue(src, out.bits)) { out.known = true; return Status::Ok; }
        if (src == 255) { out = {literal, true}; return Status::Ok; }
        if (src == 8 && vertex) return Status::Ok; // trusted base vertex, no pointer provenance
        if (src >= 256 && src < 256 + kAllocatedVgprs) {
            const unsigned r = src - 256;
            if (!(defined & (1u << r))) return Status::UninitializedRegister;
            out = values[r]; return Status::Ok;
        }
        return src >= 256 ? Status::RegisterLimit : Status::InvalidOperand;
    }
    Status destination(unsigned r) const {
        if (r >= kAllocatedVgprs) return Status::RegisterLimit;
        // v0 belongs to the primitive lanes, which may not execute this body.
        return vertex && !r ? Status::InvalidOperand : Status::Ok;
    }
    void write(unsigned r, Value value) {
        values[r] = value; defined |= 1u << r;
        if (required <= r) required = r + 1;
    }
    Status mask(unsigned src) const {
        if (src == 1) return mask1 ? Status::Ok : Status::UninitializedRegister;
        if (src == 106) return vcc ? Status::Ok : Status::UninitializedRegister;
        return Status::InvalidOperand;
    }
};
enum class Op : uint8_t { Mov, Add, Select, Compare, Dual, Delay };
struct Instruction {
    Op op;
    unsigned words, destination, sources[3], count, maskSource, neg;
    unsigned otherDestination, otherSources[2], dualOps[2];
    uint32_t literal;
};

// Body instructions cannot change EXEC, branch, access memory, export, alter
// scalar system values or end execution. Special scalar writes are permitted
// only for compare-generated wave32 masks in s1 or VCC_LO, never ordinary data.
Status decode(const uint8_t* p, size_t bytes, bool vertex, Instruction& i) {
    if (bytes < 4) return Status::Truncated;
    i = {}; i.words = 1;
    const uint32_t w = word(p);
    if ((w & 0xffff0000) == 0xbf870000 && vertex) {
        const unsigned imm = w & 65535;
        // ACO: two 4-bit dependency IDs, a 3-bit skip, reserved high five bits.
        if ((imm & 0xf800) || (imm & 15) > 11 || ((imm >> 7) & 15) > 11 ||
            ((imm >> 4) & 7) > 5) return Status::ReservedBits;
        i.op = Op::Delay; return Status::Ok;
    }
    if ((w & 0xfe000000) == 0x7e000000) {
        if (((w >> 9) & 255) != 1) return Status::UnsupportedInstruction;
        i.op = Op::Mov; i.destination = (w >> 17) & 255;
        i.sources[0] = w & 511; i.count = 1;
    } else if ((w & 0xfe000000) == 0x7c000000 && vertex) {
        const unsigned op = (w >> 17) & 255;
        if (op != 0x42 && op != 0x45) return Status::UnsupportedInstruction;
        i.op = Op::Compare; i.destination = 106;
        i.sources[0] = w & 511; i.sources[1] = 256 + ((w >> 9) & 255); i.count = 2;
    } else if ((w & 0xfc000000) == 0xd4000000 && vertex) {
        if (bytes < 8) return Status::Truncated;
        const uint32_t second = word(p + 4);
        const unsigned op = (w >> 16) & 1023;
        if (w & 0xff00 || second & 0x98000000) return Status::ReservedBits;
        i.words = 2; i.destination = w & 255;
        for (unsigned n = 0; n < 3; ++n) i.sources[n] = (second >> (9*n)) & 511;
        i.neg = second >> 29;
        if (op == 0x101) {
            i.op = Op::Select; i.count = 2; i.maskSource = i.sources[2];
        } else if (op == 0x42 || op == 0x45) {
            if (i.sources[2] != 128 || i.neg) return Status::ReservedBits;
            i.op = Op::Compare; i.count = 2;
        } else if (op == 0x181) {
            if (i.sources[1] != 128 || i.sources[2] != 128 || i.neg) return Status::ReservedBits;
            i.op = Op::Mov; i.count = 1;
        } else return Status::UnsupportedInstruction;
    } else if ((w & 0xfc000000) == 0xc8000000) {
        if (bytes < 8) return Status::Truncated;
        const uint32_t second = word(p + 4);
        i.op = Op::Dual; i.words = 2;
        i.dualOps[0] = (w >> 22) & 15; i.dualOps[1] = (w >> 17) & 31;
        for (unsigned n = 0; n < 2; ++n)
            if (i.dualOps[n] != 8 && !(vertex && i.dualOps[n] == 9))
                return Status::UnsupportedInstruction;
        i.destination = second >> 24;
        i.otherDestination = ((second >> 17) & 127)*2 + 1 - (i.destination & 1);
        i.sources[0] = w & 511; i.sources[1] = 256 + ((w >> 9) & 255);
        i.otherSources[0] = second & 511; i.otherSources[1] = 256 + ((second >> 9) & 255);
        if ((i.dualOps[0] == 8 && i.sources[1] != 256) ||
            (i.dualOps[1] == 8 && i.otherSources[1] != 256)) return Status::ReservedBits;
        // On gfx11 each corresponding source port must use opposite VGPR
        // parity. This also excludes same-register forwarding between halves.
        for (unsigned n = 0; n < 2; ++n) {
            if (n && (i.dualOps[0] == 8 || i.dualOps[1] == 8)) continue;
            if (i.sources[n] >= 256 && i.otherSources[n] >= 256 &&
                (i.sources[n] & 1) == (i.otherSources[n] & 1)) return Status::InvalidOperand;
        }
        // This profile has no ordinary SGPR data in VOPD. Its only SGPR
        // operand is implicit VCC for CNDMASK, which excludes a literal.
        if (i.sources[0] < 128 || i.otherSources[0] < 128) return Status::InvalidOperand;
        const bool literal = i.sources[0] == 255 || i.otherSources[0] == 255;
        if (literal && (i.dualOps[0] == 9 || i.dualOps[1] == 9)) return Status::InvalidOperand;
        if (literal) {
            if (bytes < 12) return Status::Truncated;
            i.literal = word(p + 8); i.words = 3;
        }
        return Status::Ok;
    } else if (!(w & 0x80000000) && vertex) {
        const unsigned op = w >> 25;
        if (op != 0x25 && op != 1) return Status::UnsupportedInstruction;
        i.op = op == 0x25 ? Op::Add : Op::Select;
        i.destination = (w >> 17) & 255; i.maskSource = 106;
        i.sources[0] = w & 511; i.sources[1] = 256 + ((w >> 9) & 255); i.count = 2;
    } else return Status::UnsupportedInstruction;
    bool literal = false;
    unsigned scalarReads = i.op == Op::Select ? 1 : 0;
    bool baseRead = false;
    for (unsigned n = 0; n < i.count; ++n) {
        literal |= i.sources[n] == 255; baseRead |= i.sources[n] == 8;
    }
    scalarReads += unsigned(literal) + unsigned(baseRead);
    if (scalarReads > 2) return Status::InvalidOperand;
    if (literal) {
        if (bytes < (i.words + 1)*4) return Status::Truncated;
        i.literal = word(p + i.words*4); ++i.words;
    }
    return Status::Ok;
}

Status apply(const Instruction& i, State& state) {
    if (i.op == Op::Delay) return Status::Ok;
    if (i.op == Op::Compare && i.destination != 1 && i.destination != 106)
        return Status::InvalidOperand;
    if (i.op != Op::Compare) {
        const Status result = state.destination(i.destination);
        if (result != Status::Ok) return result;
    }
    Value value[2]{};
    if (i.op == Op::Dual) {
        Status result = state.destination(i.otherDestination);
        if (result != Status::Ok) return result;
        // Read both halves before either write: an uninitialized source cannot
        // be made valid by the other destination of the same instruction.
        for (unsigned n = 0; n < 2; ++n) {
            const unsigned* src = n ? i.otherSources : i.sources;
            result = state.read(src[0], i.literal, value[n]);
            if (result != Status::Ok) return result;
            if (i.dualOps[n] == 9) {
                Value other{};
                result = state.read(src[1], i.literal, other);
                if (result != Status::Ok) return result;
                result = state.mask(106);
                if (result != Status::Ok) return result;
                value[n] = {}; // lane-varying select; initialized, not an exact constant
            }
        }
        state.write(i.destination, value[0]); state.write(i.otherDestination, value[1]);
        return Status::Ok;
    }
    for (unsigned n = 0; n < i.count; ++n) {
        const Status result = state.read(i.sources[n], i.literal, value[n]);
        if (result != Status::Ok) return result;
    }
    if (i.op == Op::Compare) {
        if (i.destination == 1) state.mask1 = true; else state.vcc = true;
        return Status::Ok;
    }
    if (i.op == Op::Select) {
        const Status result = state.mask(i.maskSource);
        if (result != Status::Ok) return result;
        state.write(i.destination, {});
    } else if (i.op == Op::Add) {
        Value sum{value[0].bits + value[1].bits, value[0].known && value[1].known};
        state.write(i.destination, sum);
    } else state.write(i.destination, value[0]);
    return Status::Ok;
}

Status fail(Report& out, Status status, Stage stage, size_t offset, uint32_t w = 0) {
    out = {}; out.status = status; out.stage = stage;
    out.byteOffset = uint32_t(offset); out.instructionWord = w; return status;
}
Status body(const uint8_t* code, size_t start, size_t end, size_t imageOffset,
            Stage stage, State& state, Report& out) {
    for (size_t at = start; at < end;) {
        Instruction instruction{};
        Status status = decode(code + at, end - at, state.vertex, instruction);
        if (status == Status::Ok) status = apply(instruction, state);
        if (status != Status::Ok) return fail(out, status, stage, imageOffset + at, word(code + at));
        ++state.instructions; at += instruction.words*4;
    }
    return Status::Ok;
}
Program program(size_t executable, const State& state, unsigned fixedInstructions, uint32_t rsrc2) {
    return {uint32_t(executable), state.instructions + fixedInstructions, state.required,
        kAllocatedVgprs, kHardwareSgprs, kWaveSize,
        0x002c0000u | ((kAllocatedVgprs - 1)/8), rsrc2};
}
Status vertex(const uint8_t* code, size_t executable, Program& result, Report& out) {
    // This control skeleton is the real ac_nir_lower_ngg_nogs/ACO wave32 ABI.
    // The branch skips *all* vertex work when EXEC is empty. The only join is
    // at expcnt(0), before restoring the independent primitive-lane mask.
    if (executable < 56) return fail(out, Status::InvalidControlFlow, Stage::Vertex, 0);
    constexpr uint32_t prefix[] = {0xbfb50003, 0x84008803, 0x95828003, 0x930000c1, 0xbefe0002};
    for (unsigned n = 0; n < 5; ++n)
        if (word(code + n*4) != prefix[n])
            return fail(out, Status::InvalidControlFlow, Stage::Vertex, n*4, word(code + n*4));
    const size_t pos = executable - 32, wait = pos + 8;
    const uint32_t guard = 0xbfa50000u | uint32_t((wait - 24)/4);
    if (word(code + 20) != guard)
        return fail(out, Status::InvalidControlFlow, Stage::Vertex, 20, word(code + 20));
    constexpr uint32_t suffix[] = {0xbf89fff0, 0xbefe0000, 0xbfa50002, 0xf8000941, 0x80808000, kEnd};
    for (unsigned n = 0; n < 6; ++n)
        if (word(code + wait + n*4) != suffix[n])
            return fail(out, Status::InvalidControlFlow, Stage::Vertex, wait + n*4, word(code + wait + n*4));
    if (word(code + pos) != 0xf80008cf)
        return fail(out, Status::InvalidExport, Stage::Vertex, pos, word(code + pos));
    State state(true);
    Status status = body(code, 24, pos, 0, Stage::Vertex, state, out);
    if (status != Status::Ok) return status;
    const uint32_t operands = word(code + pos + 4);
    for (unsigned n = 0; n < 4; ++n) {
        Value value{};
        status = state.read(256 + ((operands >> (8*n)) & 255), 0, value);
        if (status != Status::Ok) return fail(out, status, Stage::Vertex, pos + 4, operands);
    }
    // Prefix6, POS1, wait/restore/branch/PRIM/END5. Primitive v0 is read only
    // after switching masks and is never considered initialized in the body.
    result = program(executable, state, 12, 2); return Status::Ok;
}
Status vertexBuffer(const uint8_t* code, size_t executable, Program& result, Report& out) {
    if (executable != 104) return fail(out, Status::InvalidControlFlow, Stage::Vertex, 0);
    constexpr uint32_t prefix[] = {0xbfb50003,0x84008803,0x95828003,0x930000c1,0xbefe0002,0xbfa5000e};
    constexpr uint32_t suffix[] = {0xbf89fff0,0xbefe0000,0xbfa50002,0xf8000941,0x80808000,kEnd};
    for (unsigned n = 0; n < 6; ++n) {
        if (word(code+n*4) != prefix[n])
            return fail(out, Status::InvalidControlFlow, Stage::Vertex, n*4, word(code+n*4));
        if (word(code+80+n*4) != suffix[n])
            return fail(out, Status::InvalidControlFlow, Stage::Vertex, 80+n*4, word(code+80+n*4));
    }
    // GFX11 V_ADD_LSHL_U32: s12 + system v5, shifted by inline4. No
    // modifiers, scalar/EXEC writes or other pointer provenance are admitted.
    const uint32_t addressOp = word(code+24);
    const unsigned address = addressOp & 255;
    if ((addressOp & 0xffffff00u) != 0xd6470000u || word(code+28) != 0x02120a0c)
        return fail(out, Status::InvalidOperand, Stage::Vertex, 24, addressOp);
    if (!address || address >= kAllocatedVgprs)
        return fail(out, Status::RegisterLimit, Stage::Vertex, 24, addressOp);
    if (word(code+32) != 0xbf850003)
        return fail(out, Status::InvalidControlFlow, Stage::Vertex, 32, word(code+32));
    uint8_t component[kAllocatedVgprs]{};
    unsigned required = address < 6 ? 6 : address+1;
    uint32_t loaded = 0, pending = 0;
    for (unsigned n = 0; n < 4; ++n) {
        const size_t at = 36+n*8;
        const uint32_t op = word(code+at), operands = word(code+at+4);
        // BUFFER_LOAD_DWORD, OFFEN, s8..s11, inline-zero SOFFSET. All
        // remaining flags/reserved fields are rejected. Preserve the address
        // until the last load; its final destination may alias the address.
        if ((op & 0xfffffff0u) != 0xe0500000u || (op & 3u) ||
            (operands & 0xffff00ffu) != (0x80420000u | address))
            return fail(out, Status::InvalidOperand, Stage::Vertex, at, op);
        const unsigned source = (op & 15u)/4, dest = (operands >> 8) & 255;
        if (!dest || dest >= kAllocatedVgprs)
            return fail(out, Status::RegisterLimit, Stage::Vertex, at+4, operands);
        if ((loaded & (1u << source)) || (pending & (1u << dest)) || (dest == address && n != 3))
            return fail(out, Status::InvalidOperand, Stage::Vertex, at+4, operands);
        loaded |= 1u << source; pending |= 1u << dest; component[dest] = uint8_t(source+1);
        if (required <= dest) required = dest+1;
    }
    if (word(code+68) != 0xbf8903f7)
        return fail(out, Status::UninitializedRegister, Stage::Vertex, 68, word(code+68));
    if (word(code+72) != 0xf80008cf)
        return fail(out, Status::InvalidExport, Stage::Vertex, 72, word(code+72));
    const uint32_t exported = word(code+76);
    for (unsigned n = 0; n < 4; ++n) {
        const unsigned source = (exported >> (n*8)) & 255;
        if (source >= kAllocatedVgprs || !(pending & (1u << source)) || component[source] != n+1)
            return fail(out, Status::InvalidExport, Stage::Vertex, 76, exported);
    }
    result = {uint32_t(executable),19,required,kAllocatedVgprs,kHardwareSgprs,kWaveSize,
        0x002c0000u | ((kAllocatedVgprs-1)/8),5u << 1};
    return Status::Ok;
}
Status fragment(const uint8_t* code, size_t executable, size_t imageOffset,
                Program& result, uint32_t (&color)[4], Report& out) {
    if (executable < 12) return fail(out, Status::InvalidExport, Stage::Fragment, imageOffset);
    const size_t exp = executable - 12;
    if (word(code + exp) != 0xf800080f || word(code + executable - 4) != kEnd)
        return fail(out, Status::InvalidExport, Stage::Fragment, imageOffset + exp, word(code + exp));
    State state(false);
    Status status = body(code, 0, exp, imageOffset, Stage::Fragment, state, out);
    if (status != Status::Ok) return status;
    const uint32_t operands = word(code + exp + 4);
    for (unsigned n = 0; n < 4; ++n) {
        Value value{};
        status = state.read(256 + ((operands >> (8*n)) & 255), 0, value);
        if (status != Status::Ok || !value.known)
            return fail(out, status == Status::Ok ? Status::InvalidExport : status,
                Stage::Fragment, imageOffset + exp + 4, operands);
        color[n] = value.bits;
    }
    result = program(executable, state, 2, 0); return Status::Ok;
}
Status fragmentBuffer(const uint8_t* code, size_t executable, size_t imageOffset,
                      Program& result, Report& out) {
    // Real Apple AIR -> SPIR-V -> pinned ACO profile. Decode the resource,
    // offsets and data provenance; this is not an image/hash allowlist.
    // Clause4, four MUBUF DWORD loads, vmcnt(0), RGBA export, endpgm.
    if (executable != 52)
        return fail(out, Status::InvalidControlFlow, Stage::Fragment, imageOffset);
    if (word(code) != 0xbf850003)
        return fail(out, Status::InvalidControlFlow, Stage::Fragment, imageOffset, word(code));
    uint8_t component[kAllocatedVgprs]{};
    uint32_t pending = 0, loaded = 0;
    unsigned required = 1;
    for (unsigned n = 0; n < 4; ++n) {
        const size_t at = 4 + n*8;
        const uint32_t op = word(code + at), operands = word(code + at + 4);
        // GFX11 BUFFER_LOAD_DWORD, OFFEN/IDXEN/TFE/LDS/GLC/SLC/DLC=0.
        // SRSRC=0 selects s0..3; SOFFSET inline0; unused VADDR=128.
        // Reject every other encoding bit rather than masking a future mode.
        if ((op & 0xfffffff0u) != 0xe0500000u || (op & 3u) ||
            (operands & 0xffff00ffu) != 0x80000080u)
            return fail(out, Status::InvalidOperand, Stage::Fragment, imageOffset + at, op);
        const unsigned source = (op & 15u)/4, dest = (operands >> 8) & 255u;
        if (dest >= kAllocatedVgprs)
            return fail(out, Status::RegisterLimit, Stage::Fragment, imageOffset + at + 4, operands);
        if ((loaded & (1u << source)) || (pending & (1u << dest)))
            return fail(out, Status::InvalidOperand, Stage::Fragment, imageOffset + at, op);
        loaded |= 1u << source; pending |= 1u << dest;
        component[dest] = uint8_t(source + 1);
        if (required <= dest) required = dest + 1;
    }
    // Exact GFX11 wait: vmcnt0, other counters at their disabled maxima.
    // No export can consume pending memory data before this instruction.
    if (word(code + 36) != 0xbf8903f7)
        return fail(out, Status::UninitializedRegister, Stage::Fragment, imageOffset + 36, word(code + 36));
    const uint32_t ready = pending;
    if (word(code + 40) != 0xf800080f || word(code + 48) != kEnd)
        return fail(out, Status::InvalidExport, Stage::Fragment, imageOffset + 40, word(code + 40));
    const uint32_t operands = word(code + 44);
    for (unsigned n = 0; n < 4; ++n) {
        const unsigned source = (operands >> (n*8)) & 255u;
        if (source >= kAllocatedVgprs || !(ready & (1u << source)) || component[source] != n + 1)
            return fail(out, Status::InvalidExport, Stage::Fragment, imageOffset + 44, operands);
    }
    result = {uint32_t(executable), 8, required, kAllocatedVgprs,
        kHardwareSgprs, kWaveSize, 0x002c0000u | ((kAllocatedVgprs - 1)/8), 4u << 1};
    return Status::Ok;
}
} // namespace

Status layout(size_t vertexBytes, size_t fragmentBytes, Layout& output) {
    if (vertexBytes < 76 || fragmentBytes < 32 || vertexBytes > kMaxStageBytes ||
        fragmentBytes > kMaxStageBytes || vertexBytes % 4 || fragmentBytes % 4)
        return Status::InvalidLayout;
    const size_t offset = (vertexBytes + kCodeAlignment - 1) & ~(kCodeAlignment - 1);
    const size_t bytes = (offset + fragmentBytes + kImageAlignment - 1) & ~(kImageAlignment - 1);
    output = {vertexBytes, offset, fragmentBytes, bytes}; return Status::Ok;
}
static Status validateProfile(const Request& input, Report& out, bool buffered, bool vertexBuffered = false) {
    // Snapshot the small request before output publication. Refuse output/code
    // aliasing rather than invalidating the caller's immutable image ourselves.
    const Request request = input;
    if (request.image && request.imageBytes &&
        overlap(request.image, request.imageBytes, &out, sizeof(out))) return Status::InvalidArgument;
    if (!span(request.image, request.imageBytes) || request.imageBytes > 4096)
        return fail(out, Status::InvalidArgument, Stage::Image, 0);
    Layout image{};
    const Status shaped = layout(request.vertexBytes, request.fragmentBytes, image);
    if (shaped != Status::Ok || request.imageBytes != image.imageBytes)
        return fail(out, Status::InvalidLayout, Stage::Image, 0);
    for (size_t at = image.vertexBytes; at < image.fragmentOffset; ++at)
        if (request.image[at]) return fail(out, Status::InvalidPadding, Stage::Image, at);
    for (size_t at = image.fragmentOffset + image.fragmentBytes; at < image.imageBytes; ++at)
        if (request.image[at]) return fail(out, Status::InvalidPadding, Stage::Image, at);
    for (unsigned stage = 0; stage < 2; ++stage) {
        const size_t offset = stage ? image.fragmentOffset : 0;
        const size_t count = stage ? image.fragmentBytes : image.vertexBytes;
        for (size_t at = count - 20; at < count; at += 4)
            if (word(request.image + offset + at) != kPadding)
                return fail(out, Status::InvalidPadding, stage ? Stage::Fragment : Stage::Vertex,
                    offset + at, word(request.image + offset + at));
    }
    Report derived{};
    Status status = vertexBuffered ? vertexBuffer(request.image, image.vertexBytes - 20, derived.vertex, out) :
        vertex(request.image, image.vertexBytes - 20, derived.vertex, out);
    if (status != Status::Ok) return status;
    status = buffered ? fragmentBuffer(request.image + image.fragmentOffset, image.fragmentBytes - 20,
        image.fragmentOffset, derived.fragment, out) :
        fragment(request.image + image.fragmentOffset, image.fragmentBytes - 20,
        image.fragmentOffset, derived.fragment, derived.fragmentColorBits, out);
    if (status != Status::Ok) return status;
    derived.status = Status::Ok; derived.image = image;
    derived.vgtShaderStagesEn = (2u << 3) | (1u << 13) | (2u << 15) |
        (1u << 22) | (1u << 25) | (1u << 26);
    derived.spiShaderPosFormat = 4;
    derived.spiPsInputEna = derived.spiPsInputAddr = 128;
    derived.spiPsInControl = 1u << 15;
    derived.spiShaderColFormat = 9; derived.cbShaderMask = 15;
    out = derived; return Status::Ok;
}
Status validate(const Request& request, Report& out) { return validateProfile(request, out, false); }
Status validateFragmentBuffer(const Request& request, Report& out) { return validateProfile(request, out, true); }
Status validateVertexBuffer(const Request& request, Report& out) { return validateProfile(request, out, false, true); }
Status validateVertexFragmentBuffers(const Request& request, Report& out) { return validateProfile(request, out, true, true); }
const char* statusString(Status status) {
    switch (status) {
    case Status::Ok: return "ok";
    case Status::InvalidArgument: return "invalid-argument";
    case Status::InvalidLayout: return "invalid-layout";
    case Status::InvalidPadding: return "invalid-padding";
    case Status::InvalidControlFlow: return "invalid-control-flow";
    case Status::Truncated: return "truncated";
    case Status::UnsupportedInstruction: return "unsupported-instruction";
    case Status::ReservedBits: return "reserved-bits";
    case Status::InvalidOperand: return "invalid-operand";
    case Status::UninitializedRegister: return "uninitialized-register";
    case Status::RegisterLimit: return "register-limit";
    case Status::InvalidExport: return "invalid-export";
    }
    return "unknown";
}
} }
