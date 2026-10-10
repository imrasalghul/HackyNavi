// SPDX-License-Identifier: MIT
// MM_INDEX encoding/order adapted from Cobalt64 Navi48Bringup.cpp:160-165:
// https://github.com/Almosst-DEV/Cobalt64/blob/d6c9aedf8e6526a39e349b5886cbc8c7ddf77f65/src/navi48-bringup/src/Navi48Bringup.cpp
// Copyright (c) 2026 Navi48-MacOS contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Independently authored bounded acquisition around that transport. Register,
// ready-bit and location protocol checked against Linux commit
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c:140-150,288-375
// drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.h:29-30 (10 KiB size, 64 KiB offset)
// drivers/gpu/drm/amd/amdgpu/amdgpu_device.c:717-751.
// Linux holds mmio_idx_lock and does not restore selectors. Its generic path
// also supports system memory/SR-IOV; neither is implemented here. Unlike Linux,
// a firmware-ready timeout here fails explicitly instead of continuing.
#include "hackynavi/device_discovery.hpp"

namespace hackynavi {
namespace {
enum : uint32_t {
    kIndex = 0x0 * 4, kIndexHi = 0x6 * 4, kData = 0x1 * 4,
    kMemSize = 0xde3 * 4, kReady = 0x16061 * 4,
    kScratchLo = 0x94 * 4, kScratchHi = 0x95 * 4,
    kScratchSize = 0x96 * 4, kSignature = 0x28211407
};

bool vramWord(const DeviceDiscoveryIo& io, uint64_t offset,
              DeviceDiscoveryResult& out, uint32_t& value) {
    out.selectorWritesAttempted = true;
    if (!io.write32(io.context, kIndexHi, static_cast<uint32_t>(offset >> 31)))
        return false;
    if (!io.write32(io.context, kIndex,
                   (static_cast<uint32_t>(offset) & 0x7ffffffcu) | 0x80000000u))
        return false;
    return io.read32(io.context, kData, value);
}

bool validRegion(uint64_t offset, uint32_t size, uint64_t vramBytes) {
    return (offset & 3u) == 0 && (size & 3u) == 0 && size >= 12 &&
           offset <= vramBytes && size <= vramBytes - offset;
}

void copyWord(uint8_t* buffer, size_t offset, uint32_t word, size_t count) {
    for (size_t i = 0; i < count; ++i)
        buffer[offset + i] = static_cast<uint8_t>(word >> (8 * i));
}
} // namespace

DeviceDiscoveryStatus acquireDeviceDiscovery(const DeviceDiscoveryIo& io,
    const DeviceDiscoveryOptions& options, uint8_t* buffer, size_t capacity,
    DeviceDiscoveryResult& out) {
    out = DeviceDiscoveryResult{};
    if (!io.read32 || !io.write32 || !io.delayUs || !buffer ||
        options.readyPollLimit == 0 ||
        options.readyPollLimit > kDeviceDiscoveryMaxReadyPolls ||
        options.readyPollDelayUs == 0 ||
        options.readyPollDelayUs > kDeviceDiscoveryMaxPollDelayUs)
        return DeviceDiscoveryStatus::InvalidArgument;
    if (capacity < 12) return DeviceDiscoveryStatus::BufferTooSmall;

    uint32_t value = 0;
    bool ready = false;
    for (uint32_t i = 0; i < options.readyPollLimit; ++i) {
        ++out.readyPolls;
        if (!io.read32(io.context, kReady, value))
            return DeviceDiscoveryStatus::TransportError;
        if (value == UINT32_MAX) return DeviceDiscoveryStatus::DeviceUnavailable;
        if (value & 0x80000000u) { ready = true; break; }
        if (i + 1 < options.readyPollLimit)
            io.delayUs(io.context, options.readyPollDelayUs);
    }
    if (!ready) return DeviceDiscoveryStatus::ReadyTimeout;
    if (!io.read32(io.context, kMemSize, value))
        return DeviceDiscoveryStatus::TransportError;
    if (value == UINT32_MAX) return DeviceDiscoveryStatus::DeviceUnavailable;
    if (value == 0) return DeviceDiscoveryStatus::SystemMemoryUnsupported;
    out.vramBytes = static_cast<uint64_t>(value) << 20;
    out.regionSize = kDeviceDiscoveryDefaultRegionBytes;
    out.sourceOffset = out.vramBytes - kDeviceDiscoveryMaxBytes;

    uint32_t header[3] = {};
    if (!vramWord(io, out.sourceOffset, out, header[0]))
        return DeviceDiscoveryStatus::TransportError;
    if (header[0] != kSignature) {
        uint32_t size = 0, lo = 0, hi = 0;
        if (!io.read32(io.context, kScratchSize, size))
            return DeviceDiscoveryStatus::TransportError;
        if (!size) return DeviceDiscoveryStatus::BadSignature;
        if (!io.read32(io.context, kScratchLo, lo) ||
            !io.read32(io.context, kScratchHi, hi))
            return DeviceDiscoveryStatus::TransportError;
        out.usedScratch = true;
        out.sourceOffset = (static_cast<uint64_t>(hi) << 32) | lo;
        out.regionSize = size;
        if (!validRegion(out.sourceOffset, out.regionSize, out.vramBytes))
            return DeviceDiscoveryStatus::InvalidVramRange;
        if (!vramWord(io, out.sourceOffset, out, header[0]))
            return DeviceDiscoveryStatus::TransportError;
        if (header[0] != kSignature) return DeviceDiscoveryStatus::BadSignature;
    }
    if (!vramWord(io, out.sourceOffset + 4, out, header[1]) ||
        !vramWord(io, out.sourceOffset + 8, out, header[2]))
        return DeviceDiscoveryStatus::TransportError;
    out.binarySize = header[2] >> 16;
    if (out.binarySize < 12 || out.binarySize > out.regionSize)
        return DeviceDiscoveryStatus::MalformedHeader;
    if (out.binarySize > capacity) return DeviceDiscoveryStatus::BufferTooSmall;

    for (size_t i = 0; i < 3; ++i) copyWord(buffer, i * 4, header[i], 4);
    out.bytesCopied = 12;
    for (size_t offset = 12; offset < out.binarySize; offset += 4) {
        if (!vramWord(io, out.sourceOffset + offset, out, value))
            return DeviceDiscoveryStatus::TransportError;
        const size_t remaining = out.binarySize - offset;
        const size_t count = remaining < 4 ? remaining : 4;
        copyWord(buffer, offset, value, count);
        out.bytesCopied += count;
    }
    return DeviceDiscoveryStatus::Ok;
}

const char* deviceDiscoveryStatusString(DeviceDiscoveryStatus status) {
    switch (status) {
    case DeviceDiscoveryStatus::Ok: return "ok";
    case DeviceDiscoveryStatus::InvalidArgument: return "invalid argument";
    case DeviceDiscoveryStatus::ReadyTimeout: return "firmware-ready timeout";
    case DeviceDiscoveryStatus::TransportError: return "MMIO transport failure";
    case DeviceDiscoveryStatus::DeviceUnavailable: return "device unavailable";
    case DeviceDiscoveryStatus::SystemMemoryUnsupported: return "system-memory discovery unsupported";
    case DeviceDiscoveryStatus::BadSignature: return "discovery signature not found";
    case DeviceDiscoveryStatus::MalformedHeader: return "invalid discovery binary length";
    case DeviceDiscoveryStatus::InvalidVramRange: return "invalid discovery VRAM range";
    case DeviceDiscoveryStatus::BufferTooSmall: return "capture buffer too small";
    }
    return "unknown device discovery status";
}
} // namespace hackynavi
