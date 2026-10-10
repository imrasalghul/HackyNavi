// SPDX-License-Identifier: MIT
// Host-only formatting and bounded input helpers; never used in the kext.
#ifndef HACKYNAVI_INSPECT_SUPPORT_HPP
#define HACKYNAVI_INSPECT_SUPPORT_HPP
#include <stdint.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace inspect {
inline std::string quoted(const std::string& text) {
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
inline std::string hex(uint64_t value) {
    std::ostringstream out;
    out << "\"0x" << std::hex << value << '"';
    return out.str();
}
inline bool readBinary(const char* path, std::vector<uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) { std::cerr << "Cannot open input file.\n"; return false; }
    const std::streamoff length = file.tellg();
    if (length <= 0 || length > 16 * 1024 * 1024) {
        std::cerr << "Input must contain between 1 byte and 16 MiB.\n";
        return false;
    }
    file.seekg(0);
    bytes.resize(static_cast<size_t>(length));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), length)) {
        std::cerr << "Could not read the complete input.\n";
        return false;
    }
    return true;
}
} // namespace inspect
#endif
