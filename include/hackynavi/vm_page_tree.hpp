// SPDX-License-Identifier: MIT
#pragma once
#include "hackynavi/gpu_vm.hpp"
namespace hackynavi { namespace vmpt {
constexpr uint32_t kPageBytes = 4096, kEntries = 512, kMaxPages = 1024;
constexpr uint64_t kVaBegin = 4096, kVaEnd = 1ULL << 47;
enum class Status { Ok, InvalidArgument, InvalidState, NoMemory, NoSpace,
                    Overlap, MissingMapping, CorruptTree, ReleaseFailed };
struct Storage { uint8_t* bytes; uint64_t dmaAddress; void* owner; };
struct Allocator {
    void* context;
    // Return a zeroable owned 4KiB CPU view and a prepared, contiguous, snooped
    // system DMA page (44-bit). Address zero is valid. No physical inference.
    bool (*allocate)(void*, Storage&);
    // Always consumes the allocation, including when reporting false. False
    // reports teardown failure and forbids further device use; it does not
    // return ownership to Tree. Native callback resets/deletes the DMA owner.
    bool (*release)(void*, Storage&);
};
struct Page {
    Storage storage;
    uint16_t children[kEntries]; // page index + 1; zero means no child
    uint16_t parent, parentEntry;
    uint8_t level; // 3=PDB2, 2=PDB1, 1=PDB0, 0=PTB
};
// CPU construction of a sparse PDB2->PDB1->PDB0->PTB tree, using the real
// GMC11 encoders (translate-further off). Parent/leaf indices follow Linux
// amdgpu_vm_pt_level_shift(), 12+9*level, with 512 entries per 4KiB table
// (Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0, amdgpu_vm_pt.c).
// Caller owns the metadata array and serializes EVERY operation. This class
// has no hardware-retirement authority: mutations/release are allowed ONLY
// before any root publication, or after independently proven GPU retirement.
// Native HackyNaviVMContext enforces that rule and keeps exposed pages pinned.
// Mapping failure rolls back newly allocated pages and leaves prior mappings
// unchanged. Empty table pages are kept until release; capacity is explicit.
class Tree final {
public:
    Tree();
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
    Status initialize(Page*, uint16_t capacity, const Allocator&);
    Status map(uint64_t va, const uint64_t* dmaPages, size_t count,
               gmc11::LeafAttributes);
    Status unmap(uint64_t va, size_t count);
    Status release();
    const Page* page(uint16_t n) const { return n < count_ ? &pages_[n] : nullptr; }
    uint16_t pageCount() const { return count_; }
    bool ready() const { return ready_; }
private:
    Status append(uint8_t level, uint16_t parent, uint16_t entry, uint16_t& index);
    Status leaf(uint64_t va, bool create, uint16_t& index);
    bool rollback(uint16_t previousCount);
    Page* pages_;
    uint16_t capacity_, count_;
    Allocator allocator_;
    bool ready_;
};
} }
