// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DEVICE_DISCOVERY_HPP
#define HACKYNAVI_DEVICE_DISCOVERY_HPP
#include <stddef.h>
#include <stdint.h>

namespace hackynavi {
enum class DeviceDiscoveryStatus {
    Ok, InvalidArgument, ReadyTimeout, TransportError, DeviceUnavailable,
    SystemMemoryUnsupported, BadSignature, MalformedHeader, InvalidVramRange,
    BufferTooSmall
};
enum : uint32_t {
    kDeviceDiscoveryMaxBytes = 65536,
    kDeviceDiscoveryDefaultRegionBytes = 10240,
    kDeviceDiscoveryMaxReadyPolls = 2000,
    kDeviceDiscoveryMaxPollDelayUs = 1000
};
struct DeviceDiscoveryIo {
    void* context;
    // Aligned BAR MMIO BYTE offsets, not dword register indices. Callbacks must
    // implement ordered device MMIO and return false on transport failure.
    bool (*read32)(void* context, uint32_t byteOffset, uint32_t& value);
    bool (*write32)(void* context, uint32_t byteOffset, uint32_t value);
    void (*delayUs)(void* context, uint32_t microseconds);
};
struct DeviceDiscoveryOptions {
    // 1..2000 polls, with 1..1000 us between unsuccessful polls. {2000,1000}
    // gives at most 1999 requested delays (callback duration is caller-owned).
    uint32_t readyPollLimit, readyPollDelayUs;
};
struct DeviceDiscoveryResult {
    uint64_t vramBytes, sourceOffset; // Zero-based VRAM byte offset, not CPU PA.
    uint32_t regionSize, readyPolls;
    size_t binarySize, bytesCopied;
    bool usedScratch, selectorWritesAttempted;
};

// Active acquisition for an exclusively owned, powered bare-metal Navi31 dGPU.
// The caller must serialize the ENTIRE call against every MM_INDEX/HI/DATA user
// and device reset/removal, and supply a writable non-aliasing caller buffer.
// The serialization mechanism must permit delayUs to sleep when it needs to.
// Not a passive/read-only operation: this writes MM_INDEX_HI and MM_INDEX. It
// never writes MM_DATA, firmware/mailbox payloads, display or reset registers.
// Selectors remain changed: Linux/Cobalt64 do not establish a readback/restore
// protocol. A failed write can still have reached hardware; the result records
// attempts, including failures. The caller must release its lock on all returns.
//
// Polls firmware-ready, obtains VRAM size, tries the 10 KiB default region at
// VRAM-end minus 64 KiB, then the
// Linux driver-scratch location only if the default signature is absent. Rejects
// scratch ranges outside VRAM. System-memory TMR and SR-IOV are unsupported.
// Reads the declared binary size (maximum 65535 encoded bytes), not the entire
// TMR. Final dword reads stay inside the validated region; only remaining bytes
// are copied. The CPU buffer need not be aligned. No allocation occurs.
//
// Ok means a complete, signature/length-checked capture, NOT a validated IP
// discovery binary. Call parseDiscovery(buffer,out.binarySize,...) separately.
// On error, buffer may contain an incomplete capture and MUST NOT be parsed;
// bytesCopied is diagnostic only. out is initialized before any callback.
DeviceDiscoveryStatus acquireDeviceDiscovery(const DeviceDiscoveryIo& io,
    const DeviceDiscoveryOptions& options, uint8_t* buffer, size_t capacity,
    DeviceDiscoveryResult& out);
const char* deviceDiscoveryStatusString(DeviceDiscoveryStatus status);
} // namespace hackynavi
#endif
