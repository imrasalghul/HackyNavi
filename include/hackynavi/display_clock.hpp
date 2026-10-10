// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DISPLAY_CLOCK_HPP
#define HACKYNAVI_DISPLAY_CLOCK_HPP
#include "discovery.hpp"
namespace hackynavi { namespace displayclock {
enum class Register : uint8_t { PixelDiv,DtoControl,DtoParam,OtgControl,HubpControl,Count };
constexpr size_t kRegisterCount=size_t(Register::Count),kPipeCount=4;
struct Layout { bool valid;uint64_t barBytes;uint32_t registers[kPipeCount][kRegisterCount]; };
struct Io { void* context;bool (*read32)(void*,uint32_t,uint32_t&);bool (*write32)(void*,uint32_t,uint32_t); };
struct Config { uint32_t referenceDppClockKHz,requiredDppClockKHz;uint8_t k1Divisor,k2Divisor; };
struct Plan { uint32_t dtoParam,pixelDivBits,minimumOutputKHz;uint8_t phase; };
enum class Status { Ok,InvalidArgument,DiscoveryRejected,UnsupportedDmu,OutsideBar,ActivePipe,
    ForeignControl,TransportError,DeviceUnavailable,VerifyFailed,InvalidState };
struct Result { Status status;uint32_t reads,writes,lastRegister,lastValue;bool writeAttempted; };
Status plan(const Config&,Plan&);
Status buildLayout(const uint8_t*,size_t,uint64_t,Layout&);
// Reference frequency is the actual global DPP clock established by the
// clock/SMU owner, never an EDID-derived guess. Caller holds global DCCG/PM
// exclusion and has exclusively disconnected this DPP from every MPC output.
// Requires powered, blank/drained HUBP and stopped OTG. Preserves neighboring
// pipes' fields in shared registers. Does not set global clock or ungate DPP.
class Controller {
public:
    Controller();
    Status configure(const Layout&,const Io&,uint8_t pipe,const Config&,Result&);
    // Requires HUBP drained/OTG stopped again. Restores the acquired per-pipe
    // divider/parameter fields after disabling DTO; no DMA retirement claim.
    Status stop(Result&);
    bool requiresHardwareRetirement() const{return touched_;}
    void clearAfterVerifiedReset();
private:
    Controller(const Controller&)=delete;Controller& operator=(const Controller&)=delete;
    Layout layout_;Io io_;uint8_t pipe_;bool touched_,configured_;
    uint32_t savedParam_,savedDiv_;
    Status finish(Status,Result&);
    Status read(Register,uint32_t&,Result&);
    Status update(Register,uint32_t,uint32_t,Result&);
    Status idle(Result&);
};
} }
#endif
