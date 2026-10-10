// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DISPLAY_POWER_HPP
#define HACKYNAVI_DISPLAY_POWER_HPP
#include "discovery.hpp"
namespace hackynavi { namespace displaypower {
constexpr size_t kPipeCount=4;
enum class Register : uint8_t {
    PowerConfig,PowerStatus,RequestControl,DtoControl,OtgControl,OtgClock,
    HubpControl,HubpClock,DppControl,CursorControl,DmdataControl,
    MpcTop,MpcBottom,MpcOpp,MpcStatus,MpcOutput,MpcLock,Count
};
constexpr size_t kRegisterCount=size_t(Register::Count);
struct Layout { bool valid;uint64_t barBytes;uint32_t registers[kPipeCount][kRegisterCount]; };
struct Io { void* context;bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t);void (*delayUs)(void*,uint32_t); };
struct Poll { uint32_t iterations,intervalUs; }; // Nonzero, <=100ms total.
enum class Status { Ok,InvalidArgument,DiscoveryRejected,UnsupportedDmu,OutsideBar,
    InvalidState,TransportError,DeviceUnavailable,PowerChanged,UnsettledPower,
    ActiveTiming,OutstandingFetch,ActiveClock,ConnectedCombiner,BusyCombiner,
    ForeignControl,VerifyFailed,Timeout };
enum class Stage { None,Admission,EnableRequest,GateCommand,WaitPower,RestoreRequest,FinalVerify };
enum class Phase { Fresh,Powered,Off,Faulted };
struct Result { Status status;Stage stage;uint32_t reads,writes,polls,lastRegister,lastValue;
    bool writeAttempted,retentionRequired,powerVerified,powered; };
Status buildLayout(const uint8_t*,size_t,uint64_t barBytes,Layout&);

// Real DCN3.2 DOMAIN0..3 transitions for HUBP0..3. There is no separate DPP
// PG callback in the DCN3.2 sequencer; this does not invent older odd domains.
// Caller exclusively owns PCI/MMIO, device PM/reset/removal, display firmware,
// every IP_REQUEST_EN requester and the selected HUBP/DPP/MPC/OPP/OTG route
// throughout each operation and the powered lifetime. This controller neither
// obtains that exclusion nor enables/stops clocks, pipes, firmware or DMA.
//
// Supports the same-index, one-HUBP/one-DPP/one-MPC/one-OPP/one-OTG route.
// Before either transition, callers disconnect it and finish pending routing
// updates; reads reject live selected routes and references from other MPCs.
// Power-off additionally requires caller-retired fetch clients, blank/drained
// HUBP, disabled cursor/DMDATA, stopped HUBP/DPP clocks and disabled DPP DTO.
// A successful power-off retires ONLY this domain, not other display clients,
// global GPU DMA, writeback, firmware or CPU mappings. No memory is freed here.
class Controller {
public:
    Controller();
    // Adopts an already-on, quiescent domain or powers up a settled off one.
    // Gated HUBP-window registers are never read; freshly enabled HUBP state
    // still needs real clock, request/deadline and surface initialization.
    Status powerOn(const Layout&,const Io&,uint8_t pipe,const Poll&,Result&);
    Status powerOff(const Poll&,Result&);
    Phase phase() const{return phase_;}
    bool powered() const{return phase_==Phase::Powered;}
    bool faulted() const{return phase_==Phase::Faulted;}
    bool requiresHardwareRetirement() const{return owned_||touched_;}
    // An ambiguous write/transition retains ownership and prohibits retries.
    // Only an independently verified device reset/retirement may clear it.
    void clearAfterVerifiedReset();
private:
    Controller(const Controller&)=delete;Controller& operator=(const Controller&)=delete;
    Layout layout_;Io io_;uint8_t pipe_;Phase phase_;bool owned_,touched_;
    Status finish(Status,Result&);
    Status fail(Status,Result&);
    Status read(uint8_t,Register,uint32_t&,Result&);
    Status write(Register,uint32_t,Result&);
    Status observePower(uint32_t&,uint32_t&,Result&);
    Status samePower(uint32_t,uint32_t,Result&);
    Status idle(bool hubpOn,Result&);
    Status request(bool original, bool enable,Result&);
    Status transition(bool,uint32_t,uint32_t,const Poll&,Result&);
};
} }
#endif
