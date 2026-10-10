// SPDX-License-Identifier: MIT
// Offline SDMA 6 packet construction. No ring, MMIO, submission, or execution.
#ifndef HACKYNAVI_SDMA_HPP
#define HACKYNAVI_SDMA_HPP

#include "hackynavi/rom.hpp"

namespace hackynavi {
namespace sdma6 {

enum : size_t {
    kCopyLinearWords = 7,
    kConstantFillWords = 5,
    kFenceWords = 4,
    kTrapWords = 2
};
constexpr uint32_t kMaxTransferBytes = 0x40000000U; // Linux SDMA 6 limit: 1 GiB.

struct CommandBuffer {
    uint32_t* words;
    size_t capacity; // Number of writable uint32_t elements, not bytes.
    size_t used;     // Existing word count; successful appends advance this.
};

// The caller owns an aligned words[capacity] allocation and keeps this metadata
// separate from that allocation. No allocations occur here. Any failure leaves
// every output word and used unchanged. Invalid arguments use InvalidArgument;
// insufficient remaining word capacity uses CapacityExceeded.
//
// Words are numeric packet dwords in native uint32_t storage. On the intended
// little-endian x86_64 target they already have SDMA's little-endian byte order.
// A big-endian host must serialize each word to little endian before use. This
// library does not validate mapped GPU addresses, VM ownership, residency, cache
// visibility, ring alignment, queue state, or device support.

// One unencrypted linear-copy packet, with no endian swap. Accepts 1..1 GiB
// bytes and byte-aligned addresses. Rejects overlapping ranges (not memmove).
// Range validation includes the last byte; wrapping beyond UINT64_MAX fails.
Status appendCopyLinear(CommandBuffer& buffer, uint64_t source,
                        uint64_t destination, uint32_t byteCount);

// Repeat the 32-bit value using Linux's default constant-fill packet. Destination
// and byteCount must both be multiples of four; count is 4..1 GiB bytes.
Status appendConstantFill(CommandBuffer& buffer, uint64_t destination,
                          uint32_t value, uint32_t byteCount);

// Addresses must be four-byte aligned. Optional trap has interrupt context zero,
// matching Linux's SDMA 6 fence emitter. This encodes a request only; no interrupt
// is installed or delivered by these functions.
Status appendFence32(CommandBuffer& buffer, uint64_t address,
                     uint32_t value, bool trap = false);

// Emits two 32-bit fence writes, low then high, optionally followed by one trap.
// This is not an atomic 64-bit store. Four-byte alignment is sufficient.
Status appendFence64(CommandBuffer& buffer, uint64_t address,
                     uint64_t value, bool trap = false);

} // namespace sdma6
} // namespace hackynavi

#endif // HACKYNAVI_SDMA_HPP
