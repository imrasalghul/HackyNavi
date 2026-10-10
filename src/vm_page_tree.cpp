// SPDX-License-Identifier: MIT
#include "hackynavi/vm_page_tree.hpp"
namespace hackynavi { namespace vmpt {
namespace {
bool range(uint64_t va, size_t count) {
    return count && va >= kVaBegin && va < kVaEnd && !(va & (kPageBytes - 1)) &&
        count <= (kVaEnd - va) / kPageBytes;
}
uint16_t entry(uint64_t va, uint8_t level) {
    return static_cast<uint16_t>((va >> (12 + 9 * level)) & 511);
}
uint64_t read(const uint8_t* bytes) {
    uint64_t value = 0;
    for (unsigned n = 0; n < 8; ++n) value |= uint64_t(bytes[n]) << (8 * n);
    return value;
}
void clear(uint8_t* bytes, size_t count) { for (size_t n = 0; n < count; ++n) bytes[n] = 0; }
bool address(uint64_t value) {
    return !(value & (kPageBytes - 1)) && value < gmc11::kSystemDmaAddressLimit;
}
}
Tree::Tree(): pages_(nullptr), capacity_(0), count_(0), allocator_{}, ready_(false) {}
Status Tree::initialize(Page* pages, uint16_t capacity, const Allocator& allocator) {
    if (pages_ || ready_) return Status::InvalidState;
    if (!pages || !capacity || capacity > kMaxPages || !allocator.allocate || !allocator.release)
        return Status::InvalidArgument;
    pages_ = pages; capacity_ = capacity; allocator_ = allocator;
    uint16_t index = 0;
    const auto status = append(3, 0, 0, index);
    ready_ = status == Status::Ok;
    return status;
}
Status Tree::append(uint8_t level, uint16_t parent, uint16_t slot, uint16_t& index) {
    if (count_ == capacity_) return Status::NoSpace;
    Storage storage{};
    if (!allocator_.allocate(allocator_.context, storage)) return Status::NoMemory;
    if (!storage.bytes || !storage.owner || !address(storage.dmaAddress) ||
        reinterpret_cast<uintptr_t>(storage.bytes) > UINTPTR_MAX - (kPageBytes - 1)) {
        if (!allocator_.release(allocator_.context, storage)) return Status::ReleaseFailed;
        return Status::InvalidArgument;
    }
    // Do not trust allocator zeroing; inactive holes must have VALID=SYSTEM=0.
    clear(storage.bytes, kPageBytes);
    Page& page = pages_[count_];
    page.storage = storage; page.level = level; page.parent = parent; page.parentEntry = slot;
    for (size_t n = 0; n < kEntries; ++n) page.children[n] = 0;
    index = count_++;
    return Status::Ok;
}
Status Tree::leaf(uint64_t va, bool create, uint16_t& index) {
    index = 0;
    for (uint8_t level = 3; level; --level) {
        if (index >= count_ || pages_[index].level != level) return Status::CorruptTree;
        Page& parent = pages_[index];
        const uint16_t slot = entry(va, level);
        const uint16_t child = parent.children[slot];
        if (child) {
            const uint16_t next = child - 1;
            if (next >= count_ || pages_[next].parent != index ||
                pages_[next].parentEntry != slot || pages_[next].level != level - 1)
                return Status::CorruptTree;
            // Metadata is not proof that the serialized hardware pointer still
            // names the retained child. No extra permission/TF/fragment flags.
            if (read(parent.storage.bytes + slot * 8) != (pages_[next].storage.dmaAddress | 7))
                return Status::CorruptTree;
            index = next; continue;
        }
        if (read(parent.storage.bytes + slot * 8)) return Status::CorruptTree;
        if (!create) return Status::MissingMapping;
        uint16_t next = 0;
        const auto allocated = append(level - 1, index, slot, next);
        if (allocated != Status::Ok) return allocated;
        const auto directoryLevel = level == 3 ? gmc11::DirectoryLevel::Pdb2 :
            level == 2 ? gmc11::DirectoryLevel::Pdb1 : gmc11::DirectoryLevel::Pdb0;
        const gmc11::DirectoryAttributes flags = {gmc11::MemoryDomain::SystemDma,
            true, directoryLevel, false};
        if (gmc11::writeDirectoryPointer({parent.storage.bytes, kPageBytes}, slot,
                pages_[next].storage.dmaAddress, flags) != hackynavi::Status::Ok)
            return Status::CorruptTree;
        parent.children[slot] = next + 1;
        index = next;
    }
    return pages_[index].level == 0 ? Status::Ok : Status::CorruptTree;
}
bool Tree::rollback(uint16_t previousCount) {
    bool success = true;
    while (count_ > previousCount) {
        Page& page = pages_[count_ - 1];
        if (count_ > 1) {
            Page& parent = pages_[page.parent];
            parent.children[page.parentEntry] = 0;
            clear(parent.storage.bytes + page.parentEntry * 8, 8);
        }
        if (!allocator_.release(allocator_.context, page.storage)) success = false;
        page.storage = Storage{}; --count_;
    }
    return success;
}
Status Tree::map(uint64_t va, const uint64_t* addresses, size_t count,
                 gmc11::LeafAttributes attributes) {
    if (!ready_) return Status::InvalidState;
    if (!range(va, count) || !addresses ||
        (reinterpret_cast<uintptr_t>(addresses) % alignof(uint64_t)) ||
        count > (UINTPTR_MAX - reinterpret_cast<uintptr_t>(addresses)) / sizeof(uint64_t) ||
        attributes.domain != gmc11::MemoryDomain::SystemDma || !attributes.snooped ||
        attributes.memoryType != gmc11::MemoryType::Uncached || !attributes.readable || attributes.noAllocate)
        return Status::InvalidArgument;
    // Preflight all addresses and overlaps before allocating or modifying any
    // table. Input must be caller-owned stable storage outside the table arena.
    const uintptr_t input = reinterpret_cast<uintptr_t>(addresses);
    const size_t inputBytes = count * sizeof(uint64_t);
    for (uint16_t p = 0; p < count_; ++p) {
        const uintptr_t bytes = reinterpret_cast<uintptr_t>(pages_[p].storage.bytes);
        if (input <= bytes + kPageBytes - 1 && bytes <= input + inputBytes - 1)
            return Status::InvalidArgument;
    }
    for (size_t n = 0; n < count; ++n) {
        if (!address(addresses[n])) return Status::InvalidArgument;
        uint16_t p = 0;
        const auto found = leaf(va + n * kPageBytes, false, p);
        if (found == Status::Ok && read(pages_[p].storage.bytes + entry(va + n * kPageBytes, 0) * 8))
            return Status::Overlap;
        if (found != Status::Ok && found != Status::MissingMapping) return found;
    }
    const uint16_t previousCount = count_;
    for (size_t n = 0; n < count; ++n) {
        uint16_t p = 0;
        const auto allocated = leaf(va + n * kPageBytes, true, p);
        if (allocated != Status::Ok) {
            if (!rollback(previousCount)) { ready_ = false; return Status::ReleaseFailed; }
            return allocated;
        }
    }
    // With all paths and input preflighted, no allocation/fallible native call
    // remains. Each encoder writes exactly one PTE and validates before write.
    for (size_t n = 0; n < count; ++n) {
        uint16_t p = 0;
        if (leaf(va + n * kPageBytes, false, p) != Status::Ok ||
            gmc11::writeContiguousPtes({pages_[p].storage.bytes, kPageBytes},
                entry(va + n * kPageBytes, 0), addresses[n], 1, attributes) != hackynavi::Status::Ok) {
            // A corrupt internal tree cannot be made reusable by a partial undo.
            ready_ = false; return Status::CorruptTree;
        }
    }
    return Status::Ok;
}
Status Tree::unmap(uint64_t va, size_t count) {
    if (!ready_) return Status::InvalidState;
    if (!range(va, count)) return Status::InvalidArgument;
    for (size_t n = 0; n < count; ++n) {
        uint16_t p = 0;
        const auto found = leaf(va + n * kPageBytes, false, p);
        if (found != Status::Ok) return found;
        if (!(read(pages_[p].storage.bytes + entry(va + n * kPageBytes, 0) * 8) & 1))
            return Status::MissingMapping;
    }
    for (size_t n = 0; n < count; ++n) {
        uint16_t p = 0;
        (void)leaf(va + n * kPageBytes, false, p);
        clear(pages_[p].storage.bytes + entry(va + n * kPageBytes, 0) * 8, 8);
    }
    return Status::Ok;
}
Status Tree::release() {
    if (!pages_) return Status::Ok;
    const bool success = rollback(0);
    pages_ = nullptr; capacity_ = 0; ready_ = false; allocator_ = Allocator{};
    return success ? Status::Ok : Status::ReleaseFailed;
}
} }
