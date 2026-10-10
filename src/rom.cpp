// SPDX-License-Identifier: MIT
// Original implementation; no upstream implementation code is incorporated.
// Wire-layout references: AMD's MIT-licensed atomfirmware.h in Linux
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0, especially
// atom_rom_header_v2_2, atom_master_data_table_v2_1,
// atom_vram_info_header_v3_0 and display_object_info_table_v1_5:
// https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/include/atomfirmware.h
// The VRAM 3.0 channelCount * 16 interpretation is documented in:
// https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/amdgpu_atomfirmware.c
// Reservation layouts: atomfirmware.h:575-647,742-800; operation flag encoding:
// atombios.h:4298-4302 at that same pin. Linux amdgpu_atomfirmware.c:105-169,
// 1006-1051 separates firmware reservation size, usage-region flags, and the
// two-stage memory-training capability. We decode metadata, not its allocator.

#include "hackynavi/rom.hpp"

namespace hackynavi {
namespace {

// Subtraction is intentional: offset + length could wrap for hostile inputs.
bool contains(size_t limit, size_t offset, size_t length) {
    return offset <= limit && length <= limit - offset;
}

uint16_t little16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t little32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// The caller has already established a bounded enclosing PCI image.
Status readTable(const uint8_t* base, size_t imageSize, uint16_t offset,
                 TableInfo& out) {
    out = TableInfo{};
    if (offset == 0)
        return Status::NotFound;
    if (!contains(imageSize, offset, 4))
        return Status::Truncated;
    const uint8_t* p = base + offset;
    const uint16_t tableSize = little16(p);
    if (tableSize < 4)
        return Status::Malformed;
    if (!contains(imageSize, offset, tableSize))
        return Status::Truncated;
    out.offset = offset;
    out.size = tableSize;
    out.formatRevision = p[2];
    out.contentRevision = p[3];
    return Status::Ok;
}

// The VRAM 3.0 offsets refer to opaque payloads within the same VRAM table.
// Checking only their containing byte is sufficient here: we do not decode or
// execute those payloads, or assume that they have ATOM common headers.
bool validVramOffset(size_t tableSize, size_t payloadStart, uint16_t offset) {
    return offset == 0 ||
           (offset >= payloadStart && contains(tableSize, offset, 1));
}

Status validateDisplayRecords(const uint8_t* table, size_t tableSize,
                              size_t payloadStart, uint16_t offset) {
    if (offset == 0)
        return Status::Ok;
    if (offset < payloadStart)
        return Status::Malformed;
    size_t cursor = offset;
    // Each nonterminal record advances by at least two bytes. Thus malformed
    // input cannot create an unbounded loop or wrap cursor.
    while (contains(tableSize, cursor, 2)) {
        const uint8_t type = table[cursor];
        const uint8_t length = table[cursor + 1];
        if (type == 0xff) {
            // Firmware commonly encodes the terminal record as FF 00.
            if (length != 0 &&
                (length < 2 || !contains(tableSize, cursor, length)))
                return Status::Malformed;
            return Status::Ok;
        }
        if (length < 2)
            return Status::Malformed;
        if (!contains(tableSize, cursor, length))
            return Status::Truncated;
        cursor += length;
    }
    return Status::Truncated; // Missing or incomplete terminal record.
}

} // namespace

Status parseImage(const uint8_t* data, size_t size, size_t offset, ImageInfo& out) {
    out = ImageInfo{};
    if (data == nullptr || offset > size)
        return Status::InvalidArgument;
    if (!contains(size, offset, 0x1a))
        return Status::Truncated;
    const uint8_t* base = data + offset;
    if (base[0] != 0x55 || base[1] != 0xaa)
        return Status::BadSignature;
    const uint16_t pcirOffset = little16(base + 0x18);
    if (pcirOffset < 0x1a)
        return Status::Malformed;
    const size_t available = size - offset;
    if (!contains(available, pcirOffset, 0x18))
        return Status::Truncated;
    const uint8_t* pcir = base + pcirOffset;
    if (pcir[0] != 'P' || pcir[1] != 'C' ||
        pcir[2] != 'I' || pcir[3] != 'R')
        return Status::BadSignature;
    const uint16_t pcirSize = little16(pcir + 0x0a);
    if (pcirSize < 0x18)
        return Status::Malformed;
    const size_t imageSize = static_cast<size_t>(little16(pcir + 0x10)) * 512;
    if (imageSize == 0)
        return Status::Malformed;
    if (imageSize > available)
        return Status::Truncated;
    if (!contains(imageSize, pcirOffset, pcirSize))
        return Status::Malformed;

    ImageInfo result = {};
    result.offset = offset;
    result.size = imageSize;
    result.pcirOffset = pcirOffset;
    result.pcirSize = pcirSize;
    result.vendorId = little16(pcir + 4);
    result.deviceId = little16(pcir + 6);
    result.codeType = pcir[0x14];
    result.lastImage = (pcir[0x15] & 0x80) != 0;
    result.checksumApplicable = result.codeType == 0;
    if (result.checksumApplicable) {
        uint8_t sum = 0;
        for (size_t i = 0; i < imageSize; ++i)
            sum = static_cast<uint8_t>(sum + base[i]);
        result.checksumValid = sum == 0;
    }
    out = result;
    return Status::Ok;
}

Status findNextImage(const uint8_t* data, size_t size, size_t startOffset,
                     ImageInfo& out) {
    out = ImageInfo{};
    if (data == nullptr || startOffset > size)
        return Status::InvalidArgument;
    for (size_t offset = startOffset; contains(size, offset, 0x1a); ++offset) {
        if (data[offset] != 0x55 || data[offset + 1] != 0xaa)
            continue;
        ImageInfo candidate = {};
        if (parseImage(data, size, offset, candidate) == Status::Ok) {
            out = candidate;
            return Status::Ok;
        }
    }
    return Status::NotFound;
}

Status parseAtom(const uint8_t* data, size_t size, const ImageInfo& image,
                 AtomInfo& out) {
    out = AtomInfo{};
    ImageInfo bounded = {};
    Status status = parseImage(data, size, image.offset, bounded);
    if (status != Status::Ok)
        return status;
    if (bounded.codeType != 0)
        return Status::NotFound;
    const uint8_t* base = data + bounded.offset;
    if (!contains(bounded.size, 0x48, 2))
        return Status::Truncated;
    AtomInfo result = {};
    status = readTable(base, bounded.size, little16(base + 0x48), result.header);
    if (status != Status::Ok)
        return status;
    if (result.header.size < 8)
        return Status::Malformed;
    const uint8_t* header = base + result.header.offset;
    if (header[4] != 'A' || header[5] != 'T' ||
        header[6] != 'O' || header[7] != 'M')
        return Status::BadSignature;
    if (result.header.formatRevision != 2 ||
        (result.header.contentRevision != 2 && result.header.contentRevision != 3)) {
        out = result;
        return Status::UnsupportedVersion;
    }
    // Decode only the established v2.2 prefix, also present in the Navi 31
    // v2.3 header. Any v2.3 extension bytes remain opaque.
    if (result.header.size < 40)
        return Status::Malformed;
    result.subsystemVendorId = little16(header + 24);
    result.subsystemDeviceId = little16(header + 26);
    result.commandTableOffset = little16(header + 30);
    if (result.commandTableOffset != 0) {
        TableInfo command = {};
        status = readTable(base, bounded.size, result.commandTableOffset, command);
        if (status != Status::Ok)
            return status;
    }
    status = readTable(base, bounded.size, little16(header + 32), result.masterData);
    if (status != Status::Ok)
        return status == Status::NotFound ? Status::Malformed : status;
    if (result.masterData.formatRevision != 2 || result.masterData.contentRevision != 1) {
        out = result;
        return Status::UnsupportedVersion;
    }
    if (result.masterData.size < 4 + kAtomTableCount * 2 ||
        ((result.masterData.size - 4) % 2) != 0)
        return Status::Malformed;
    const uint8_t* directory = base + result.masterData.offset + 4;
    for (size_t i = 0; i < kAtomTableCount; ++i) {
        const uint16_t offset = little16(directory + i * 2);
        if (offset == 0)
            continue;
        status = readTable(base, bounded.size, offset, result.tables[i]);
        if (status != Status::Ok)
            return status;
    }
    out = result;
    return Status::Ok;
}

Status parseVram(const uint8_t* data, size_t size, const ImageInfo& image,
                 VramInfo& out) {
    out = VramInfo{};
    AtomInfo atom = {};
    Status status = parseAtom(data, size, image, atom);
    if (status != Status::Ok)
        return status;
    VramInfo result = {};
    result.table = atom.tables[kVramTableIndex];
    if (result.table.offset == 0)
        return Status::NotFound;
    if (result.table.formatRevision != 3 || result.table.contentRevision != 0) {
        out = result;
        return Status::UnsupportedVersion;
    }
    const size_t headerSize = 64;
    const size_t moduleSize = 68;
    if (result.table.size < headerSize)
        return Status::Malformed;
    const uint8_t* table = data + image.offset + result.table.offset;
    const uint8_t moduleCount = table[20];
    const size_t payloadStart = headerSize + static_cast<size_t>(moduleCount) * moduleSize;
    if (payloadStart > result.table.size)
        return Status::Truncated;
    if (moduleCount > kMaxVramModules)
        return Status::CapacityExceeded;
    if (moduleCount == 0 || table[25] == 0)
        return Status::Malformed;
    // Six specified subtable offsets; the following two are reserved.
    for (size_t i = 0; i < 6; ++i) {
        if (!validVramOffset(result.table.size, payloadStart, little16(table + 4 + i * 2)))
            return Status::Malformed;
    }
    result.memoryType = table[24];
    result.channelCount = table[25];
    result.channelWidthRaw = table[26];
    result.busWidthBits = static_cast<uint16_t>(result.channelCount * 16);
    result.moduleCount = moduleCount;
    for (size_t i = 0; i < moduleCount; ++i) {
        const uint8_t* module = table + headerSize + i * moduleSize;
        for (size_t pointer = 4; pointer <= 8; pointer += 2) {
            if (!validVramOffset(result.table.size, payloadStart, little16(module + pointer)))
                return Status::Malformed;
        }
        VramModuleInfo& entry = result.modules[i];
        entry.density = module[0];
        entry.vendorId = module[3];
        entry.sizePerChannel = little32(module + 12);
        for (size_t j = 0; j < 40; ++j) {
            const uint8_t ch = module[28 + j];
            if (ch == 0)
                break;
            // Do not emit terminal control bytes supplied by a ROM image.
            entry.partNumber[j] = ch >= 0x20 && ch <= 0x7e ? static_cast<char>(ch) : '?';
        }
        entry.partNumber[40] = '\0';
    }
    out = result;
    return Status::Ok;
}

Status parseDisplay(const uint8_t* data, size_t size, const ImageInfo& image,
                    DisplayInfo& out) {
    out = DisplayInfo{};
    AtomInfo atom = {};
    Status status = parseAtom(data, size, image, atom);
    if (status != Status::Ok)
        return status;
    DisplayInfo result = {};
    result.table = atom.tables[kDisplayTableIndex];
    if (result.table.offset == 0)
        return Status::NotFound;
    if (result.table.formatRevision != 1 || result.table.contentRevision != 5) {
        out = result;
        return Status::UnsupportedVersion;
    }
    if (result.table.size < 8)
        return Status::Malformed;
    const uint8_t* table = data + image.offset + result.table.offset;
    const uint8_t pathCount = table[6];
    const size_t payloadStart = 8 + static_cast<size_t>(pathCount) * 16;
    if (payloadStart > result.table.size)
        return Status::Truncated;
    if (pathCount > kMaxDisplayPaths)
        return Status::CapacityExceeded;
    result.supportedDevices = little16(table + 4);
    result.pathCount = pathCount;
    for (size_t i = 0; i < pathCount; ++i) {
        const uint8_t* path = table + 8 + i * 16;
        DisplayPathInfo& entry = result.paths[i];
        entry.displayObjectId = little16(path);
        entry.recordOffset = little16(path + 2);
        entry.encoderObjectId = little16(path + 4);
        entry.deviceTag = little16(path + 12);
        status = validateDisplayRecords(table, result.table.size, payloadStart,
                                        entry.recordOffset);
        if (status != Status::Ok)
            return status;
    }
    out = result;
    return Status::Ok;
}

Status parseFirmwareInfo(const uint8_t* data, size_t size, const ImageInfo& image,
                         FirmwareInfo& out) {
    out = FirmwareInfo{};
    AtomInfo atom = {};
    const Status status = parseAtom(data, size, image, atom);
    if (status != Status::Ok)
        return status;
    const TableInfo table = atom.tables[kFirmwareInfoTableIndex];
    if (!table.offset)
        return Status::NotFound;
    // Retain only the validated common header if decoding cannot proceed.
    out.present = true;
    out.table = table;
    if (table.formatRevision != 3 ||
        (table.contentRevision != 4 && table.contentRevision != 5))
        return Status::UnsupportedVersion;
    if (table.size < (table.contentRevision == 4 ? 108 : 172))
        return Status::Malformed;
    const uint8_t* p = data + image.offset + table.offset;
    FirmwareInfo result = out;
    result.firmwareRevision = little32(p + 4);
    result.firmwareCapability = little32(p + 16);
    result.biosScratchRegisterDword = little32(p + 24);
    result.firmwareReservedKiB = little32(p + 84);
    result.twoStageMemoryTraining = (result.firmwareCapability & 0x400u) != 0;
    if (table.contentRevision == 5) {
        result.protectedRegionSizePresent = true;
        result.protectedRegionSizeKiB = little32(p + 20);
    }
    out = result;
    return Status::Ok;
}

Status parseVramUsage(const uint8_t* data, size_t size, const ImageInfo& image,
                      VramUsageInfo& out) {
    out = VramUsageInfo{};
    AtomInfo atom = {};
    const Status status = parseAtom(data, size, image, atom);
    if (status != Status::Ok)
        return status;
    const TableInfo table = atom.tables[kVramUsageTableIndex];
    if (!table.offset)
        return Status::NotFound;
    out.present = true;
    out.table = table;
    if (table.formatRevision != 2 ||
        (table.contentRevision != 1 && table.contentRevision != 2))
        return Status::UnsupportedVersion;
    if (table.size < (table.contentRevision == 1 ? 12 : 48))
        return Status::Malformed;
    const uint8_t* p = data + image.offset + table.offset;
    VramUsageInfo result = out;
    result.firmware.startAddressPresent = true;
    result.firmware.startAddressKiBRaw = little32(p + 4);
    result.firmware.startAddressKiB = result.firmware.startAddressKiBRaw & 0x3fffffffu;
    result.firmware.operationFlags = static_cast<uint8_t>(result.firmware.startAddressKiBRaw >> 30);
    result.firmware.sizeKiB = little16(p + 8);
    if (table.contentRevision == 1) {
        result.driver.sizeKiB = little16(p + 10);
    } else {
        result.driver.startAddressPresent = true;
        result.driver.startAddressKiBRaw = little32(p + 12);
        result.driver.startAddressKiB = result.driver.startAddressKiBRaw & 0x3fffffffu;
        result.driver.operationFlags = static_cast<uint8_t>(result.driver.startAddressKiBRaw >> 30);
        result.driver.sizeKiB = little32(p + 16);
    }
    out = result;
    return Status::Ok;
}

const char* statusString(Status status) {
    switch (status) {
    case Status::Ok: return "ok";
    case Status::InvalidArgument: return "invalid argument";
    case Status::NotFound: return "not found";
    case Status::Truncated: return "truncated";
    case Status::BadSignature: return "bad signature";
    case Status::Malformed: return "malformed";
    case Status::UnsupportedVersion: return "unsupported version";
    case Status::CapacityExceeded: return "capacity exceeded";
    }
    return "unknown status";
}

} // namespace hackynavi
