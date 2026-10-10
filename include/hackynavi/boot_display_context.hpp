// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_BOOT_DISPLAY_CONTEXT_HPP
#define HACKYNAVI_BOOT_DISPLAY_CONTEXT_HPP
#include "hackynavi/boot_display.hpp"
namespace hackynavi { namespace bootdisplaycontext {
constexpr size_t kPipeCount=4,kPipeRegisters=6,kVmRegisters=8,kRegisterCount=32,kPasses=2;
enum class PipeRegister : uint8_t { VtgControl,OtgControl,OtgClock,HubpClock,MallConfig,MallSubVp };
enum class VmRegister : uint8_t { AgpBase,Control,BaseHigh,BaseLow,StartHigh,StartLow,EndHigh,EndLow };
constexpr size_t pipeIndex(size_t p,PipeRegister r){return p*kPipeRegisters+size_t(r);}
constexpr size_t vmIndex(VmRegister r){return kPipeCount*kPipeRegisters+size_t(r);}
enum class Status { Ok,InvalidArgument,DiscoveryRejected,UnsupportedDmu,OutsideBar,
                    AccessDenied,ReadFailed,DeviceUnavailable,PowerUnavailable,Changed };
struct Layout {
    bool valid;uint64_t barBytes;
    uint32_t power[kPipeCount],powerConfig[kPipeCount],registers[kRegisterCount];
};
struct Io {
    void* context;
    // Recheck actual native mapping/lifetime/revocation permission, before and
    // after every read, including the always-on power guards. No GPU write.
    bool (*valid)(void*);
    bool (*read32)(void*,uint32_t byteOffset,uint32_t&);
};
struct Capture {
    uint32_t values[kPasses][kRegisterCount];
    uint32_t attemptedMask[kPasses],readSucceededMask[kPasses],validMask[kPasses];
    // Changed bits compare only registers valid in both passes. All raw
    // differences count, including live status bits; no programming inference.
    uint32_t changedMask;
    uint32_t powerStatus[kPasses][kPipeCount],powerConfig[kPasses][kPipeCount];
    uint8_t powerValidMask[kPasses],powerChangedMask;
    uint8_t completedPasses,failedPass,failedPipe,failedRegisterIndex;
    bool readsComplete; // Both complete passes + final guards, even if Changed.
    uint32_t readsAttempted,lastRegister,lastValue;
    bool lastReadSucceeded; // Callback succeeded; includes all-ones/permission loss.
    bootdisplay::PowerProbe lastPowerProbe;
};
// Same exact DMU3.2 discovery selection as bootdisplay::buildLayout. No MMIO.
// Offsets are resolved from BASE_IDX2, bounded and unique before capture.
// Output is zero on failure. Input/output must not overlap.
Status buildLayout(const uint8_t* discovery,size_t bytes,uint64_t barBytes,Layout&);
// Raw diagnostic inventory only: no mode classification, reservations, engine
// readiness, memory ownership, or physical-address-domain interpretation.
// At most208 MMIO reads: two fixed passes with actual ON power triplets before
// every HUBP read, after each pipe, and at both ends of each pass. Any OFF or
// transitioning/unknown power state aborts; no gated window is skipped as zero.
// OTG/VTG control and clock state are the registers Linux DCN3.2 reads while
// inspecting/stopping inherited timing generators; VM words are in the same
// ungated DCN control aperture as FB/AGP globals. The caller must supply safely
// readable initialized DCN controls and serialize its device removal/reset/PM
// and mapping lifetime. Guards detect observed changes; they cannot lock out
// firmware or other programmers. Even Ok/unchanged data is historical, never
// authority to skip a requester or adopt a firmware address mode.
// Values remain zero unless a callback succeeded. readSucceededMask includes
// all-ones and post-read permission loss; validMask excludes either. A transport
// error/all-ones/revocation aborts globally, retaining partial masks/raw values.
// Input, output, callback state must be disjoint and stable during capture.
Status capture(const Layout&,const Io&,Capture&);
const char* statusString(Status);
} }
#endif
