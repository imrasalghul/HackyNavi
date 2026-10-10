// SPDX-License-Identifier: MIT
#include "hackynavi/discovery.hpp"
#include "inspect_support.hpp"

namespace {
struct IpOutput { std::ostringstream text; bool first = true; };
void writeIp(const hackynavi::DiscoveryIp& ip, void* context) {
    IpOutput& output = *static_cast<IpOutput*>(context);
    if (!output.first) output.text << ',';
    output.first = false;
    output.text << "{\"die_id\":" << ip.dieId << ",\"offset\":" << ip.offset
                << ",\"hardware_id\":" << ip.hardwareId << ",\"instance\":" << unsigned(ip.instance)
                << ",\"major\":" << unsigned(ip.major) << ",\"minor\":" << unsigned(ip.minor)
                << ",\"revision\":" << unsigned(ip.revision)
                << ",\"harvest_raw\":" << unsigned(ip.harvest)
                << ",\"sub_revision\":" << unsigned(ip.subRevision)
                << ",\"variant\":" << unsigned(ip.variant) << ",\"base_addresses_raw\":[";
    for (size_t i = 0; i < ip.baseAddressCount; ++i) {
        if (i) output.text << ',';
        output.text << inspect::hex(ip.baseAddresses[i]);
    }
    output.text << "]}";
}
void writeValidated(const hackynavi::DiscoveryInfo& info, const IpOutput& ips) {
        std::cout << ",\"binary_major\":" << info.binaryMajor << ",\"binary_minor\":" << info.binaryMinor
                  << ",\"binary_bytes\":" << info.binarySize << ",\"ip_table_version\":" << info.ipVersion
                  << ",\"ip_table_id\":" << inspect::hex(info.ipTableId)
                  << ",\"base_addresses_64_bit\":" << (info.baseAddresses64Bit ? "true" : "false")
                  << ",\"ip_count\":" << info.ipCount << ",\"tables\":[";
        for (size_t i = 0; i < info.tableCount; ++i) {
            if (i) std::cout << ',';
            std::cout << "{\"index\":" << i << ",\"offset\":" << info.tables[i].offset
                      << ",\"bytes\":" << info.tables[i].size
                      << ",\"checksum\":" << info.tables[i].checksum << '}';
        }
        std::cout << "],\"dies\":[";
        for (size_t i = 0; i < info.dieCount; ++i) {
            if (i) std::cout << ',';
            std::cout << "{\"id\":" << info.dies[i].id << ",\"offset\":" << info.dies[i].offset
                      << ",\"ip_count\":" << info.dies[i].ipCount << '}';
        }
        std::cout << "],\"ips\":[" << ips.text.str() << ']';
}
void writeNotes() {
    std::cout << ",\"notes\":[\"Base addresses retain the firmware encoding and are not CPU physical addresses.\","
                 "\"Checksums do not verify authenticity or identify the installed GPU.\","
                 "\"This tool does not acquire discovery data from hardware.\"]}\n";
}

int scan(const std::vector<uint8_t>& bytes) {
    const size_t maxCandidates = 128;
    const size_t maxParserBytes = 4 * 1024 * 1024;
    // A binary_size is uint16. Bound the parser window independently of every
    // untrusted candidate header. Charge the entire window conservatively.
    const size_t maxWindow = 65535;
    size_t candidates = 0, validated = 0, parserBytes = 0, stoppedAt = 0;
    const char* scanStatus = "complete";
    bool complete = true;
    std::cout << "{\"schema_version\":1,\"read_only\":true,\"mode\":\"scan\",\"input_bytes\":"
              << bytes.size() << ",\"candidates\":[";
    // Search every byte, including signatures nested within another candidate.
    // No declared size (valid or otherwise) controls scanner advancement.
    for (size_t offset = 0; bytes.size() >= 4 && offset <= bytes.size() - 4; ++offset) {
        if (bytes[offset] != 0x07 || bytes[offset + 1] != 0x14 ||
            bytes[offset + 2] != 0x21 || bytes[offset + 3] != 0x28) continue;
        const size_t remaining = bytes.size() - offset;
        const size_t window = remaining < maxWindow ? remaining : maxWindow;
        if (candidates == maxCandidates || window > maxParserBytes - parserBytes) {
            scanStatus = candidates == maxCandidates ? "candidate_limit_reached" : "parser_byte_limit_reached";
            complete = false;
            stoppedAt = offset;
            break;
        }
        hackynavi::DiscoveryInfo info = {};
        IpOutput ips;
        const auto status = hackynavi::parseDiscovery(bytes.data() + offset, window,
                                                     info, writeIp, &ips);
        parserBytes += window;
        if (candidates++) std::cout << ',';
        std::cout << "{\"offset\":" << offset << ",\"offset_hex\":" << inspect::hex(offset)
                  << ",\"status\":" << inspect::quoted(hackynavi::discoveryStatusString(status));
        if (status == hackynavi::DiscoveryStatus::Ok) {
            ++validated;
            writeValidated(info, ips);
        }
        std::cout << '}';
    }
    std::cout << "],\"scan_complete\":" << (complete ? "true" : "false")
              << ",\"scan_status\":" << inspect::quoted(scanStatus)
              << ",\"candidate_count\":" << candidates << ",\"validated_count\":" << validated
              << ",\"parser_input_bytes\":" << parserBytes
              << ",\"scan_limits\":{\"max_candidates\":" << maxCandidates
              << ",\"max_parser_input_bytes\":" << maxParserBytes << '}';
    if (!complete) {
        std::cout << ",\"next_candidate_offset\":" << stoppedAt;
        std::cerr << "Scan incomplete: " << scanStatus << " at offset " << stoppedAt << ".\n";
    }
    writeNotes();
    return !complete ? 4 : validated ? 0 : 3;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: hackynavi-discovery BINARY_FILE\n"
                     "       hackynavi-discovery --scan FILE\n"
                     "Read-only AMD IP discovery binary inspection; emits JSON. Maximum input 16 MiB.\n"
                     "Default input starts at the discovery signature, without a PSP prefix.\n"
                     "Scan checks every signature candidate, including invalid/nested candidates.\n"
                     "Scan limits: 128 candidates, 4 MiB cumulative parser-input windows.\n"
                     "Exit 0: validated binary (or complete scan with >=1 valid candidate);\n"
                     "2: input error; 3: rejected/no valid candidate; 4: incomplete scan (limit reached).\n";
        return 0;
    }
    const bool scanMode = argc == 3 && std::string(argv[1]) == "--scan";
    if ((!scanMode && argc != 2) || (argc == 2 && std::string(argv[1]) == "--scan")) {
        std::cerr << "Usage: hackynavi-discovery BINARY_FILE | --scan FILE\n";
        return 2;
    }
    std::vector<uint8_t> bytes;
    if (!inspect::readBinary(argv[scanMode ? 2 : 1], bytes)) return 2;
    if (scanMode) return scan(bytes);
    hackynavi::DiscoveryInfo info = {};
    IpOutput ips;
    const auto status = hackynavi::parseDiscovery(bytes.data(), bytes.size(), info, writeIp, &ips);
    std::cout << "{\"schema_version\":1,\"read_only\":true,\"input_bytes\":" << bytes.size()
              << ",\"status\":" << inspect::quoted(hackynavi::discoveryStatusString(status));
    if (status == hackynavi::DiscoveryStatus::Ok) writeValidated(info, ips);
    writeNotes();
    return status == hackynavi::DiscoveryStatus::Ok ? 0 : 3;
}
