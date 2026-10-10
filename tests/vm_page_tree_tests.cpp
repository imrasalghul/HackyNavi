// SPDX-License-Identifier: MIT
// CPU-only table construction tests. The allocator below is a unit-test
// fixture, never a kext backend and not evidence of working GPU isolation.
#include "hackynavi/vm_page_tree.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <array>
#include <limits>
using namespace hackynavi;
namespace {
size_t checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (false)
uint64_t read(const uint8_t* p) {
    uint64_t v = 0; for (unsigned n = 0; n < 8; ++n) v |= uint64_t(p[n]) << (n * 8); return v;
}
struct Fixture {
    struct Allocation { uint8_t bytes[4096]; };
    std::vector<vmpt::Page> metadata;
    vmpt::Tree tree;
    size_t calls = 0, frees = 0, live = 0, failAt = 0, releaseFailAt = 0;
    uint64_t nextAddress = 0;
    bool badAddress = false;
    explicit Fixture(uint16_t capacity = vmpt::kMaxPages): metadata(capacity) {}
    static bool allocate(void* opaque, vmpt::Storage& out) {
        auto& f = *static_cast<Fixture*>(opaque);
        if (++f.calls == f.failAt) return false;
        auto* a = new Allocation;
        std::memset(a->bytes, 0xab, 4096); // production tree must actually zero
        out = {a->bytes, f.badAddress ? 1 : f.nextAddress, a};
        f.nextAddress += 8192; ++f.live; return true;
    }
    static bool release(void* opaque, vmpt::Storage& storage) {
        auto& f = *static_cast<Fixture*>(opaque);
        CHECK(storage.owner != nullptr);
        delete static_cast<Allocation*>(storage.owner); storage = {};
        ++f.frees; --f.live; return f.frees != f.releaseFailAt;
    }
    vmpt::Status init() { return tree.initialize(metadata.data(), static_cast<uint16_t>(metadata.size()), {this, allocate, release}); }
    ~Fixture() { (void)tree.release(); CHECK(live == 0); }
    // Walk the actual serialized GPU table entries by DMA address, independently
    // of the tree's children[] metadata. This checks address/level/flag encoding.
    uint64_t walk(uint64_t va) {
        const vmpt::Page* current = tree.page(0);
        if (!current) return 0;
        for (int level = 3; level; --level) {
            uint64_t value = read(current->storage.bytes + ((va >> (12 + 9 * level)) & 511) * 8);
            if (!value) return 0;
            CHECK((value & ~gmc11::kPdeAddressMask) == 7);
            const uint64_t address = value & gmc11::kPdeAddressMask;
            current = nullptr;
            for (uint16_t p = 0; p < tree.pageCount(); ++p)
                if (tree.page(p)->storage.dmaAddress == address) { current = tree.page(p); break; }
            CHECK(current != nullptr);
            if (!current) return 0;
            CHECK(current->level == level - 1);
        }
        return read(current->storage.bytes + ((va >> 12) & 511) * 8);
    }
    std::vector<uint8_t> snapshot() {
        std::vector<uint8_t> bytes;
        for (uint16_t p = 0; p < tree.pageCount(); ++p) {
            const auto* page = tree.page(p);
            bytes.insert(bytes.end(), page->storage.bytes, page->storage.bytes + 4096);
            const auto* metadataBytes = reinterpret_cast<const uint8_t*>(page->children);
            bytes.insert(bytes.end(), metadataBytes, metadataBytes + sizeof(page->children));
        }
        return bytes;
    }
};
gmc11::LeafAttributes flags(bool write = true, bool execute = false) {
    return {gmc11::MemoryDomain::SystemDma, gmc11::MemoryType::Uncached, true, true, write, execute, false};
}
uint64_t expected(uint64_t address, bool write = true, bool execute = false) {
    return address | (3ULL << 48) | 7 | 32 | (write ? 64 : 0) | (execute ? 16 : 0);
}
void initialization() {
    Fixture f;
    CHECK(f.init() == vmpt::Status::Ok);
    CHECK(f.tree.pageCount() == 1 && f.tree.page(0)->storage.dmaAddress == 0);
    CHECK(f.tree.page(0)->level == 3);
    for (size_t n = 0; n < 4096; ++n) CHECK(f.tree.page(0)->storage.bytes[n] == 0);
    CHECK(f.init() == vmpt::Status::InvalidState);
    CHECK(f.tree.page(1) == nullptr);
    CHECK(f.tree.release() == vmpt::Status::Ok && f.live == 0 && !f.tree.ready());
    CHECK(f.tree.release() == vmpt::Status::Ok);
    CHECK(f.init() == vmpt::Status::Ok);
    Fixture bad;
    bad.badAddress = true;
    CHECK(bad.init() == vmpt::Status::InvalidArgument && bad.live == 0);
    Fixture noMemory;
    noMemory.failAt = 1;
    CHECK(noMemory.init() == vmpt::Status::NoMemory && noMemory.live == 0);
    vmpt::Tree empty;
    uint64_t dma = 0;
    CHECK(empty.map(4096, &dma, 1, flags()) == vmpt::Status::InvalidState);
    CHECK(empty.unmap(4096, 1) == vmpt::Status::InvalidState);
    CHECK(empty.initialize(nullptr, 1, {}) == vmpt::Status::InvalidArgument);
}
void geometry() {
    const uint64_t points[] = {4096, (1ULL << 21) - 4096, 1ULL << 21,
        (1ULL << 30) - 4096, 1ULL << 30, (1ULL << 39) - 4096, 1ULL << 39,
        (1ULL << 46), vmpt::kVaEnd - 4096};
    for (bool writable : {false, true}) for (bool executable : {false, true}) {
        Fixture f; CHECK(f.init() == vmpt::Status::Ok);
        for (size_t n = 0; n < sizeof(points)/sizeof(points[0]); ++n) {
            const uint64_t dma = n == 0 ? 0 : gmc11::kSystemDmaAddressLimit - n * 4096;
            CHECK(f.tree.map(points[n], &dma, 1, flags(writable, executable)) == vmpt::Status::Ok);
            CHECK(f.walk(points[n]) == expected(dma, writable, executable));
        }
        const auto before = f.snapshot();
        uint64_t dma = 0;
        for (auto va : points) CHECK(f.tree.map(va, &dma, 1, flags()) == vmpt::Status::Overlap);
        CHECK(before == f.snapshot());
        const auto pages = f.tree.pageCount();
        for (auto va : points) {
            CHECK(f.tree.unmap(va, 1) == vmpt::Status::Ok);
            CHECK(f.walk(va) == 0);
            CHECK(f.tree.unmap(va, 1) == vmpt::Status::MissingMapping);
        }
        CHECK(f.tree.pageCount() == pages); // no live table recycling
        for (auto va : points) CHECK(f.tree.map(va, &dma, 1, flags()) == vmpt::Status::Ok);
        CHECK(f.tree.pageCount() == pages);
    }
}
void scatterAndAtomicity() {
    Fixture f; CHECK(f.init() == vmpt::Status::Ok);
    std::vector<uint64_t> pages(4096); // native16MiB BO maximum
    for (size_t n = 0; n < pages.size(); ++n) pages[n] = ((n * 977) % 4096) * 4096;
    const uint64_t va = (1ULL << 39) - (7ULL << 20); // cross PDB2, PDB1, PTB boundaries
    CHECK(f.tree.map(va, pages.data(), pages.size(), flags()) == vmpt::Status::Ok);
    for (size_t n = 0; n < pages.size(); ++n) CHECK(f.walk(va + n * 4096) == expected(pages[n]));
    const auto before = f.snapshot();
    CHECK(f.tree.unmap(va - 4096, pages.size() + 1) == vmpt::Status::MissingMapping);
    CHECK(f.snapshot() == before);
    CHECK(f.tree.unmap(va, pages.size() + 1) == vmpt::Status::MissingMapping);
    CHECK(f.snapshot() == before);
    CHECK(f.tree.map(va - 4096, pages.data(), pages.size(), flags()) == vmpt::Status::Overlap);
    CHECK(f.snapshot() == before);
    CHECK(f.tree.unmap(va, pages.size()) == vmpt::Status::Ok);
    for (size_t n = 0; n < pages.size(); ++n) CHECK(f.walk(va + n * 4096) == 0);
    CHECK(f.tree.map(va, pages.data(), pages.size(), flags(false, true)) == vmpt::Status::Ok);
    for (size_t n = 0; n < pages.size(); ++n) CHECK(f.walk(va + n * 4096) == expected(pages[n], false, true));
}
void failuresAndLimits() {
    for (size_t fail = 1; fail <= 6; ++fail) {
        Fixture f; CHECK(f.init() == vmpt::Status::Ok);
        uint64_t old = 0;
        CHECK(f.tree.map(4096, &old, 1, flags()) == vmpt::Status::Ok);
        const auto before = f.snapshot(); const auto count = f.tree.pageCount();
        f.failAt = f.calls + fail;
        uint64_t dma[] = {8192, 12288};
        CHECK(f.tree.map((1ULL << 40) - 4096, dma, 2, flags()) == vmpt::Status::NoMemory);
        CHECK(f.tree.pageCount() == count && f.live == count && before == f.snapshot());
        CHECK(f.walk(4096) == expected(0));
        f.failAt = 0;
        CHECK(f.tree.map((1ULL << 40) - 4096, dma, 2, flags()) == vmpt::Status::Ok);
    }
    // Allocation fails while adding children beneath an already populated
    // branch. Existing entries, including an empty leaf slot preceding the
    // boundary, must remain byte-for-byte unchanged on every failed step.
    const uint64_t branchEnds[] = {(1ULL << 21), (1ULL << 30)};
    for (auto end : branchEnds) {
        const size_t allocations = end == (1ULL << 21) ? 1 : 3;
        for (size_t fail = 1; fail <= allocations; ++fail) {
            Fixture f; CHECK(f.init() == vmpt::Status::Ok);
            uint64_t dma[] = {0, 8192};
            CHECK(f.tree.map(4096, dma, 1, flags()) == vmpt::Status::Ok);
            const auto before = f.snapshot(); const auto count = f.tree.pageCount();
            f.failAt = f.calls + fail;
            CHECK(f.tree.map(end - 4096, dma, 2, flags()) == vmpt::Status::NoMemory);
            CHECK(f.snapshot() == before && f.tree.pageCount() == count && f.live == count);
            CHECK(f.walk(4096) == expected(0));
            CHECK(f.walk(end - 4096) == 0 && f.walk(end) == 0);
        }
    }
    for (uint16_t capacity = 1; capacity <= 4; ++capacity) {
        Fixture f(capacity); CHECK(f.init() == vmpt::Status::Ok);
        const auto before = f.snapshot(); uint64_t dma = 0;
        const auto status = f.tree.map(4096, &dma, 1, flags());
        CHECK(status == (capacity == 4 ? vmpt::Status::Ok : vmpt::Status::NoSpace));
        if (capacity < 4) CHECK(f.snapshot() == before && f.live == 1);
    }
    Fixture corrupt; CHECK(corrupt.init() == vmpt::Status::Ok);
    uint64_t dma = 0; corrupt.tree.page(0)->storage.bytes[0] = 1;
    CHECK(corrupt.tree.map(4096, &dma, 1, flags()) == vmpt::Status::CorruptTree);
    for (uint16_t p = 0; p < 3; ++p) {
        Fixture wrongPde; CHECK(wrongPde.init() == vmpt::Status::Ok);
        CHECK(wrongPde.tree.map(4096, &dma, 1, flags()) == vmpt::Status::Ok);
        wrongPde.tree.page(p)->storage.bytes[1] ^= 0x20; // retained-child DMA mismatch
        const auto before = wrongPde.snapshot();
        CHECK(wrongPde.tree.map(8192, &dma, 1, flags()) == vmpt::Status::CorruptTree);
        CHECK(wrongPde.tree.unmap(4096, 1) == vmpt::Status::CorruptTree);
        CHECK(wrongPde.snapshot() == before);
    }
    Fixture releaseFail; CHECK(releaseFail.init() == vmpt::Status::Ok);
    releaseFail.failAt = 4; releaseFail.releaseFailAt = 1;
    CHECK(releaseFail.tree.map(4096, &dma, 1, flags()) == vmpt::Status::ReleaseFailed);
    CHECK(!releaseFail.tree.ready() && releaseFail.live == 1);
    CHECK(releaseFail.tree.map(4096, &dma, 1, flags()) == vmpt::Status::InvalidState);
}
void invalidInput() {
    Fixture f; CHECK(f.init() == vmpt::Status::Ok);
    const auto before = f.snapshot(); uint64_t dma[] = {0, 4096};
    const uint64_t invalidVa[] = {0, 1, 4095, 4097, vmpt::kVaEnd, UINT64_MAX, UINT64_MAX - 4095};
    for (auto va : invalidVa) CHECK(f.tree.map(va, dma, 1, flags()) == vmpt::Status::InvalidArgument);
    CHECK(f.tree.map(vmpt::kVaEnd - 4096, dma, 2, flags()) == vmpt::Status::InvalidArgument);
    CHECK(f.tree.map(4096, nullptr, 1, flags()) == vmpt::Status::InvalidArgument);
    CHECK(f.tree.map(4096, dma, 0, flags()) == vmpt::Status::InvalidArgument);
    CHECK(f.tree.map(4096, dma, SIZE_MAX, flags()) == vmpt::Status::InvalidArgument);
    CHECK(f.tree.map(4096, reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(dma) + 1), 1, flags()) == vmpt::Status::InvalidArgument);
    CHECK(f.tree.map(4096, reinterpret_cast<uint64_t*>(f.tree.page(0)->storage.bytes), 1, flags()) == vmpt::Status::InvalidArgument);
    CHECK(f.tree.map(4096, reinterpret_cast<uint64_t*>(UINTPTR_MAX - 7), 2, flags()) == vmpt::Status::InvalidArgument);
    for (uint64_t bad : {uint64_t(1), gmc11::kSystemDmaAddressLimit, UINT64_MAX}) {
        dma[1] = bad;
        CHECK(f.tree.map(4096, dma, 2, flags()) == vmpt::Status::InvalidArgument);
        CHECK(f.snapshot() == before);
    }
    dma[1] = 4096;
    for (unsigned n = 0; n < 5; ++n) {
        auto attributes = flags();
        if (n == 0) attributes.domain = gmc11::MemoryDomain::VramPhysical;
        if (n == 1) attributes.memoryType = gmc11::MemoryType::CacheCoherent;
        if (n == 2) attributes.snooped = false;
        if (n == 3) attributes.readable = false;
        if (n == 4) attributes.noAllocate = true;
        CHECK(f.tree.map(4096, dma, 2, attributes) == vmpt::Status::InvalidArgument);
    }
    CHECK(f.snapshot() == before && f.calls == 1);
    CHECK(f.tree.unmap(0, 1) == vmpt::Status::InvalidArgument);
    CHECK(f.tree.unmap(4096, 0) == vmpt::Status::InvalidArgument);
    CHECK(f.tree.unmap(4096, 1) == vmpt::Status::MissingMapping);
}
}
int main() {
    initialization(); geometry(); scatterAndAtomicity(); failuresAndLimits(); invalidInput();
    std::printf("vm-page-tree: %zu checks, %zu failures\n", checks, failures);
    return failures ? 1 : 0;
}
