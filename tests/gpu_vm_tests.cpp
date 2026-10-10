// SPDX-License-Identifier: MIT
// Hardware-neutral byte encoding tests. No GPU allocations or submissions.
#include "hackynavi/gpu_vm.hpp"
#include <cstring>
#include <iostream>

using hackynavi::Status;
using namespace hackynavi::gmc11;

namespace {
unsigned checks = 0, failures = 0;
#define CHECK(expression) do { ++checks; if (!(expression)) { ++failures; \
    std::cerr << "FAIL line " << __LINE__ << ": " #expression "\n"; } } while (0)

const LeafAttributes vramRw = {MemoryDomain::VramPhysical,
    MemoryType::NonCached, false, true, true, false, false};
const DirectoryAttributes ordinaryDirectory = {MemoryDomain::VramPhysical,
    false, DirectoryLevel::Pdb0, false};

struct Fixture {
    uint8_t storage[135];
    TableBuffer table;
    // Deliberately byte-unaligned output, with 3 trailing non-entry bytes.
    Fixture() : storage{}, table{storage + 1, 131} {
        std::memset(storage, 0xa5, sizeof(storage));
    }
    void untouched() const {
        for (size_t index = 0; index < sizeof(storage); ++index)
            CHECK(storage[index] == 0xa5);
    }
    void outside(size_t firstEntry, size_t count) const {
        const size_t begin = 1 + firstEntry * 8, end = begin + count * 8;
        for (size_t index = 0; index < sizeof(storage); ++index)
            if (index < begin || index >= end) CHECK(storage[index] == 0xa5);
    }
};

template<size_t Count>
void bytesEqual(const uint8_t* actual, const uint8_t (&expected)[Count]) {
    for (size_t index = 0; index < Count; ++index)
        CHECK(actual[index] == expected[index]);
}

uint64_t readLe(const uint8_t* bytes) {
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index)
        value |= static_cast<uint64_t>(bytes[index]) << (index * 8);
    return value;
}

void goldenLeaves() {
    Fixture f;
    LeafAttributes a = vramRw;
    a.memoryType = MemoryType::Uncached;
    a.executable = true;
    // Independent literal bytes across the 24 GiB address boundary. These
    // addresses test the field width, not ownership of an actual 24 GiB card.
    const uint8_t expected[] = {
        0x71, 0xf0, 0xff, 0xff, 0x05, 0x00, 0x03, 0x00,
        0x71, 0x00, 0x00, 0x00, 0x06, 0x00, 0x03, 0x00
    };
    CHECK(writeContiguousPtes(f.table, 2, 0x00000005fffff000ULL, 2, a) == Status::Ok);
    bytesEqual(f.table.bytes + 16, expected);
    f.outside(2, 2);

    Fixture system;
    a = {MemoryDomain::SystemDma, MemoryType::CacheCoherent,
         true, true, true, false, true};
    const uint64_t address = 0x00000012abcde000ULL;
    const uint8_t dma[] = {0x67, 0xe0, 0xcd, 0xab, 0x12, 0x00, 0x02, 0x04};
    CHECK(writeScatterPtes(system.table, 0, &address, 1, a) == Status::Ok);
    bytesEqual(system.table.bytes, dma);
    system.outside(0, 1);

    Fixture scattered;
    const uint64_t addresses[] = {0x100000000ULL, 0, 0x23456000ULL, 0x100000000ULL};
    const uint8_t scatterBytes[] = {
        0x61, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x61, 0x60, 0x45, 0x23, 0x00, 0x00, 0x00, 0x00,
        0x61, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
    };
    CHECK(writeScatterPtes(scattered.table, 3, addresses, 4, vramRw) == Status::Ok);
    bytesEqual(scattered.table.bytes + 24, scatterBytes);
    scattered.outside(3, 4);
}

void goldenDirectories() {
    const uint8_t ordinary[] = {0x45, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x00, 0x00};
    const uint8_t bfs9[] = {0x45, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x00, 0x48};
    const uint8_t tf[] = {0x45, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x00, 0x01};
    for (unsigned level = 0; level < 4; ++level) {
        for (unsigned translate = 0; translate < 2; ++translate) {
            Fixture f;
            const DirectoryAttributes a = {MemoryDomain::VramPhysical, true,
                static_cast<DirectoryLevel>(level), translate != 0};
            CHECK(writeDirectoryPointer(f.table, 15, 0x0000abcdef123440ULL, a)
                  == Status::Ok);
            if (translate && level == 2) bytesEqual(f.table.bytes + 120, bfs9);
            else if (translate && level == 3) bytesEqual(f.table.bytes + 120, tf);
            else bytesEqual(f.table.bytes + 120, ordinary);
            f.outside(15, 1);
        }
    }
    Fixture system;
    const DirectoryAttributes a = {MemoryDomain::SystemDma, true,
        DirectoryLevel::Pdb0, true};
    const uint8_t topDma[] = {0xc7, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x01};
    CHECK(writeDirectoryPointer(system.table, 0, 0x00000fffffffffc0ULL, a)
          == Status::Ok);
    bytesEqual(system.table.bytes, topDma);
    system.outside(0, 1);
}

void policies() {
    // Exhaust all supported permission/snoop/no-allocate/cache/domain choices
    // against numeric constants taken independently from the Linux layout.
    for (unsigned domain = 0; domain < 2; ++domain) {
        for (unsigned type = 0; type < 4; ++type) {
            for (unsigned policy = 0; policy < 32; ++policy) {
                Fixture f;
                const LeafAttributes a = {static_cast<MemoryDomain>(domain),
                    static_cast<MemoryType>(type), (policy & 1) != 0,
                    (policy & 2) != 0, (policy & 4) != 0,
                    (policy & 8) != 0, (policy & 16) != 0};
                uint64_t expected = 0x0000001234567001ULL;
                if (domain) expected |= 0x2;
                if (policy & 1) expected |= 0x4;
                if (policy & 2) expected |= 0x20;
                if (policy & 4) expected |= 0x40;
                if (policy & 8) expected |= 0x10;
                if (policy & 16) expected |= 0x0400000000000000ULL;
                const uint64_t mtypes[] = {0, 0x0001000000000000ULL,
                    0x0002000000000000ULL, 0x0003000000000000ULL};
                expected |= mtypes[type];
                CHECK(writeContiguousPtes(f.table, 1, 0x0000001234567000ULL,
                                          1, a) == Status::Ok);
                CHECK(readLe(f.table.bytes + 8) == expected);
                f.outside(1, 1);
            }
        }
    }
}

void addressLimits() {
    CHECK(kPageBytes == 4096 && kEntryBytes == 8);
    CHECK(kPteAddressMask == 0x0000fffffffff000ULL);
    CHECK(kPdeAddressMask == 0x0000ffffffffffc0ULL);
    for (unsigned domain = 0; domain < 2; ++domain) {
        LeafAttributes a = vramRw;
        a.domain = static_cast<MemoryDomain>(domain);
        DirectoryAttributes d = ordinaryDirectory;
        d.domain = a.domain;
        const uint64_t limit = domain ? 0x0000100000000000ULL :
                                       0x0001000000000000ULL;
        {
            Fixture f;
            CHECK(writeContiguousPtes(f.table, 0, limit - 4096, 1, a) == Status::Ok);
            CHECK(readLe(f.table.bytes) == ((limit - 4096) | (domain ? 0x63 : 0x61)));
            f.outside(0, 1);
        }
        {
            Fixture f;
            CHECK(writeContiguousPtes(f.table, 0, limit - 4096, 2, a)
                  == Status::InvalidArgument);
            f.untouched();
        }
        {
            Fixture f;
            CHECK(writeDirectoryPointer(f.table, 0, limit - 64, d) == Status::Ok);
            CHECK(readLe(f.table.bytes) == ((limit - 64) | (domain ? 3 : 1)));
            f.outside(0, 1);
        }
        const uint64_t invalid[] = {limit, limit + 4096, UINT64_MAX,
            0xfffffffffffff000ULL, 1, 4095, 4097};
        for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
            Fixture f;
            CHECK(writeContiguousPtes(f.table, 0, invalid[index], 1, a)
                  == Status::InvalidArgument);
            CHECK(writeScatterPtes(f.table, 0, &invalid[index], 1, a)
                  == Status::InvalidArgument);
            CHECK(writeDirectoryPointer(f.table, 0, invalid[index], d)
                  == Status::InvalidArgument);
            f.untouched();
        }
        // Every nonzero low page bit is invalid, and every low PDE bit too.
        for (unsigned bit = 0; bit < 12; ++bit) {
            Fixture f;
            CHECK(writeContiguousPtes(f.table, 0, 1ULL << bit, 1, a)
                  == Status::InvalidArgument);
            if (bit < 6)
                CHECK(writeDirectoryPointer(f.table, 0, 1ULL << bit, d)
                      == Status::InvalidArgument);
            f.untouched();
        }
    }
}

void failuresLeaveBytes() {
    Fixture f;
    const uint64_t address = 0;
    CHECK(writeContiguousPtes(f.table, 0, 0, 0, vramRw) == Status::InvalidArgument);
    CHECK(writeScatterPtes(f.table, 0, &address, 0, vramRw) == Status::InvalidArgument);
    CHECK(writeScatterPtes(f.table, 0, nullptr, 1, vramRw) == Status::InvalidArgument);
    for (size_t first = 0; first <= 16; ++first) {
        CHECK(writeContiguousPtes(f.table, first, 0, 17 - first, vramRw)
              == Status::CapacityExceeded);
        CHECK(writeScatterPtes(f.table, first, &address, 17 - first, vramRw)
              == Status::CapacityExceeded);
    }
    CHECK(writeDirectoryPointer(f.table, 16, 0, ordinaryDirectory)
          == Status::CapacityExceeded);
    CHECK(writeContiguousPtes(f.table, SIZE_MAX, 0, 1, vramRw)
          == Status::CapacityExceeded);
    CHECK(writeContiguousPtes(f.table, 1, 0, SIZE_MAX, vramRw)
          == Status::CapacityExceeded);
    CHECK(writeScatterPtes(f.table, 0, &address, SIZE_MAX, vramRw)
          == Status::CapacityExceeded);
    CHECK(writeDirectoryPointer(f.table, SIZE_MAX, 0, ordinaryDirectory)
          == Status::CapacityExceeded);
    CHECK(writeContiguousPtes({nullptr, 128}, 0, 0, 1, vramRw)
          == Status::InvalidArgument);
    CHECK(writeScatterPtes({nullptr, 128}, 0, &address, 1, vramRw)
          == Status::InvalidArgument);
    CHECK(writeDirectoryPointer({nullptr, 128}, 0, 0, ordinaryDirectory)
          == Status::InvalidArgument);
    for (size_t bytes = 0; bytes < 8; ++bytes) {
        CHECK(writeContiguousPtes({f.table.bytes, bytes}, 0, 0, 1, vramRw)
              == Status::CapacityExceeded);
        CHECK(writeScatterPtes({f.table.bytes, bytes}, 0, &address, 1, vramRw)
              == Status::CapacityExceeded);
        CHECK(writeDirectoryPointer({f.table.bytes, bytes}, 0, 0, ordinaryDirectory)
              == Status::CapacityExceeded);
    }
    LeafAttributes bad = vramRw;
    bad.domain = static_cast<MemoryDomain>(99);
    CHECK(writeContiguousPtes(f.table, 0, 0, 1, bad) == Status::InvalidArgument);
    CHECK(writeScatterPtes(f.table, 0, &address, 1, bad) == Status::InvalidArgument);
    bad = vramRw;
    bad.memoryType = static_cast<MemoryType>(4);
    CHECK(writeContiguousPtes(f.table, 0, 0, 1, bad) == Status::InvalidArgument);
    CHECK(writeScatterPtes(f.table, 0, &address, 1, bad) == Status::InvalidArgument);
    DirectoryAttributes badDirectory = ordinaryDirectory;
    badDirectory.domain = static_cast<MemoryDomain>(99);
    CHECK(writeDirectoryPointer(f.table, 0, 0, badDirectory) == Status::InvalidArgument);
    badDirectory = ordinaryDirectory;
    badDirectory.parentLevel = static_cast<DirectoryLevel>(4);
    CHECK(writeDirectoryPointer(f.table, 0, 0, badDirectory) == Status::InvalidArgument);
    f.untouched();

    // Misaligned native input and wrapping claimed pointer extents must be
    // rejected before dereference. These values are never accessed by a test.
    alignas(uint64_t) uint8_t unaligned[16] = {};
    CHECK(writeScatterPtes(f.table, 0, reinterpret_cast<const uint64_t*>(unaligned + 1),
                          1, vramRw) == Status::InvalidArgument);
    TableBuffer wrap = {reinterpret_cast<uint8_t*>(UINTPTR_MAX - 3), 8};
    CHECK(writeContiguousPtes(wrap, 0, 0, 1, vramRw) == Status::InvalidArgument);
    CHECK(writeScatterPtes(wrap, 0, &address, 1, vramRw) == Status::InvalidArgument);
    CHECK(writeDirectoryPointer(wrap, 0, 0, ordinaryDirectory) == Status::InvalidArgument);
    const uint64_t* inputWrap = reinterpret_cast<const uint64_t*>(UINTPTR_MAX - 7);
    CHECK(writeScatterPtes(f.table, 0, inputWrap, 2, vramRw) == Status::InvalidArgument);
    f.untouched();
}

void scatterRollbackAndOverlap() {
    uint64_t addresses[16];
    for (size_t invalidIndex = 0; invalidIndex < 16; ++invalidIndex) {
        for (size_t index = 0; index < 16; ++index) addresses[index] = index * 4096;
        addresses[invalidIndex] = 0x1000000000000ULL;
        Fixture f;
        CHECK(writeScatterPtes(f.table, 0, addresses, 16, vramRw)
              == Status::InvalidArgument);
        f.untouched();
        addresses[invalidIndex] = 17;
        CHECK(writeScatterPtes(f.table, 0, addresses, 16, vramRw)
              == Status::InvalidArgument);
        f.untouched();
    }

    uint64_t shared[8] = {0, 4096, 8192, 12288, 16384, 20480, 24576, 28672};
    uint64_t saved[8];
    std::memcpy(saved, shared, sizeof(shared));
    TableBuffer alias = {reinterpret_cast<uint8_t*>(shared), sizeof(shared)};
    CHECK(writeScatterPtes(alias, 0, shared, 4, vramRw) == Status::InvalidArgument);
    CHECK(writeScatterPtes(alias, 1, shared, 4, vramRw) == Status::InvalidArgument);
    CHECK(writeScatterPtes(alias, 0, shared + 1, 4, vramRw) == Status::InvalidArgument);
    CHECK(std::memcmp(shared, saved, sizeof(shared)) == 0);
    // Disjoint ranges in the same underlying allocation are permitted.
    CHECK(writeScatterPtes(alias, 4, shared, 4, vramRw) == Status::Ok);
    for (size_t index = 0; index < 4; ++index) {
        CHECK(shared[index] == saved[index]);
        CHECK(readLe(alias.bytes + (index + 4) * 8) == (saved[index] | 0x61));
    }
}

void compareContiguousWithScatter() {
    for (size_t count = 1; count <= 16; ++count) {
        for (size_t first = 0; first <= 16 - count; ++first) {
            Fixture contiguous, scatter;
            uint64_t addresses[16];
            for (size_t index = 0; index < count; ++index)
                addresses[index] = 0x567890000ULL + index * 4096;
            CHECK(writeContiguousPtes(contiguous.table, first, addresses[0], count,
                                      vramRw) == Status::Ok);
            CHECK(writeScatterPtes(scatter.table, first, addresses, count,
                                   vramRw) == Status::Ok);
            CHECK(std::memcmp(contiguous.storage, scatter.storage,
                              sizeof(contiguous.storage)) == 0);
            contiguous.outside(first, count);
        }
    }
}

void invalidGartEntries() {
    Fixture f;
    const uint64_t dummy = 0x00000abcdef12000ULL;
    CHECK(writeInvalidGartPtes(f.table, 3, dummy, 5) == Status::Ok);
    for (size_t index = 3; index < 8; ++index) {
        CHECK(readLe(f.table.bytes + index * 8) == dummy);
        CHECK((f.table.bytes[index * 8] & 3) == 0); // VALID and SYSTEM clear.
    }
    f.outside(3, 5);
    for (unsigned choice = 0; choice < 6; ++choice) {
        Fixture rejected;
        const auto status = choice == 0 ? writeInvalidGartPtes(rejected.table, 0, dummy + 1, 1) :
            choice == 1 ? writeInvalidGartPtes(rejected.table, 0, kSystemDmaAddressLimit, 1) :
            choice == 2 ? writeInvalidGartPtes(rejected.table, 16, dummy, 1) :
            choice == 3 ? writeInvalidGartPtes(rejected.table, 0, dummy, 17) :
            choice == 4 ? writeInvalidGartPtes(rejected.table, 0, dummy, 0) :
                          writeInvalidGartPtes(rejected.table, SIZE_MAX, dummy, SIZE_MAX);
        CHECK(status != Status::Ok);
        rejected.untouched();
    }
    CHECK(writeInvalidGartPtes(f.table, 0, 0, 1) == Status::Ok);
    CHECK(readLe(f.table.bytes) == 0);
    CHECK(writeInvalidGartPtes(f.table, 15, kSystemDmaAddressLimit - kPageBytes, 1) == Status::Ok);
    CHECK(readLe(f.table.bytes + 15 * 8) == kSystemDmaAddressLimit - kPageBytes);
}

} // namespace

int main() {
    goldenLeaves();
    goldenDirectories();
    policies();
    addressLimits();
    failuresLeaveBytes();
    scatterRollbackAndOverlap();
    compareContiguousWithScatter();
    invalidGartEntries();
    std::cout << checks << " GPUVM assertions, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
