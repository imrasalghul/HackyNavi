// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DMCUB_HPP
#define HACKYNAVI_DMCUB_HPP
#include "hackynavi/discovery.hpp"
#include "hackynavi/rom.hpp"
namespace hackynavi { namespace dmcub {
constexpr uint32_t kSlotBytes=64, kInboxBytes=8192, kMailboxBytes=16384;
enum class Register : size_t { Control, Control2, BootStatus, AxiSpace,
    Cw4Base, Cw4Top, Cw4Offset, Cw4OffsetHigh, InboxBase, InboxSize, Wptr, Rptr, Count };
constexpr size_t kRegisterCount=static_cast<size_t>(Register::Count);
struct Layout { bool valid; uint64_t barBytes; uint32_t registers[kRegisterCount]; };
enum class Status { Ok, InvalidArgument, DiscoveryRejected, UnsupportedDmu,
    OutsideBar, RomRejected, UnsupportedRoute, Closed, Busy, Faulted,
    FirmwareNotReady, MappingMismatch, PointerCorrupt, TransportError,
    DeviceUnavailable, FlushFailed, VerifyFailed, CompletionTimeout };
// Native DP/HDMI routing only; no USB-C, external encoder, retimer or DPIA.
// Each zero-based GPIO index is independently resolved through the ROM LUT;
// DDC, HPD and PHY are deliberately not assumed to share the same index.
struct Route { uint16_t connectorObject,encoderObject; uint8_t phy,ddc,hpd;
    bool hpdActiveHigh; };
Status buildLayout(const uint8_t*,size_t,uint64_t barBytes,Layout&);
Status parseRoute(const uint8_t* rom,size_t,const ImageInfo&,const Layout&,
    uint8_t path,Route&);
struct Io {
    void* context;
    bool (*read32)(void*,uint32_t barByteOffset,uint32_t&);
    bool (*write32)(void*,uint32_t barByteOffset,uint32_t);
    // Actual owned mailbox memory at its GPU MC/GART aperture address, not
    // a CPU pointer or a VRAM-local offset. The owner can access a GART-backed
    // runtime BO through its real pinned CPU mapping. Transfers are ordered.
    bool (*readMemory)(void*,uint64_t gpuAddress,uint8_t*,size_t);
    bool (*writeMemory)(void*,uint64_t gpuAddress,const uint8_t*,size_t);
    bool (*flushForDevice)(void*); // ordered publish and real HDP as required
    void (*delayUs)(void*,uint32_t);
};
struct PollOptions { uint32_t maxPolls,delayUs; }; // product <=2s, delay1..100us
struct Result { uint32_t reads,writes,polls,lastRegister,wptr,rptr;
    bool slotWritten,submitted,consumed; };
enum class Action : uint8_t { Disable=0, Enable=1 };
// Disable takes lanes=0 and clock=0, matching the Linux disable command.
// HDMI clocks are truncated to10kHz units by the firmware ABI, as in Linux.
struct Transmitter { Action action; uint8_t lanes; uint32_t symbolClockKHz; };
// The clock owner selects an ATOM COMBOPHY_PLL0..4 (20..24), or DP_DTO(11).
// pixelClock100Hz is the exact requested hardware clock, zero disables it.
// miscInfo accepts force-program(1), PHYPLL-only(2), YUV420(4), reference
// source bits0x30, atomic-update0x40 and disable-SS0x80; dual-link DVI is absent.
struct PixelClock { uint32_t pixelClock100Hz; uint8_t pll,crtc,deepColorRatio,miscInfo; };
struct Encoder { uint8_t engine,lanes,bitsPerColor; uint32_t pixelClockKHz; };
struct DpDrive { uint8_t lanes,voltageSwing,preEmphasis,postCursor2; uint32_t symbolClockKHz; };
class Inbox {
public:
    Inbox();
    // Binds an already booted DAL DMCUB to its owner's existing mailbox.
    // This does not load firmware, steal VBIOS state or initialize a ring.
    // Requires exclusively owned live firmware/VBIOS/CW4 allocations, D0,
    // display clocks, PCI access and one lock excluding all DMCUB commands,
    // AUX/DDC users, modesets, PM/reset/removal. Physical connector ownership
    // and board/clock constraints belong to the same display controller.
    // Verifies DCN3.2, enable/reset, DAL+mailbox ready, FB physical AXI mode,
    // the exact CW4+inbox mapping, aligned idle hardware pointers. mailboxGpu
    // names the 16KiB allocation's GPU MC address established by firmware boot;
    // this may be a GART aperture address backed by pinned system memory.
    // It is retained
    // independently of this transport until firmware is stopped/reset.
    Status open(const Layout&,const Io&,uint64_t mailboxGpu,Result&);
    // Real VBIOS command table operations through the framebuffer inbox.
    // Route must come from parseRoute for the VBIOS loaded into this DMCUB.
    // HDMI uses TMDS only; DP uses 8b/10b SST only. Required PLL, DIO lane/
    // frontend setup, SCDC, DP training and stream modeset remain ordered by
    // the caller. A consumed command is NOT proof of a trained visible link.
    Status transmitter(const Route&,const Transmitter&,const PollOptions&,Result&);
    Status pixelClock(const Route&,const PixelClock&,const PollOptions&,Result&);
    Status setupEncoder(const Route&,const Encoder&,const PollOptions&,Result&);
    // One actual VSEMPH(11) command. Levels0..3; swing+pre<=3. Postcursor2
    // is accepted only at HBR2. The v1.7 ABI carries no per-lane selector;
    // caller supports uniform settings and may repeat once per active lane
    // to match dcn10_link_encoder_dp_set_lane_settings's command sequence.
    Status dpDrive(const Route&,const DpDrive&,const PollOptions&,Result&);
    // Observe actual hardware RPTR after a timeout without replaying command.
    // A fault remains sticky even if pending work is subsequently consumed.
    Status wait(const PollOptions&,Result&);
    Status close(Result&); // no firmware reset or allocation retirement
    bool isOpen() const { return phase_==Phase::Open; }
    bool pending() const { return pending_; }
    bool needsRecovery() const { return phase_==Phase::Faulted; }
    // CPU state only, after independently verified DMCUB reset/power loss.
    void clearAfterVerifiedReset();
private:
    enum class Phase { Closed,Open,Faulted };
    Phase phase_; Io io_; uint32_t regs_[kRegisterCount];
    uint64_t mailbox_; uint32_t producer_,start_; bool pending_;
    Status read(Register,uint32_t&,Result&);
    Status verify(Result&,bool poll);
    Status send(const uint8_t*,const PollOptions&,Result&);
    Status waitInternal(const PollOptions&,Result&);
    Status fail(Status);
    Inbox(const Inbox&)=delete; Inbox& operator=(const Inbox&)=delete;
};
const char* statusString(Status);
} }
#endif
