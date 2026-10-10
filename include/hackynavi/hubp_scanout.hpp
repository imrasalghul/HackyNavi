// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_HUBP_SCANOUT_HPP
#define HACKYNAVI_HUBP_SCANOUT_HPP
#include "discovery.hpp"
#include "vram_allocator.hpp"
namespace hackynavi { namespace hubpscanout {
constexpr size_t kPipeCount=4;
enum class Register : uint8_t {
    Power,PowerConfig,Clock,Control,SurfaceConfig,TilingConfig,RequestSize,
    Pitch,ViewportStart,ViewportDimension,SecondaryStart,SecondaryDimension,
    SurfaceControl,FlipControl,FlipControl2,Vmid,AddressHi,AddressLo,
    InUseHi,InUseLo,EarliestHi,EarliestLo,ReturnControl,Interrupt,
    TlbControl,ApertureLow,ApertureHigh,CursorControl,DmdataControl,MallConfig,
    OtgControl,OtgInterlace,Count
};
constexpr size_t kRegisterCount=size_t(Register::Count);
struct Layout { bool valid;uint64_t barBytes;uint32_t registers[kPipeCount][kRegisterCount];
    uint32_t framebuffer[5]; }; // FB_BASE/TOP/OFFSET, AGP_BOT/TOP, all BASE_IDX2.
struct Io { void* context;bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t);void (*delayUs)(void*,uint32_t); };
struct PollOptions { uint32_t iterations,intervalUs; }; // <=100ms per wait.
// Naming describes the 32-bit pixel word; ARGB8888 is B,G,R,A in little endian.
enum class Format : uint8_t { Argb8888,Abgr8888,Argb2101010,Abgr2101010 };
struct Surface { VramAllocationHandle allocation;uint64_t byteOffset;
    uint32_t pitchPixels,x,y,width,height;Format format; };
struct Plan { Surface surface;uint64_t mcAddress,footprintBytes;
    uint32_t pitch,viewportStart,viewportDimension,surfaceConfig,crossbar; };
enum class Status { Ok,InvalidArgument,InvalidAllocation,OutsideAllocation,
    DiscoveryRejected,UnsupportedDmu,OutsideBar,InvalidState,PowerUnavailable,
    ClockUnavailable,ActivePipe,ForeignControl,AddressMode,TransportError,
    DeviceUnavailable,VerifyFailed,HardwareFault,Timeout };
enum class Phase { Closed,Prepared,Running,FlipPending,Stopped,Faulted };
struct Result { Status status;uint32_t reads,writes,polls,lastRegister,lastValue;
    bool writeAttempted,retentionRequired; };
Status buildLayout(const uint8_t*,size_t,uint64_t,Layout&);
// Resolves a LIVE allocation through allocator.lookup/addresses and verifies its
// MC/physical translations against memory. Includes preceding viewport rows and
// complete pitch padding. No raw caller-supplied GPU address is accepted.
Status plan(const MemoryLayout&,const VramAllocator&,const Surface&,Plan&);
// Exclusive caller owns PCI/power/reset/modeset and allocation lifetime. All
// allocations exposed by current()/pending() must remain pinned, including on
// error. Allocator must stay alive; this portable controller does not allocate,
// retain an IOKit object, or prevent allocator.release(). Native ownership must.
// RQ/DLG/TTU, watermarks, DET allocation, clocks and MPC/DPP/OPP must already be
// calculated/programmed by their owners before start. No guessed policy values
// are installed here. No cursor, DMDATA, MALL, stereo, DCC, or GPUVM fetches.
class Controller {
public:
    Controller();
    Status configure(const Layout&,const Io&,uint8_t pipe,const MemoryLayout&,
        const VramAllocator&,const Surface&,Result&);
    // Caller has connected the matching pixel pipeline and enabled this OTG.
    // Unblank and require actual address-in-use and pending-clear completion.
    Status start(const PollOptions&,Result&);
    // Same geometry/format only; no modeset through flips. Caller synchronizes
    // producer caches/fences before this submission. Both old and new surfaces
    // stay pinned until completeFlip succeeds, or stop/reset retires this HUBP.
    Status submitFlip(const Surface&,Result&);
    Status completeFlip(const PollOptions&,Result&);
    // Interrupt delivery must be owned/installed before enabling; an interrupt
    // is a notification, NEVER the allocation-retirement/completion predicate.
    Status setFlipInterrupt(bool enabled,Result&);
    Status acknowledgeFlipInterrupt(bool& occurred,Result&);
    // Terminal blank/drain, pending-clear and mask-off. Does not stop OTG, gate
    // power/clocks, restore old scanout, or retire any other display/DMA client.
    // Faulted operations require independent verified reset; no blind retry.
    Status stop(const PollOptions&,Result&);
    Phase phase() const{return phase_;}
    bool requiresHardwareRetirement() const{return touched_;}
    const Plan& current() const{return current_;}
    const Plan& pending() const{return pending_;}
    void clearAfterVerifiedReset();
private:
    Controller(const Controller&)=delete;Controller& operator=(const Controller&)=delete;
    Layout layout_;Io io_;uint8_t pipe_;MemoryLayout memory_;const VramAllocator* allocator_;
    Plan current_,pending_;Phase phase_;bool touched_;
    // Retain exact STATUS/CONFIG, including desired state, across owned access.
    uint32_t powerStatus_,powerConfig_;bool powerValid_;
    Status finish(Status,Result&,bool fault=true);
    Status rawRead(uint32_t,uint32_t&,Result&);
    Status power(Result&);
    Status read(Register,uint32_t&,Result&);
    Status write(Register,uint32_t,Result&);
    Status update(Register,uint32_t,uint32_t,Result&);
    Status verifyAddressMode(const Plan&,Result&);
    Status verifyProgrammed(Result&);
    Status guard(bool blank,Result&);
    Status waitControl(uint32_t,uint32_t,const PollOptions&,Result&);
    Status waitAddress(const Plan&,const PollOptions&,Result&);
    Status programAddress(const Plan&,bool immediate,Result&);
};
} }
#endif
