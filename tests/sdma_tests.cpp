// SPDX-License-Identifier: MIT
// Packet bytes/words only. This test never opens or submits to a GPU.
#include "hackynavi/sdma.hpp"
#include <cstring>
#include <iostream>

using hackynavi::Status;
using namespace hackynavi::sdma6;

namespace {
unsigned checks = 0, failures = 0;
#define CHECK(expression) do { ++checks; if (!(expression)) { ++failures; \
    std::cerr << "FAIL line " << __LINE__ << ": " #expression "\n"; } } while (0)
constexpr uint32_t kSentinel = 0xa5f05a0fU;

struct Fixture {
    uint32_t storage[40];
    CommandBuffer buffer;
    Fixture() : storage{}, buffer{storage + 2, 36, 0} {
        for (size_t i = 0; i < 40; ++i) storage[i] = kSentinel;
    }
    void guards() const {
        CHECK(storage[0] == kSentinel && storage[1] == kSentinel);
        CHECK(storage[38] == kSentinel && storage[39] == kSentinel);
        for (size_t index = buffer.used + 2; index < 38; ++index)
            CHECK(storage[index] == kSentinel);
    }
    void untouched() const {
        for (size_t i = 0; i < 40; ++i) CHECK(storage[i] == kSentinel);
    }
};

template<size_t Count>
void equals(const uint32_t* actual, const uint32_t (&expected)[Count]) {
    for (size_t i = 0; i < Count; ++i) CHECK(actual[i] == expected[i]);
}

void goldenPackets() {
    Fixture f;
    // Independent literals, including the 30-bit count-minus-one word.
    const uint32_t copy[] = {0x00000001, 0x0000000f, 0x00000000,
                            0x55667788, 0x11223344, 0xddeeff00, 0x99aabbcc};
    CHECK(appendCopyLinear(f.buffer, UINT64_C(0x1122334455667788),
                          UINT64_C(0x99aabbccddeeff00), 16) == Status::Ok);
    CHECK(f.buffer.used == 7);
    equals(f.buffer.words, copy);
    const uint32_t fill[] = {0x0000000b, 0x12345678, 0x00000002, 0xdecafbad, 0x0000007f};
    CHECK(appendConstantFill(f.buffer, UINT64_C(0x0000000212345678),
                            0xdecafbad, 128) == Status::Ok);
    CHECK(f.buffer.used == 12);
    equals(f.buffer.words + 7, fill);
    const uint32_t fence32[] = {0x00030005, 0xfffffffc, 0x12345678, 0x76543210,
                               0x00000006, 0x00000000};
    CHECK(appendFence32(f.buffer, UINT64_C(0x12345678fffffffc), 0x76543210, true) == Status::Ok);
    CHECK(f.buffer.used == 18);
    equals(f.buffer.words + 12, fence32);
    // The second 32-bit fence crosses a 4 GiB boundary. Four-byte alignment is
    // enough, and only one trap is emitted after both writes.
    const uint32_t fence64[] = {0x00030005, 0xfffffffc, 0x00000001, 0x55667788,
                               0x00030005, 0x00000000, 0x00000002, 0x11223344,
                               0x00000006, 0x00000000};
    CHECK(appendFence64(f.buffer, UINT64_C(0x00000001fffffffc),
                       UINT64_C(0x1122334455667788), true) == Status::Ok);
    CHECK(f.buffer.used == 28);
    equals(f.buffer.words + 18, fence64);
    f.guards();

    // Verify actual emitted byte order on little-endian hosts without making
    // host-endian uint32_t arrays a claim of portable byte serialization.
    const uint32_t endian = 1;
    if (*reinterpret_cast<const unsigned char*>(&endian) == 1) {
        const unsigned char bytes[] = {0x05, 0x00, 0x03, 0x00, 0xfc, 0xff, 0xff, 0xff,
                                       0x78, 0x56, 0x34, 0x12, 0x10, 0x32, 0x54, 0x76};
        CHECK(std::memcmp(f.buffer.words + 12, bytes, sizeof(bytes)) == 0);
    }
}

void countAndRangeBoundaries() {
    Fixture f;
    CHECK(appendCopyLinear(f.buffer, 1, 2, 1) == Status::Ok); // Unaligned byte copy.
    CHECK(f.buffer.words[1] == 0);
    CHECK(appendCopyLinear(f.buffer, 0, UINT64_C(0x80000000), 0x40000000) == Status::Ok);
    CHECK(f.buffer.words[8] == 0x3fffffff);
    CHECK(appendConstantFill(f.buffer, 0, 0, 0x40000000) == Status::Ok);
    CHECK(f.buffer.words[18] == 0x3fffffff);
    f.guards();

    Fixture high;
    CHECK(appendCopyLinear(high.buffer, UINT64_MAX, 0, 1) == Status::Ok);
    CHECK(appendCopyLinear(high.buffer, 0, UINT64_MAX, 1) == Status::Ok);
    CHECK(appendConstantFill(high.buffer, UINT64_MAX - 3, 0xffffffff, 4) == Status::Ok);
    CHECK(appendFence32(high.buffer, UINT64_MAX - 3, 7) == Status::Ok);
    CHECK(appendFence64(high.buffer, UINT64_MAX - 7, UINT64_MAX) == Status::Ok);
    CHECK(high.buffer.used == 31); // No optional traps.
    high.guards();

    Fixture adjacent;
    CHECK(appendCopyLinear(adjacent.buffer, 0x1000, 0x1004, 4) == Status::Ok);
    CHECK(appendCopyLinear(adjacent.buffer, 0x1004, 0x1000, 4) == Status::Ok);
    adjacent.guards();
}

void rejectedArguments() {
    Fixture f;
    CHECK(appendCopyLinear(f.buffer, 0, 100, 0) == Status::InvalidArgument);
    CHECK(appendCopyLinear(f.buffer, 0, UINT64_C(0x80000000), 0x40000001) == Status::InvalidArgument);
    CHECK(appendCopyLinear(f.buffer, UINT64_MAX, 0, 2) == Status::InvalidArgument);
    CHECK(appendCopyLinear(f.buffer, 0, UINT64_MAX, 2) == Status::InvalidArgument);
    CHECK(appendCopyLinear(f.buffer, 0, 0, 1) == Status::InvalidArgument);
    CHECK(appendCopyLinear(f.buffer, 0x1000, 0x1003, 4) == Status::InvalidArgument);
    CHECK(appendCopyLinear(f.buffer, 0x1003, 0x1000, 4) == Status::InvalidArgument);
    CHECK(appendCopyLinear(f.buffer, UINT64_MAX - 7, UINT64_MAX - 3, 8) == Status::InvalidArgument);
    CHECK(appendConstantFill(f.buffer, 0, 0, 0) == Status::InvalidArgument);
    CHECK(appendConstantFill(f.buffer, 0, 0, 1) == Status::InvalidArgument);
    CHECK(appendConstantFill(f.buffer, 0, 0, 6) == Status::InvalidArgument);
    CHECK(appendConstantFill(f.buffer, 1, 0, 4) == Status::InvalidArgument);
    CHECK(appendConstantFill(f.buffer, 0, 0, 0x40000004) == Status::InvalidArgument);
    CHECK(appendConstantFill(f.buffer, UINT64_MAX - 3, 0, 8) == Status::InvalidArgument);
    CHECK(appendFence32(f.buffer, 1, 0, true) == Status::InvalidArgument);
    CHECK(appendFence64(f.buffer, 2, 0, true) == Status::InvalidArgument);
    CHECK(appendFence64(f.buffer, UINT64_MAX - 3, 0, true) == Status::InvalidArgument);
    CHECK(f.buffer.used == 0);
    f.untouched();
}

void capacityAndRollback() {
    // Test every insufficient capacity for each variable fence length, including
    // room for the low fence alone: no partial fence or optional trap may appear.
    for (unsigned kind = 0; kind < 6; ++kind) {
        const size_t required[] = {7, 5, 4, 6, 8, 10};
        for (size_t room = 0; room < required[kind]; ++room) {
            Fixture f;
            f.buffer.used = 3;
            f.buffer.capacity = 3 + room;
            Status status = Status::Ok;
            switch (kind) {
            case 0: status = appendCopyLinear(f.buffer, 0, 0x1000, 64); break;
            case 1: status = appendConstantFill(f.buffer, 0, 0, 64); break;
            case 2: status = appendFence32(f.buffer, 0, 0); break;
            case 3: status = appendFence32(f.buffer, 0, 0, true); break;
            case 4: status = appendFence64(f.buffer, 0, 0); break;
            case 5: status = appendFence64(f.buffer, 0, 0, true); break;
            }
            CHECK(status == Status::CapacityExceeded);
            CHECK(f.buffer.used == 3);
            f.untouched();
        }
    }
    Fixture exact;
    exact.buffer.capacity = 10;
    CHECK(appendFence64(exact.buffer, 0, 0, true) == Status::Ok);
    CHECK(exact.buffer.used == 10);
    const uint32_t before[] = {0x00030005, 0, 0, 0, 0x00030005, 4, 0, 0, 6, 0};
    equals(exact.buffer.words, before);
    CHECK(appendFence32(exact.buffer, 0, 7) == Status::CapacityExceeded);
    CHECK(exact.buffer.used == 10);
    equals(exact.buffer.words, before);
    exact.guards();

    Fixture invalid;
    invalid.buffer.used = 37;
    CHECK(appendFence32(invalid.buffer, 0, 0) == Status::InvalidArgument);
    CHECK(invalid.buffer.used == 37);
    invalid.buffer.used = 0;
    invalid.buffer.capacity = SIZE_MAX;
    CHECK(appendFence32(invalid.buffer, 0, 0) == Status::InvalidArgument);
    invalid.untouched();
    CommandBuffer null = {nullptr, 0, 0};
    CHECK(appendFence32(null, 0, 0) == Status::InvalidArgument);
    CHECK(null.used == 0);
    alignas(uint32_t) unsigned char unaligned[32] = {};
    CommandBuffer misaligned = {reinterpret_cast<uint32_t*>(unaligned + 1), 4, 0};
    CHECK(appendFence32(misaligned, 0, 0) == Status::InvalidArgument);
    CHECK(misaligned.used == 0);
    for (unsigned char byte : unaligned) CHECK(byte == 0);
}

} // namespace

int main() {
    goldenPackets();
    countAndRangeBoundaries();
    rejectedArguments();
    capacityAndRollback();
    std::cout << checks << " SDMA packet assertions, " << failures << " failures.\n";
    return failures ? 1 : 0;
}
