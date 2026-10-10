// SPDX-License-Identifier: MIT
// Synthetic MMIO transport tests. No hardware access or hardware-success claim.
#include "hackynavi/device_discovery.hpp"
#include <stdio.h>
#include <string.h>
#include <vector>

using namespace hackynavi;
namespace {
size_t assertions = 0, failures = 0;
#define CHECK(x) do { ++assertions; if (!(x)) { ++failures; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)
const uint32_t readyReg = 0x16061u * 4, memReg = 0xde3u * 4;
const uint32_t scratchLo = 0x94u * 4, scratchHi = 0x95u * 4;
const uint32_t scratchSize = 0x96u * 4;
const uint64_t gib = UINT64_C(1) << 30;
struct Event { char kind; uint32_t offset, value; };
struct Model {
    uint32_t mib = 24576, notReadyReads = 0, readyReads = 0;
    uint32_t readyValue = 0x80000000u, scratchBytes = 0;
    uint64_t scratchOffset = 0, blobOffset = 24 * gib - 65536;
    uint32_t hi = 0x87654321u, index = 0x12345678u;
    uint32_t phase = 0;
    size_t calls = 0, failAt = 0, dataReads = 0;
    bool badProtocol = false;
    std::vector<uint8_t> blob;
    std::vector<Event> events;

    explicit Model(size_t length = 64) : blob((length + 3) & ~size_t(3), 0) {
        for (size_t i = 0; i < blob.size(); ++i)
            blob[i] = static_cast<uint8_t>((i * 17 + 3) & 255);
        blob[0] = 0x07; blob[1] = 0x14; blob[2] = 0x21; blob[3] = 0x28;
        blob[4] = 1; blob[5] = 0; blob[6] = 4; blob[7] = 0;
        setLength(length);
    }
    void setLength(size_t length) {
        blob[10] = static_cast<uint8_t>(length);
        blob[11] = static_cast<uint8_t>(length >> 8);
    }
    bool event(char kind, uint32_t offset, uint32_t value) {
        events.push_back(Event{kind, offset, value});
        ++calls;
        return failAt == 0 || calls != failAt;
    }
    static bool read(void* context, uint32_t offset, uint32_t& value) {
        Model& m = *static_cast<Model*>(context);
        if (!m.event('r', offset, 0)) return false;
        if (offset & 3u) m.badProtocol = true;
        if (offset == readyReg) {
            value = m.readyReads++ < m.notReadyReads ? 0 : m.readyValue;
        } else if (offset == memReg) value = m.mib;
        else if (offset == scratchSize) value = m.scratchBytes;
        else if (offset == scratchLo) value = static_cast<uint32_t>(m.scratchOffset);
        else if (offset == scratchHi) value = static_cast<uint32_t>(m.scratchOffset >> 32);
        else if (offset == 4) {
            if (m.phase != 2 || !(m.index & 0x80000000u)) m.badProtocol = true;
            m.phase = 0;
            ++m.dataReads;
            const uint64_t at = (static_cast<uint64_t>(m.hi) << 31) |
                                (m.index & 0x7ffffffcu);
            const uint64_t vram = static_cast<uint64_t>(m.mib) << 20;
            if ((at & 3u) || at > vram || vram - at < 4) m.badProtocol = true;
            if (at == vram - 65536 && at != m.blobOffset) value = 0;
            else if (at >= m.blobOffset && at - m.blobOffset <= m.blob.size() &&
                     m.blob.size() - static_cast<size_t>(at - m.blobOffset) >= 4) {
                const size_t p = static_cast<size_t>(at - m.blobOffset);
                value = static_cast<uint32_t>(m.blob[p]) |
                    (static_cast<uint32_t>(m.blob[p + 1]) << 8) |
                    (static_cast<uint32_t>(m.blob[p + 2]) << 16) |
                    (static_cast<uint32_t>(m.blob[p + 3]) << 24);
            } else { m.badProtocol = true; value = 0; }
        } else { m.badProtocol = true; return false; }
        return true;
    }
    static bool write(void* context, uint32_t offset, uint32_t value) {
        Model& m = *static_cast<Model*>(context);
        // A failed write may nevertheless have arrived at hardware.
        const bool ok = m.event('w', offset, value);
        if (offset == 24) { if (m.phase != 0) m.badProtocol = true; m.hi = value; m.phase = 1; }
        else if (offset == 0) { if (m.phase != 1) m.badProtocol = true; m.index = value; m.phase = 2; }
        else m.badProtocol = true; // In particular MM_DATA MUST NEVER be written.
        return ok;
    }
    static void delay(void* context, uint32_t us) {
        Model& m = *static_cast<Model*>(context);
        m.events.push_back(Event{'d', 0, us});
    }
    DeviceDiscoveryIo io() { return DeviceDiscoveryIo{this, read, write, delay}; }
};

DeviceDiscoveryStatus run(Model& m, uint8_t* b, size_t capacity,
                          DeviceDiscoveryResult& out) {
    return acquireDeviceDiscovery(m.io(), DeviceDiscoveryOptions{2000, 1000},
                                  b, capacity, out);
}

void successfulCopies() {
    const size_t lengths[] = {12, 13, 14, 15, 16, 63, 64, 1756, 10240, 10241, 65532, 65535};
    for (size_t length : lengths) {
        Model m(length);
        const bool scratch = length > kDeviceDiscoveryDefaultRegionBytes;
        if (scratch) {
            m.scratchOffset = m.blobOffset = 24 * gib - 131072;
            m.scratchBytes = 65536;
        }
        std::vector<uint8_t> output(length + 2, 0xa5);
        DeviceDiscoveryResult out{};
        CHECK(run(m, output.data() + 1, length, out) == DeviceDiscoveryStatus::Ok);
        CHECK(out.vramBytes == 24 * gib);
        CHECK(out.sourceOffset == m.blobOffset);
        CHECK(out.regionSize == (scratch ? 65536 : 10240) && out.binarySize == length);
        CHECK(out.bytesCopied == length && out.readyPolls == 1);
        CHECK(out.selectorWritesAttempted && out.usedScratch == scratch);
        CHECK(output.front() == 0xa5 && output.back() == 0xa5);
        CHECK(memcmp(output.data() + 1, m.blob.data(), length) == 0);
        CHECK(m.dataReads == (length + 3) / 4 + (scratch ? 1 : 0) && !m.badProtocol);
        CHECK(m.events.size() == 2 + m.dataReads * 3 + (scratch ? 3 : 0));
        CHECK(m.events[2].kind == 'w' && m.events[2].offset == 24);
        CHECK(m.events[2].value == 11); // 24 GiB minus 64 KiB >> 31.
        CHECK(m.events[3].kind == 'w' && m.events[3].offset == 0);
        CHECK(m.events[3].value == 0xffff0000u);
        // No guessed readback/restore transactions: last operation is MM_DATA.
        CHECK(m.events.back().kind == 'r' && m.events.back().offset == 4);
        CHECK(m.hi == 11 && m.index != 0x12345678u && m.phase == 0);
    }
    // Exercise high selectors without truncating the 32-bit size-in-MiB field.
    Model huge;
    huge.mib = 0xfffffffeu;
    huge.blobOffset = (static_cast<uint64_t>(huge.mib) << 20) - 65536;
    uint8_t output[64]; DeviceDiscoveryResult out{};
    CHECK(run(huge, output, sizeof(output), out) == DeviceDiscoveryStatus::Ok);
    CHECK(out.vramBytes == (static_cast<uint64_t>(0xfffffffeu) << 20));
    CHECK(huge.hi == static_cast<uint32_t>(huge.blobOffset >> 31));
    CHECK(!huge.badProtocol);
}

void readiness() {
    uint8_t output[64]; DeviceDiscoveryResult out{};
    Model readyLast;
    readyLast.notReadyReads = 1999;
    CHECK(run(readyLast, output, sizeof(output), out) == DeviceDiscoveryStatus::Ok);
    CHECK(out.readyPolls == 2000);
    size_t delays = 0;
    for (const Event& e : readyLast.events) if (e.kind == 'd') { ++delays; CHECK(e.value == 1000); }
    CHECK(delays == 1999);
    Model timeout;
    timeout.notReadyReads = 2000;
    memset(output, 0x9b, sizeof(output));
    CHECK(run(timeout, output, sizeof(output), out) == DeviceDiscoveryStatus::ReadyTimeout);
    CHECK(out.readyPolls == 2000 && out.bytesCopied == 0 && !out.selectorWritesAttempted);
    CHECK(timeout.calls == 2000 && timeout.events.size() == 3999);
    for (uint8_t b : output) CHECK(b == 0x9b);
    Model one;
    one.notReadyReads = 1;
    CHECK(acquireDeviceDiscovery(one.io(), DeviceDiscoveryOptions{1, 1}, output,
          sizeof(output), out) == DeviceDiscoveryStatus::ReadyTimeout);
    CHECK(one.events.size() == 1);
    Model absent; absent.readyValue = UINT32_MAX;
    CHECK(run(absent, output, sizeof(output), out) == DeviceDiscoveryStatus::DeviceUnavailable);
    CHECK(absent.calls == 1 && !out.selectorWritesAttempted);
    Model absentSize; absentSize.mib = UINT32_MAX;
    CHECK(run(absentSize, output, sizeof(output), out) == DeviceDiscoveryStatus::DeviceUnavailable);
    CHECK(absentSize.calls == 2 && !out.selectorWritesAttempted);
    Model sysmem; sysmem.mib = 0; sysmem.scratchBytes = 65536;
    CHECK(run(sysmem, output, sizeof(output), out) == DeviceDiscoveryStatus::SystemMemoryUnsupported);
    CHECK(sysmem.calls == 2 && !out.selectorWritesAttempted);
}

Model scratchModel(size_t length = 64) {
    Model m(length);
    m.scratchOffset = m.blobOffset = 20 * gib + 0x1234;
    m.scratchBytes = static_cast<uint32_t>((length + 3) & ~size_t(3));
    return m;
}

void scratchFallback() {
    uint8_t output[64]; DeviceDiscoveryResult out{};
    Model m = scratchModel(15);
    memset(output, 0xc3, sizeof(output));
    CHECK(run(m, output, sizeof(output), out) == DeviceDiscoveryStatus::Ok);
    CHECK(out.usedScratch && out.sourceOffset == 20 * gib + 0x1234);
    CHECK(out.regionSize == 16 && out.binarySize == 15 && out.bytesCopied == 15);
    CHECK(output[15] == 0xc3 && memcmp(output, m.blob.data(), 15) == 0);
    CHECK(m.events[5].offset == scratchSize && m.events[6].offset == scratchLo &&
          m.events[7].offset == scratchHi && !m.badProtocol);
    // A large containing scratch reservation is allowed; the binary stays bounded.
    Model large = scratchModel(); large.scratchBytes = 131072;
    CHECK(run(large, output, sizeof(output), out) == DeviceDiscoveryStatus::Ok);
    CHECK(out.regionSize == 131072 && out.bytesCopied == 64);
    Model none = scratchModel(); none.scratchBytes = 0;
    CHECK(run(none, output, sizeof(output), out) == DeviceDiscoveryStatus::BadSignature);
    CHECK(none.calls == 6 && out.bytesCopied == 0);
    Model badSig = scratchModel(); badSig.blob[0] ^= 1;
    CHECK(run(badSig, output, sizeof(output), out) == DeviceDiscoveryStatus::BadSignature);
    CHECK(out.bytesCopied == 0 && out.usedScratch);
    // Valid default signature takes precedence over an invalid scratch location.
    Model prefer; prefer.scratchOffset = UINT64_MAX; prefer.scratchBytes = UINT32_MAX;
    CHECK(run(prefer, output, sizeof(output), out) == DeviceDiscoveryStatus::Ok);
    CHECK(!out.usedScratch && !prefer.badProtocol);

    const uint64_t offsets[] = {UINT64_MAX - 3, 24 * gib, 24 * gib - 4,
                                20 * gib + 1, 20 * gib + 2, 20 * gib + 3};
    for (uint64_t offset : offsets) {
        Model bad = scratchModel(); bad.scratchOffset = offset;
        CHECK(run(bad, output, sizeof(output), out) == DeviceDiscoveryStatus::InvalidVramRange);
        CHECK(bad.dataReads == 1 && out.bytesCopied == 0 && !bad.badProtocol);
    }
    const uint32_t sizes[] = {1, 4, 8, 11, 13, 63, UINT32_MAX};
    for (uint32_t size : sizes) {
        Model bad = scratchModel(); bad.scratchBytes = size;
        CHECK(run(bad, output, sizeof(output), out) == DeviceDiscoveryStatus::InvalidVramRange);
        CHECK(bad.dataReads == 1 && out.bytesCopied == 0 && !bad.badProtocol);
    }
    // Exact VRAM end and a 2 GiB selector crossing.
    Model end = scratchModel(15);
    end.scratchOffset = end.blobOffset = 24 * gib - 16;
    CHECK(run(end, output, sizeof(output), out) == DeviceDiscoveryStatus::Ok);
    CHECK(!end.badProtocol);
    Model crossing = scratchModel(64);
    crossing.scratchOffset = crossing.blobOffset = 20 * gib - 12;
    CHECK(run(crossing, output, sizeof(output), out) == DeviceDiscoveryStatus::Ok);
    CHECK(!crossing.badProtocol && crossing.hi == 10);
}

void lengthsAndArguments() {
    uint8_t output[64]; DeviceDiscoveryResult out{};
    for (size_t length = 0; length < 12; ++length) {
        Model m; m.setLength(length);
        CHECK(run(m, output, sizeof(output), out) == DeviceDiscoveryStatus::MalformedHeader);
        CHECK(out.bytesCopied == 0 && m.dataReads == 3);
    }
    Model overRegion = scratchModel(); overRegion.setLength(65);
    CHECK(run(overRegion, output, sizeof(output), out) == DeviceDiscoveryStatus::MalformedHeader);
    CHECK(out.bytesCopied == 0 && !overRegion.badProtocol);
    Model overDefault(10241);
    CHECK(run(overDefault, output, sizeof(output), out) == DeviceDiscoveryStatus::MalformedHeader);
    CHECK(out.regionSize == 10240 && out.bytesCopied == 0 && overDefault.dataReads == 3);
    for (size_t cap = 0; cap < 64; ++cap) {
        Model m;
        memset(output, 0xd4, sizeof(output));
        CHECK(run(m, output, cap, out) == DeviceDiscoveryStatus::BufferTooSmall);
        CHECK(out.bytesCopied == 0);
        CHECK(m.calls == (cap < 12 ? 0 : 11));
        for (uint8_t b : output) CHECK(b == 0xd4);
    }
    const DeviceDiscoveryOptions badOptions[] = {{0, 1000}, {2001, 1000},
                                                 {1, 0}, {1, 1001}};
    for (const DeviceDiscoveryOptions& options : badOptions) {
        Model m;
        out.bytesCopied = 123; out.selectorWritesAttempted = true;
        CHECK(acquireDeviceDiscovery(m.io(), options, output, sizeof(output), out) ==
              DeviceDiscoveryStatus::InvalidArgument);
        CHECK(m.calls == 0 && out.bytesCopied == 0 && !out.selectorWritesAttempted);
    }
    for (unsigned missing = 0; missing < 4; ++missing) {
        Model m; DeviceDiscoveryIo io = m.io();
        if (missing == 0) io.read32 = nullptr;
        if (missing == 1) io.write32 = nullptr;
        if (missing == 2) io.delayUs = nullptr;
        CHECK(acquireDeviceDiscovery(io, DeviceDiscoveryOptions{2000, 1000},
              missing == 3 ? nullptr : output, sizeof(output), out) == DeviceDiscoveryStatus::InvalidArgument);
        CHECK(m.calls == 0);
    }
}

void transportFailures(bool scratch) {
    Model baseline = scratch ? scratchModel(1756) : Model(1756);
    std::vector<uint8_t> output(1758, 0xa7);
    DeviceDiscoveryResult out{};
    CHECK(run(baseline, output.data() + 1, 1756, out) == DeviceDiscoveryStatus::Ok);
    const size_t operationCount = baseline.calls;
    // Fail each register read/write, including a write that has taken effect.
    for (size_t fail = 1; fail <= operationCount; ++fail) {
        Model m = scratch ? scratchModel(1756) : Model(1756);
        m.failAt = fail;
        for (uint8_t& b : output) b = 0xa7;
        CHECK(run(m, output.data() + 1, 1756, out) == DeviceDiscoveryStatus::TransportError);
        CHECK(m.calls == fail); // No further reads, writes or invented restoration.
        CHECK(out.bytesCopied < 1756 && !m.badProtocol);
        CHECK(out.selectorWritesAttempted == (fail >= 3));
        CHECK(output.front() == 0xa7 && output.back() == 0xa7);
        CHECK(memcmp(output.data() + 1, m.blob.data(), out.bytesCopied) == 0);
        CHECK(output[1 + out.bytesCopied] == 0xa7);
        // Caller-buffer contents beyond bytesCopied remain untouched.
        CHECK(output[1756] == 0xa7);
    }
}
} // namespace

int main() {
    successfulCopies(); readiness(); scratchFallback(); lengthsAndArguments();
    transportFailures(false); transportFailures(true);
    CHECK(strcmp(deviceDiscoveryStatusString(DeviceDiscoveryStatus::Ok), "ok") == 0);
    CHECK(strcmp(deviceDiscoveryStatusString(static_cast<DeviceDiscoveryStatus>(-1)),
                 "unknown device discovery status") == 0);
    printf("device discovery acquisition: %zu assertions, %zu failures\n", assertions, failures);
    return failures ? 1 : 0;
}
