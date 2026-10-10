// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_COLD_DMA_HPP
#define HACKYNAVI_COLD_DMA_HPP
#include "hackynavi/discovery.hpp"
namespace hackynavi { namespace colddma {
enum class IhRegister : size_t { Status, Control0, Control1, Control2, Retry,
    ReadPointer0, WritePointer0, ReadPointer1, WritePointer1, RingStatus,
    MemoryError, Count };
enum class WbRegister : size_t { MemoryPower, HubMemoryPower, HubMemoryControl,
    Reset, HubReset, Enable, Capture, Update, Overflow, BufferControl,
    BufferStatus, Buffer0, Buffer1, Buffer2, Buffer3, Warmup,
    Outstanding0, Outstanding1, Count };
constexpr size_t kIhRegisters=static_cast<size_t>(IhRegister::Count);
constexpr size_t kWbRegisters=static_cast<size_t>(WbRegister::Count);
constexpr size_t kPipes=4;
struct Layout {
    bool valid; uint64_t barBytes;
    uint32_t sos,dmubControl,dmubReset,ih[kIhRegisters],wb[kWbRegisters],mall;
    uint32_t power[kPipes],powerConfig[kPipes],mallConfig[kPipes],mallStatus[kPipes];
    uint32_t mallSubvp[kPipes],vmPageConfig[kPipes];
};
enum class Status { Ok,InvalidArgument,DiscoveryRejected,UnsupportedLayout,OutsideBar,
    ReadFailed,DeviceUnavailable,PriorDriverRequiresReset,FirmwareNotStopped,
    PowerUnavailable,IhNotStopped,WritebackNotStopped,MallNotStopped,UnstableSnapshot };
struct Io { void* context; bool (*read32)(void*,uint32_t byteOffset,uint32_t&); };
struct Result {
    Status status; uint32_t reads,lastRegister,lastValue,mask,expected;
    uint8_t pass,pipe,poweredOffMask;
};
// Exact unharvested die0 OSSSYS6.0.0, DMU3.2.0 and MP0 13.0.0, with
// validated 32-bit discovery DWORD bases. Resolves every BAR byte offset.
Status buildLayout(const uint8_t*,size_t,uint64_t barBytes,Layout&);
// Read-only cold admission after inherited DMCUB has been stopped/reset.
// Two stable observations require disabled IH DMA producers, actual IH idle
// and clean signals, equal ring pointers; disabled DWB capture/writeback,
// inactive MCIF buffers, disabled warmup, and zero WBIF outstanding counters.
// MALL/CAB caches existing scanout data; it is not a separate VRAM save-area
// allocation in this DCN3.2 path. Require global/per-pipe MALL and SubVP off,
// start-line/one-row triggers unarmed, requests inactive, and actual MALL
// outstanding-counter-zero status.
// Stable completed HUBP power-off skips that window, as in posted cold boot
// capture. Every powered HUBP access is bracketed by PGFSM/config checks.
// SOS=0 and DMCUB disabled/in reset bracket both passes. Unknown/transitioning
// power, gated IH/DWB/MCIF SRAM, or held writeback reset fail closed: these are
// not substituted for observable idle/drain. No engine is reset or woken here.
// Caller must hold exclusive PCI/reset/PM/display/firmware/submission ownership
// and keep admitted producers disabled and gated pipes OFF throughout memory
// reuse. MMIO always-on windows must be powered/readable. Matching snapshots
// are not that exclusion, a complete GPU drain proof, or a free-VRAM inventory;
// scanout reservations, CP/SDMA/media, PSP and training remain separate owners.
// Input/output/callback storage must be disjoint and stable for this call.
Status checkStopped(const Layout&,const Io&,Result&);
const char* statusString(Status);
} }
#endif
