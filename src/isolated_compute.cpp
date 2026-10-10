// SPDX-License-Identifier: MIT
#include "hackynavi/isolated_compute.hpp"
namespace hackynavi { namespace gfxisolated {
namespace {
constexpr uint64_t kVaEnd = 1ULL << 47;
bool empty(const Binding& b) {
    return !b.owner && !b.epoch && !b.id && !b.generation && !b.gpu && !b.bytes &&
        !b.bufferOffset && !b.allocationBytes && !b.access;
}
Status binding(const Request& r, const Binding& b, uint32_t required, bool immutable) {
    if (b.owner != r.owner || b.epoch != r.epoch) return Status::ForeignContext;
    if (!b.id || !b.generation || !b.bytes || b.gpu < 4096 || b.gpu >= kVaEnd ||
        b.bytes > kVaEnd - b.gpu || ((b.gpu | b.bytes | b.bufferOffset | b.allocationBytes) & 4095) ||
        b.bufferOffset > b.allocationBytes || b.bytes > b.allocationBytes - b.bufferOffset)
        return Status::InvalidBinding;
    if ((b.access & required) != required || (b.access & ~(Read | Write | Execute)) ||
        (immutable && (b.access & Write))) return Status::Permission;
    return Status::Ok;
}
bool contains(const Binding& b, gfxcompute::Buffer range) {
    return range.bytes && range.gpu >= b.gpu && range.gpu - b.gpu < b.bytes &&
        range.bytes <= b.bytes - (range.gpu - b.gpu);
}
bool overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) {
    return a < b + bn && b < a + an;
}
bool sameBuffer(const Binding& a, const Binding& b) {
    return a.id == b.id; // differing generation for one retained slot is invalid too
}
}
Status validate(const Request& r, const uint32_t* captured, size_t words) {
    if (!r.owner || !r.epoch || r.vmid < 1 || r.vmid > 7 || !captured || !words ||
        words > gfxcompute::kMaxCommandDwords ||
        (reinterpret_cast<uintptr_t>(captured) % alignof(uint32_t)) ||
        words * sizeof(uint32_t) - 1 > UINTPTR_MAX - reinterpret_cast<uintptr_t>(captured) ||
        (r.dispatch.operation != gfxcompute::Operation::Clear && r.dispatch.operation != gfxcompute::Operation::Copy))
        return Status::InvalidArgument;
    Status status = binding(r, r.shader, Read | Execute, true);
    if (status != Status::Ok) return status;
    status = binding(r, r.indirect, Read | Execute, true);
    if (status != Status::Ok) return status;
    status = binding(r, r.destination, Read | Write, false);
    if (status != Status::Ok) return status;
    const bool copy = r.dispatch.operation == gfxcompute::Operation::Copy;
    if (copy) {
        status = binding(r, r.source, Read, false);
        if (status != Status::Ok) return status;
    } else if (!empty(r.source)) return Status::InvalidBinding;
    // Control storage must never share a BO with writable data, even through a
    // nonoverlapping VA alias. Native capture also rejects every writable alias.
    if (sameBuffer(r.shader, r.indirect) || sameBuffer(r.shader, r.destination) ||
        sameBuffer(r.indirect, r.destination) || (copy &&
        (sameBuffer(r.shader, r.source) || sameBuffer(r.indirect, r.source))))
        return Status::AliasedControlStorage;
    if (!contains(r.shader, r.dispatch.shader) ||
        r.dispatch.shader.bytes != gfxcompute::kShaderImageBytes ||
        !contains(r.destination, r.dispatch.destination) ||
        r.dispatch.destination.bytes != r.dispatch.bytes ||
        words * sizeof(uint32_t) > r.indirect.bytes ||
        (copy && (!contains(r.source, r.dispatch.source) || r.dispatch.source.bytes != r.dispatch.bytes)))
        return Status::InvalidBinding;
    const gfxcompute::Buffer active[] = {r.dispatch.shader,
        {r.indirect.gpu, r.indirect.bytes}, r.dispatch.destination, r.dispatch.source};
    for (size_t a = 0; a < (copy ? 4U : 3U); ++a)
        for (size_t b = a + 1; b < (copy ? 4U : 3U); ++b)
            if (overlap(active[a].gpu, active[a].bytes, active[b].gpu, active[b].bytes))
                return Status::Overlap;
    if (copy && sameBuffer(r.source, r.destination)) {
        if (r.source.generation != r.destination.generation ||
            r.source.allocationBytes != r.destination.allocationBytes) return Status::InvalidBinding;
        const uint64_t source = r.source.bufferOffset + (r.dispatch.source.gpu - r.source.gpu);
        const uint64_t destination = r.destination.bufferOffset + (r.dispatch.destination.gpu - r.destination.gpu);
        if (overlap(source, r.dispatch.bytes, destination, r.dispatch.bytes)) return Status::Overlap;
    }
    uint32_t canonical[gfxcompute::kMaxCommandDwords]; size_t canonicalWords = 0;
    if (gfxcompute::encode(r.dispatch, canonical, gfxcompute::kMaxCommandDwords, canonicalWords) != gfxcompute::Status::Ok)
        return Status::InvalidArgument;
    if (words != canonicalWords) return Status::InvalidStream;
    for (size_t n = 0; n < words; ++n) if (captured[n] != canonical[n]) return Status::InvalidStream;
    return Status::Ok;
}
const char* statusString(Status status) {
    switch (status) {
#define NAME(x) case Status::x: return #x
    NAME(Ok); NAME(InvalidArgument); NAME(ForeignContext); NAME(InvalidBinding);
    NAME(Permission); NAME(AliasedControlStorage); NAME(Overlap); NAME(InvalidStream);
#undef NAME
    }
    return "Unknown";
}
} }
