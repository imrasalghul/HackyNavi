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
// Read-only cold-path admission, two complete hardware observations bracketed
// by zero PSP sOS. Require VCN reset/stall/ring-disable and actual LMI clean
// signals, JPEG JMI reset/no-fetch/job-done/clean, and LSDMA ring/IB/poll-disable,
// PIO completion plus engine/copy/translation drain signals. Exact completed
// PGFSM OFF plus no pending ON request is admitted only for this cold path;
// gated windows are never read. Other media must match a documented powered
// register-access configuration before each affected read, with DPG disabled.
// Both passes and final power checks must agree. This does not stop active
// engines or wake domains; no reset or assumed default is substituted here.
// Caller holds the actual exclusive PCI/PM/reset/register/submission domain
// and keeps these uninitialized engines stopped/OFF while replacing translation
// or allocating memory. Ok alone is neither that exclusion nor a reservation
// inventory. Display/DMUB, CP/SDMA, memory training and PSP are separate owners.
Status checkStopped(const Layout&,const Io&,Result&);
const char* statusString(Status);
} }
#endif
