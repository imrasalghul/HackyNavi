// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_BOOT_DISPLAY_BACKEND_HPP
#define HACKYNAVI_BOOT_DISPLAY_BACKEND_HPP
#include "hackynavi/dmcub.hpp"
namespace hackynavi { namespace bootbackend {
constexpr size_t kLinks=5,kPipes=4;
enum class Bank : uint8_t { Link, Frontend, Timing, Combiner };
enum class LinkRegister : uint8_t { Mux,Control,Enable,Count };
enum class FrontendRegister : uint8_t { Control,Fifo,DpStream,Hdmi,Vbi,General,Count };
enum class TimingRegister : uint8_t { Control,Clock,Lock,Global,Gsl,Stereo,Structure,
    Interlace,InputGlobal,DataSource,DataFormat,Memory,Count };
enum class CombinerRegister : uint8_t { Top,Bottom,Opp,Lock,Status,Output,Count };
constexpr size_t kLinkRegisters=size_t(LinkRegister::Count);
constexpr size_t kFrontendRegisters=size_t(FrontendRegister::Count);
constexpr size_t kTimingRegisters=size_t(TimingRegister::Count);
constexpr size_t kCombinerRegisters=size_t(CombinerRegister::Count);
constexpr size_t kSamplesPerPass=kLinks*(kLinkRegisters+kFrontendRegisters)+
    kPipes*(kTimingRegisters+kCombinerRegisters);
struct Io {
    void* context;
    // Actual lexical Operation, ownership/exclusion and elapsed deadline.
    bool (*valid)(void*);
    // Mandatory native address/access-domain permission, checked before and
    // after each access. A permissive copied boolean is not a native owner.
    bool (*canRead)(void*,Bank,uint8_t instance,uint32_t barByteOffset);
    bool (*read32)(void*,uint32_t barByteOffset,uint32_t&);
};
enum class Status { Ok,InvalidArgument,DiscoveryRejected,UnsupportedDmu,OutsideBar,
    AliasedRegisters,RomRejected,UnsupportedRoute,AccessDenied,ReadFailed,
    DeviceUnavailable,Changed,UnsupportedMux,UnsupportedSignal,SharedRoute,
    Inactive,Unsettled,UnsupportedTiming,UnsupportedCombiner };
struct Capture {
    bool valid;
    uint32_t link[kLinks][kLinkRegisters],frontend[kLinks][kFrontendRegisters];
    uint32_t timing[kPipes][kTimingRegisters],combiner[kPipes][kCombinerRegisters];
    uint32_t linkReadMask[kLinks],frontendReadMask[kLinks],timingReadMask[kPipes],combinerReadMask[kPipes];
    dmcub::Route route;
    uint8_t path,backend,frontendIndex,timingIndex,opp,mpcc,dpp;
    bool scrambled,quarterRate;uint8_t bitsPerComponent;
};
struct Result {
    Status status;uint32_t reads,lastRegister,lastValue;
    Bank bank;uint8_t instance,reg,pass;bool lastReadSucceeded;
};
// Read-only, fixed two-pass observation. Derives register locations from the
// supplied real DMU discovery and the physical route from checksummed Atom ROM.
// First profile: exactly one legacy HDMI-TMDS RGB route, one OTG/OPP/MPCC/DPP;
// no FRL/HPO, DP, DSC, ODM, stereo, update lock or shared route. All sampled raw
// values must match across passes. This does NOT establish stop, clock, DMA,
// sink state, firmware exclusion or inherited controller ownership. It has no
// write callback. Partial raw data/masks are diagnostic only; valid stays false.
Status capture(const uint8_t* discovery,size_t discoveryBytes,uint64_t barBytes,
    const uint8_t* rom,size_t romBytes,size_t imageOffset,const Io&,Capture&,Result&);
const char* statusString(Status);
} }
#endif
