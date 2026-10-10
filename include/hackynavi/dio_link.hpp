// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DIO_LINK_HPP
#define HACKYNAVI_DIO_LINK_HPP
#include "hackynavi/dmcub.hpp"
namespace hackynavi { namespace dio {
constexpr size_t kLinkCount=5;
enum class Register : size_t { BackendControl,BackendEnable,TmdsControl,PhyMux,
    DpConfig,Scrambler,TrainingPattern,LinkControl,PhyControl,PrbsControl,
    PanelControl,FramingControl,StreamControl,FrontendControl,FifoControl,
    HdmiControl,HdmiVbiControl,HdmiGeneralControl,Count };
constexpr size_t kRegisterCount=static_cast<size_t>(Register::Count);
struct Layout { bool valid; uint64_t barBytes; uint32_t registers[kLinkCount][kRegisterCount]; };
enum class Status { Ok, InvalidArgument, DiscoveryRejected, UnsupportedDmu,
    OutsideBar, InvalidState, ActiveOutput, ForeignRoute, TransportError,
    DeviceUnavailable, VerifyFailed, FirmwareFailed, EnableTimeout, Faulted };
enum class Phase { Closed,Prepared,Enabled,TrainingPattern,VideoPattern,Faulted };
enum class Pattern : uint8_t { Tps1=0,Tps2=1,Tps3=2,Tps4=3 };
struct Io { void* context; bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t); void (*delayUs)(void*,uint32_t); };
struct Result { uint32_t reads,writes,polls,lastRegister,lastValue,firmwareCommands;
    dmcub::Status firmwareStatus; bool phyCommandSubmitted,phyEnableObserved; };
// RGB or YCbCr444 have the same TMDS container programming. The modeset
// owner supplies the pixel encoding through its pixel pipeline and AVI frame.
struct HdmiConfig { uint32_t pixelClockKHz; uint8_t bitsPerComponent; bool scramble; };
// Character rate rounded upwards to a whole kHz for capability comparisons.
// The PLL owner must have programmed the actual pixel clock / deep-color ratio.
Status hdmiCharacterClock(const HdmiConfig&,uint32_t& tmdsClockKHz);
Status buildLayout(const uint8_t*,size_t,uint64_t barBytes,Layout&);
class Link {
public:
    Link();
    // Cold, exclusive native DP-SST8b/10b or HDMI-TMDS link ownership only.
    // Requires D0/DCN power, clocks and PLL programming, AUX hardware init,
    // and the board route returned by dmcub::parseRoute. Caller owns physical
    // PHY/HPD and selected frontend under the shared display lock, excludes
    // all firmware/modeset/PM/reset users, and has quiesced its stream.
    // Rejects active backend, active selected frontend, an HPO mux, any
    // preexisting target FE selection or the selected FE routed elsewhere.
    // Programs TMDS_CTL0, legacy DIO mux, independent HPD/mode/FE routing,
    // and DP lane count/scrambler advance. No PHY is enabled by prepare.
    Status prepare(const Layout&,const Io&,const dmcub::Route&,uint8_t frontend,
        uint8_t lanes,Result&);
    // Requires HDMI, Prepared, selected DIG FIFO off and stream idle. Sends
    // the real DMCUB encoder-setup command, then programs and reads back the
    // selected FRONTEND's HDMI deep color, scramble and clock ratio, TMDS
    // pixel container and general-control packets. Leaves AVMUTE asserted.
    // SCDC must match before enable; caller owns PLL, OTG, FIFO and timing. Exact
    // 340000kHz uses the legacy ratio on both source and sink; only >340000
    // selects the 1/40 bit-clock ratio. 8/10/12/16bpc RGB/444 containers only.
    Status configureHdmi(dmcub::Inbox&,const HdmiConfig&,
        const dmcub::PollOptions&,Result&);
    // Calls the real DMCUB transmitter command and requires observed hardware
    // DIG_ENABLE=1 afterwards. The controller validates the chosen clock
    // against sink/board capability and programs SCDC when HDMI requires it.
    // HDMI requires configureHdmi first and exactly its computed character rate.
    Status enable(dmcub::Inbox&,uint32_t symbolClockKHz,const dmcub::PollOptions&,Result&);
    Status trainingPattern(Pattern,Result&);
    // Uniform lane settings; repeats the actual VSEMPH command for every
    // active lane, matching Linux. This is not a sink link-training loop.
    Status drive(dmcub::Inbox&,uint8_t swing,uint8_t pre,uint8_t postCursor2,
        const dmcub::PollOptions&,Result&);
    // Caller has verified receiver CR/EQ/symbol/align status through AUX and
    // ended sink training in the required order. Programs passthrough, not
    // stream enable. Phase::VideoPattern describes transmitter programming;
    // it is deliberately not a claim that a sink is trained or displaying.
    Status videoPattern(Result&);
    // Caller has stopped stream/OTG and, for DP, paired receiver power-down.
    // Requires an idle selected frontend; DMCUB disable then actual DIG_ENABLE0,
    // DP training cleanup and FE disconnection. No reset proof is inferred.
    Status disable(dmcub::Inbox&,const dmcub::PollOptions&,Result&);
    Phase phase() const { return phase_; }
    const dmcub::Route& route() const { return route_; }
    uint8_t laneCount() const { return lanes_; }
    uint8_t frontendIndex() const { return frontend_; }
    bool needsRecovery() const { return phase_==Phase::Faulted; }
    // CPU state only, after independently verified reset/power loss and user withdrawal.
    void clearAfterVerifiedReset();
private:
    Phase phase_; Io io_; Layout layout_; dmcub::Route route_;
    uint8_t frontend_,lanes_; uint32_t clock_,hdmiControl_; bool hdmiConfigured_;
    Status read(uint8_t,Register,uint32_t&,Result&);
    Status write(Register,uint32_t,Result&);
    Status update(Register,uint32_t,uint32_t,Result&);
    Status updateAt(uint8_t,Register,uint32_t,uint32_t,Result&);
    Status guard(bool enabled,Result&);
    Status pollEnable(bool,const dmcub::PollOptions&,Result&);
    Status fail(Status);
    Link(const Link&)=delete; Link& operator=(const Link&)=delete;
};
const char* statusString(Status);
} }
#endif
