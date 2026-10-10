// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_HDMI_STREAM_HPP
#define HACKYNAVI_HDMI_STREAM_HPP
#include "avi.hpp"
#include "otg.hpp"
namespace hackynavi { namespace hdmistream {
enum class Register : uint8_t { Frontend,Fifo,Hdmi,General,Packets,ExtraPackets,Line,DoubleBuffer,
    Clock,Backend,BackendEnable,DpStream,Vbi,Infoframe,Acr,Count };
constexpr size_t kRegisterCount=size_t(Register::Count);
struct Layout { bool valid;uint64_t barBytes;uint32_t registers[5][kRegisterCount];vpg::Layout packets;otg::Layout timing; };
struct Config { uint8_t frontend,backend,pipe;avi::Config infoframe; };
enum class Status { Ok,InvalidArgument,InvalidState,DiscoveryRejected,OutsideBar,ForeignControl,
    TransportError,DeviceUnavailable,VerifyFailed,Timeout,PacketFailed,FifoError,Faulted };
enum class Phase { Closed,Prepared,Running,Stopped,Faulted };
struct Result { Status status;uint32_t reads,writes,polls,lastRegister,lastValue,firstFrame,lastFrame;
    bool writeAttempted,framesAdvanced,muted,fifoDisabled;vpg::Result packet; };
Status buildLayout(const uint8_t*,size_t,uint64_t,Layout&);
// Exclusive HDMI TMDS frontend owner. PLL, DCCG, pixel format/range and
// HUBP/DPP/MPC/OPP surface pipeline must already match Config. Backend must be
// enabled with negotiated SCDC through the DIO owner. No DMA retirement or
// physical sink-lock proof is inferred from FIFO or OTG status.
class Controller {
public:
    Controller();
    Status prepare(const Layout&,const vpg::Io&,const Config&,const edid::Monitor&,const vpg::Poll&,Result&);
    // Requires the configured OTG actually enabled, clocked and advancing.
    // Enables FIFO, observes two frame advances without FIFO error, then unmutes.
    Status start(const vpg::Poll&,Result&);
    // Mutes first, stops FIFO and AVI transmission, closes owned VPG and
    // restores AFMT clock/HDMI double-buffer settings. Call before PHY stop.
    Status stop(const vpg::Poll&,Result&);
    Phase phase() const{return phase_;}
    bool requiresHardwareRetirement() const{return touched_;}
    void clearAfterVerifiedReset();
private:
    Controller(const Controller&)=delete;Controller& operator=(const Controller&)=delete;
    Layout layout_;vpg::Io io_;Config config_;vpg::Engine packets_;Phase phase_;bool touched_;
    uint32_t savedClock_,savedDb_;
    Status finish(Status,Result&);
    Status readAddress(uint32_t,uint32_t&,Result&);
    Status read(Register,uint32_t&,Result&);
    Status update(Register,uint32_t,uint32_t,Result&);
    Status wait(Register,uint32_t,uint32_t,const vpg::Poll&,Result&);
    Status route(Result&);
};
} }
#endif
