// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_READ_CAPTURE_HPP
#define HACKYNAVI_READ_CAPTURE_HPP
#include <stddef.h>
#include <stdint.h>

namespace hackynavi { namespace readcapture {
// Historical callback records only. A returned word is not an MMIO observation
// unless callbackSucceeded is set. nonAllOnes adds only value != UINT32_MAX;
// neither mask establishes power, stable contents, idle state or ownership.
// Storage and callback state must be disjoint and stable throughout the call.
template<size_t Capacity> struct Journal {
    static_assert(Capacity && Capacity <= UINT32_MAX, "bounded read journal");
    static constexpr size_t capacity = Capacity;
    static constexpr size_t maskWords = (Capacity + 31) / 32;
    struct Entry { uint32_t byteOffset, returnedValue; } entries[Capacity];
    uint32_t callbackSucceeded[maskWords], nonAllOnes[maskWords];
    uint32_t attempted, recorded;
    bool truncated;
};

// Adds no I/O, retry, validity probe or allocation. The original callback and
// its result remain authoritative. Overflow loses diagnostic records only;
// it must never alter the underlying protocol's read order or return value.
template<size_t Capacity> class Reader {
public:
    using Read = bool (*)(void*, uint32_t, uint32_t&);
    Reader(void* context, Read callback, Journal<Capacity>& journal)
        : context_(context), callback_(callback), journal_(journal) {}
    static bool read(void* context, uint32_t offset, uint32_t& value) {
        auto& self = *static_cast<Reader*>(context);
        auto& out = self.journal_;
        if (out.attempted != UINT32_MAX) ++out.attempted;
        else out.truncated = true;
        const bool succeeded = self.callback_(self.context_, offset, value);
        const uint32_t returned = value;
        if (out.recorded < Capacity) {
            const uint32_t index = out.recorded++;
            out.entries[index] = {offset, returned};
            const uint32_t bit = UINT32_C(1) << (index % 32);
            if (succeeded) out.callbackSucceeded[index / 32] |= bit;
            if (succeeded && returned != UINT32_MAX) out.nonAllOnes[index / 32] |= bit;
        } else out.truncated = true;
        return succeeded;
    }
private:
    void* const context_;
    const Read callback_;
    Journal<Capacity>& journal_;
};
} }
#endif
