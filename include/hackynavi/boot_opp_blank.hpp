// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_BOOT_OPP_BLANK_HPP
#define HACKYNAVI_BOOT_OPP_BLANK_HPP
#include "hackynavi/boot_display_backend.hpp"
namespace hackynavi { namespace bootopp {
enum class Register : uint8_t { Control,Dimensions,Offset,Blue,Green,Red,Pending,
    Clock,Format,Horizontal,Vertical,TimingControl,TimingClock,Count };
constexpr size_t kRegisters=size_t(Register::Count);
struct Io {
    bootbackend::Io backend;
    // Same real operation/context as backend. The native owner must exclude
    // firmware, modeset, clock, reset and PM writers for this entire call.
    bool (*canAccess)(void*,Register,uint8_t instance,uint32_t offset,bool write);
    bool (*write32)(void*,uint32_t offset,uint32_t value);
    void (*delayUs)(void*,uint32_t);
};
struct Poll { uint32_t iterations,intervalUs; }; // total delay <=100ms
enum class Status { Ok,InvalidArgument,InvalidState,DiscoveryRejected,OutsideBar,
    CaptureRejected,AccessDenied,ReadFailed,WriteFailed,DeviceUnavailable,
    UnsupportedState,Changed,ReadbackMismatch,Timeout };
enum class Phase { Fresh,Blanking,Blanked,Faulted };
struct Result {
    Status status;bootbackend::Result backend;
    uint32_t reads,writes,polls,lastRegister,lastValue,width,height;
    uint8_t opp,timing;bool writesMayHaveOccurred,blanked,lastReadSucceeded;
};
// Inherited single-OPP RGB HDMI profile. This performs its own real backend
// capture, reads active dimensions from OTG, and programs/polls OPP solid black.
// It does not blank/drain HUBP, detach MPC, stop HDMI/OTG, gate clocks, retire
// memory or authorize a later clock transition. No native caller is supplied
// until a real inherited-display owner provides the exclusion above.
class Controller {
public:
    Controller();
    Status blank(const uint8_t* discovery,size_t discoveryBytes,uint64_t barBytes,
        const uint8_t* rom,size_t romBytes,size_t imageOffset,const Io&,const Poll&,Result&);
    Phase phase() const { return phase_; }
    bool writesMayHaveOccurred() const { return touched_; }
private:
    Controller(const Controller&)=delete;Controller& operator=(const Controller&)=delete;
    Phase phase_;bool touched_;Io io_;uint32_t addresses_[4][kRegisters];
    uint32_t expected_[kRegisters];bootbackend::Capture before_,after_;
    Status finish(Status,Result&);
    Status read(Register,uint32_t&,Result&);
    Status guard(Result&);
    Status write(Register,uint32_t,Result&);
    static bool backendValid(void*);
    static bool backendCanRead(void*,bootbackend::Bank,uint8_t,uint32_t);
    static bool backendRead(void*,uint32_t,uint32_t&);
};
const char* statusString(Status);
} }
#endif
