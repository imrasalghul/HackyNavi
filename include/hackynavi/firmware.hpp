// SPDX-License-Identifier: MIT
// Original bounded decoder for AMD firmware container metadata. No execution.
#ifndef HACKYNAVI_FIRMWARE_HPP
#define HACKYNAVI_FIRMWARE_HPP

#include "hackynavi/rom.hpp"

namespace hackynavi {

enum : size_t {
    kCommonFirmwareHeaderBytes = 32,
    kMaxPspFirmwareDescriptors = 32 // Parser capacity, not a hardware limit.
};

struct FirmwareRange {
    uint32_t offset; // Absolute byte offset from the start of the supplied file.
    uint32_t size;
};

struct FirmwareHeader {
    uint32_t sizeBytes;
    uint32_t headerSizeBytes;
    uint16_t headerMajor; // Family-specific layout version, not an AMD magic.
    uint16_t headerMinor;
    uint16_t ipMajor;
    uint16_t ipMinor;
    uint32_t ucodeVersion;
    uint32_t payloadSize;
    uint32_t payloadOffset;
    uint32_t storedCrc32; // Metadata only. Not verified and not authentication.
};

struct GfxFirmwareV2 {
    FirmwareHeader header;
    uint32_t featureVersion;
    FirmwareRange instruction;
    FirmwareRange data;
    uint64_t startAddress; // Raw firmware execution address, never dereferenced.
};

struct SdmaFirmwareV2 {
    FirmwareHeader header;
    uint32_t featureVersion;
    FirmwareRange context;
    FirmwareRange control;
    FirmwareRange contextJumpTable;
    FirmwareRange controlJumpTable;
};

struct PspFirmwareDescriptor {
    uint32_t type; // Opaque PSP firmware type enum; unknown types are preserved.
    uint32_t version;
    uint32_t payloadRelativeOffset;
    FirmwareRange image;
};

struct PspFirmwareV2 {
    FirmwareHeader header;
    uint32_t descriptorCount;
    PspFirmwareDescriptor descriptors[kMaxPspFirmwareDescriptors];
};

// The Linux common header is a 32-byte little-endian prefix with no signature.
// Validate declared file size exactly, header bounds and the common payload
// range. Accept any version here; only a family-specific decoder can interpret
// it. Linux amdgpu_ucode_validate at the source pin compares size_bytes only;
// it does NOT calculate the stored CRC32 or establish its polynomial/seed.
// This decoder therefore reports storedCrc32 without inventing CRC semantics.
Status parseFirmwareHeader(const uint8_t* data, size_t size, FirmwareHeader& out);

// Exact header version 2.0 only. GFX and SDMA offsets are absolute file bytes;
// PSP descriptors instead hold offsets relative to the common payload start.
// Callers select the known firmware family: version 2.0 is shared by unrelated
// families and cannot identify a file or prove Navi 31 compatibility by itself.
// Additional GFX/SDMA segments are bounded against the file, not assumed to be
// contained in the common range (Linux addresses them independently).
// These functions perform no signature, authenticity, IP compatibility, firmware
// loading or hardware-state validation. No input data is written or allocated.
// All output is zero on error except UnsupportedVersion, which retains only
// the validated common header. No borrowed pointer is stored in the output.
Status parseGfxFirmwareV2(const uint8_t* data, size_t size, GfxFirmwareV2& out);
Status parseSdmaFirmwareV2(const uint8_t* data, size_t size, SdmaFirmwareV2& out);
Status parsePspFirmwareV2(const uint8_t* data, size_t size, PspFirmwareV2& out);

} // namespace hackynavi
#endif
