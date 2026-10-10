// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_CLIENT_MEMORY_HPP
#define HACKYNAVI_CLIENT_MEMORY_HPP

#include <IOKit/IOReturn.h>
#include <mach/mach_types.h>
#include <stddef.h>
#include <stdint.h>

class IOBufferMemoryDescriptor;
class IOMemoryMap;

// CPU-only storage mapped into one retained client task. This is the memory
// ownership primitive for the accelerator clients, not their event/ring
// protocol. There is no GPU mapping or DMA address. The client must serialize
// all calls (including reset/destruction) in a sleepable context and close
// admission/drain its operations before reset. No raw kernel pointer escapes.
// A ReadWrite client can change bytes concurrently: copied bytes are untrusted
// input, not an atomic multi-field snapshot or a verified protocol message.
class HackyNaviClientMemory final {
public:
    enum : size_t { kPageBytes = 4096, kMaxBytes = 1024U * 1024U };
    enum class Access : uint8_t { ReadOnly, ReadWrite };

    HackyNaviClientMemory();
    ~HackyNaviClientMemory();
    HackyNaviClientMemory(const HackyNaviClientMemory&) = delete;
    HackyNaviClientMemory& operator=(const HackyNaviClientMemory&) = delete;

    // Allocate whole isolated pages, clear every byte including page padding,
    // copy optional trusted initial data, then create the unique task mapping.
    // Only the actual current non-kernel task can receive a new mapping.
    // Failure releases all partial ownership and exposes no client address.
    IOReturn allocate(task_t, size_t logicalBytes, Access,
                      const void* initialBytes = nullptr, size_t initialLength = 0);
    void reset();
    bool ready() const { return mapping_ != nullptr; }
    size_t length() const { return ready() ? logicalBytes_ : 0; }
    size_t allocatedSize() const { return ready() ? allocatedBytes_ : 0; }
    // Authorization uses exact task identity. Output is unchanged on failure.
    IOReturn clientAddress(task_t caller, uint64_t& address) const;
    // Kernel-owned source/destination only: never pass an unvalidated user
    // pointer. Copy methods do not establish ring/event synchronization.
    IOReturn copyFromClient(size_t offset, void* destination, size_t bytes) const;
    IOReturn copyToClient(size_t offset, const void* source, size_t bytes);
    // Aligned single-word producer/consumer operations. Their acquire/release
    // ordering is real CPU ordering, not GPU completion or DMA coherence.
    IOReturn load32(size_t offset, uint32_t& value) const;
    IOReturn store32(size_t offset, uint32_t value);

private:
    bool contains(size_t offset, size_t bytes) const;
    task_t task_;
    IOBufferMemoryDescriptor* memory_;
    IOMemoryMap* mapping_;
    uint8_t* bytes_;
    size_t logicalBytes_, allocatedBytes_;
    uint64_t clientAddress_;
};
#endif
