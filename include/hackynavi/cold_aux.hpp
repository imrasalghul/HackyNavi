// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_COLD_AUX_HPP
#define HACKYNAVI_COLD_AUX_HPP
#include "hackynavi/discovery.hpp"
namespace hackynavi { namespace coldaux {
constexpr size_t kVcnInstances=2,kVcnRegisters=8,kJpegRegisters=5,kLsdmaRegisters=12;
struct Layout {
    bool valid; uint64_t barBytes;
    uint32_t sos,vcnPower[kVcnInstances],vcnPowerConfig[kVcnInstances],vcn[kVcnInstances][kVcnRegisters];
    uint32_t jpeg[kJpegRegisters],lsdma[kLsdmaRegisters];
};
enum class Status { Ok,InvalidArgument,DiscoveryRejected,UnsupportedLayout,OutsideBar,
    ReadFailed,DeviceUnavailable,PriorDriverRequiresReset,PowerUnavailable,
    VcnNotStopped,JpegNotStopped,LsdmaNotIdle };
struct Io { void* context; bool (*read32)(void*,uint32_t,uint32_t&); };
struct Result { Status status; uint32_t reads,lastRegister,lastValue,expected,mask;
    uint8_t instance,poweredOffMask; }; // bit0 VCN0, bit1 VCN1, bit2 JPEG0
// Exact Navi31 topology from validated discovery: UVD0 4.0.0 variant0 and
// UVD1 4.0.1 variant1, LSDMA6.0.0, MP0 13.0.0. JPEG4 uses UVD0's registers.
// Resolves all direct BAR offsets before any read. No caller-supplied claim
// that media is uninitialized is accepted.
Status buildLayout(const uint8_t*,size_t,uint64_t barBytes,Layout&);
// Diagnostic register-access prerequisite, deliberately separate from the
// stopped-engine Result. Linux amdgpu_vcn_dump_ip_state reads POWER_STATUS
// (Layout::vcn[instance][0]) first and omits the other registers when bit0,
// UVD_POWER_STATUS_TILES_OFF, is set. That software-written bit is not a
// physical power-completion or DMA-retirement certificate.
enum class PowerInventoryStatus { NotAttempted,ReadAccessObserved,InvalidArgument,
    ReadFailed,DeviceUnavailable,Changing,TilesOff };
struct PowerProbe {
    uint32_t reg,raw[2];
    uint8_t instance,attemptedMask,readSucceededMask,validMask;
};
struct PowerInventory {
    PowerInventoryStatus status;
    PowerProbe probes[kVcnInstances];
    uint32_t reads,lastRegister,lastValue;
    bool lastReadSucceeded;
    uint8_t failedInstance;
};
// At most two consecutive POWER_STATUS samples per instance; no SOS, PGFSM,
// client-window reads, writes, or power commands. Stop immediately on failed
// transport, all ones, or unequal samples. A coherent TILES_OFF pair does not
// prevent observing the other instance's always-readable POWER_STATUS, but
// does prevent ReadAccessObserved. validMask marks readable
// non-all-ones values; readSucceededMask also records a successful all-ones
// transaction. Unattempted/failed raw values are zero, not observed hardware.
// ReadAccessObserved means only both pairs matched with bit0 clear. It gives
// no stopped result or continuing hardware authority. The native caller must
// retain its actual PCI/PM/register capture gate through any subsequent reads.
PowerInventoryStatus inventoryVcnPower(const Layout&,const Io&,PowerInventory&);
const char* statusString(PowerInventoryStatus);
// Read-only cold-path admission, two complete hardware observations bracketed
// by zero PSP sOS. Require VCN reset/stall/ring-disable and actual LMI clean
// signals, JPEG JMI reset/no-fetch/job-done/clean, and LSDMA ring/IB/poll-disable,
// PIO completion plus engine/copy/translation drain signals. Exact completed
// PGFSM OFF plus no pending ON request is admitted only for this cold path;
// gated tile client windows are never read. The PGFSM register itself may be
// unavailable when the enclosing media domain is off; UINT32_MAX still fails
// admission and does not prove that the engine is stopped. Linux powers that
// domain through SMU before accessing PGFSM; this read-only checker does not.
// Other media must match a documented powered
// register-access configuration before each affected read, with DPG disabled.
// Both passes and final power checks must agree. This does not stop active
// engines or wake domains; no reset or assumed default is substituted here.
// Caller holds the actual exclusive PCI/PM/reset/register/submission domain
// and keeps these uninitialized engines stopped/OFF while replacing translation
// or allocating memory. Ok alone is neither that exclusion nor a reservation
// inventory. Display/DMUB, CP/SDMA, memory training and PSP are separate owners.
Status checkStopped(const Layout&,const Io&,Result&);
const char* statusString(Status);
// Deliberately distinct from Result/Status: an LSDMA-only observation cannot
// be passed to a consumer requiring a full media stop result. No VCN/JPEG
// power, ring, or client register is read, even if its window is unavailable.
enum class LsdmaStatus { Ok,InvalidArgument,ReadFailed,DeviceUnavailable,
    PriorDriverRequiresReset,LsdmaNotIdle };
struct LsdmaResult {
    LsdmaStatus status;
    uint32_t reads,lastRegister,lastValue,expected,mask;
    uint8_t pass; // 0/1 observations;2 is the final sOS guard,0xff before reads.
};
// Validate the same exact discovery Layout, then observe all twelve LSDMA
// fields twice with zero sOS before each pass and after the second pass.
// This preserves the cold-only sOS policy: nonzero requires a separate reset
// path and all-ones remains unavailable. Linux soc21_need_reset_on_init uses
// sOS for prior-driver classification, not as proof of global DMA retirement.
// The LSDMA fields are exactly those already required by checkStopped.
//
// This read-only API neither stops LSDMA nor polls an active engine to idle.
// Ok means only that these LSDMA enable/completion/drain fields matched at
// both observations within the caller's exclusive PCI/PM/reset/register epoch.
// It supplies no media proof, reservation inventory, first-attach authority,
// persistent exclusion, or permission to release previously published memory.
LsdmaStatus checkLsdmaStopped(const Layout&,const Io&,LsdmaResult&);
const char* statusString(LsdmaStatus);
} }
#endif
