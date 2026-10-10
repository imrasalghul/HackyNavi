// SPDX-License-Identifier: MIT
// GFX1100LeafBufferV1: instruction/dataflow policy, not compiler identity.
// Encoding and integer operand definitions: Mesa 35b085c4c067f198ce0b6437d94678e47b9c645f,
// aco_assembler.cpp, aco_opcodes.py, aco_ir.cpp and aco_form_hard_clauses.cpp.
// The reproducible source-derived rows are userspace/compiler/generated/
// gfx1100-leaf-decoder-rows.json. No Mesa implementation is linked here.
#include "hackynavi/gfx11_compute_product.hpp"
#include "hackynavi/gfx11_occupancy.hpp"

namespace hackynavi { namespace gfx11product { namespace {
constexpr unsigned kSgprs = 106, kVgprs = 256;
enum Kind : uint8_t { Unknown, Data, Constant, DescriptorRoot, PushRoot, Descriptor };
struct Scalar {
    uint32_t value; // exact constant, or the descriptor binding index
    Kind kind;
    // Descriptor/root tags use the final byte as their tuple component;
    // ordinary Data uses it as a proven trailing-zero count. This preserves
    // the bounded kernel stack layout without conflating data and provenance.
    uint8_t pending, first, componentOrZeros;
};
struct Vector { uint8_t state, zeros; }; // 0 unknown, 1 ready, 2 VMEM pending
struct Value { uint32_t value; unsigned zeros; bool constant; };
enum class Op : uint8_t {
    Mov, Mul, ScalarAdd, ScalarShift, ScalarXor, VectorMov, ReadFirst,
    Clause, Wait, Delay, End, ScalarLoad, ScalarLoad4,
    LshlAdd, AddLshl, Add3, VectorMul, Xad, LshlRev, Add, And, BufferLoad, BufferStore
};
struct Instruction {
    Op op;
    uint32_t word, second, literal;
    unsigned words, destination, source[3];
    bool hasLiteral;
};

uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
        (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
unsigned minimum(unsigned a, unsigned b) { return a < b ? a : b; }
unsigned zeros(uint32_t v) {
    if (!v) return 32;
    unsigned n = 0;
    while (!(v & 1)) { ++n; v >>= 1; }
    return n;
}
unsigned shifted(unsigned zeroBits, const Value& shift) {
    return minimum(32, zeroBits + (shift.constant ? (shift.value & 31) : 0));
}

// More-specific scalar/vector encodings are considered before broad SOP2/VOP2.
// Unlisted opcodes, including all special-register/control/branch operations,
// are rejected. Literal words are consumed only by reviewed source selectors.
Status decode(const uint8_t* code, size_t left, Instruction& i) {
    if (left < 4) return Status::Truncated;
    i = {}; i.word = le32(code); i.words = 1;
    const uint32_t w = i.word;
    unsigned operands = 0;
    if ((w & 0xff800000) == 0xbe800000) {
        if (((w >> 8) & 255) != 0) return Status::UnsupportedInstruction;
        i.op = Op::Mov; i.destination = (w >> 16) & 127;
        i.source[0] = w & 255; operands = 1;
    } else if ((w & 0xff800000) == 0xbf800000) {
        switch ((w >> 16) & 127) {
            case 5: i.op = Op::Clause; break;
            case 7: i.op = Op::Delay; break;
            case 9: i.op = Op::Wait; break;
            case 48: i.op = Op::End; break;
            default: return Status::UnsupportedInstruction;
        }
    } else if ((w & 0xf0000000) == 0xb0000000) {
        // SOPK/SOPC and every other scalar control encoding are outside V1.
        return Status::UnsupportedInstruction;
    } else if ((w & 0xc0000000) == 0x80000000) {
        switch ((w >> 23) & 127) {
            case 0: i.op = Op::ScalarAdd; break;
            case 8: i.op = Op::ScalarShift; break;
            case 26: i.op = Op::ScalarXor; break;
            case 44: i.op = Op::Mul; break;
            default: return Status::UnsupportedInstruction;
        }
        i.destination = (w >> 16) & 127;
        i.source[0] = w & 255; i.source[1] = (w >> 8) & 255; operands = 2;
    } else if ((w & 0xfc000000) == 0xf4000000) {
        const unsigned op = (w >> 18) & 255;
        if (op != 0 && op != 2) return Status::UnsupportedInstruction;
        // GFX11 DLC/GLC, reserved fields and signed/extended offsets excluded.
        if (w & 0x0003e000) return Status::ReservedBits;
        i.op = op ? Op::ScalarLoad4 : Op::ScalarLoad;
        i.destination = (w >> 6) & 127; i.words = 2;
    } else if ((w & 0xfc000000) == 0xe0000000) {
        const unsigned op = (w >> 18) & 255;
        if (op != 20 && op != 26) return Status::UnsupportedInstruction;
        // Initial raw-dword policy also keeps the immediate offset at zero.
        // No cache flags, LDS, addr64, typed memory, TFE, or index addressing.
        if (w & 0x0003ffff) return Status::ReservedBits;
        i.op = op == 20 ? Op::BufferLoad : Op::BufferStore; i.words = 2;
    } else if ((w & 0xfc000000) == 0xd4000000) {
        switch ((w >> 16) & 1023) {
            case 582: i.op = Op::LshlAdd; break;
            case 583: i.op = Op::AddLshl; break;
            case 597: i.op = Op::Add3; break;
            case 812: i.op = Op::VectorMul; break;
            case 581: i.op = Op::Xad; break;
            default: return Status::UnsupportedInstruction;
        }
        if (w & 0x0000ff00) return Status::ReservedBits;
        i.destination = w & 255; i.words = 2;
    } else if ((w & 0xfe000000) == 0x7e000000) {
        // Only full-width MOV and READFIRSTLANE, with no extended encoding.
        // READFIRSTLANE's destination field names an SGPR, not a VGPR.
        const unsigned op = (w >> 9) & 255;
        if (op != 1 && op != 2) return Status::UnsupportedInstruction;
        i.op = op == 1 ? Op::VectorMov : Op::ReadFirst;
        i.destination = (w >> 17) & 255;
        i.source[0] = w & 511; operands = 1;
        if (i.op == Op::ReadFirst && i.source[0] < 256) return Status::InvalidOperand;
    } else if (!(w & 0x80000000)) {
        // VOPC (container62) and every other VOP2 opcode remain excluded.
        switch ((w >> 25) & 63) {
            case 24: i.op = Op::LshlRev; break;
            case 27: i.op = Op::And; break;
            case 37: i.op = Op::Add; break;
            default: return Status::UnsupportedInstruction;
        }
        i.destination = (w >> 17) & 255;
        i.source[0] = w & 511; i.source[1] = 256 + ((w >> 9) & 255);
        operands = 1;
    } else {
        return Status::UnsupportedInstruction;
    }
    if (i.words == 2) {
        if (left < 8) return Status::Truncated;
        i.second = le32(code + 4);
        if (i.op == Op::ScalarLoad || i.op == Op::ScalarLoad4) {
            if (i.second & 0x01f00000) return Status::ReservedBits;
        } else if (i.op == Op::BufferLoad || i.op == Op::BufferStore) {
            if (i.second & 0x00a00000) return Status::ReservedBits; // no TFE/IDXEN
            // OFFEN=0 ignores VADDR. Accept the actual ACO canonical unused
            // field (128), not a second unreviewed address recipe.
            if (!(i.second & 0x00400000) && (i.second & 255) != 128)
                return Status::ReservedBits;
        } else {
            if (i.second & 0xf8000000) return Status::ReservedBits;
            for (unsigned n = 0; n < 3; ++n) i.source[n] = (i.second >> (9*n)) & 511;
            operands = i.op == Op::VectorMul ? 2 : 3;
            if (operands == 2 && i.source[2] != 128) return Status::ReservedBits;
        }
    }
    for (unsigned n = 0; n < operands; ++n) i.hasLiteral |= i.source[n] == 255;
    if (i.op != Op::Mov && i.op != Op::Mul && i.op != Op::ScalarAdd &&
        i.op != Op::ScalarShift && i.op != Op::ScalarXor) {
        // gfx11 VALU permits at most two distinct SGPR/literal bus reads.
        // Inline constants do not consume the bus. Source: aco_validate.cpp
        // constant_bus_limit and physical-register operand deduplication.
        unsigned busReads = i.hasLiteral ? 1 : 0;
        for (unsigned n = 0; n < operands; ++n) if (i.source[n] < kSgprs) {
            bool duplicate = false;
            for (unsigned p = 0; p < n; ++p) duplicate |= i.source[n] == i.source[p];
            if (!duplicate) ++busReads;
        }
        if (busReads > 2) return Status::InvalidOperand;
    }
    if (i.hasLiteral) {
        if (left < (i.words + 1)*4) return Status::Truncated;
        i.literal = le32(code + i.words*4); ++i.words;
    }
    return Status::Ok;
}

struct State {
    Scalar scalar[kSgprs];
    Vector vector[kVgprs];
    uint32_t clauseReads[4], clauseWrites[4];
    unsigned requiredSgprs, requiredVgprs, scalarLoads, vectorLoads, stores;
    unsigned clauseRemaining, instructions;
    uint32_t reads, writes;
    const Request& request;

    explicit State(const Request& r) : scalar{}, vector{}, clauseReads{}, clauseWrites{},
        requiredSgprs(8), requiredVgprs(1), scalarLoads(0), vectorLoads(0), stores(0),
        clauseRemaining(0), instructions(0), reads(0), writes(0), request(r) {
        for (unsigned n = 0; n < 8; ++n) scalar[n].kind = Data;
        for (unsigned n = 0; n < 4; ++n) {
            scalar[n].kind = n < 2 ? DescriptorRoot : PushRoot;
            scalar[n].first = uint8_t(n & ~1u); scalar[n].componentOrZeros = uint8_t(n & 1);
        }
        vector[0].state = 1;
        // Only CP's current active lanes are initialized in v0. Every admitted
        // VALU/VMEM instruction preserves EXEC and accesses those same lanes.
        // SCC/VCC/EXEC/m0/TTMP source and destination selectors are not admitted.
    }
    Status sgpr(unsigned n) {
        if (n >= kSgprs) return Status::InvalidOperand;
        if (n >= request.numSgprs) return Status::RegisterLimit;
        if (requiredSgprs <= n) requiredSgprs = n + 1;
        return Status::Ok;
    }
    Status vgpr(unsigned n) {
        if (n >= kVgprs) return Status::InvalidOperand;
        if (n >= request.numVgprs) return Status::RegisterLimit;
        if (requiredVgprs <= n) requiredVgprs = n + 1;
        return Status::Ok;
    }
    Status readScalar(unsigned n, Value& out) {
        Status s = sgpr(n); if (s != Status::Ok) return s;
        if (scalar[n].pending) return Status::PendingLoad;
        if (scalar[n].kind == Unknown) return Status::UninitializedRegister;
        out = {scalar[n].value, 0, scalar[n].kind == Constant};
        if (out.constant) out.zeros = zeros(out.value);
        else if (scalar[n].kind == Data) out.zeros = scalar[n].componentOrZeros;
        return Status::Ok;
    }
    Status readVector(unsigned n, Value& out) {
        Status s = vgpr(n); if (s != Status::Ok) return s;
        if (vector[n].state == 2) return Status::PendingLoad;
        if (!vector[n].state) return Status::UninitializedRegister;
        out = {0, vector[n].zeros, false}; return Status::Ok;
    }
    Status operand(unsigned n, const Instruction& i, bool allowVector, Value& out) {
        if (n < kSgprs) return readScalar(n, out);
        if (n >= 128 && n <= 208) {
            const uint32_t value = n <= 192 ? n - 128 : 192u - n;
            out = {value, zeros(value), true}; return Status::Ok;
        }
        if (n == 255 && i.hasLiteral) {
            out = {i.literal, zeros(i.literal), true}; return Status::Ok;
        }
        if (allowVector && n >= 256 && n <= 511) return readVector(n - 256, out);
        return Status::InvalidOperand; // includes GFX11 null/m0, SCC, VCC, DPP/SDWA
    }
    Status canWriteScalar(unsigned first, unsigned count) {
        for (unsigned n = first; n < first + count; ++n) {
            Status s = sgpr(n); if (s != Status::Ok) return s;
            if (scalar[n].pending) return Status::PendingLoad;
        }
        return Status::Ok;
    }
    void breakTag(unsigned n) {
        const Kind kind = scalar[n].kind;
        if (kind != Descriptor && kind != DescriptorRoot && kind != PushRoot) return;
        const unsigned first = scalar[n].first, count = kind == Descriptor ? 4 : 2;
        // No instruction copies provenance. Destroy the whole original tuple
        // on any component write, even a move of its own previous value.
        for (unsigned k = first; k < first + count; ++k) {
            scalar[k].kind = Data; scalar[k].componentOrZeros = 0;
        }
    }
    void writeScalar(unsigned n, const Value& value) {
        breakTag(n);
        scalar[n] = {value.value, value.constant ? Constant : Data, 0, 0,
            uint8_t(value.constant ? 0 : value.zeros)};
    }
    Status writeVector(unsigned n, unsigned zeroBits, bool pending = false) {
        Status s = vgpr(n); if (s != Status::Ok) return s;
        if (vector[n].state == 2) return Status::PendingLoad;
        vector[n] = {uint8_t(pending ? 2 : 1), uint8_t(zeroBits)}; return Status::Ok;
    }
    Status scalarLoad(const Instruction& i) {
        const unsigned base = (i.word & 63)*2, offsetReg = i.second >> 25;
        const unsigned count = i.op == Op::ScalarLoad4 ? 4 : 1;
        Value unused{};
        Status s = readScalar(base, unused); if (s != Status::Ok) return s;
        s = readScalar(base + 1, unused); if (s != Status::Ok) return s;
        const Kind root = scalar[base].kind;
        if ((root != DescriptorRoot && root != PushRoot) || scalar[base+1].kind != root ||
            scalar[base].first != base || scalar[base+1].first != base ||
            scalar[base].componentOrZeros != 0 || scalar[base+1].componentOrZeros != 1)
            return Status::InvalidProvenance;
        uint64_t offset = i.second & 0xfffff;
        if (offsetReg != 124) { // GFX11 SGPR_NULL is 124, not GFX10's 125.
            Value value{}; s = readScalar(offsetReg, value); if (s != Status::Ok) return s;
            if (!value.constant) return Status::InvalidProvenance;
            offset += value.value;
        }
        const uint32_t extent = root == DescriptorRoot ? kMaxBindings*kDescriptorBytes : kPushBytes;
        if ((offset & 3) || offset > extent || count*4 > extent-offset) return Status::OutOfBounds;
        if (count == 4 && (i.destination & 3)) return Status::InvalidOperand;
        if (root == DescriptorRoot && (count != 4 || (offset & 15))) return Status::InvalidProvenance;
        s = canWriteScalar(i.destination, count); if (s != Status::Ok) return s;
        // A hard clause can replay: reject both RAW and WAR dependencies inside
        // it, and destination overlap with this instruction's own base/offset.
        uint32_t instructionReads[4] = {};
        instructionReads[base/32] |= 1u << (base%32);
        instructionReads[(base+1)/32] |= 1u << ((base+1)%32);
        if (offsetReg != 124) instructionReads[offsetReg/32] |= 1u << (offsetReg%32);
        for (unsigned n = i.destination; n < i.destination + count; ++n) {
            const uint32_t bit = 1u << (n%32);
            if (instructionReads[n/32] & bit) return Status::InvalidProvenance;
            if (clauseRemaining && (clauseReads[n/32] & bit)) return Status::InvalidClause;
        }
        if (clauseRemaining) {
            for (unsigned n = 0; n < 4; ++n) {
                if (instructionReads[n] & clauseWrites[n]) return Status::InvalidClause;
                clauseReads[n] |= instructionReads[n];
            }
            for (unsigned n = i.destination; n < i.destination+count; ++n)
                clauseWrites[n/32] |= 1u << (n%32);
        }
        if (scalarLoads >= 63) return Status::InvalidWait;
        ++scalarLoads;
        for (unsigned n = i.destination; n < i.destination + count; ++n) breakTag(n);
        for (unsigned n = 0; n < count; ++n) {
            Scalar& reg = scalar[i.destination+n];
            reg = {uint32_t(offset/16), root == DescriptorRoot ? Descriptor : Data,
                1, uint8_t(i.destination), uint8_t(root == DescriptorRoot ? n : 0)};
        }
        if (clauseRemaining) --clauseRemaining;
        return Status::Ok;
    }
    Status buffer(const Instruction& i) {
        const unsigned descriptor = ((i.second >> 16) & 31)*4;
        Value unused{};
        for (unsigned n = 0; n < 4; ++n) {
            Status s = readScalar(descriptor+n, unused); if (s != Status::Ok) return s;
            const Scalar& reg = scalar[descriptor+n];
            if (reg.kind != Descriptor || reg.first != descriptor || reg.componentOrZeros != n ||
                reg.value >= kMaxBindings || reg.value != scalar[descriptor].value)
                return Status::InvalidProvenance;
        }
        Value offset{};
        Status s = operand(i.second >> 24, i, false, offset); if (s != Status::Ok) return s;
        if (i.second & 0x00400000) {
            // Original vector-offset recipe retains a zero scalar offset.
            if (!offset.constant || offset.value) return Status::InvalidProvenance;
            Value address{}; s = readVector(i.second & 255, address); if (s != Status::Ok) return s;
            if (address.zeros < 2) return Status::InvalidProvenance;
        } else {
            // Uniform ACO accesses use OFFEN=0 and an aligned scalar byte
            // offset. Native raw descriptors have stride0, OOB_SELECT=3 and
            // dword-aligned base/length: the hardware's exact range check
            // includes SOFFSET and payload (AMD RDNA3 ISA9.4.1/Table46).
            // Neither descriptor provenance nor its native bounds can be
            // changed by these admitted ALU instructions.
            if (offset.zeros < 2) return Status::InvalidProvenance;
        }
        const unsigned data = (i.second >> 8) & 255;
        const uint32_t mask = uint32_t(1) << scalar[descriptor].value;
        if (i.op == Op::BufferLoad) {
            if (vectorLoads >= 63) return Status::InvalidWait;
            s = writeVector(data, 0, true); if (s != Status::Ok) return s;
            ++vectorLoads; reads |= mask;
        } else {
            s = readVector(data, unused); if (s != Status::Ok) return s;
            if (stores >= 63) return Status::InvalidWait;
            ++stores; writes |= mask;
            // On GFX11 stores do not retain source VGPR locks (Mesa
            // aco_insert_waitcnt.cpp:893-905). They have a distinct VS counter;
            // s_waitcnt below never pretends to retire a store. Native command
            // completion still requires its CS flush/fence/visibility sequence.
        }
        return Status::Ok;
    }
    Status execute(const Instruction& i) {
        if (clauseRemaining && i.op != Op::ScalarLoad && i.op != Op::ScalarLoad4)
            return Status::InvalidClause;
        const uint32_t imm = i.word & 65535;
        switch (i.op) {
        case Op::Clause:
            // Pinned Mesa caps GFX11 clauses at 32 due to a hardware erratum.
            // Only SMEM hard clauses are in V1; VMEM clauses remain rejected.
            if (imm > 31) return Status::InvalidClause;
            clauseRemaining = imm + 1;
            for (unsigned n = 0; n < 4; ++n) clauseReads[n] = clauseWrites[n] = 0;
            return Status::Ok;
        case Op::Wait:
            if (imm & 8) return Status::InvalidWait;
            // GFX11: VM[15:10], LGKM[9:4], EXP[2:0]. A nonzero partial
            // wait is legal but proves no register ready in this conservative
            // verifier. In particular LGKM SMEM responses can be unordered.
            if (((imm >> 4) & 63) == 0) {
                for (auto& reg : scalar) reg.pending = 0;
                scalarLoads = 0;
            }
            if (((imm >> 10) & 63) == 0) {
                for (auto& reg : vector) if (reg.state == 2) reg.state = 1;
                vectorLoads = 0;
            }
            return Status::Ok;
        case Op::Delay: {
            // INSTID0[3:0], INSTSKIP[6:4], INSTID1[10:7]. Reviewed
            // aco_builder_h.py values are NO_DEP0, VALU1..4, TRANS5..7,
            // SALU9..11; FMA_ACCUM8 is outside this integer-only subset,
            // and12..15 are unlisted. Delays do not finish loads.
            const unsigned a = imm & 15, b = (imm >> 7) & 15, skip = (imm >> 4) & 7;
            if ((imm & 0xf800) || a == 8 || b == 8 || a > 11 || b > 11 || skip > 5 ||
                (!b && skip)) return Status::ReservedBits;
            return Status::Ok;
        }
        case Op::End:
            if (imm) return Status::ReservedBits;
            if (scalarLoads || vectorLoads) return Status::PendingLoad;
            return Status::Ok;
        case Op::ScalarLoad: case Op::ScalarLoad4: return scalarLoad(i);
        case Op::BufferLoad: case Op::BufferStore: return buffer(i);
        case Op::ReadFirst: {
            Value value{};
            Status s = readVector(i.source[0] - 256, value); if (s != Status::Ok) return s;
            s = canWriteScalar(i.destination, 1); if (s != Status::Ok) return s;
            // The direct nonempty dispatch starts every launched wave with
            // at least one active lane; no admitted instruction changes EXEC.
            // Every active lane of this VGPR was initialized and waited, so
            // its first active lane is initialized too. Data only: never
            // manufacture descriptor/root provenance from a lane value.
            writeScalar(i.destination, value); return Status::Ok;
        }
        default: break;
        }
        Value a{}, b{}, c{};
        const bool scalarAlu = i.op == Op::Mov || i.op == Op::Mul ||
            i.op == Op::ScalarAdd || i.op == Op::ScalarShift || i.op == Op::ScalarXor;
        Status s = operand(i.source[0], i, !scalarAlu, a); if (s != Status::Ok) return s;
        if (i.op != Op::Mov && i.op != Op::VectorMov) {
            s = operand(i.source[1], i, !scalarAlu, b); if (s != Status::Ok) return s;
        }
        if (scalarAlu) {
            s = canWriteScalar(i.destination, 1); if (s != Status::Ok) return s;
            if (i.op == Op::Mul) {
                a.constant = a.constant && b.constant;
                a.value *= b.value;
                a.zeros = minimum(32, a.zeros + b.zeros);
            } else if (i.op == Op::ScalarAdd) {
                a.constant = a.constant && b.constant;
                a.value += b.value;
                a.zeros = minimum(a.zeros, b.zeros);
            } else if (i.op == Op::ScalarShift) {
                a.zeros = shifted(a.zeros, b);
                a.constant = a.constant && b.constant;
                a.value <<= b.value & 31;
            } else if (i.op == Op::ScalarXor) {
                a.constant = a.constant && b.constant;
                a.value ^= b.value;
                a.zeros = minimum(a.zeros, b.zeros);
            }
            // ADD/LSHL/XOR implicitly update SCC. No admitted instruction reads
            // SCC or uses it for control flow, so it conveys no data authority.
            writeScalar(i.destination, a); return Status::Ok;
        }
        unsigned resultZeros = 0;
        if (i.op == Op::VectorMov) resultZeros = a.zeros;
        else if (i.op == Op::LshlRev) resultZeros = shifted(b.zeros, a);
        else if (i.op == Op::Add) resultZeros = minimum(a.zeros, b.zeros);
        else if (i.op == Op::And) resultZeros = a.zeros > b.zeros ? a.zeros : b.zeros;
        else if (i.op == Op::VectorMul) resultZeros = minimum(32, a.zeros + b.zeros);
        else {
            s = operand(i.source[2], i, true, c); if (s != Status::Ok) return s;
            switch (i.op) {
                case Op::LshlAdd: resultZeros = minimum(shifted(a.zeros, b), c.zeros); break;
                case Op::AddLshl: resultZeros = shifted(minimum(a.zeros, b.zeros), c); break;
                case Op::Add3: case Op::Xad: resultZeros = minimum(minimum(a.zeros, b.zeros), c.zeros); break;
                default: return Status::UnsupportedInstruction;
            }
        }
        return writeVector(i.destination, resultZeros);
    }
};
static_assert(sizeof(State) < 2048, "validator state exceeds kernel stack budget");

Status fail(Report& report, Status status, size_t offset = 0, uint32_t word = 0) {
    report = {}; report.status = status;
    report.byteOffset = uint32_t(offset); report.instructionWord = word;
    return status;
}
} // namespace

Status validate(const Request& request, Report& report) {
    if (!request.code || !request.codeBytes || request.codeBytes > kMaxCodeBytes ||
        (request.codeBytes & 3) || !request.executableBytes || (request.executableBytes & 3) ||
        request.executableBytes > request.codeBytes ||
        request.codeBytes - 1 > UINTPTR_MAX - reinterpret_cast<uintptr_t>(request.code))
        return fail(report, Status::InvalidArgument);
    uint64_t threads = 1;
    for (unsigned n = 0; n < 3; ++n) {
        if (!request.local[n] || request.local[n] > kMaxLocalThreads)
            return fail(report, Status::InvalidLaunch);
        threads *= request.local[n];
    }
    if (threads > kMaxLocalThreads || request.waveSize != kWaveSize ||
        request.numSgprs < 8 || request.numSgprs > 128 || !request.numVgprs || request.numVgprs > kVgprs)
        return fail(report, Status::InvalidLaunch);
    // The RSRC profile below fixes Navi31 wave32 WGP mode with no scratch,
    // LDS or shared VGPRs. Declared VGPR allocation must let the whole group
    // reside in that WGP; the architectural 1024-thread bound alone is not
    // sufficient for high-register products. Validate before publishing RSRCs.
    if (!gfx11occupancy::fits(request.numVgprs, uint32_t(threads)))
        return fail(report, Status::InvalidLaunch);
    State state(request);
    bool ended = false;
    for (size_t offset = 0; offset < request.executableBytes;) {
        Instruction i{};
        Status status = decode(request.code + offset, request.executableBytes - offset, i);
        if (status != Status::Ok) return fail(report, status, offset, i.word);
        status = state.execute(i);
        if (status != Status::Ok) return fail(report, status, offset, i.word);
        ++state.instructions;
        offset += i.words*4;
        if (i.op == Op::End) {
            if (offset != request.executableBytes) return fail(report, Status::MissingEnd, offset-4, i.word);
            ended = true;
        }
    }
    if (!ended || state.clauseRemaining) return fail(report, Status::MissingEnd, request.executableBytes);
    for (size_t offset = request.executableBytes; offset < request.codeBytes; offset += 4) {
        const uint32_t word = le32(request.code + offset);
        if (word != 0xbf9f0000) return fail(report, Status::InvalidPadding, offset, word);
    }
    report = {};
    report.status = Status::Ok; report.instructionCount = state.instructions;
    report.requiredSgprs = state.requiredSgprs; report.requiredVgprs = state.requiredVgprs;
    report.readMask = state.reads; report.writeMask = state.writes;
    // Fixed Navi31 WGP/no-scratch/no-LDS/nonprivileged wave32 ABI, integer
    // instructions; the matching register-residency profile was checked above.
    // GFX11 allocates scalar registers independently of RSRC1.SGPRS; there is
    // no caller-controlled PRIV, trap, shared-VGPR or memory-ordering bit here.
    report.rsrc1 = 0x202c0000 | ((request.numVgprs - 1)/8);
    report.rsrc2 = 0x788 | ((request.local[2] > 1 ? 2u : request.local[1] > 1 ? 1u : 0u) << 11);
    report.rsrc3 = 0;
    return Status::Ok;
}
const char* statusString(Status status) {
    switch (status) {
#define NAME(x) case Status::x: return #x
        NAME(Ok); NAME(InvalidArgument); NAME(InvalidLaunch); NAME(Truncated);
        NAME(UnsupportedInstruction); NAME(ReservedBits); NAME(InvalidOperand);
        NAME(UninitializedRegister); NAME(PendingLoad); NAME(RegisterLimit);
        NAME(InvalidProvenance); NAME(OutOfBounds); NAME(InvalidClause);
        NAME(InvalidWait); NAME(MissingEnd); NAME(InvalidPadding);
#undef NAME
    }
    return "Unknown";
}
} }
