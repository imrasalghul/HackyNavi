// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace hackynavi { namespace graphicstopology {
constexpr size_t kMaxSe=8,kMaxSa=2,kSlots=kMaxSe*kMaxSa;
enum class Status : uint32_t { Ok, InvalidArgument, DiscoveryRejected, UnsupportedMetadata,
    InvalidLayout, TransportError, DeviceUnavailable, InvalidSelector, Changed, NoActiveUnits };
// Declared firmware maxima, never a live harvesting observation.
struct Metadata { uint32_t maxSe,maxSaPerSe,maxRbPerSe,maxWgpPerSa; };
struct Layout { uint32_t selector,saHardware,saUser,rbHardware,rbUser,wgpHardware,wgpUser;uint64_t barBytes; };
struct Raw {
    uint32_t originalSelector,saHardware,saUser,rbHardware,rbUser,observedSa;
    uint32_t wgpHardware[kSlots],wgpUser[kSlots];
};
struct Facts {
    uint32_t maxSe,maxSaPerSe,maxRenderBackends,activeSe,activeCu,minGoodCuPerSa;
    uint32_t activeSaMask,activeRbMask,cuMask[kSlots];
};
struct Snapshot { Raw raw;Facts facts; };
struct Trace {
    uint32_t readsAttempted,selectorWritesAttempted,selectorWritesCompleted;
    uint32_t restoreWritesAttempted,restoreWritesCompleted,sweepsCompleted;
    uint32_t originalSelector;
    bool originalReadComplete;
};
struct Io { void* context;bool (*read32)(void*,uint32_t,uint32_t&);bool (*write32)(void*,uint32_t,uint32_t); };
// Complete validated discovery + exact observed GC1.2 schema (132 bytes).
// Newer/unverified schemas are explicit failures. No MMIO. Clears outputs.
Status build(const uint8_t*,size_t,uint64_t barBytes,Metadata&,Layout&);
// Caller serializes the entire call with powered GC, reset and GRBM selectors.
// Each selected SE/SA is checked before AND after its two mask observations.
// A successful sweep restores and verifies its exact validated prior selector.
// Any failed/ambiguous access stops immediately: no cleanup write or retry.
// Two full matching sweeps precede publication. Trace records writes BEFORE
// callbacks; native owner must retain it and fault on a failed attempted call.
// Snapshot output is cleared on failure. All arguments have distinct stable
// caller-owned storage. Numeric results do not certify native ownership.
Status capture(const Metadata&,const Layout&,const Io&,Snapshot&,Trace&);
bool validSelector(uint32_t,const Metadata&);
} }
