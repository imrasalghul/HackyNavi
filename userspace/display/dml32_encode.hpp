// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DML32_ENCODE_HPP
#define HACKYNAVI_DML32_ENCODE_HPP
#include "dml32_plan.h"
#include "hackynavi/display_bandwidth.hpp"
namespace hackynavi { namespace displaycalculation {
// IDs refer to retained calculation records; they are not hardware authority.
// status must be the result of hn_dml32_calculate, never inferred from a
// zero-initialized result (DML_VALIDATION_OK itself has value zero).
struct Calculation {
    displaybandwidth::Identity identity;
    hn_dml_status status;
    const hn_dml_plan* result;
};
struct Binding {
    uint32_t dccgReferenceKHz,dchubReferenceKHz,timerDivider;
    uint8_t pipe;
};
// Round required clock minima UP, preserving sub-MHz requirements. DRAM is
// expressed as ktransfers/s, not UCLK; the native SMU owner applies /16.
struct Clocks {
    uint32_t dispKHz,dppKHz,dcfKHz,socKHz,fabricKHz,deepSleepDcfKHz;
    uint32_t dramKTransfersPerSecond,referenceDtbKHz,voltageLevel;
    bool dtbRequired;
};
struct TransitionSupport {
    clock_change_support dram;
    dm_fclock_change_support fabric;
    bool retraining;
};
struct SurfaceMode {
    uint32_t width,height,pitchPixels,surfaceWidth,surfaceHeight,viewportX,viewportY;
    uint32_t htotal,vtotal,hblankStart,hblankEnd,vblankStart,vblankEnd,vfrontPorch;
    uint64_t pixelClockHz;
    int outputType,dpRate,dpLanes;
};
struct Encoding {
    displaybandwidth::Input registers;
    Clocks timingClocks,watermarkClocks[4];
    TransitionSupport transitions[4];
    SurfaceMode mode;
};
enum class Status { Ok,InvalidCalculation,StaleCalculation,ModeMismatch,
    UnsupportedProfile,InvalidReferenceClock,NumericOverflow,InvalidRegisterPlan };
// In-process userspace bridge to the integer kernel plan, not an IPC ABI.
// All four supplied sets are independently identified real calculations;
// this function does not choose firmware watermark policies or duplicate A.
// The native owner must retain/validate the full mode and clock records and
// select the correct watermark state under its operational gate. Nothing
// here authorizes MMIO, display DMA, pstate switching or hardware activation.
Status encode(const Calculation& timing,const Calculation (&sets)[4],
              const Binding&,Encoding&);
} }
#endif
