// SPDX-License-Identifier: MIT
#pragma once
#include "hackynavi/gfx11_color_target.h"
namespace hackynavi { namespace colortopology {
// Numeric capture machinery only. Native ownership, serialization, powered GC,
// generation and deadline checks belong to the actual callback owner.
enum class Status : uint32_t { Ok, InvalidArgument, InvalidLayout, DiscoveryRejected,
    TransportError, DeviceUnavailable, IndirectHighNotZero, Changed, UnsupportedTopology };
struct Layout {
    uint32_t gbAddrConfig, revisionStrap, index, data, indexHi;
    uint64_t barBytes;
};
struct Identity {
    uint32_t vendorDevice, revisionClass, subsystem, bar4, bar5;
    uint16_t command;
    uint8_t headerType, reserved;
};
struct Snapshot { Identity pci; HNColorTopology topology; };
struct Trace {
    uint32_t identitiesAttempted, readsAttempted, selectorWritesAttempted;
    uint32_t selectorWritesCompleted, postingReadsCompleted, snapshotsCompleted;
};
struct Io {
    void* context;
    bool (*identity)(void*, Identity&);
    bool (*read32)(void*, uint32_t, uint32_t&);
    bool (*write32)(void*, uint32_t, uint32_t);
};
// Resolves only GC11.0.0 BASE0 GB_ADDR_CONFIG and NBIF4.3.0 BASE2
// RCC_STRAP0_RCC_DEV0_EPF0_STRAP0 from a fully validated discovery image.
// RSMU window must fit BAR; strap may be indirect. Failure clears output.
Status buildLayout(const uint8_t* discovery, size_t bytes, uint64_t barBytes, Layout&);
// Exactly two complete matching captures, each bracketed by identical PCI
// identity reads. No delay/retry/selector restoration. An indirect read writes
// INDEX after observing INDEX_HI==0; posting read is ordering, not equality.
// All writes are selector writes; trace records attempts BEFORE callbacks.
// Caller preserves trace and prevents replay after any ambiguous transport.
// Outputs are distinct caller-owned objects, serialized against all mutation.
// On failure snapshot is cleared; trace remains diagnostic, never authority.
Status capture(const Layout&, const Io&, Snapshot&, Trace&);
} }
