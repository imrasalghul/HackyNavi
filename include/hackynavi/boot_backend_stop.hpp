// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_BOOT_BACKEND_STOP_HPP
#define HACKYNAVI_BOOT_BACKEND_STOP_HPP
#include "hackynavi/boot_display_backend.hpp"
#include "hackynavi/boot_display_stop.hpp"
namespace hackynavi { namespace bootbackendstop {
enum class Domain : uint8_t { Backend, Global, HubpPower, Hubp, Vtg, Dpg };
struct Io {
    bootbackend::Io backend;
    // Native owner validates the exact discovery-derived address, access
    // domain, same device/epoch, fresh PCI D0 and all competing programmers.
    // This is an access callback, never a copied success/ownership flag.
    bool (*permit)(void*,Domain,uint8_t instance,uint32_t byteOffset,bool write);
    bool (*write32)(void*,uint32_t byteOffset,uint32_t);
    bool (*delayUs)(void*,uint32_t);
};
struct Poll { uint32_t iterations,intervalUs; }; // One shared <=150ms budget.
enum class Phase { Fresh,Armed,MpcDetached,TimingStopped,Faulted };
enum class Status { Ok,InvalidArgument,InvalidState,DiscoveryRejected,CaptureRejected,
    DrainRequired,AccessDenied,ReadFailed,WriteFailed,DeviceUnavailable,Changed,
    OPPNotBlank,TimingNotRunning,LinkNotStopped,Timeout };
struct Result {
    Status status;bootbackend::Result backend;
    uint32_t reads,writes,polls,lastRegister,lastValue;
    uint16_t attempted,acknowledged; // Bits0..4 MPC; bits5..8 ODM/OTG/VTG.
    bool lastReadSucceeded,retentionRequired;
};
class Controller {
public:
    Controller();
    // Heap-own this object. No caller-populated backend capture is accepted.
    // The real HUBP State must stay alive/immutable, come from this same native
    // owner/epoch and have completed stop(). All four HUBP domains are freshly
    // checked; no inferred HUBP-index -> DPP-index correspondence is used.
    // The selected OPP must already replace pixel input (DPG_EN=1, !pending).
    Status capture(const uint8_t* discovery,size_t discoveryBytes,uint64_t barBytes,
        const uint8_t* rom,size_t romBytes,size_t imageOffset,const Io&,
        const bootdisplaystop::State&,Result&);
    // Exactly one source-backed MPC disconnect, with TG/VTG and clocks live.
    // Mux readback alone is insufficient: actual MPCC_IDLE&&!BUSY is required.
    Status disconnect(const Poll&,Result&);
    // Separate terminal timing step AFTER the outer real HDMI teardown owner
    // has stopped packets/audio/SCDC/firmware transmitter and FE routing.
    // Physical DIG/FE checks here supplement, never replace, that owner.
    // No native entrypoint is connected until those exclusions are available.
    // Leaves OTG/input/DCCG/PHY clocks and OPP DPG unchanged. Success claims
    // only timing stopped, not PHY, DMA, firmware or memory retirement.
    Status stopTiming(const Poll&,Result&);
    Phase phase() const {return phase_;}
    bool retentionRequired() const {return phase_!=Phase::Fresh;}
    const bootbackend::Capture& observation() const {return initial_;}
private:
    Controller(const Controller&)=delete;Controller& operator=(const Controller&)=delete;
    Status finish(Status,Result&);Status fail(Status,Result&);
    bool read(Domain,uint8_t,uint32_t,uint32_t&,Result&);
    bool power(uint8_t,Result&);bool drained(Result&);
    bool guard(bool allowExternalLink,bool timingTransition,Result&);
    bool write(bootbackend::Bank,uint8_t,uint8_t,uint32_t,uint32_t,uint16_t,Result&);
    bool writeVtg(uint32_t,uint16_t,Result&);
    uint32_t address(bootbackend::Bank,uint8_t,uint8_t) const;
    uint32_t& expected(bootbackend::Bank,uint8_t,uint8_t);
    Phase phase_;Io io_;const bootdisplaystop::State* drain_;
    bootbackend::Capture initial_,expected_;
    bootdisplay::Layout hubp_;uint64_t pipeBase_,mpcBase_;
    uint32_t vtgAddress_,vtg_,dpgAddress_,dpgStatusAddress_,dpg_,dpgStatus_;
    uint16_t attempted_,acknowledged_;
};
const char* statusString(Status);
} }
#endif
