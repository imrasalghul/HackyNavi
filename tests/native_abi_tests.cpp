// SPDX-License-Identifier: MIT
// Host protocol tests; they do not simulate GPU completion or prove isolation.
#include "hackynavi/native_abi.hpp"
#include <cstring>
#include <iostream>
#include <vector>
namespace a = hackynavi::nativeabi;
namespace {
size_t checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
a::HelloReply hello() {
    a::HelloReply r{}; r.header = a::makeHeader(sizeof(r), 23); r.capabilities = a::kRequiredCapabilities;
    r.registryId = 99; r.vramBytes = 24ULL << 30; r.cpuVisibleVramBytes = 256ULL << 20;
    r.systemBudget = 4ULL << 30; r.vaBegin = 0x10000; r.vaEnd = 1ULL << 47;
    r.maxAllocationBytes = 1ULL << 30; r.vendorId = 0x1002; r.deviceId = 0x744c;
    r.gcVersion = 0x0b0000; r.pageBytes = 4096; r.maxIbs = a::kMaxIbs;
    r.maxResources = a::kMaxResources; r.maxIbBytes = a::kMaxIbBytes; return r;
}
std::vector<uint8_t> submission(uint32_t ibs, uint32_t refs) {
    size_t bytes = sizeof(a::SubmitRequest) + ibs * sizeof(a::Ib) + refs * sizeof(a::Resource);
    std::vector<uint8_t> data(bytes);
    a::SubmitRequest request{a::makeHeader(bytes, 23), ibs, refs, 0};
    std::memcpy(data.data(), &request, sizeof(request));
    for (uint32_t n = 0; n < ibs; ++n) {
        a::Ib ib{{n + 1, 7}, uint64_t(n) * 256, 16, 0};
        std::memcpy(data.data() + sizeof(request) + n * sizeof(ib), &ib, sizeof(ib));
    }
    for (uint32_t n = 0; n < refs; ++n) {
        a::Resource resource{{n + 1, 7}, a::Read | a::Execute, 0};
        std::memcpy(data.data() + sizeof(request) + ibs * sizeof(a::Ib) + n * sizeof(resource), &resource, sizeof(resource));
    }
    return data;
}
template<typename T> T read(const std::vector<uint8_t>& b, size_t p) {
    T value{}; std::memcpy(&value, b.data() + p, sizeof(value)); return value;
}
template<typename T> void write(std::vector<uint8_t>& b, size_t p, T value) { std::memcpy(b.data() + p, &value, sizeof(value)); }
void headers() {
    const auto h = hello(); CHECK(a::validateHello(h, 99) == a::Validation::Ok);
    CHECK(a::validateHello(h, 100) == a::Validation::Epoch);
    for (uint32_t bit = 0; bit < 7; ++bit) {
        auto r = h; r.capabilities &= ~(1ULL << bit); CHECK(a::validateHello(r) == a::Validation::Unsupported);
    }
    auto r = h; r.header.epoch = 0; CHECK(a::validateHello(r) == a::Validation::Epoch);
    r = h; ++r.header.major; CHECK(a::validateHello(r) == a::Validation::Version);
    r = h; r.header.minor = a::kMaxMinor + 1; CHECK(a::validateHello(r) == a::Validation::Version);
    r = h; ++r.header.bytes; CHECK(a::validateHello(r) == a::Validation::Size);
    r = h; ++r.header.magic; CHECK(a::validateHello(r) == a::Validation::Header);
    r = h; r.header.flags = 1; CHECK(a::validateHello(r) == a::Validation::Header);
    r = h; r.reserved[1] = 1; CHECK(a::validateHello(r) == a::Validation::Reserved);
    r = h; r.gcVersion = 0x0c0001; CHECK(a::validateHello(r) == a::Validation::Unsupported);
    r = h; r.pageBytes = 0; CHECK(a::validateHello(r) == a::Validation::Bounds);
    r = h; r.pageBytes = 4095; CHECK(a::validateHello(r) == a::Validation::Bounds);
    r = h; r.vaEnd = UINT64_MAX; CHECK(a::validateHello(r) == a::Validation::Bounds);
    r = h; r.vaEnd = r.vaBegin; CHECK(a::validateHello(r) == a::Validation::Bounds);
    r = h; ++r.vaBegin; CHECK(a::validateHello(r) == a::Validation::Bounds);
    r = h; r.cpuVisibleVramBytes = r.vramBytes + 1; CHECK(a::validateHello(r) == a::Validation::Bounds);
    r = h; r.maxIbs = a::kMaxIbs + 1; CHECK(a::validateHello(r) == a::Validation::Bounds);
    r = h; r.maxResources = 0; CHECK(a::validateHello(r) == a::Validation::Bounds);
    r = h; r.maxIbBytes = a::kMaxIbBytes + 4; CHECK(a::validateHello(r) == a::Validation::Bounds);
}
void allocation() {
    const auto h = hello();
    a::AllocateRequest r{a::makeHeader(sizeof(r), 23), 4096, 4096, uint32_t(a::Domain::Vram), a::CpuVisible, 0};
    CHECK(a::validateAllocate(r, h) == a::Validation::Ok);
    const auto good = r;
    r.bytes = 0; CHECK(a::validateAllocate(r, h) == a::Validation::Bounds);
    r = good; r.bytes = UINT64_MAX; CHECK(a::validateAllocate(r, h) == a::Validation::Bounds);
    r = good; r.bytes = h.cpuVisibleVramBytes + 4096; CHECK(a::validateAllocate(r, h) == a::Validation::Bounds);
    r.flags = 0; CHECK(a::validateAllocate(r, h) == a::Validation::Ok);
    r = good; r.alignment = 0; CHECK(a::validateAllocate(r, h) == a::Validation::Bounds);
    r = good; r.alignment = 8191; CHECK(a::validateAllocate(r, h) == a::Validation::Bounds);
    r = good; r.domain = 3; CHECK(a::validateAllocate(r, h) == a::Validation::Unsupported);
    r = good; r.flags = 2; CHECK(a::validateAllocate(r, h) == a::Validation::Unsupported);
    r = good; r.reserved = 1; CHECK(a::validateAllocate(r, h) == a::Validation::Reserved);
    r = good; r.header.epoch = 22; CHECK(a::validateAllocate(r, h) == a::Validation::Epoch);
    a::VaRequest v{a::makeHeader(sizeof(v), 23), {1, 7}, 0x10000, 0, 4096, a::Read | a::Write, 0};
    CHECK(a::validateVa(v, h, 4096, false) == a::Validation::Ok);
    const auto valid = v;
    CHECK(a::validateVa(v, h, 4096, true) == a::Validation::Access);
    v.access = 0; CHECK(a::validateVa(v, h, 4096, true) == a::Validation::Ok);
    CHECK(a::validateVa(v, h, 4096, false) == a::Validation::Access);
    v = valid; v.va = h.vaEnd - 4096; CHECK(a::validateVa(v, h, 4096, false) == a::Validation::Ok);
    v.bytes = 8192; CHECK(a::validateVa(v, h, 8192, false) == a::Validation::Bounds);
    v = valid; v.va = UINT64_MAX - 4095; CHECK(a::validateVa(v, h, 4096, false) == a::Validation::Bounds);
    v = valid; v.offset = UINT64_MAX - 4095; CHECK(a::validateVa(v, h, UINT64_MAX, false) == a::Validation::Bounds);
    v = valid; v.bytes = 0; CHECK(a::validateVa(v, h, 4096, false) == a::Validation::Bounds);
    v = valid; v.handle.generation = 0; CHECK(a::validateVa(v, h, 4096, false) == a::Validation::Handle);
    v = valid; v.va = 0; CHECK(a::validateVa(v, h, 4096, false) == a::Validation::Bounds);
}
void commands() {
    const auto h = hello(); a::CapturedSubmit decoded{};
    for (uint32_t ibs = 1; ibs <= a::kMaxIbs; ++ibs) for (uint32_t refs = ibs; refs <= a::kMaxResources; ++refs) {
        auto b = submission(ibs, refs);
        CHECK(a::decodeSubmit(b.data(), b.size(), h, decoded) == a::Validation::Ok);
        CHECK(decoded.request.ibCount == ibs && decoded.request.resourceCount == refs);
        CHECK(decoded.ibs[ibs - 1].handle.id == ibs && decoded.resources[refs - 1].handle.id == refs);
        std::vector<uint8_t> unaligned(b.size() + 1); std::memcpy(unaligned.data() + 1, b.data(), b.size());
        CHECK(a::decodeSubmit(unaligned.data() + 1, b.size(), h, decoded) == a::Validation::Ok);
    }
    const auto good = submission(2, 2);
    for (size_t n = 0; n < good.size(); ++n) {
        decoded.request.ibCount = 999;
        CHECK(a::decodeSubmit(good.data(), n, h, decoded) != a::Validation::Ok);
        CHECK(decoded.request.ibCount == 0);
    }
    CHECK(a::decodeSubmit(nullptr, good.size(), h, decoded) == a::Validation::Size);
    auto b = good; b.push_back(0); CHECK(a::decodeSubmit(b.data(), b.size(), h, decoded) == a::Validation::Size);
    b = good; auto request = read<a::SubmitRequest>(b, 0); request.ibCount = UINT32_MAX; write(b, 0, request);
    CHECK(a::decodeSubmit(b.data(), b.size(), h, decoded) == a::Validation::Bounds);
    b = good; auto ib = read<a::Ib>(b, sizeof(request)); ib.offset = UINT64_MAX - 3; write(b, sizeof(request), ib);
    CHECK(a::decodeSubmit(b.data(), b.size(), h, decoded) == a::Validation::Bounds);
    b = good; ib = read<a::Ib>(b, sizeof(request)); ib.words = UINT32_MAX; write(b, sizeof(request), ib);
    CHECK(a::decodeSubmit(b.data(), b.size(), h, decoded) == a::Validation::Bounds);
    b = good; ib = read<a::Ib>(b, sizeof(request)); ib.handle.generation = 8; write(b, sizeof(request), ib);
    CHECK(a::decodeSubmit(b.data(), b.size(), h, decoded) == a::Validation::Access);
    size_t resourceOffset = sizeof(request) + 2 * sizeof(a::Ib);
    b = good; auto resource = read<a::Resource>(b, resourceOffset); resource.access = a::Read; write(b, resourceOffset, resource);
    CHECK(a::decodeSubmit(b.data(), b.size(), h, decoded) == a::Validation::Access);
    b = good; resource = read<a::Resource>(b, resourceOffset); resource.handle = {2, 8}; write(b, resourceOffset, resource);
    CHECK(a::decodeSubmit(b.data(), b.size(), h, decoded) == a::Validation::Handle);
    b = good; resource = read<a::Resource>(b, resourceOffset); resource.reserved = 1; write(b, resourceOffset, resource);
    CHECK(a::decodeSubmit(b.data(), b.size(), h, decoded) == a::Validation::Reserved);
    // Mutation/truncation coverage over arbitrary byte-aligned envelopes.
    uint32_t random = 0x5894fd17;
    for (size_t n = 0; n < 100000; ++n) {
        b = good; random = random * 1664525U + 1013904223U;
        const auto offset = random % b.size(); random = random * 1664525U + 1013904223U;
        b[offset] ^= uint8_t(random >> 24);
        const auto result = a::decodeSubmit(b.data(), b.size(), h, decoded);
        if (result == a::Validation::Ok) {
            CHECK(decoded.request.ibCount == 2 && decoded.request.resourceCount == 2);
            CHECK(decoded.request.header.epoch == 23);
            CHECK(decoded.ibs[0].words && decoded.ibs[0].words <= a::kMaxIbBytes / 4);
        } else CHECK(decoded.request.ibCount == 0);
    }
}
void fences() {
    a::WaitReply r{a::makeHeader(sizeof(r), 23), 10, 9, uint32_t(a::FenceState::Pending), 0};
    CHECK(a::validateWait(r, 23, 10) == a::Validation::Ok);
    r.state = uint32_t(a::FenceState::Complete); CHECK(a::validateWait(r, 23, 10) == a::Validation::Bounds);
    r.completedSequence = 10; CHECK(a::validateWait(r, 23, 10) == a::Validation::Ok);
    r.state = uint32_t(a::FenceState::Pending); CHECK(a::validateWait(r, 23, 10) == a::Validation::Bounds);
    r.state = uint32_t(a::FenceState::DeviceLost); CHECK(a::validateWait(r, 23, 10) == a::Validation::Ok);
    CHECK(a::validateWait(r, 22, 10) == a::Validation::Epoch);
    CHECK(a::validateWait(r, 23, 11) == a::Validation::Bounds);
    r.state = 3; CHECK(a::validateWait(r, 23, 10) == a::Validation::Bounds);
    r.state = 1; r.reserved = 1; CHECK(a::validateWait(r, 23, 10) == a::Validation::Reserved);
}
void builtIn() {
    auto h = hello(); h.capabilities = a::kBuiltInCapabilities; h.maxIbs = h.maxIbBytes = 0;
    CHECK(a::validateHello(h, 99, a::kBuiltInCapabilities) == a::Validation::Ok);
    CHECK(a::validateHello(h) == a::Validation::Unsupported); // Generic consumer cannot silently downgrade.
    auto legacy = hello(); legacy.header.minor = a::kGenericMinor;
    CHECK(a::validateHello(legacy) == a::Validation::Ok);
    for (uint64_t bit = 1; bit <= a::SealedSingleJob; bit <<= 1) if (a::kBuiltInCapabilities & bit) {
        auto bad = h; bad.capabilities &= ~bit;
        CHECK(a::validateHello(bad, 0, a::kBuiltInCapabilities) == a::Validation::Unsupported);
    }
    auto bad = h; bad.capabilities |= a::CpuMapping;
    CHECK(a::validateHello(bad, 0, a::kBuiltInCapabilities) == a::Validation::Unsupported);
    bad = h; bad.capabilities |= a::CapturedValidatedIb;
    CHECK(a::validateHello(bad, 0, a::kBuiltInCapabilities) == a::Validation::Unsupported);
    bad = h; bad.header.minor = 0;
    CHECK(a::validateHello(bad, 0, a::kBuiltInCapabilities) == a::Validation::Unsupported);
    bad = h; bad.maxIbs = 1;
    CHECK(a::validateHello(bad, 0, a::kBuiltInCapabilities) == a::Validation::Bounds);
    a::HelloRequest request{a::makeHeader(sizeof(request), 0), a::kBuiltInCapabilities, 0};
    CHECK(a::validateHelloRequest(request, a::kBuiltInCapabilities) == a::Validation::Ok);
    request.requiredCapabilities = a::kRequiredCapabilities;
    CHECK(a::validateHelloRequest(request, a::kBuiltInCapabilities) == a::Validation::Unsupported);
    request.requiredCapabilities = 1ULL << 63;
    CHECK(a::validateHelloRequest(request, UINT64_MAX) == a::Validation::Unsupported);
    request.requiredCapabilities = 0;
    CHECK(a::validateHelloRequest(request, UINT64_MAX) == a::Validation::Unsupported);
    request.requiredCapabilities = a::kBuiltInCapabilities; request.header.minor = 0;
    CHECK(a::validateHelloRequest(request, a::kBuiltInCapabilities) == a::Validation::Unsupported);
    request.header.minor = a::kMinor; request.header.epoch = 23;
    CHECK(a::validateHelloRequest(request, a::kBuiltInCapabilities) == a::Validation::Epoch);
    a::AllocateRequest allocation{a::makeHeader(sizeof(allocation), 23), 4096, 4096, uint32_t(a::Domain::System), 0, 0};
    CHECK(a::validateAllocate(allocation, h) == a::Validation::Ok);
    allocation.flags = a::CpuVisible;
    CHECK(a::validateAllocate(allocation, h) == a::Validation::Unsupported);
    allocation.flags = 0; allocation.domain = uint32_t(a::Domain::Vram);
    CHECK(a::validateAllocate(allocation, h) == a::Validation::Unsupported);

    a::BufferIoRequest io{a::makeHeader(sizeof(io) + a::kMaxCopyBytes, 23), {1, 7}, 4096, a::kMaxCopyBytes, 0};
    CHECK(a::validateBufferIo(io, io.header.bytes, 23, true) == a::Validation::Ok);
    CHECK(a::validateBufferIo(io, io.header.bytes, 23, false) == a::Validation::Size);
    io.header.bytes = sizeof(io);
    CHECK(a::validateBufferIo(io, sizeof(io), 23, false) == a::Validation::Ok);
    CHECK(a::validateBufferIo(io, sizeof(io), 22, false) == a::Validation::Epoch);
    auto goodIo = io; io.bytes++;
    CHECK(a::validateBufferIo(io, sizeof(io), 23, false) == a::Validation::Bounds);
    io = goodIo; io.bytes = 0;
    CHECK(a::validateBufferIo(io, sizeof(io), 23, false) == a::Validation::Bounds);
    io = goodIo; io.offset = UINT64_MAX - io.bytes + 1;
    CHECK(a::validateBufferIo(io, sizeof(io), 23, false) == a::Validation::Bounds);
    io = goodIo; io.handle.generation = 0;
    CHECK(a::validateBufferIo(io, sizeof(io), 23, false) == a::Validation::Handle);
    io = goodIo; io.reserved = 1;
    CHECK(a::validateBufferIo(io, sizeof(io), 23, false) == a::Validation::Reserved);

    a::ComputeRequest job{}; job.header = a::makeHeader(sizeof(job), 23);
    job.destination = {2, 7}; job.bytes = 4096; job.operation = uint32_t(a::ComputeOperation::Clear);
    job.pattern[3] = UINT32_MAX;
    CHECK(a::validateCompute(job, 23) == a::Validation::Ok);
    job.source = {1, 7};
    CHECK(a::validateCompute(job, 23) == a::Validation::Reserved);
    job.operation = uint32_t(a::ComputeOperation::Copy);
    CHECK(a::validateCompute(job, 23) == a::Validation::Reserved);
    job.pattern[3] = 0;
    CHECK(a::validateCompute(job, 23) == a::Validation::Ok);
    const auto goodJob = job;
    job.source = job.destination;
    CHECK(a::validateCompute(job, 23) == a::Validation::Bounds);
    job.sourceOffset = 4096;
    CHECK(a::validateCompute(job, 23) == a::Validation::Ok); // Exact adjacency is not overlap.
    job.sourceOffset = 4080;
    CHECK(a::validateCompute(job, 23) == a::Validation::Bounds);
    job = goodJob; job.sourceOffset = UINT64_MAX - 15;
    CHECK(a::validateCompute(job, 23) == a::Validation::Bounds);
    job = goodJob; job.destinationOffset = UINT64_MAX - 15;
    CHECK(a::validateCompute(job, 23) == a::Validation::Bounds);
    job = goodJob; job.bytes = 4095;
    CHECK(a::validateCompute(job, 23) == a::Validation::Bounds);
    job = goodJob; job.operation = 0;
    CHECK(a::validateCompute(job, 23) == a::Validation::Unsupported);
    job = goodJob; job.reserved = 1;
    CHECK(a::validateCompute(job, 23) == a::Validation::Reserved);
    job = goodJob;
    CHECK(a::validateCompute(job, 24) == a::Validation::Epoch);

    a::ReadResultRequest read{a::makeHeader(sizeof(read), 23), 1, 4096, a::kMaxCopyBytes, 0};
    CHECK(a::validateReadResult(read, 23) == a::Validation::Ok);
    auto goodRead = read; read.sequence = 0;
    CHECK(a::validateReadResult(read, 23) == a::Validation::Bounds);
    read = goodRead; read.offset = UINT64_MAX;
    CHECK(a::validateReadResult(read, 23) == a::Validation::Bounds);
    read = goodRead; read.bytes++;
    CHECK(a::validateReadResult(read, 23) == a::Validation::Bounds);
    read = goodRead; read.reserved = 1;
    CHECK(a::validateReadResult(read, 23) == a::Validation::Reserved);
}
void compilerProfile() {
    auto h = hello(); h.header.minor = a::kCompilerMinor;
    h.capabilities = a::kCompilerCapabilities; h.maxIbs = h.maxIbBytes = 0;
    CHECK(a::validateHello(h, 99, a::kCompilerCapabilities) == a::Validation::Ok);
    CHECK(a::validateHello(h, 99, a::kBuiltInCapabilities) == a::Validation::Unsupported);
    CHECK(a::validateHello(h, 99) == a::Validation::Unsupported);
    for (uint64_t bit = 1; bit <= a::CompilerFixtureCompute; bit <<= 1) if (a::kCompilerCapabilities & bit) {
        auto bad = h; bad.capabilities &= ~bit;
        CHECK(a::validateHello(bad, 99, a::kCompilerCapabilities) == a::Validation::Unsupported);
    }
    auto bad = h; bad.header.minor = a::kMinor;
    CHECK(a::validateHello(bad, 99, a::kCompilerCapabilities) == a::Validation::Unsupported);
    for (const auto cap : {a::CapturedValidatedIb, a::CpuMapping}) {
        bad = h; bad.capabilities |= cap;
        CHECK(a::validateHello(bad, 99, a::kCompilerCapabilities) == a::Validation::Unsupported);
    }
    a::HelloRequest helloRequest{a::makeHeader(sizeof(helloRequest), 0, a::kCompilerMinor), a::kCompilerCapabilities, 0};
    CHECK(a::validateHelloRequest(helloRequest, a::kCompilerCapabilities) == a::Validation::Ok);
    CHECK(a::validateHelloRequest(helloRequest, a::kBuiltInCapabilities) == a::Validation::Unsupported);
    helloRequest.requiredCapabilities = a::kBuiltInCapabilities;
    CHECK(a::validateHelloRequest(helloRequest, a::kCompilerCapabilities) == a::Validation::Unsupported);
    helloRequest.requiredCapabilities = a::kCompilerCapabilities; helloRequest.header.minor = a::kMinor;
    CHECK(a::validateHelloRequest(helloRequest, a::kCompilerCapabilities) == a::Validation::Unsupported);

    a::CompilerComputeRequest r{a::makeHeader(sizeof(r), 23, a::kCompilerMinor), {1, 7}, {2, 7},
        4096, 8192, uint32_t(a::CompilerShaderClass::AppleAddOneGfx1100V1), 0};
    const auto good = r;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Ok);
    bad = h; bad.capabilities &= ~a::CompilerFixtureCompute;
    CHECK(a::validateCompilerCompute(r, bad) == a::Validation::Unsupported);
    r.header.minor = a::kMinor;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Version);
    r = good; r.header.epoch++;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Epoch);
    r = good; r.reserved = 1;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Reserved);
    r = good; r.shaderClass = 0;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Unsupported);
    r = good; r.shaderClass++;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Unsupported);
    r = good; r.source.generation = 0;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Handle);
    r = good; r.destination.id = 0;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Handle);
    r = good; r.sourceOffset++;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Bounds);
    r = good; r.destinationOffset++;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Bounds);
    r = good; r.sourceOffset = UINT64_MAX - 3;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Bounds);
    r = good; r.destinationOffset = UINT64_MAX - 3;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Bounds);
    r = good; r.source = r.destination;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Bounds);
    r.sourceOffset = r.destinationOffset + a::kCompilerResultBytes;
    CHECK(a::validateCompilerCompute(r, h) == a::Validation::Ok); // Native capture still requires distinct BOs.

    // Every shared method must reject cross-profile headers, while accepting
    // the explicitly negotiated v1.2 connection for copied I/O and results.
    a::BufferIoRequest io{a::makeHeader(sizeof(io), 23, a::kCompilerMinor), {1, 7}, 0, 16, 0};
    CHECK(a::validateBufferIo(io, sizeof(io), 23, false, a::kCompilerMinor) == a::Validation::Ok);
    CHECK(a::validateBufferIo(io, sizeof(io), 23, false) == a::Validation::Version);
    a::ComputeRequest clear{}; clear.header = a::makeHeader(sizeof(clear), 23, a::kCompilerMinor);
    clear.destination = {2, 7}; clear.bytes = 16; clear.operation = uint32_t(a::ComputeOperation::Clear);
    CHECK(a::validateCompute(clear, 23, a::kCompilerMinor) == a::Validation::Ok);
    CHECK(a::validateCompute(clear, 23) == a::Validation::Version);
    a::ReadResultRequest read{a::makeHeader(sizeof(read), 23, a::kCompilerMinor), 1, 0, 16, 0};
    CHECK(a::validateReadResult(read, 23, a::kCompilerMinor) == a::Validation::Ok);
    CHECK(a::validateReadResult(read, 23) == a::Validation::Version);
    a::AllocateRequest alloc{a::makeHeader(sizeof(alloc), 23, a::kCompilerMinor), 20480, 4096,
        uint32_t(a::Domain::System), 0, 0};
    CHECK(a::validateAllocate(alloc, h) == a::Validation::Ok);
    alloc.header.minor = a::kMinor;
    CHECK(a::validateAllocate(alloc, h) == a::Validation::Version);
    a::VaRequest va{a::makeHeader(sizeof(va), 23, a::kCompilerMinor), {1, 7}, 0x10000, 0, 20480, a::Read, 0};
    CHECK(a::validateVa(va, h, 20480, false) == a::Validation::Ok);
    va.header.minor = a::kMinor;
    CHECK(a::validateVa(va, h, 20480, false) == a::Validation::Version);
}
a::HelloReply productHello() {
    auto h = hello(); h.header.minor = a::kProductMinor; h.capabilities = a::kProductCapabilities;
    h.maxIbs = h.maxIbBytes = 0; return h;
}
a::CapturedProductCompute productCapture(const uint32_t grid[3], const uint32_t local[3], uint32_t resources = 2) {
    a::CapturedProductCompute capture{}; auto& request = capture.request;
    std::memcpy(request.grid, grid, sizeof(request.grid));
    std::memcpy(request.nominalLocal, local, sizeof(request.nominalLocal));
    request.resourceCount = resources;
    for (unsigned mask = 0; mask < 8; ++mask) {
        a::ProductVariant variant{}; bool nonempty = true;
        for (unsigned d = 0; d < 3; ++d) {
            const uint32_t full = grid[d] / local[d], tail = grid[d] % local[d];
            if (mask & (1U << d)) { variant.local[d] = tail; if (!tail) nonempty = false; }
            else { variant.local[d] = local[d]; if (!full) nonempty = false; }
        }
        if (!nonempty) continue;
        variant.code = {50, 7}; variant.offset = request.variantCount * 256;
        variant.codeBytes = 128; variant.executableBytes = 64;
        variant.waveSize = 32; variant.numSgprs = 12; variant.numVgprs = 8;
        capture.variants[request.variantCount++] = variant;
    }
    for (uint32_t n = 0; n < resources; ++n)
        capture.resources[n] = {n, 0, {100 + n, 7}, 256, 1024};
    request.result = capture.resources[resources - 1].buffer;
    request.resultOffset = 512; request.resultBytes = 256;
    request.header = a::makeHeader(sizeof(request) + request.variantCount * sizeof(a::ProductVariant) +
        resources * sizeof(a::ProductResource), 23, a::kProductMinor);
    return capture;
}
std::vector<uint8_t> productEnvelope(const a::CapturedProductCompute& capture) {
    const auto& request = capture.request;
    const size_t bytes = sizeof(request) + request.variantCount * sizeof(a::ProductVariant) +
        request.resourceCount * sizeof(a::ProductResource);
    std::vector<uint8_t> result(bytes);
    std::memcpy(result.data(), &request, sizeof(request));
    std::memcpy(result.data() + sizeof(request), capture.variants, request.variantCount * sizeof(a::ProductVariant));
    std::memcpy(result.data() + sizeof(request) + request.variantCount * sizeof(a::ProductVariant),
        capture.resources, request.resourceCount * sizeof(a::ProductResource));
    return result;
}
void productProfile() {
    const auto h = productHello();
    CHECK(a::validateHello(h, 99, a::kProductCapabilities) == a::Validation::Ok);
    for (const auto required : {a::kRequiredCapabilities, a::kBuiltInCapabilities, a::kCompilerCapabilities})
        CHECK(a::validateHello(h, 99, required) == a::Validation::Unsupported);
    a::HelloRequest request{a::makeHeader(sizeof(request), 0, a::kProductMinor), a::kProductCapabilities, 0};
    CHECK(a::validateHelloRequest(request, a::kProductCapabilities) == a::Validation::Ok);
    for (uint64_t bit = 1; bit <= a::ProductLeafCompute; bit <<= 1) if (a::kProductCapabilities & bit) {
        auto bad = h; bad.capabilities &= ~bit;
        CHECK(a::validateHello(bad, 99, a::kProductCapabilities) == a::Validation::Unsupported);
        CHECK(a::validateHelloRequest(request, a::kProductCapabilities & ~bit) == a::Validation::Unsupported);
    }
    for (const auto cap : {a::CompilerFixtureCompute, a::CapturedValidatedIb, a::CpuMapping}) {
        auto bad = h; bad.capabilities |= cap;
        CHECK(a::validateHello(bad, 99, a::kProductCapabilities) == a::Validation::Unsupported);
    }
    for (uint16_t minor = 0; minor < a::kProductMinor; ++minor) {
        auto bad = h; bad.header.minor = minor;
        CHECK(a::validateHello(bad, 99, a::kProductCapabilities) == a::Validation::Unsupported);
        auto badRequest = request; badRequest.header.minor = minor;
        CHECK(a::validateHelloRequest(badRequest, a::kKnownCapabilities) == a::Validation::Unsupported);
    }
    for (const auto required : {a::kRequiredCapabilities, a::kBuiltInCapabilities, a::kCompilerCapabilities}) {
        auto badRequest = request; badRequest.requiredCapabilities = required;
        CHECK(a::validateHelloRequest(badRequest, a::kKnownCapabilities) == a::Validation::Unsupported);
    }
    auto badRequest = request; badRequest.requiredCapabilities |= a::CompilerFixtureCompute;
    CHECK(a::validateHelloRequest(badRequest, a::kKnownCapabilities) == a::Validation::Unsupported);
    a::BufferIoRequest io{a::makeHeader(sizeof(io), 23, a::kProductMinor), {1, 7}, 0, 16, 0};
    CHECK(a::validateBufferIo(io, sizeof(io), 23, false, a::kProductMinor) == a::Validation::Ok);
    CHECK(a::validateBufferIo(io, sizeof(io), 23, false, a::kCompilerMinor) == a::Validation::Version);
    a::ReadResultRequest read{a::makeHeader(sizeof(read), 23, a::kProductMinor), 1, 0, 16, 0};
    CHECK(a::validateReadResult(read, 23, a::kProductMinor) == a::Validation::Ok);
    CHECK(a::validateReadResult(read, 23, a::kCompilerMinor) == a::Validation::Version);
}
void products() {
    const auto h = productHello(); a::CapturedProductCompute decoded{}, zero{};
    const uint32_t local[3] = {8, 4, 2};
    // Exercise actual full/tail partition envelopes, including dimensions
    // smaller than the nominal group and all eight simultaneous tail regions.
    for (uint32_t x = 1; x <= 17; ++x) for (uint32_t y = 1; y <= 9; ++y) for (uint32_t z = 1; z <= 5; ++z) {
        const uint32_t grid[3] = {x, y, z}; const auto capture = productCapture(grid, local);
        const auto bytes = productEnvelope(capture);
        CHECK(a::validateProductCompute(capture, 23) == a::Validation::Ok);
        CHECK(a::decodeProductCompute(bytes.data(), bytes.size(), h, decoded) == a::Validation::Ok);
        CHECK(!std::memcmp(&capture, &decoded, sizeof(capture)));
        std::vector<uint8_t> unaligned(bytes.size() + 1);
        std::memcpy(unaligned.data() + 1, bytes.data(), bytes.size());
        CHECK(a::decodeProductCompute(unaligned.data() + 1, bytes.size(), h, decoded) == a::Validation::Ok);
    }
    const uint32_t grid[3] = {9, 5, 3}; const auto good = productCapture(grid, local, a::kMaxProductResources);
    CHECK(good.request.variantCount == 8 && good.request.header.bytes == 1552);
    const auto bytes = productEnvelope(good);
    auto reject = [&](const a::CapturedProductCompute& capture, a::Validation expected) {
        CHECK(a::validateProductCompute(capture, 23) == expected);
        if (capture.request.variantCount > a::kMaxProductVariants ||
            capture.request.resourceCount > a::kMaxProductResources) return;
        const auto envelope = productEnvelope(capture); decoded = good;
        CHECK(a::decodeProductCompute(envelope.data(), envelope.size(), h, decoded) != a::Validation::Ok);
        CHECK(!std::memcmp(&decoded, &zero, sizeof(zero)));
    };
    for (size_t n = 0; n < bytes.size(); ++n) {
        decoded = good;
        CHECK(a::decodeProductCompute(bytes.data(), n, h, decoded) != a::Validation::Ok);
        CHECK(!std::memcmp(&decoded, &zero, sizeof(zero)));
    }
    CHECK(a::decodeProductCompute(nullptr, bytes.size(), h, decoded) == a::Validation::Size);
    auto extra = bytes; extra.push_back(0);
    CHECK(a::decodeProductCompute(extra.data(), extra.size(), h, decoded) == a::Validation::Size);
    auto bad = good; bad.request.variantCount = UINT32_MAX; reject(bad, a::Validation::Bounds);
    auto countBytes = bytes; write(countBytes, 0, bad.request);
    CHECK(a::decodeProductCompute(countBytes.data(), countBytes.size(), h, decoded) == a::Validation::Bounds);
    bad = good; bad.request.resourceCount = UINT32_MAX; reject(bad, a::Validation::Bounds);
    bad = good; bad.request.variantCount = 0; reject(bad, a::Validation::Bounds);
    bad = good; bad.request.resourceCount = 0; reject(bad, a::Validation::Bounds);
    bad = good; bad.request.reserved = 1; reject(bad, a::Validation::Reserved);
    bad = good; bad.request.header.epoch++; reject(bad, a::Validation::Epoch);
    bad = good; bad.request.header.minor = a::kCompilerMinor; reject(bad, a::Validation::Version);
    bad = good; bad.request.header.bytes++; reject(bad, a::Validation::Size);
    bad = good; bad.request.header.flags = 1; reject(bad, a::Validation::Header);
    bad = good; bad.request.grid[0] = 0; reject(bad, a::Validation::Bounds);
    bad = good; for (auto& dimension : bad.request.grid) dimension = UINT32_MAX;
    reject(bad, a::Validation::Bounds);
    bad = good; bad.request.nominalLocal[1] = 0; reject(bad, a::Validation::Bounds);
    bad = good; bad.request.nominalLocal[2] = 1025; reject(bad, a::Validation::Bounds);
    bad = good; bad.request.nominalLocal[2] = 1024; reject(bad, a::Validation::Bounds);
    bad = good; bad.request.result.id = 999; reject(bad, a::Validation::Handle);
    bad = good; bad.request.result.generation++; reject(bad, a::Validation::Handle);
    bad = good; bad.request.resultBytes = 0; reject(bad, a::Validation::Bounds);
    bad = good; bad.request.resultOffset++; reject(bad, a::Validation::Bounds);
    bad = good; bad.request.resultOffset = UINT64_MAX - 3; reject(bad, a::Validation::Bounds);
    bad = good; bad.request.resultOffset = 252; reject(bad, a::Validation::Bounds);
    bad = good; bad.request.resultOffset = 1028; reject(bad, a::Validation::Bounds);
    bad = good; bad.request.resultOffset = 1024;
    CHECK(a::validateProductCompute(bad, 23) == a::Validation::Ok); // Exact resource end.
    for (uint32_t n = 0; n < good.request.variantCount; ++n) {
        for (unsigned reserved = 0; reserved < 2; ++reserved) {
            bad = good; bad.variants[n].reserved[reserved] = 1; reject(bad, a::Validation::Reserved);
        }
        bad = good; bad.variants[n].code.generation = 0; reject(bad, a::Validation::Handle);
        bad = good; bad.variants[n].codeBytes = 0; reject(bad, a::Validation::Bounds);
        bad = good; bad.variants[n].codeBytes = a::kMaxProductCodeBytes + 4; reject(bad, a::Validation::Bounds);
        bad = good; bad.variants[n].executableBytes = 132; reject(bad, a::Validation::Bounds);
        bad = good; bad.variants[n].executableBytes = 0; reject(bad, a::Validation::Bounds);
        bad = good; bad.variants[n].executableBytes++; reject(bad, a::Validation::Bounds);
        bad = good; bad.variants[n].offset++; reject(bad, a::Validation::Bounds);
        bad = good; bad.variants[n].offset = UINT64_MAX - 3; reject(bad, a::Validation::Bounds);
        bad = good; bad.variants[n].waveSize = 64; reject(bad, a::Validation::Bounds);
        bad = good; bad.variants[n].numSgprs = 7; reject(bad, a::Validation::Bounds);
        bad = good; bad.variants[n].numSgprs = 129; reject(bad, a::Validation::Bounds);
        bad = good; bad.variants[n].numVgprs = 257; reject(bad, a::Validation::Bounds);
        bad = good; bad.variants[n].local[0] = 3; reject(bad, a::Validation::Bounds);
        bad = good; bad.variants[n].code = good.resources[0].buffer; reject(bad, a::Validation::Handle);
    }
    bad = good; bad.variants[1].code.generation++; reject(bad, a::Validation::Handle);
    bad = good; bad.variants[1].offset = 124; reject(bad, a::Validation::Bounds);
    bad = good; bad.variants[1].offset = 128;
    CHECK(a::validateProductCompute(bad, 23) == a::Validation::Ok); // Adjacent staging bytes.
    bad = good; std::memcpy(bad.variants[1].local, bad.variants[0].local, sizeof(bad.variants[0].local));
    reject(bad, a::Validation::Bounds);
    for (uint32_t n = 0; n < good.request.resourceCount; ++n) {
        bad = good; bad.resources[n].reserved = 1; reject(bad, a::Validation::Reserved);
        bad = good; bad.resources[n].binding = 32; reject(bad, a::Validation::Bounds);
        bad = good; bad.resources[n].buffer.id = 0; reject(bad, a::Validation::Handle);
        bad = good; bad.resources[n].offset++; reject(bad, a::Validation::Bounds);
        bad = good; bad.resources[n].offset = UINT64_MAX - 3; reject(bad, a::Validation::Bounds);
        bad = good; bad.resources[n].bytes = 0; reject(bad, a::Validation::Bounds);
        bad = good; bad.resources[n].bytes = UINT64_C(1) << 32; reject(bad, a::Validation::Bounds);
        bad = good; bad.resources[n].bytes++; reject(bad, a::Validation::Bounds);
    }
    bad = good; bad.resources[1].binding = bad.resources[0].binding; reject(bad, a::Validation::Bounds);
    bad = good; bad.resources[1].buffer = bad.resources[0].buffer; reject(bad, a::Validation::Handle);
    bad = good; bad.resources[1].buffer = bad.resources[0].buffer; bad.resources[1].buffer.generation++;
    reject(bad, a::Validation::Handle);
    for (uint16_t minor = 0; minor < a::kProductMinor; ++minor) {
        auto old = h; old.header.minor = minor;
        CHECK(a::decodeProductCompute(bytes.data(), bytes.size(), old, decoded) == a::Validation::Unsupported);
    }
    for (uint64_t bit = 1; bit <= a::ProductLeafCompute; bit <<= 1) if (a::kProductCapabilities & bit) {
        auto old = h; old.capabilities &= ~bit;
        CHECK(a::decodeProductCompute(bytes.data(), bytes.size(), old, decoded) == a::Validation::Unsupported);
    }
    auto small = h; small.maxResources = 31;
    CHECK(a::decodeProductCompute(bytes.data(), bytes.size(), small, decoded) == a::Validation::Bounds);
    const uint32_t hugeGrid[3] = {UINT32_MAX, 1, 1}, scalarLocal[3] = {1, 1, 1};
    auto maximum = productCapture(hugeGrid, scalarLocal);
    maximum.variants[0].codeBytes = a::kMaxProductCodeBytes;
    maximum.variants[0].executableBytes = a::kMaxProductCodeBytes;
    maximum.variants[0].numSgprs = 128; maximum.variants[0].numVgprs = 256;
    maximum.resources[0].bytes = UINT32_MAX - 3;
    CHECK(a::validateProductCompute(maximum, 23) == a::Validation::Ok);
    auto minimum = maximum; minimum.variants[0].codeBytes = minimum.variants[0].executableBytes = 4;
    minimum.variants[0].numSgprs = 8; minimum.variants[0].numVgprs = 1;
    CHECK(a::validateProductCompute(minimum, 23) == a::Validation::Ok);
    minimum.variants[0].numVgprs = 0; reject(minimum, a::Validation::Bounds);
    // Each possible one-bit envelope corruption is either rejected with fully
    // cleared output or returns a structurally valid captured request. This
    // says nothing about ISA admission: no code bytes exist in these envelopes.
    for (size_t n = 0; n < bytes.size(); ++n) for (unsigned bit = 0; bit < 8; ++bit) {
        auto changed = bytes; changed[n] ^= uint8_t(1U << bit); decoded = good;
        const auto status = a::decodeProductCompute(changed.data(), changed.size(), h, decoded);
        if (status == a::Validation::Ok)
            CHECK(a::validateProductCompute(decoded, 23) == a::Validation::Ok);
        else CHECK(!std::memcmp(&decoded, &zero, sizeof(zero)));
    }
}
void reusableProfile() {
    static_assert(a::kReusableMinor == 4 && a::kMaxRetainedResults == 8, "fixed reusable contract");
    static_assert(uint32_t(a::Selector::ProductCompute) == 13 && uint32_t(a::Selector::ReleaseResult) == 14,
                  "old selectors stay fixed");
    auto h = hello(); h.header.minor = a::kReusableMinor; h.capabilities = a::kReusableCapabilities;
    h.maxIbs = h.maxIbBytes = 0;
    const uint64_t available = a::kCompilerCapabilities | a::kProductCapabilities | a::kReusableCapabilities;
    a::HelloRequest request{a::makeHeader(sizeof(request), 0, a::kReusableMinor), a::kReusableCapabilities, 0};
    CHECK(a::validateHelloRequest(request, available) == a::Validation::Ok);
    CHECK(a::validateHello(h, 99, a::kReusableCapabilities) == a::Validation::Ok);
    for (uint16_t minor = 0; minor <= a::kMaxMinor + 1; ++minor) {
        auto changed = request; changed.header.minor = minor;
        CHECK((a::validateHelloRequest(changed, available) == a::Validation::Ok) == (minor == a::kReusableMinor));
        auto reply = h; reply.header.minor = minor;
        CHECK((a::validateHello(reply, 99, a::kReusableCapabilities) == a::Validation::Ok) == (minor == a::kReusableMinor));
    }
    for (unsigned n = 0; n < 64; ++n) {
        const uint64_t bit = UINT64_C(1) << n;
        if (a::kReusableCapabilities & bit) {
            auto reply = h; reply.capabilities &= ~bit;
            CHECK(a::validateHello(reply, 99, a::kReusableCapabilities) == a::Validation::Unsupported);
            CHECK(a::validateHelloRequest(request, available & ~bit) == a::Validation::Unsupported);
        }
        auto changed = request; changed.requiredCapabilities |= bit;
        auto reply = h; reply.capabilities |= bit;
        if ((bit & (a::SealedSingleJob | a::CompilerFixtureCompute)) || !(bit & a::kKnownCapabilities))
            CHECK(a::validateHelloRequest(changed, available) == a::Validation::Unsupported);
        if (bit & (a::SealedSingleJob | a::CompilerFixtureCompute | a::CpuMapping | a::CapturedValidatedIb))
            CHECK(a::validateHello(reply, 99, a::kReusableCapabilities) == a::Validation::Unsupported);
    }
    // Old profiles cannot acquire the new capability implicitly; required
    // features must also exist in the returned profile, not just the union.
    for (uint16_t minor = 0; minor < a::kReusableMinor; ++minor) {
        auto old = h; old.header.minor = minor;
        old.capabilities = minor == 3 ? a::kProductCapabilities : minor == 2 ? a::kCompilerCapabilities : a::kBuiltInCapabilities;
        CHECK(a::validateHello(old, 99, a::kReusableCapabilities) == a::Validation::Unsupported);
        old.capabilities |= a::ReusableCompute;
        CHECK(a::validateHello(old, 99, minor == 3 ? a::kProductCapabilities : minor == 2 ? a::kCompilerCapabilities : a::kBuiltInCapabilities)
              == a::Validation::Unsupported);
    }
    auto changed = request; changed.requiredCapabilities |= a::CpuMapping;
    CHECK(a::validateHelloRequest(changed, available) == a::Validation::Unsupported);
    CHECK(a::validateHello(h, 99, a::kReusableCapabilities | a::CpuMapping) == a::Validation::Unsupported);
    a::AllocateRequest allocation{a::makeHeader(sizeof(allocation), 23, a::kReusableMinor),4096,4096,uint32_t(a::Domain::System),0,0};
    CHECK(a::validateAllocate(allocation, h) == a::Validation::Ok);
    allocation.domain = uint32_t(a::Domain::Vram);
    CHECK(a::validateAllocate(allocation, h) == a::Validation::Unsupported);
    allocation.domain = uint32_t(a::Domain::System); allocation.flags = a::CpuVisible;
    CHECK(a::validateAllocate(allocation, h) == a::Validation::Unsupported);
    a::ReleaseResultRequest release{a::makeHeader(sizeof(release),23,a::kReusableMinor),7,0};
    CHECK(a::validateReleaseResult(release,h) == a::Validation::Ok);
    release.sequence = UINT64_MAX; CHECK(a::validateReleaseResult(release,h) == a::Validation::Ok);
    // Actual sequence membership is checked by the native session, not syntax.
    release.sequence = 0; CHECK(a::validateReleaseResult(release,h) == a::Validation::Bounds);
    release.sequence = 7;
    const auto goodRelease = release;
    release.reserved = 1; CHECK(a::validateReleaseResult(release,h) == a::Validation::Reserved);
    release = goodRelease; release.header.epoch++; CHECK(a::validateReleaseResult(release,h) == a::Validation::Epoch);
    release = goodRelease; release.header.bytes++; CHECK(a::validateReleaseResult(release,h) == a::Validation::Size);
    release = goodRelease; release.header.flags++; CHECK(a::validateReleaseResult(release,h) == a::Validation::Header);
    for (uint16_t minor = 0; minor < a::kReusableMinor; ++minor) {
        auto old = h; old.header.minor = minor;
        CHECK(a::validateReleaseResult(goodRelease,old) == a::Validation::Unsupported);
        release = goodRelease; release.header.minor = minor;
        CHECK(a::validateReleaseResult(release,h) == a::Validation::Version);
    }
    const uint32_t grid[3] = {4099,1,1}, local[3] = {64,1,1};
    auto captured = productCapture(grid, local); captured.request.header.minor = a::kReusableMinor;
    auto bytes = productEnvelope(captured);
    a::CapturedProductCompute decoded{}, zero{};
    CHECK(a::validateProductCompute(captured,23,a::kReusableMinor) == a::Validation::Ok);
    CHECK(a::validateProductCompute(captured,23) == a::Validation::Version);
    CHECK(a::validateProductCompute(captured,23,a::kCompilerMinor) == a::Validation::Unsupported);
    CHECK(a::decodeProductCompute(bytes.data(),bytes.size(),h,decoded) == a::Validation::Ok);
    for (uint16_t minor = 0; minor < a::kReusableMinor; ++minor) {
        auto old = h; old.header.minor = minor;
        CHECK(a::decodeProductCompute(bytes.data(),bytes.size(),old,decoded) != a::Validation::Ok);
        CHECK(!std::memcmp(&decoded,&zero,sizeof(zero)));
    }
    for (uint64_t bit = 1; bit <= a::ReusableCompute; bit <<= 1) {
        auto bad = h;
        if (a::kReusableCapabilities & bit) bad.capabilities &= ~bit;
        else if (bit & (a::CpuMapping | a::CapturedValidatedIb | a::CompilerFixtureCompute | a::SealedSingleJob)) bad.capabilities |= bit;
        else continue;
        CHECK(a::decodeProductCompute(bytes.data(),bytes.size(),bad,decoded) == a::Validation::Unsupported);
        CHECK(!std::memcmp(&decoded,&zero,sizeof(zero)));
    }
    for (size_t n = 0; n < bytes.size(); ++n) {
        CHECK(a::decodeProductCompute(bytes.data(),n,h,decoded) != a::Validation::Ok);
        CHECK(!std::memcmp(&decoded,&zero,sizeof(zero)));
    }
    for (size_t n = 0; n < bytes.size(); ++n) for (unsigned bit = 0; bit < 8; ++bit) {
        auto changedBytes = bytes; changedBytes[n] ^= uint8_t(1U << bit);
        const auto status = a::decodeProductCompute(changedBytes.data(),changedBytes.size(),h,decoded);
        if (status == a::Validation::Ok) CHECK(a::validateProductCompute(decoded,23,a::kReusableMinor) == a::Validation::Ok);
        else CHECK(!std::memcmp(&decoded,&zero,sizeof(zero)));
    }
}
void graphicsProfile() {
    static_assert(a::kGraphicsMinor == 5 && uint32_t(a::Selector::GraphicsInformation) == 15 &&
                  uint32_t(a::Selector::ProductGraphics) == 16, "graphics ABI numbering");
    auto h = hello(); h.header.minor = a::kGraphicsMinor; h.capabilities = a::kGraphicsCapabilities;
    h.maxIbs = h.maxIbBytes = 0;
    const uint64_t available = a::kCompilerCapabilities | a::kProductCapabilities | a::kGraphicsCapabilities;
    const a::HelloRequest request{a::makeHeader(sizeof(request),0,a::kGraphicsMinor),a::kGraphicsCapabilities,0};
    CHECK(a::validateHelloRequest(request,available) == a::Validation::Ok);
    CHECK(a::validateHello(h,99,a::kGraphicsCapabilities) == a::Validation::Ok);
    for (uint16_t minor = 0; minor <= a::kMaxMinor + 1; ++minor) {
        auto changed = request; changed.header.minor = minor;
        CHECK((a::validateHelloRequest(changed,available) == a::Validation::Ok) == (minor == a::kGraphicsMinor));
        auto reply = h; reply.header.minor = minor;
        CHECK((a::validateHello(reply,99,a::kGraphicsCapabilities) == a::Validation::Ok) == (minor == a::kGraphicsMinor));
    }
    for (uint64_t bit = 1; bit; bit <<= 1) {
        auto reply = h;
        if (bit & a::kGraphicsCapabilities) {
            reply.capabilities &= ~bit;
            CHECK(a::validateHello(reply,99,a::kGraphicsCapabilities) == a::Validation::Unsupported);
            CHECK(a::validateHelloRequest(request,available & ~bit) == a::Validation::Unsupported);
        }
        if (bit & (a::SealedSingleJob | a::CompilerFixtureCompute | a::CapturedValidatedIb | a::CpuMapping)) {
            reply = h; reply.capabilities |= bit;
            CHECK(a::validateHello(reply,99,a::kGraphicsCapabilities) == a::Validation::Unsupported);
        }
    }
    // New graphics authority cannot silently appear in a negotiated v1.4 reply.
    auto legacy = h; legacy.header.minor = a::kReusableMinor;
    CHECK(a::validateHello(legacy,99,a::kReusableCapabilities) == a::Validation::Unsupported);
    CHECK(a::validateHello(h,99,a::kReusableCapabilities) == a::Validation::Unsupported);
    a::GraphicsInformationReply info{a::makeHeader(sizeof(info),23,a::kGraphicsMinor),{0x345,0x0e000000},0};
    CHECK(a::validateGraphicsInformation(info,h) == a::Validation::Ok);
    info.reserved = 1; CHECK(a::validateGraphicsInformation(info,h) == a::Validation::Reserved);
    info.reserved = 0; info.header.epoch++; CHECK(a::validateGraphicsInformation(info,h) == a::Validation::Epoch);
    info.header.epoch--; CHECK(a::validateGraphicsInformation(info,legacy) == a::Validation::Unsupported);
    a::ProductGraphicsRequest draw{};
    draw.header = a::makeHeader(sizeof(draw),23,a::kGraphicsMinor);
    draw.code = {1,7}; draw.target = {2,3}; draw.vertexBytes = 144; draw.fragmentBytes = 56;
    draw.targetLayout = {128,64,128,512,32768,32768,256,0,0x2800a,0,0,
        (127u << 14) | 63u,0x01000000,0x428};
    a::ProductGraphicsRequest decoded{}, zero{};
    CHECK(a::validateProductGraphics(draw,23) == a::Validation::Ok);
    CHECK(a::decodeProductGraphics(&draw,sizeof(draw),h,decoded) == a::Validation::Ok);
    CHECK(!std::memcmp(&draw,&decoded,sizeof(draw)));
    const auto reject = [&](const a::ProductGraphicsRequest& bad,a::Validation expected) {
        CHECK(a::validateProductGraphics(bad,23) == expected);
        decoded = draw;
        CHECK(a::decodeProductGraphics(&bad,sizeof(bad),h,decoded) == expected);
        CHECK(!std::memcmp(&decoded,&zero,sizeof(zero)));
    };
    auto bad = draw; bad.reserved = 1; reject(bad,a::Validation::Reserved);
    bad = draw; bad.code.id = 0; reject(bad,a::Validation::Handle);
    bad = draw; bad.target.generation = 0; reject(bad,a::Validation::Handle);
    bad = draw; bad.target.id = bad.code.id; reject(bad,a::Validation::Handle);
    for (uint32_t bytes : {0u,20u,23u,1025u,UINT32_MAX}) {
        bad = draw; bad.vertexBytes = bytes; reject(bad,a::Validation::Bounds);
        bad = draw; bad.fragmentBytes = bytes; reject(bad,a::Validation::Bounds);
    }
    for (uint64_t offset : {uint64_t(1),UINT64_MAX,uint64_t(UINT64_MAX-4091)}) {
        bad = draw; bad.codeOffset = offset; reject(bad,a::Validation::Bounds);
        bad = draw; bad.targetOffset = offset; reject(bad,a::Validation::Bounds);
    }
    bad = draw; bad.codeOffset = UINT64_MAX-4099; CHECK(a::validateProductGraphics(bad,23) == a::Validation::Ok);
    bad = draw; bad.targetOffset = UINT64_MAX-draw.targetLayout.total_bytes-4095;
    CHECK(a::validateProductGraphics(bad,23) == a::Validation::Ok);
    bad.targetOffset += 4096; reject(bad,a::Validation::Bounds);
    for (uint32_t dimension : {0u,16385u,UINT32_MAX}) {
        bad = draw; bad.targetLayout.width = dimension; reject(bad,a::Validation::Bounds);
        bad = draw; bad.targetLayout.height = dimension; reject(bad,a::Validation::Bounds);
    }
    bad = draw; bad.targetLayout.pitch_pixels = 64; reject(bad,a::Validation::Bounds);
    bad = draw; bad.targetLayout.pitch_pixels++; reject(bad,a::Validation::Bounds);
    bad = draw; bad.targetLayout.pitch_bytes++; reject(bad,a::Validation::Bounds);
    bad = draw; bad.targetLayout.slice_bytes++; reject(bad,a::Validation::Bounds);
    bad = draw; bad.targetLayout.total_bytes++; reject(bad,a::Validation::Bounds);
    bad = draw; bad.targetLayout.base_alignment = 4096; reject(bad,a::Validation::Bounds);
    bad = draw; bad.targetLayout.height = 32768; bad.targetLayout.slice_bytes = bad.targetLayout.total_bytes = 16ULL<<20;
    reject(bad,a::Validation::Bounds);
    bad = draw; bad.targetLayout.width = bad.targetLayout.pitch_pixels = 16384;
    bad.targetLayout.pitch_bytes = 65536; bad.targetLayout.height = 256;
    bad.targetLayout.slice_bytes = bad.targetLayout.total_bytes = 16ULL<<20;
    CHECK(a::validateProductGraphics(bad,23) == a::Validation::Ok); // Native layout fields are checked later.
    bad.targetLayout.height++; bad.targetLayout.slice_bytes += 65536; bad.targetLayout.total_bytes += 65536;
    reject(bad,a::Validation::Bounds);
    for (size_t bytes = 0; bytes < sizeof(draw); ++bytes) {
        decoded = draw; CHECK(a::decodeProductGraphics(&draw,bytes,h,decoded) == a::Validation::Size);
        CHECK(!std::memcmp(&decoded,&zero,sizeof(zero)));
    }
    CHECK(a::decodeProductGraphics(nullptr,sizeof(draw),h,decoded) == a::Validation::Size);
    CHECK(a::decodeProductGraphics(&draw,sizeof(draw)+1,h,decoded) == a::Validation::Size);
    for (uint16_t minor = 0; minor < a::kGraphicsMinor; ++minor) {
        auto old = h; old.header.minor = minor;
        CHECK(a::decodeProductGraphics(&draw,sizeof(draw),old,decoded) == a::Validation::Unsupported);
        CHECK(a::validateProductGraphics(draw,23,minor) == a::Validation::Unsupported);
    }
    std::vector<uint8_t> unaligned(sizeof(draw)+1); std::memcpy(unaligned.data()+1,&draw,sizeof(draw));
    CHECK(a::decodeProductGraphics(unaligned.data()+1,sizeof(draw),h,decoded) == a::Validation::Ok);
    for (size_t n = 1; n < unaligned.size(); ++n) for (unsigned bit = 0; bit < 8; ++bit) {
        auto bytes = unaligned; bytes[n] ^= uint8_t(1u << bit);
        const auto status = a::decodeProductGraphics(bytes.data()+1,sizeof(draw),h,decoded);
        if (status == a::Validation::Ok) CHECK(a::validateProductGraphics(decoded,23) == a::Validation::Ok);
        else CHECK(!std::memcmp(&decoded,&zero,sizeof(zero)));
    }
    // Compute and retained result release remain real v1.5 operations.
    const uint32_t grid[3] = {4099,1,1}, local[3] = {64,1,1};
    auto compute = productCapture(grid,local); compute.request.header.minor = a::kGraphicsMinor;
    const auto bytes = productEnvelope(compute); a::CapturedProductCompute captured{};
    CHECK(a::decodeProductCompute(bytes.data(),bytes.size(),h,captured) == a::Validation::Ok);
    a::ReleaseResultRequest release{a::makeHeader(sizeof(release),23,a::kGraphicsMinor),7,0};
    CHECK(a::validateReleaseResult(release,h) == a::Validation::Ok);
}

void bufferedGraphicsProfile() {
    static_assert(a::kBufferedGraphicsMinor==6&&uint32_t(a::Selector::ProductBufferedGraphics)==17,
                  "additive buffer graphics numbering");
    auto h=hello();h.header.minor=a::kBufferedGraphicsMinor;h.capabilities=a::kBufferedGraphicsCapabilities;
    h.maxIbs=h.maxIbBytes=0;
    const a::HelloRequest greeting{a::makeHeader(sizeof(greeting),0,a::kBufferedGraphicsMinor),a::kBufferedGraphicsCapabilities,0};
    CHECK(a::validateHelloRequest(greeting,a::kBufferedGraphicsCapabilities)==a::Validation::Ok);
    CHECK(a::validateHello(h,99,a::kBufferedGraphicsCapabilities)==a::Validation::Ok);
    CHECK(a::validateHello(h,99,a::kGraphicsCapabilities)==a::Validation::Unsupported);
    for(uint16_t minor=0;minor<=a::kMaxMinor+1;++minor)if(minor!=a::kBufferedGraphicsMinor) {
        auto request=greeting;request.header.minor=minor;
        CHECK(a::validateHelloRequest(request,a::kBufferedGraphicsCapabilities)==a::Validation::Unsupported || minor>a::kMaxMinor);
        auto reply=h;reply.header.minor=minor;
        CHECK(a::validateHello(reply,99,a::kBufferedGraphicsCapabilities)!=a::Validation::Ok);
    }
    for(uint64_t bit=1;bit<=a::PositionFragmentBufferGraphics;bit<<=1) {
        auto reply=h;
        if(bit&a::kBufferedGraphicsCapabilities)reply.capabilities&=~bit;
        else if(bit&(a::SealedSingleJob|a::CompilerFixtureCompute|a::CapturedValidatedIb|a::CpuMapping))reply.capabilities|=bit;
        else continue;
        CHECK(a::validateHello(reply,99,a::kBufferedGraphicsCapabilities)==a::Validation::Unsupported);
    }
    a::ProductBufferedGraphicsRequest draw{};
    draw.draw.header=a::makeHeader(sizeof(draw),23,a::kBufferedGraphicsMinor);
    draw.draw.code={1,7};draw.draw.target={2,3};draw.draw.vertexBytes=144;draw.draw.fragmentBytes=72;
    draw.draw.targetLayout={128,64,128,512,32768,32768,256,0,0x2800a,0,0,(127u<<14)|63u,0x01000000,0x428};
    draw.fragment={3,11};draw.fragmentOffset=4080;draw.fragmentBytes=16;
    a::ProductBufferedGraphicsRequest decoded{},zero{};
    CHECK(a::validateProductBufferedGraphics(draw,23)==a::Validation::Ok);
    CHECK(a::decodeProductBufferedGraphics(&draw,sizeof(draw),h,decoded)==a::Validation::Ok);
    CHECK(!std::memcmp(&draw,&decoded,sizeof(draw)));
    const auto reject=[&](const a::ProductBufferedGraphicsRequest& bad,a::Validation expected) {
        CHECK(a::validateProductBufferedGraphics(bad,23)==expected);
        decoded=draw;CHECK(a::decodeProductBufferedGraphics(&bad,sizeof(bad),h,decoded)==expected);
        CHECK(!std::memcmp(&decoded,&zero,sizeof(zero)));
    };
    for(uint64_t size:{uint64_t(0),uint64_t(4),uint64_t(15),uint64_t(17),uint64_t(4096),UINT64_MAX}) {
        auto bad=draw;bad.fragmentBytes=size;reject(bad,a::Validation::Bounds);
    }
    for(uint64_t offset:{uint64_t(1),uint64_t(4081),UINT64_MAX,uint64_t(UINT64_MAX-15)}) {
        auto bad=draw;bad.fragmentOffset=offset;reject(bad,a::Validation::Bounds);
    }
    for(uint32_t id:{0u,1u,2u}){auto bad=draw;bad.fragment.id=id;reject(bad,a::Validation::Handle);}
    auto bad=draw;bad.fragment.generation=0;reject(bad,a::Validation::Handle);
    bad=draw;bad.draw.reserved=1;reject(bad,a::Validation::Reserved);
    bad=draw;bad.draw.header.epoch++;reject(bad,a::Validation::Epoch);
    bad=draw;bad.draw.header.bytes=sizeof(draw.draw);reject(bad,a::Validation::Size);
    for(size_t size:{size_t(0),sizeof(draw.draw),sizeof(draw)-1,sizeof(draw)+1}) {
        CHECK(a::decodeProductBufferedGraphics(&draw,size,h,decoded)==a::Validation::Size);
        CHECK(!std::memcmp(&decoded,&zero,sizeof(zero)));
    }
    auto old=h;old.header.minor=a::kGraphicsMinor;old.capabilities=a::kGraphicsCapabilities;
    CHECK(a::decodeProductBufferedGraphics(&draw,sizeof(draw),old,decoded)==a::Validation::Unsupported);
    CHECK(a::validateProductBufferedGraphics(draw,23,a::kGraphicsMinor)==a::Validation::Unsupported);
    std::vector<uint8_t> bytes(sizeof(draw)+1);std::memcpy(bytes.data()+1,&draw,sizeof(draw));
    CHECK(a::decodeProductBufferedGraphics(bytes.data()+1,sizeof(draw),h,decoded)==a::Validation::Ok);
    // Existing graphics/compute and actual result release remain usable in6.
    auto constant=draw.draw;constant.header.bytes=sizeof(constant);a::ProductGraphicsRequest constantOut{};
    CHECK(a::decodeProductGraphics(&constant,sizeof(constant),h,constantOut)==a::Validation::Ok);
    a::GraphicsInformationReply info{a::makeHeader(sizeof(info),23,a::kBufferedGraphicsMinor),{0x545,0},0};
    CHECK(a::validateGraphicsInformation(info,h)==a::Validation::Ok);
    const uint32_t grid[3]={4099,1,1},local[3]={64,1,1};auto compute=productCapture(grid,local);
    compute.request.header.minor=a::kBufferedGraphicsMinor;const auto envelope=productEnvelope(compute);
    a::CapturedProductCompute captured{};CHECK(a::decodeProductCompute(envelope.data(),envelope.size(),h,captured)==a::Validation::Ok);
    a::ReleaseResultRequest release{a::makeHeader(sizeof(release),23,a::kBufferedGraphicsMinor),7,0};
    CHECK(a::validateReleaseResult(release,h)==a::Validation::Ok);
}
}
int main() {
    headers(); allocation(); commands(); fences(); builtIn(); compilerProfile(); productProfile(); products(); reusableProfile(); graphicsProfile();bufferedGraphicsProfile();
    std::cout << "native-abi: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
