// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DISPLAY_DENTIST_HPP
#define HACKYNAVI_DISPLAY_DENTIST_HPP
#include "display_model_input.hpp"
namespace hackynavi { namespace displaydentist {
enum class Clock : uint8_t { Disp, Dpp, DpRef, Dcf, Dtb };
constexpr size_t kClockCount=5;
constexpr uint32_t kAllClocks=(1u<<kClockCount)-1;
enum class Status { Ok,InvalidArgument,UnsupportedClock,UnsupportedDivider,
    UnrepresentableFrequency,DiscoveryRejected,OutsideBar,Revoked,Timeout,
    ClockWentBackwards,ReadFailed,DeviceUnavailable,ChangedProgramming,ReferenceMismatch };
struct Preset {
    uint32_t requiredKHz,requestPresetKHz,programmedKHz,scaledDivider;
    uint16_t requestMHz;
    uint8_t dividerId;
};
// Source DCN3.2 integer preset rounding, then DCE/DCN20's piecewise DID
// conversion. requestMHz is the source-required FLOOR, not ceil(pixel clock).
// programmedKHz is a prediction from the DID, not an observed clock. No
// saturation of requests above the VCO/2 or 1950MHz hardware profile ceiling.
Status plan(uint32_t programmedVcoKHz,uint32_t requiredKHz,Preset&);
// Exact supported DID only. Unlike upstream diagnostic clamping, invalid
// input must never acquire a plausible frequency. Divider is scaled by four.
Status decodeDivider(uint32_t dividerId,uint32_t& scaledDivider);
struct Layout {
    bool valid;uint64_t barBytes;
    uint32_t pll,dentist,dfs[kClockCount];
};
Status buildLayout(const uint8_t* discovery,size_t bytes,uint64_t barBytes,Layout&);
struct Observation {
    uint32_t pll,dentist,rawDfs[kClockCount],programmedKHz[kClockCount],validClockMask;
    uint32_t programmedVcoKHz;
    bool dispChangeDone,dppChangeDone,dispDividerAgrees;
};
struct Result { Status status;uint32_t reads,lastRegister,lastValue;uint64_t elapsedUs; };
struct Io {
    void* context;bool (*valid)(void*);
    bool (*read32)(void*,uint32_t,uint32_t&);uint64_t (*timeUs)(void*);
};
// Two identical complete register snapshots under one elapsed deadline.
// Requires exact same PLL programming as the retained reference observation.
// Unknown/gated DID values remain raw with no frequency; only requiredMask's
// clocks must decode. All-ones reads always fail. No writes or counters, and
// no claim of measured running frequency, fulfilled minimum or enduring hold.
// Native caller supplies real normal scope/GART access and serialized owners.
Status capture(const Layout&,const displaymodel::BoardInfo&,
    const displaymodel::ReferenceInfo&,uint32_t requiredMask,const Io&,
    uint32_t timeoutUs,Observation&,Result&);
} }
#endif
