// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_PIXEL_PIPELINE_HPP
#define HACKYNAVI_PIXEL_PIPELINE_HPP
#include "hubp_scanout.hpp"
namespace hackynavi { namespace pixelpipeline {
constexpr size_t kPipeCount=4;
using Format=hubpscanout::Format;
enum class Register : uint8_t {
    Power,PowerConfig,HubpControl,HubpClock,HubpRequestDebug,OtgControl,OtgLock,DppControl,FormatControl,
    SurfaceFormat,PreDegamma,PreDealpha,PreRealpha,ColorKey,Cursor,CmControl,
    Autocal,DsclControl,RecoutStart,RecoutSize,MpcSize,ScalerMode,LbFormat,
    LbMemory,LbPower,LbStatus,Top,Bottom,OppId,MpccControl,Stereo,LockSelect,
    TopGain,BottomGainInside,BottomGainOutside,MpccStatus,Ogamma,Shaper,Lut3d,
    OutMux,Denorm,ClampG,ClampB,OutCsc,DwbMux,OppControl,FmtControl,
    FmtDepth,FmtClamp,FmtExpansion,FmtStereo,OppBuffer,Pattern,Count
};
constexpr size_t kRegisterCount=size_t(Register::Count);
struct Layout { bool valid;uint64_t barBytes;uint32_t registers[kPipeCount][kRegisterCount]; };
struct Io { void* context;bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t);void (*delayUs)(void*,uint32_t); };
struct Poll { uint32_t iterations,intervalUs; }; // each wait is bounded to 100ms.
struct Config { uint32_t width,height;Format format;uint8_t outputBpc; };
struct Plan { uint32_t size,surfaceFormat,formatControl,lbMemory,denorm,expansion; };
enum class Status { Ok,InvalidArgument,DiscoveryRejected,UnsupportedDmu,OutsideBar,
    InvalidState,PowerUnavailable,ActivePipe,ForeignControl,TransportError,
    DeviceUnavailable,VerifyFailed,Timeout };
enum class Phase { Closed,Programming,Configured,Stopping,Faulted };
struct Result { Status status;uint32_t reads,writes,polls,lastRegister,lastValue;
    bool writeAttempted,retentionRequired,disconnected,idle; };
Status plan(const Config&,Plan&);
Status buildLayout(const uint8_t*,size_t,uint64_t,Layout&);
// Exclusive caller owns display, power/reset, DMCUB/FAMS/PM and modeset locks.
// Caller has established global DISPCLK/DPPCLK and this pipe's DCCG DTO, and
// powered HUBP DOMAIN[pipe]. DCN3.2 has no separate legacy odd DPP PG callback;
// this controller owns HUBP_CLOCK_ENABLE, DPP_CLOCK_ENABLE and OPP_PIPE_CLOCK_EN,
// never odd PG domains. HUBP clock is enabled before DPP/OPP programming and
// disabled after verified MPCC disconnect/idle, before disabling DPP clock.
// HUBP must remain blank/drained and OTG stopped while configuring/disconnecting.
// After HUBP clock enable this performs the hardware part of hubp3_init:
// DEDCN21-133 HUBPREQ_DEBUG=1<<26 and HUBP_TTU_DISABLE=0. The caller still owns
// memory translation/aperture setup and calculated RQ/DLG/TTU before any fetch;
// these two initialization writes do not synthesize those prerequisites.
// HUBP owns ARGB/ABGR component crossbar: DPP always receives canonical RGB.
// Full-size progressive RGB only; opaque plane, unity scale, full-range 8/10bpc.
// Surface dimensions/format and OTG active area MUST match Config; no bandwidth,
// RQ/DLG/TTU, PLL, link, allocation or DMA-retirement claim is made here.
class Controller {
public:
    Controller();
    Status configure(const Layout&,const Io&,uint8_t pipe,const Config&,const Poll&,Result&);
    // Stops our partial or complete route after a configure fault too. Retains
    // ownership after ANY failed/ambiguous write until successful verified
    // disconnect+MPCC idle+clock off, or an independently verified device reset.
    Status stop(const Poll&,Result&);
    bool requiresHardwareRetirement() const{return touched_;}
    Phase phase() const{return phase_;}
    void clearAfterVerifiedReset();
private:
    Controller(const Controller&)=delete;Controller& operator=(const Controller&)=delete;
    Layout layout_;Io io_;uint8_t pipe_;bool touched_;Phase phase_;
    // Retain exact STATUS/CONFIG, including desired state, across owned access.
    uint32_t powerStatus_,powerConfig_;bool powerValid_;
    Status finish(Status,Result&);
    Status rawRead(uint8_t,Register,uint32_t&,Result&);
    Status power(Result&);
    Status read(uint8_t,Register,uint32_t&,Result&);
    Status update(Register,uint32_t,uint32_t,Result&);
    Status wait(Register,uint32_t,uint32_t,const Poll&,Result&);
    Status guard(Result&);
    Status routes(bool acquired,Result&);
};
} }
#endif
