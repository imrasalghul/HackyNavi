// SPDX-License-Identifier: MIT
// Offline inspection only. This executable never opens hardware or writes a ROM.
#include "hackynavi/rom.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::string quoted(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c >= 0x20 && c < 0x7f) out << static_cast<char>(c);
        else out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    }
    out << '"';
    return out.str();
}
std::string hexValue(uint64_t value) {
    std::ostringstream out;
    out << "\"0x" << std::hex << value << '"';
    return out.str();
}
void table(const hackynavi::TableInfo& t) {
    std::cout << "{\"relative_offset\":" << t.offset << ",\"bytes\":" << t.size
              << ",\"format\":" << unsigned(t.formatRevision)
              << ",\"content\":" << unsigned(t.contentRevision) << '}';
}
void usageRegion(const hackynavi::VramUsageRegion& region) {
    std::cout << "{\"start_address_present\":" << (region.startAddressPresent ? "true" : "false")
              << ",\"size_kib\":" << region.sizeKiB;
    if (region.startAddressPresent) {
        std::cout << ",\"start_address_kib_raw\":" << hexValue(region.startAddressKiBRaw)
                  << ",\"start_address_kib\":" << region.startAddressKiB
                  << ",\"operation_flags_raw\":" << unsigned(region.operationFlags);
    }
    std::cout << '}';
}
void reservationMetadata(const std::vector<uint8_t>& data, const hackynavi::ImageInfo& image) {
    using namespace hackynavi;
    FirmwareInfo firmware = {};
    Status status = parseFirmwareInfo(data.data(), data.size(), image, firmware);
    std::cout << ",\"firmware_info\":{\"status\":" << quoted(statusString(status))
              << ",\"present\":" << (firmware.present ? "true" : "false")
              << ",\"table\":"; table(firmware.table);
    if (status == Status::Ok) {
        std::cout << ",\"firmware_revision_raw\":" << hexValue(firmware.firmwareRevision)
                  << ",\"firmware_capability_raw\":" << hexValue(firmware.firmwareCapability)
                  << ",\"bios_scratch_register_dword\":" << hexValue(firmware.biosScratchRegisterDword)
                  << ",\"firmware_reserved_kib\":" << firmware.firmwareReservedKiB
                  << ",\"two_stage_memory_training_capability\":"
                  << (firmware.twoStageMemoryTraining ? "true" : "false")
                  << ",\"protected_region_size_present\":"
                  << (firmware.protectedRegionSizePresent ? "true" : "false");
        if (firmware.protectedRegionSizePresent)
            std::cout << ",\"protected_region_size_kib\":" << firmware.protectedRegionSizeKiB;
    }
    std::cout << '}';
    VramUsageInfo usage = {};
    status = parseVramUsage(data.data(), data.size(), image, usage);
    std::cout << ",\"vram_usage_by_firmware\":{\"status\":" << quoted(statusString(status))
              << ",\"present\":" << (usage.present ? "true" : "false")
              << ",\"table\":"; table(usage.table);
    if (status == Status::Ok) {
        std::cout << ",\"firmware_region\":"; usageRegion(usage.firmware);
        std::cout << ",\"driver_region\":"; usageRegion(usage.driver);
    }
    std::cout << '}';
}
void atom(const std::vector<uint8_t>& data, const hackynavi::ImageInfo& image) {
    using namespace hackynavi;
    AtomInfo a = {};
    Status status = parseAtom(data.data(), data.size(), image, a);
    std::cout << ",\"atom\":{\"status\":" << quoted(statusString(status));
    if (status == Status::UnsupportedVersion) {
        std::cout << ",\"header\":"; table(a.header);
        if (a.masterData.offset) {
            std::cout << ",\"master_data\":"; table(a.masterData);
        }
    }
    if (status == Status::Ok) {
        std::cout << ",\"header\":"; table(a.header);
        std::cout << ",\"subsystem_vendor_id\":" << hexValue(a.subsystemVendorId)
                  << ",\"subsystem_device_id\":" << hexValue(a.subsystemDeviceId)
                  << ",\"master_data\":"; table(a.masterData);
        std::cout << ",\"tables\":[";
        bool first = true;
        for (size_t i = 0; i < kAtomTableCount; ++i) {
            if (!a.tables[i].offset) continue;
            if (!first) std::cout << ',';
            first = false;
            std::cout << "{\"index\":" << i << ",\"table\":";
            table(a.tables[i]); std::cout << '}';
        }
        std::cout << ']';
        reservationMetadata(data, image);
        VramInfo v = {};
        status = parseVram(data.data(), data.size(), image, v);
        std::cout << ",\"vram\":{\"status\":" << quoted(statusString(status))
                  << ",\"table\":"; table(v.table);
        if (status == Status::Ok) {
            std::cout << ",\"memory_type_raw\":" << hexValue(v.memoryType)
                      << ",\"channel_count\":" << unsigned(v.channelCount)
                      << ",\"channel_width_raw\":" << unsigned(v.channelWidthRaw)
                      << ",\"bus_width_bits\":" << v.busWidthBits
                      << ",\"module_definitions\":[";
            for (size_t i = 0; i < v.moduleCount; ++i) {
                if (i) std::cout << ',';
                std::cout << "{\"vendor_raw\":" << unsigned(v.modules[i].vendorId)
                          << ",\"size_per_channel_raw\":" << v.modules[i].sizePerChannel
                          << ",\"part_number\":" << quoted(v.modules[i].partNumber) << '}';
            }
            std::cout << ']';
        }
        std::cout << '}';
        DisplayInfo d = {};
        status = parseDisplay(data.data(), data.size(), image, d);
        std::cout << ",\"display\":{\"status\":" << quoted(statusString(status))
                  << ",\"table\":"; table(d.table);
        if (status == Status::Ok) {
            std::cout << ",\"supported_devices_raw\":" << hexValue(d.supportedDevices)
                      << ",\"paths\":[";
            for (size_t i = 0; i < d.pathCount; ++i) {
                if (i) std::cout << ',';
                const DisplayPathInfo& p = d.paths[i];
                std::cout << "{\"display_object_id\":" << hexValue(p.displayObjectId)
                          << ",\"encoder_object_id\":" << hexValue(p.encoderObjectId)
                          << ",\"record_offset_within_table\":" << p.recordOffset
                          << ",\"device_tag\":" << hexValue(p.deviceTag) << '}';
            }
            std::cout << ']';
        }
        std::cout << '}';
    }
    std::cout << '}';
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: hackynavi-rom ROM_FILE\n"
                     "Read-only PCI/ATOM metadata inspection; emits JSON. Maximum input 16 MiB.\n"
                     "Table offsets are image-relative; image offsets refer to the input file.\n"
                     "Exit 0: PCI images found; 2: input error; 3: no valid PCI images.\n"
                     "Inspect individual ATOM/table status fields for unsupported or malformed data.\n";
        return 0;
    }
    if (argc != 2) {
        std::cerr << "Usage: hackynavi-rom ROM_FILE\n";
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if (!file) { std::cerr << "Cannot open input file.\n"; return 2; }
    const std::streamoff length = file.tellg();
    if (length <= 0 || length > 16 * 1024 * 1024) {
        std::cerr << "Input must contain between 1 byte and 16 MiB.\n"; return 2;
    }
    file.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), length)) {
        std::cerr << "Could not read the complete input.\n"; return 2;
    }
    std::cout << "{\"schema_version\":1,\"read_only\":true,\"input_bytes\":" << bytes.size()
              << ",\"images\":[";
    size_t offset = 0, count = 0;
    hackynavi::ImageInfo image = {};
    while (hackynavi::findNextImage(bytes.data(), bytes.size(), offset, image) == hackynavi::Status::Ok) {
        if (count++) std::cout << ',';
        std::cout << "{\"file_offset\":" << image.offset << ",\"bytes\":" << image.size
                  << ",\"pcir_offset\":" << image.pcirOffset
                  << ",\"vendor_id\":" << hexValue(image.vendorId)
                  << ",\"device_id\":" << hexValue(image.deviceId)
                  << ",\"code_type\":" << unsigned(image.codeType)
                  << ",\"last_in_chain\":" << (image.lastImage ? "true" : "false")
                  << ",\"legacy_checksum_valid\":";
        if (image.checksumApplicable) std::cout << (image.checksumValid ? "true" : "false");
        else std::cout << "null";
        if (image.codeType == 0) atom(bytes, image);
        std::cout << '}';
        offset = image.offset + image.size;
    }
    std::cout << "],\"notes\":["
                 "\"Module definitions do not identify the populated memory chips.\","
                 "\"Display paths do not indicate connected monitors or working scanout.\","
                 "\"Legacy checksums do not verify firmware authenticity.\","
                 "\"Reservation metadata comes from the supplied ROM, not live firmware state; absent or zero values do not establish free VRAM.\","
                 "\"Image discovery skips structurally invalid candidates; this is not a whole-dump integrity check.\","
                 "\"Duplicate images do not establish a physical dual-BIOS arrangement.\"]}\n";
    if (!count) { std::cerr << "No structurally valid PCI option-ROM images found.\n"; return 3; }
    return 0;
}
