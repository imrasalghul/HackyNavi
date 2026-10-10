// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_OTG_HPP
#define HACKYNAVI_OTG_HPP
#include "discovery.hpp"
#include "edid.hpp"
namespace hackynavi { namespace otg {
constexpr size_t kPipeCount=4;
enum class Register : uint8_t {
    HTotal,HBlank,HSync,HSyncControl,VTotal,VBlank,VSync,VSyncControl,
    VTotalMin,VTotalMax,VTotalControl,Interlace,Control,Clock,HTiming,
    VStartup,VUpdate,VReady,FrameCount,UpdateLock,GlobalControl,Gsl,Stereo,
    Structure3d,DoubleBuffer,Interrupt0,Interrupt1,Interrupt2,
    Vtg,InputClock,InputGlobal,DataSource,Memory,DataFormat,Count
};
constexpr size_t kRegisterCount=size_t(Register::Count);
enum class Status { Ok,InvalidArgument,DiscoveryRejected,UnsupportedDmu,OutsideBar,
    InvalidTiming,InvalidSync,InvalidState,ActivePipe,ForeignControl,TransportError,
    DeviceUnavailable,VerifyFailed,ClockTimeout,EnableTimeout,FrameTimeout,
    DisableTimeout,Underflow,Faulted };
enum class Phase { Closed,Programming,Programmed,Enabling,Running,Disabling,Stopped,Faulted };
enum class Signal { Hdmi,DisplayPort };
struct Layout { bool valid;uint64_t barBytes;uint32_t registers[kPipeCount][kRegisterCount]; };
struct Io {
    void* context;
    bool (*read32)(void*,uint32_t byteOffset,uint32_t&);
    bool (*write32)(void*,uint32_t byteOffset,uint32_t);
    void (*delayUs)(void*,uint32_t);
};
struct Poll { uint32_t maxPolls,delayUs; };
// These are hardware global-sync parameters supplied by the owning display
// pipeline's bandwidth calculation. They cannot be derived from EDID alone.
// VSTARTUP is in lines; VUPDATE and VREADY use the hardware pixel-clock units.
struct GlobalSync { uint16_t vstartup,vupdateOffset,vupdateWidth,vreadyOffset; };
struct TimingValues {
    uint32_t hTotal,hBlank,hSync,vTotal,vBlank,vSync,vtg;
    uint32_t hPolarity,vPolarity,startPoint,vStartup,vUpdate,vReady;
};
struct Result {
    Status status;uint32_t reads,writes,polls,lastRegister,lastValue;
    uint32_t firstFrame,lastFrame;
    bool writeAttempted,inputClockOn,otgClockOn,currentEnabled,frameAdvanced;
    bool busyCleared,clockControlsDisabled,underflowObserved;
};
Status buildLayout(const uint8_t* discovery,size_t bytes,uint64_t barBytes,Layout&);
Status encode(const edid::Timing&,const GlobalSync&,Signal,TimingValues&);
class Controller {
public:
    Controller();
    // The native owner must have exclusively acquired this OTG/OPP, programmed
    // the actual pixel PLL/DCCG clocks, and established its RGB non-DSC single
    // pipe surface/HUBP/DPP/MPC/OPP pipeline and bandwidth settings. All other
    // display/DMCUB/FAMS/PM actors must be excluded. No caller boolean can stand
    // in for those operations. This module controls OTG/VTG/ODM timing only;
    // it neither allocates scanout nor configures PHY/stream encoders.
    // Refuses a live selected OTG, GSL/stereo/interlace/DSC, update locks or
    // interrupts owned by another component. OPP n maps to OTG n (no ODM).
    Status configure(const Layout&,const Io&,uint8_t pipe,const edid::Timing&,
        const GlobalSync&,Signal,const Poll&,Result&);
    // Enables actual VTG then OTG. Success requires CURRENT_MASTER_EN_STATE,
    // both clock status bits, and a changed 24-bit hardware frame counter.
    Status enable(const Poll&,Result&);
    // Caller has stopped the stream and blanked/drained its HUBP fetch before
    // disabling the timing source. Disconnects local ODM inputs, disables OTG
    // and VTG, waits actual master-off and BUSY=0, then disables clock controls.
    // It never claims a DMA/VRAM reference was retired by timing disable alone.
    // Faulted instances permit this bounded stop attempt while ownership and
    // MMIO validity are still guaranteed; failed stop retains the fault state.
    Status disable(const Poll&,Result&);
    Phase phase() const { return phase_; }
    bool requiresHardwareRetirement() const { return touched_; }
    uint8_t pipe() const { return pipe_; }
    // Bookkeeping only, for the native owner's independently verified reset.
    void clearAfterVerifiedReset();
private:
    Controller(const Controller&)=delete;Controller& operator=(const Controller&)=delete;
    Status finish(Status,Result&);
    Status read(uint8_t,Register,uint32_t&,Result&);
    Status update(Register,uint32_t mask,uint32_t value,Result&);
    Status wait(Register,uint32_t mask,uint32_t value,const Poll&,Status,Result&);
    Status admit(Result&);
    Status verifyProgrammed(Result&);
    Status clearUnderflow(Result&);
    Layout layout_;Io io_;Phase phase_;uint8_t pipe_;bool touched_;
    TimingValues timing_;Signal signal_;
};
const char* statusString(Status);
} }
#endif
