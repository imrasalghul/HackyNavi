// SPDX-License-Identifier: MIT
// Original bounded, read-only PCI/ATOM metadata parser. No hardware access.
#ifndef HACKYNAVI_ROM_HPP
#define HACKYNAVI_ROM_HPP

#include <stddef.h>
#include <stdint.h>

namespace hackynavi {

enum class Status {
    Ok,
    InvalidArgument,
    NotFound,
    Truncated,
    BadSignature,
    Malformed,
    UnsupportedVersion,
    CapacityExceeded
};

enum : size_t {
    kAtomTableCount = 35,
    kMaxVramModules = 8,
    kMaxDisplayPaths = 16,
    kFirmwareInfoTableIndex = 4,
    kVramUsageTableIndex = 11,
    kDisplayTableIndex = 22,
    kVramTableIndex = 28
};

struct ImageInfo {
    size_t offset;             // Absolute byte offset in the supplied buffer.
    size_t size;               // PCIR image length in bytes, not the flash size.
    size_t pcirOffset;         // Relative to this image, like all ATOM offsets.
    uint16_t pcirSize;
    uint16_t vendorId;
    uint16_t deviceId;
    uint8_t codeType;          // 0 = legacy x86, 3 = EFI.
    bool lastImage;
    bool checksumApplicable;  // Only a legacy image has this check here.
    bool checksumValid;       // Diagnostic only; not an authenticity check.
};

struct TableInfo {
    uint16_t offset;           // Relative to the enclosing PCI image; 0 = absent.
    uint16_t size;
    uint8_t formatRevision;
    uint8_t contentRevision;
};

struct AtomInfo {
    TableInfo header;
    TableInfo masterData;
    uint16_t subsystemVendorId;
    uint16_t subsystemDeviceId;
    uint16_t commandTableOffset;
    TableInfo tables[kAtomTableCount];
};

struct VramModuleInfo {
    uint8_t density;
    uint8_t vendorId;          // Raw table value, not the selected live device.
    uint32_t sizePerChannel;  // Raw dram_size_per_ch value from the table.
    char partNumber[41];      // 40-byte firmware string plus guaranteed NUL.
};

struct VramInfo {
    TableInfo table;
    uint8_t memoryType;       // 0x70 = GDDR6.
    uint8_t channelCount;
    uint8_t channelWidthRaw;
    uint16_t busWidthBits;    // VRAM 3.0 rule: channelCount * 16.
    uint8_t moduleCount;      // Alternative definitions, not populated modules.
    VramModuleInfo modules[kMaxVramModules];
};

struct DisplayPathInfo {
    uint16_t displayObjectId;
    uint16_t recordOffset;    // Relative to the display table, NOT the image.
    uint16_t encoderObjectId;
    uint16_t deviceTag;
};

struct DisplayInfo {
    TableInfo table;
    uint16_t supportedDevices;
    uint8_t pathCount;        // Firmware paths, not detected connected displays.
    DisplayPathInfo paths[kMaxDisplayPaths];
};

struct FirmwareInfo {
    TableInfo table;
    bool present;             // A bounded common header was found, not live state.
    uint32_t firmwareRevision;
    uint32_t firmwareCapability;
    uint32_t biosScratchRegisterDword; // Absolute DWORD register index; not bytes.
    uint32_t firmwareReservedKiB; // fw_reserved_size_in_kb, not a free-space map.
    bool twoStageMemoryTraining; // Reported capability bit 0x400 only.
    bool protectedRegionSizePresent; // FirmwareInfo 3.5 supplies this field.
    uint32_t protectedRegionSizeKiB; // Reported write-protected region at FB top.
};

struct VramUsageRegion {
    bool startAddressPresent;
    uint32_t startAddressKiBRaw; // Includes the operation flags in bits 31:30.
    uint32_t startAddressKiB;  // Address with those two flags removed.
    uint32_t sizeKiB;
    uint8_t operationFlags;   // Raw flags: 0=reservation, 1=no-reservation,
                              // 2=SR-IOV message-share; 3 is preserved opaque.
};

struct VramUsageInfo {
    TableInfo table;
    bool present;
    VramUsageRegion firmware;
    VramUsageRegion driver;   // 2.1 has a size only; no explicit driver address.
};

// All routines are allocation-free and make no writes to data. Unless noted
// below, outputs are zeroed on failure. Descriptors supplied by a caller are
// revalidated against data; the parser never trusts their size or table fields.
Status parseImage(const uint8_t* data, size_t size, size_t offset, ImageInfo& out);

// Searches from startOffset inclusively, at byte granularity, for the first
// structurally valid PCI image. Malformed/truncated candidates are skipped.
// To enumerate distinct images, pass previous.offset + previous.size next.
// A last-image flag terminates its chain, not a full flash dump scan.
Status findNextImage(const uint8_t* data, size_t size, size_t startOffset,
                     ImageInfo& out);

// Decodes the shared ATOM 2.2/2.3 prefix and master data 2.1. On an unsupported
// ATOM or master revision, returns UnsupportedVersion and retains validated
// common headers in out; no unknown-version fields/tables are interpreted.
Status parseAtom(const uint8_t* data, size_t size, const ImageInfo& image,
                 AtomInfo& out);

// These routines locate their tables through a fresh parseAtom call. Only
// VRAM 3.0 and display 1.5 are decoded. For unsupported leaf table versions,
// out.table is preserved; all remaining fields stay zero.
Status parseVram(const uint8_t* data, size_t size, const ImageInfo& image,
                 VramInfo& out);
Status parseDisplay(const uint8_t* data, size_t size, const ImageInfo& image,
                    DisplayInfo& out);

// Decode FirmwareInfo 3.4/3.5 and VRAM usage 2.1/2.2 only. Each repeats the
// bounded image/ATOM lookup and requires the whole known table structure:
// 108/172 bytes for FirmwareInfo, 12/48 for VRAM usage. Additional bytes remain
// opaque. On UnsupportedVersion or a too-short supported table (Malformed),
// only present/table survive; payload fields stay zero. Earlier envelope/ATOM
// errors leave all output zero. Only NotFound means a confirmed absent table.
// These are values in the supplied ROM image. Firmware can update usage at
// runtime; absent/zero entries do not prove that any VRAM is free. No region
// end, live-VRAM bounds, overlap, ownership, or derived 2.1 driver start is
// inferred. A caller must validate those against independent runtime evidence.
Status parseFirmwareInfo(const uint8_t* data, size_t size, const ImageInfo& image,
                         FirmwareInfo& out);
Status parseVramUsage(const uint8_t* data, size_t size, const ImageInfo& image,
                      VramUsageInfo& out);

const char* statusString(Status status);

} // namespace hackynavi

#endif // HACKYNAVI_ROM_HPP
