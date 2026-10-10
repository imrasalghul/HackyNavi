// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DISPLAY_IDLE_HPP
#define HACKYNAVI_DISPLAY_IDLE_HPP
#include "discovery.hpp"
namespace hackynavi { namespace displayidle {
enum class Register : uint8_t { OtgControl,OtgClock,HubpControl,MpcTop,MpcBottom,
    MpcOpp,MpcStatus,MpcOutput,MpcLock,HubpPowerConfig,HubpPowerStatus,Count };
constexpr size_t kPipeCount=4,kRegisterCount=size_t(Register::Count);
struct Layout { bool valid;uint64_t barBytes;uint32_t registers[kPipeCount][kRegisterCount]; };
struct Io { void* context;bool (*read32)(void*,uint32_t,uint32_t&); };
enum class Status { Ok,InvalidArgument,DiscoveryRejected,UnsupportedDmu,OutsideBar,
    ReadFailed,DeviceUnavailable,ActiveTiming,OutstandingFetch,ConnectedCombiner,BusyCombiner,ForeignLock,
    PowerUnavailable,PowerChanged };
struct Result { Status status;uint32_t reads,lastRegister,lastValue;uint8_t pipe;Register reg;
    uint8_t poweredOffMask; };
Status buildLayout(const uint8_t*,size_t,uint64_t barBytes,Layout&);
// Bounded read-only admission for shared display-clock changes. Checks all four
// OTGs stopped/not busy, powered HUBPs blank/drained, and MPC inputs/outputs
// disconnected and idle with no update lock. Settled DOMAIN0..3 power-off is
// observed through status/config/status reads; skip ONLY that HUBP's control
// read because a gated HUBP can read as zero. OTG/MPC checks are still required.
// Transitional/inconsistent power requests and changes during checking reject.
// Does not quiesce anything or prove full GPU DMA
// retirement. Caller holds global display/DMCUB/PM exclusion from before these
// reads through the subsequent clock operation; the result is not a reusable
// token and must not be cached across unlocks or device/reset epochs.
Status verify(const Layout&,const Io&,Result&);
} }
#endif
