// SPDX-License-Identifier: MIT
// Negotiates the real transport only; never allocates or submits commands.
#include "iokit_client.hpp"
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
int main(int argc, char** argv) {
    uint64_t registryId = 0;
    const bool builtIn = argc > 1 && !std::strcmp(argv[1], "--built-in");
    const bool compiler = argc > 1 && !std::strcmp(argv[1], "--compiler");
    const int registryArgument = (builtIn || compiler) ? 2 : 1;
    if (argc > registryArgument + 1) { std::fprintf(stderr, "usage: %s [--built-in | --compiler] [registry-entry-id]\n", argv[0]); return 2; }
    if (argc == registryArgument + 1) {
        char* end = nullptr; errno = 0;
        if (argv[registryArgument][0] == '-') return 2;
        registryId = std::strtoull(argv[registryArgument], &end, 0);
        if (errno || !registryId || end == argv[registryArgument] || *end) return 2;
    }
    hackynavi::transport::Client client;
    auto result = compiler ? client.openCompiler(registryId) : builtIn ? client.openBuiltIn(registryId) : client.open(registryId);
    if (!result) {
        std::fprintf(stderr, "HackyNavi GPU transport unavailable: status=%u IOReturn=0x%08x\n",
                     unsigned(result.status), unsigned(result.iokit));
        return 1;
    }
    hackynavi::nativeabi::HelloReply info{};
    result = client.info(info); if (!result) return 1;
    std::printf("HackyNavi ABI %u.%u registry=%llu epoch=%llu gfx=%06x capabilities=%llx\n",
                info.header.major, info.header.minor, static_cast<unsigned long long>(info.registryId),
                static_cast<unsigned long long>(info.header.epoch), info.gcVersion,
                static_cast<unsigned long long>(info.capabilities));
    std::printf("VRAM=%llu CPU-visible=%llu system-budget=%llu; negotiation only, no GPU commands submitted\n",
                static_cast<unsigned long long>(info.vramBytes), static_cast<unsigned long long>(info.cpuVisibleVramBytes),
                static_cast<unsigned long long>(info.systemBudget));
    result = client.close();
    if (!result) { std::fprintf(stderr, "native close failed: status=%u IOReturn=0x%08x\n",
        unsigned(result.status), unsigned(result.iokit)); return 1; }
    return 0;
}
