// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_BOOT_HDMI_STREAM_HPP
#define HACKYNAVI_BOOT_HDMI_STREAM_HPP
#include "hackynavi/boot_display_backend.hpp"
namespace hackynavi { namespace boothdmistream {
enum class Domain : uint8_t { Backend,Hdmi,Afmt,Vpg,Timing };
enum class Register : uint8_t {
    AudioClock,Metadata,AudioDelay,Info0,Info1,Acr,Generic0,Generic6,Immediate,
    Line1,Line2,Line3,Line4,Line7,Line8,Line9,Line10,DoubleBuffer,
    AudioPacket,AudioPacket2,AudioInfo,AudioSource,AudioVbi,AudioMemory,
    VpgFrame,VpgImmediate,VpgStatus,VpgMemory,AudioStatus,HdmiStatus,Count
};
constexpr size_t kRegisters=size_t(Register::Count);
struct Io {
    bootbackend::Io backend;
    // Real lexical same-device ownership/exclusion, fresh D0/address access
    // and total elapsed deadline must be enforced before/after every access.
    bool (*permit)(void*,Domain,uint8_t instance,uint32_t byteOffset,bool write);
    bool (*write32)(void*,uint32_t byteOffset,uint32_t);
    bool (*delayUs)(void*,uint32_t);
};
struct Poll { uint32_t iterations,intervalUs; }; // Shared <=700ms delay budget.
enum class Phase { Fresh,Armed,MutedRetained,Faulted };
enum class Status { Ok,InvalidArgument,InvalidState,DiscoveryRejected,CaptureRejected,
    UnsupportedState,ClockUnavailable,AccessDenied,ReadFailed,WriteFailed,
    DeviceUnavailable,Changed,TimingStopped,ContradictoryStatus,Timeout };
struct Result {
    Status status;bootbackend::Result backend;
    uint32_t reads,writes,polls,lastRegister,lastValue;
    uint8_t attempted,acknowledged,cycles; // bit0 AVMUTE, bit1 AFMT sample mute
    bool lastReadSucceeded,retentionRequired;
};
// First contained inherited-stream phase. This is deliberately NOT connected
// to a native mutation entrypoint: the real native owner must first acquire
// sink/quirk, audio and PSP/HDCP exclusion. No bool/token can attest them here.
// The sampled registers do not prove HDCP inactive. Native admission remains
// unavailable until that owner exists, even when this protocol returns Ok.
class Controller {
public:
    Controller();
    // Heap-own before any MPC disconnect. Captures a fresh actual route and
    // all selected packet/audio controls, retaining two matching raw passes.
    Status capture(const uint8_t* discovery,size_t discoveryBytes,uint64_t barBytes,
        const uint8_t* rom,size_t romBytes,size_t imageOffset,const Io&,Result&);
    // Assert AVMUTE, observe VACTIVE then THREE VBLANK->VACTIVE cycles,
    // verify actual active AVMUTE, then clear immediate AFMT sample-send.
    // No clock/power/routing/packet-scheduler writes and no release authority.
    Status mute(const Poll&,Result&);
    Phase phase() const {return phase_;}
    bool retentionRequired() const {return phase_!=Phase::Fresh;}
    const bootbackend::Capture& observation() const {return route_;}
    uint32_t observed(Register r) const {return size_t(r)<kRegisters?initial_[size_t(r)]:0;}
private:
    Controller(const Controller&)=delete;Controller& operator=(const Controller&)=delete;
    Status finish(Status,Result&);Status fail(Status,Result&);
    bool read(Domain,uint8_t,uint32_t,uint32_t&,Result&);
    bool clock(Result&);bool guard(Result&);bool supported(Result&);
    bool write(Domain,uint32_t,uint32_t,uint8_t,Result&);
    bool waitState(uint32_t,const Poll&,uint32_t&,Result&);
    uint32_t backendAddress(bootbackend::Bank,uint8_t,uint8_t) const;
    Phase phase_;Io io_;bootbackend::Capture route_;
    uint64_t pipeBase_,mpcBase_;uint32_t addresses_[kRegisters],initial_[kRegisters],expected_[kRegisters];
    uint32_t statusAddress_,generalAddress_,expectedGeneral_;
    uint8_t attempted_,acknowledged_,cycles_;bool avmuteSettled_;
};
const char* statusString(Status);
} }
#endif
