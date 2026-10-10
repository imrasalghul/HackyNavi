// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DP_STREAM_HPP
#define HACKYNAVI_DP_STREAM_HPP
#include "discovery.hpp"
#include "edid.hpp"
namespace hackynavi { namespace dpstream {
enum class Register : uint8_t {
    Frontend,Fifo,Backend,BackendEnable,PhyMux,Config,Link,Phy,Framing,Panel,Prbs,
    Pixel,Color,Misc,Timing1,Timing2,Timing3,Timing4,VidTiming,N,M,Stream,Steer,
    Secondary,Secondary2,SecondaryFraming,Metadata,Pps,Dsc,DoubleBuffer,Vbid,
    Secondary1,Gsp8,Gsp9,Gsp10,Scrambler,Count
};
enum class TimingRegister : uint8_t { Control,Clock,FrameCount,HTotal,VTotal,
    HBlank,HSync,HSyncControl,VBlank,VSync,VSyncControl,Interlace,Count };
constexpr size_t kRegisterCount=size_t(Register::Count),kTimingRegisterCount=size_t(TimingRegister::Count);
struct Layout { bool valid;uint64_t barBytes;uint32_t registers[5][kRegisterCount],timing[4][kTimingRegisterCount]; };
// This is the native owner's actual trained rate/lane/board route and retained
// surface timing, not a sink capability maximum. One OTG/OPP, RGB8 full range,
// progressive, no audio/secondary packets/DSC/FEC/MST/UHBR or ODM.
struct Config { uint8_t frontend,backend,pipe,hpd,lanes;uint32_t symbolClockKHz;edid::Timing timing; };
struct Values { uint32_t timing[4],m,n; };
struct Io { void* context;bool (*valid)(void*);bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t);void (*delayUs)(void*,uint32_t); };
struct Poll { uint32_t maxPolls,delayUs; };
enum class Status { Ok,InvalidArgument,InvalidTiming,LinkCapacity,DiscoveryRejected,UnsupportedDmu,
    OutsideBar,InvalidState,ForeignControl,TransportError,DeviceUnavailable,VerifyFailed,Timeout,FifoError,Faulted };
enum class Phase { Closed,Prepared,Running,Stopped,Faulted };
struct Result { Status status;uint32_t reads,writes,polls,lastRegister,lastValue,firstFrame,lastFrame;
    bool writeAttempted,streamEnabled,streamStopped,framesAdvanced,fifoDisabled; };
Status buildLayout(const uint8_t*,size_t,uint64_t,Layout&);
Status encode(const Config&,Values&);
class Controller {
public:
    Controller();
    // Cold stream only. Caller exclusively retains actual trained DIO link,
    // PLL/DCCG transaction, OTG, bandwidth and HUBP/DPP/MPC/OPP surface owners.
    // Exclude firmware/PM/reset/modeset actors throughout. valid must check the
    // current operation, mapping and power generation before/after every I/O.
    // Programs MSA and FE routing; stream and FIFO remain disabled.
    Status prepare(const Layout&,const Io&,const Config&,Result&);
    // Requires actual matching OTG enabled/clocked and advancing. Executes the
    // DCN3.2 M/N, steer/FIFO reset/prime and next-frame video enable sequence.
    // Success proves local stream status/frame progress, not sink picture/lock.
    Status start(const Poll&,Result&);
    // Must precede OTG and PHY disable. Defers blank to vblank, polls actual
    // stream-off BEFORE steer reset, then disables DIG FIFO and M/N generator.
    // Faulted instances may stop while the same ownership is still guaranteed.
    // No resource/DMA retirement is inferred. Failure retains touched state.
    Status stop(const Poll&,Result&);
    Phase phase() const{return phase_;}
    bool requiresHardwareRetirement() const{return touched_;}
    void clearAfterVerifiedReset();
private:
    Controller(const Controller&)=delete;Controller& operator=(const Controller&)=delete;
    Layout layout_;Io io_;Config config_;Values values_;Phase phase_;bool touched_;
    Status finish(Status,Result&);
    Status readAddress(uint32_t,uint32_t&,Result&);
    Status read(uint8_t,Register,uint32_t&,Result&);
    Status update(Register,uint32_t,uint32_t,Result&);
    Status wait(Register,uint32_t,uint32_t,const Poll&,Result&);
    Status delay(uint32_t);
    Status route(bool,Result&);
    Status controls(Result&);
    Status timing(Result&);
    Status verifyProgrammed(Result&);
};
const char* statusString(Status);
} }
#endif
