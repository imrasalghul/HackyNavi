// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_VRAM_TRANSFER_HPP
#define HACKYNAVI_VRAM_TRANSFER_HPP
#include "hackynavi/vram_allocator.hpp"

namespace hackynavi {
constexpr size_t kVramTransferMaxBytes = 2U * 1024U * 1024U;
enum class VramTransferStatus {
    Ok, InvalidArgument, InvalidAlignment, OutsideRegisterBar,
    InvalidAllocation, OutsideAllocation, TransportError, VerifyMismatch
};
enum class VramTransferStage { Preflight, Writing, Verifying, Reading, Complete };
struct VramTransferIo {
    void* context;
    uint64_t barBytes;
    // Ordered, uncached, native32 accesses to BAR BYTE offsets. The adapter
    // detects transport/device failures and returns false. A false write can
    // still have reached hardware. Legitimate MM_DATA value0xffffffff is data.
    bool (*read32)(void*, uint32_t byteOffset, uint32_t& value);
    bool (*write32)(void*, uint32_t byteOffset, uint32_t value);
};
struct VramTransferResult {
    VramTransferStatus status;
    VramTransferStage stage;
    VramAllocatorStatus allocationStatus; // Result of the preflight lookup.
    uint64_t vramOffset;    // Resolved start, relative to VRAM byte zero.
    uint64_t currentOffset; // Current/last DWORD, relative to VRAM byte zero.
    size_t bytesWritten;   // Successful data-write callbacks, not a rollback boundary.
    size_t bytesRead;      // Successful data reads, including a mismatching DWORD.
    size_t bytesVerified;  // Leading bytes that matched during the separate pass.
    uint32_t selectorWriteAttempts, dataWriteAttempts, dataReadAttempts;
    uint32_t lastRegister, expectedValue, observedValue;
    bool selectorWritesMayHaveOccurred, dataWritesMayHaveOccurred;
};

// Transfers only live allocations returned by this allocator. lookup resolves
// the current slot/serial/owner; caller-supplied allocation ranges are not used.
// Offset is relative to that allocation. Offset/bytes must be multiples of4;
// bytes is nonzero and <=2MiB. The full range must lie inside the allocation.
// CPU buffers may be byte-unaligned but must contain at least bytes accessible
// bytes. Encoding is explicitly little endian, independent of host byte order.
// All argument/range checks finish before any callback; no allocation occurs.
//
// The allocator, source/destination, Io and result must be non-overlapping and
// remain stable. The source is immutable until upload returns. The owner holds
// exclusive access to the allocator/allocation and the ENTIRE MM_INDEX/HI/DATA
// window for the full call, including both upload and verification passes. No
// callback may release/reconfigure an allocation or recursively use that window.
// Handles guard mistakes; they do not establish firmware/scanout reservations.
//
// Only an exclusively owned, powered, bare-metal Navi31 is supported. Caller
// keeps its PCI/BAR mapping valid and serializes reset/power/removal and every
// other MM-window user. Allocation users must be quiescent. A PCI open alone
// does not establish these conditions. This is a slow staging path, not SDMA.
//
// Every DWORD selects HI=offset>>31 then INDEX=low31|0x80000000 before DATA.
// MMIO byte offsets are0x18,0 and4. Selectors remain changed on success/failure;
// there is no verified restore protocol. Even readVram writes selectors.
// Failure can leave partial VRAM/output writes. Results report attempts and
// progress, never atomic rollback. Failed writes may have landed despite the
// reported successful-byte count. Keep affected allocations owned/quiescent.
//
// HDP flush, GPU cache/TLB invalidation and GPU completion are external. Owner
// performs required visibility operations after upload before publishing the
// allocation to GPU consumers. Ok proves only these callback reads/writes and
// (for upload) matching readback, not working DMA or initialized GPU engines.

// Writes the complete buffer, then independently reselects and verifies every
// DWORD against the still-immutable source. Full second pass detects aliases
// that immediate per-word write/read checks could miss. No input is modified.
VramTransferStatus uploadVram(const VramAllocator& allocator,
    VramAllocationHandle handle, uint64_t allocationOffset,
    const uint8_t* source, size_t bytes, const VramTransferIo& io,
    VramTransferResult& out);

// Reads an owned range into CPU bytes, with explicit little-endian serialization.
// bytesRead bounds the initialized prefix on transport failure. The rest of the
// destination is untouched. bytesWritten/bytesVerified remain zero.
VramTransferStatus readVram(const VramAllocator& allocator,
    VramAllocationHandle handle, uint64_t allocationOffset,
    uint8_t* destination, size_t bytes, const VramTransferIo& io,
    VramTransferResult& out);
const char* vramTransferStatusString(VramTransferStatus status);
} // namespace hackynavi
#endif
