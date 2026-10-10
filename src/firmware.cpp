// SPDX-License-Identifier: MIT
// Wire layout and offset semantics audited against Linux commit
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h (common/GFX/SDMA/PSP headers),
// amdgpu_ucode.c (amdgpu_ucode_validate and SDMA PSP-loading ranges),
// gfx_v11_0.c (RS64 instruction/data file offsets), sdma_v6_0.c (byte JT fields),
// amdgpu_psp.c (parse_sos_bin_descriptor's payload-relative PSP offsets).
#include "hackynavi/firmware.hpp"

namespace hackynavi {
namespace {
uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}
uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
bool fileRange(const FirmwareHeader& header, const FirmwareRange& range) {
    // Subtraction avoids overflow even when two malicious uint32 fields sum
    // beyond UINT32_MAX on a 32-bit host or in a firmware wire address.
    return range.offset >= header.headerSizeBytes &&
           range.offset <= header.sizeBytes &&
           range.size <= header.sizeBytes - range.offset;
}
bool dwords(const FirmwareRange& range) {
    return (range.offset & 3U) == 0 && (range.size & 3U) == 0;
}
bool jumpTable(const FirmwareRange& image, uint32_t relativeOffset,
               uint32_t bytes, FirmwareRange& result) {
    if (relativeOffset > image.size || bytes > image.size - relativeOffset)
        return false;
    // image was file-bounded before this call, so this addition cannot wrap.
    result.offset = image.offset + relativeOffset;
    result.size = bytes;
    return dwords(result);
}
} // namespace

Status parseFirmwareHeader(const uint8_t* data, size_t size, FirmwareHeader& out) {
    out = FirmwareHeader{};
    if (!data) return Status::InvalidArgument;
    if (size < kCommonFirmwareHeaderBytes) return Status::Truncated;
    FirmwareHeader header = {};
    header.sizeBytes = le32(data);
    header.headerSizeBytes = le32(data + 4);
    header.headerMajor = le16(data + 8);
    header.headerMinor = le16(data + 10);
    header.ipMajor = le16(data + 12);
    header.ipMinor = le16(data + 14);
    header.ucodeVersion = le32(data + 16);
    header.payloadSize = le32(data + 20);
    header.payloadOffset = le32(data + 24);
    header.storedCrc32 = le32(data + 28);
    if (header.sizeBytes > size) return Status::Truncated;
    if (header.sizeBytes != size || header.headerSizeBytes < kCommonFirmwareHeaderBytes ||
        header.headerSizeBytes > header.sizeBytes)
        return Status::Malformed;
    const FirmwareRange payload = {header.payloadOffset, header.payloadSize};
    if (!fileRange(header, payload)) return Status::Malformed;
    out = header;
    return Status::Ok;
}

Status parseGfxFirmwareV2(const uint8_t* data, size_t size, GfxFirmwareV2& out) {
    out = GfxFirmwareV2{};
    GfxFirmwareV2 result = {};
    Status status = parseFirmwareHeader(data, size, result.header);
    if (status != Status::Ok) return status;
    if (result.header.headerMajor != 2 || result.header.headerMinor != 0) {
        out.header = result.header;
        return Status::UnsupportedVersion;
    }
    if (result.header.headerSizeBytes < 60) return Status::Malformed;
    result.featureVersion = le32(data + 32);
    result.instruction.size = le32(data + 36);
    result.instruction.offset = le32(data + 40);
    result.data.size = le32(data + 44);
    result.data.offset = le32(data + 48);
    result.startAddress = uint64_t(le32(data + 52)) | (uint64_t(le32(data + 56)) << 32);
    if (!fileRange(result.header, result.instruction) ||
        !fileRange(result.header, result.data) ||
        !dwords(result.instruction) || !dwords(result.data))
        return Status::Malformed;
    out = result;
    return Status::Ok;
}

Status parseSdmaFirmwareV2(const uint8_t* data, size_t size, SdmaFirmwareV2& out) {
    out = SdmaFirmwareV2{};
    SdmaFirmwareV2 result = {};
    Status status = parseFirmwareHeader(data, size, result.header);
    if (status != Status::Ok) return status;
    if (result.header.headerMajor != 2 || result.header.headerMinor != 0) {
        out.header = result.header;
        return Status::UnsupportedVersion;
    }
    if (result.header.headerSizeBytes < 64) return Status::Malformed;
    result.featureVersion = le32(data + 32);
    result.context.offset = result.header.payloadOffset;
    result.context.size = le32(data + 36);
    result.control.offset = le32(data + 48);
    result.control.size = le32(data + 52);
    if (!fileRange(result.header, result.context) ||
        !fileRange(result.header, result.control) ||
        !dwords(result.context) || !dwords(result.control) ||
        !jumpTable(result.context, le32(data + 40), le32(data + 44), result.contextJumpTable) ||
        !jumpTable(result.control, le32(data + 56), le32(data + 60), result.controlJumpTable))
        return Status::Malformed;
    out = result;
    return Status::Ok;
}

Status parsePspFirmwareV2(const uint8_t* data, size_t size, PspFirmwareV2& out) {
    out = PspFirmwareV2{};
    PspFirmwareV2 result = {};
    Status status = parseFirmwareHeader(data, size, result.header);
    if (status != Status::Ok) return status;
    if (result.header.headerMajor != 2 || result.header.headerMinor != 0) {
        out.header = result.header;
        return Status::UnsupportedVersion;
    }
    if (result.header.headerSizeBytes < 36) return Status::Malformed;
    result.descriptorCount = le32(data + 32);
    if (result.descriptorCount > kMaxPspFirmwareDescriptors)
        return Status::CapacityExceeded;
    // PSP 2.0 has a flexible descriptor array: real Navi 31 files report a
    // header_size_bytes of 36 (the fixed prefix) and place descriptors after
    // that prefix, before payloadOffset (usually 256). Do not incorrectly
    // require flexible records to fit within header_size_bytes itself.
    // Count is capped before multiplication; records cannot overlap payload.
    if (result.descriptorCount > (result.header.payloadOffset - 36) / 16)
        return Status::Malformed;
    for (uint32_t i = 0; i < result.descriptorCount; ++i) {
        const uint8_t* record = data + 36 + size_t(i) * 16;
        PspFirmwareDescriptor& descriptor = result.descriptors[i];
        descriptor.type = le32(record);
        descriptor.version = le32(record + 4);
        descriptor.payloadRelativeOffset = le32(record + 8);
        descriptor.image.size = le32(record + 12);
        if (descriptor.payloadRelativeOffset > result.header.payloadSize ||
            descriptor.image.size > result.header.payloadSize - descriptor.payloadRelativeOffset)
            return Status::Malformed;
        descriptor.image.offset = result.header.payloadOffset + descriptor.payloadRelativeOffset;
    }
    out = result;
    return Status::Ok;
}
} // namespace hackynavi
