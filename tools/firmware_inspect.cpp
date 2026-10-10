// SPDX-License-Identifier: MIT
#include "hackynavi/firmware.hpp"
#include "inspect_support.hpp"

namespace {
void header(const hackynavi::FirmwareHeader& h) {
    std::cout << "{\"file_bytes\":" << h.sizeBytes << ",\"header_bytes\":" << h.headerSizeBytes
              << ",\"header_major\":" << h.headerMajor << ",\"header_minor\":" << h.headerMinor
              << ",\"ip_major\":" << h.ipMajor << ",\"ip_minor\":" << h.ipMinor
              << ",\"ucode_version\":" << h.ucodeVersion << ",\"payload_offset\":" << h.payloadOffset
              << ",\"payload_bytes\":" << h.payloadSize << ",\"stored_crc32\":" << inspect::hex(h.storedCrc32)
              << ",\"crc_verified\":false}";
}
void range(const hackynavi::FirmwareRange& r) {
    std::cout << "{\"file_offset\":" << r.offset << ",\"bytes\":" << r.size << '}';
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: hackynavi-firmware common|gfx|sdma|psp BINARY_FILE\n"
                     "Read-only AMD firmware metadata inspection; emits JSON. Maximum input 16 MiB.\n"
                     "Choose the known file family; the common header has no identifying signature.\n"
                     "Typed decoders support version 2.0. CRC/authenticity are not verified.\n"
                     "Exit 0: supported structure; 2: input error; 3: parse rejected or unsupported.\n";
        return 0;
    }
    if (argc != 3) { std::cerr << "Usage: hackynavi-firmware common|gfx|sdma|psp BINARY_FILE\n"; return 2; }
    const std::string kind(argv[1]);
    if (kind != "common" && kind != "gfx" && kind != "sdma" && kind != "psp") {
        std::cerr << "Unknown firmware family.\n"; return 2;
    }
    std::vector<uint8_t> bytes;
    if (!inspect::readBinary(argv[2], bytes)) return 2;
    hackynavi::FirmwareHeader common = {};
    hackynavi::GfxFirmwareV2 gfx = {};
    hackynavi::SdmaFirmwareV2 sdma = {};
    hackynavi::PspFirmwareV2 psp = {};
    const auto commonStatus = hackynavi::parseFirmwareHeader(bytes.data(), bytes.size(), common);
    auto status = commonStatus;
    if (status == hackynavi::Status::Ok) {
        if (kind == "gfx") status = hackynavi::parseGfxFirmwareV2(bytes.data(), bytes.size(), gfx);
        else if (kind == "sdma") status = hackynavi::parseSdmaFirmwareV2(bytes.data(), bytes.size(), sdma);
        else if (kind == "psp") status = hackynavi::parsePspFirmwareV2(bytes.data(), bytes.size(), psp);
    }
    std::cout << "{\"schema_version\":1,\"read_only\":true,\"input_bytes\":" << bytes.size()
              << ",\"requested_family\":" << inspect::quoted(kind)
              << ",\"status\":" << inspect::quoted(hackynavi::statusString(status));
    if (commonStatus == hackynavi::Status::Ok) { std::cout << ",\"common_header\":"; header(common); }
    if (status == hackynavi::Status::Ok && kind == "gfx") {
        std::cout << ",\"feature_version\":" << gfx.featureVersion
                  << ",\"start_address_raw\":" << inspect::hex(gfx.startAddress) << ",\"instruction\":";
        range(gfx.instruction); std::cout << ",\"data\":"; range(gfx.data);
    } else if (status == hackynavi::Status::Ok && kind == "sdma") {
        std::cout << ",\"feature_version\":" << sdma.featureVersion << ",\"context\":";
        range(sdma.context); std::cout << ",\"control\":"; range(sdma.control);
        std::cout << ",\"context_jump_table\":"; range(sdma.contextJumpTable);
        std::cout << ",\"control_jump_table\":"; range(sdma.controlJumpTable);
    } else if (status == hackynavi::Status::Ok && kind == "psp") {
        std::cout << ",\"descriptors\":[";
        for (size_t i = 0; i < psp.descriptorCount; ++i) {
            if (i) std::cout << ',';
            const auto& d = psp.descriptors[i];
            std::cout << "{\"type_raw\":" << d.type << ",\"version\":" << d.version
                      << ",\"payload_relative_offset\":" << d.payloadRelativeOffset << ",\"image\":";
            range(d.image); std::cout << '}';
        }
        std::cout << ']';
    }
    std::cout << ",\"notes\":[\"Family selection is supplied by the caller, not detected or authenticated.\","
                 "\"Structural validity does not establish compatibility with the installed GPU.\","
                 "\"No firmware loading, PSP authentication or GPU execution is performed.\"]}\n";
    return status == hackynavi::Status::Ok ? 0 : 3;
}
