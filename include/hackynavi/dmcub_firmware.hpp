// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DMCUB_FIRMWARE_HPP
#define HACKYNAVI_DMCUB_FIRMWARE_HPP
#include "hackynavi/dmcub.hpp"
#include "hackynavi/psp_ring.hpp"
#include "hackynavi/hdp.hpp"
namespace hackynavi { namespace dmcubfw {
constexpr uint32_t kPspFirmwareType=51;
enum class Status { Ok, InvalidArgument, FirmwareRejected, UnsupportedFirmware,
    RomRejected, UnsupportedLayout, OutsideBar, InvalidState, InvalidAllocation,
    TransferFailed, VisibilityFailed, PspFailed, ReadFailed, WriteFailed,
    DeviceUnavailable, ReadbackMismatch, UnsupportedHardware, Busy,
    StopTimeout, BootTimeout, VersionMismatch };
struct Image { FirmwareHeader header; FirmwareRange pspImage, instruction, bss;
    uint32_t metadataOffset, footerBytes, firmwareStateBytes, traceBytes, version;
    uint32_t sharedStateBytes, featureBits; uint16_t sharedStateFeatures; };
// Bounded AMD DMCUB1.0/DCN3.2 container and DMUB metadata, not authentication.
// Supports the actual combined DAL image (BSS=0). PSP authenticates pspImage.
// Legacy split-BSS containers are rejected for this DCN3.2-only boot profile.
Status parse(const uint8_t*,size_t,Image&);
enum class Window : size_t { Vbios, Mailbox, Trace, FirmwareState, Count };
struct Region { uint32_t offset,bytes; };
struct Plan { Region regions[4]; uint32_t totalBytes; };
Status plan(const Image&,uint32_t vbiosBytes,Plan&);
struct Layout { bool valid; uint64_t barBytes; uint32_t regs[50]; };
Status buildLayout(const uint8_t* discovery,size_t,uint64_t barBytes,Layout&);
struct Memory { uint8_t* cpu; size_t bytes; uint64_t gpuMcAddress; };
struct Io { void* context; bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t); // false may have landed
    bool (*publishMemory)(void*); // real DMA synchronize-out + HDP of entire BO
    void (*delayUs)(void*,uint32_t); };
struct PollOptions { uint32_t iterations,intervalUs; }; // product<=1s
enum class Phase { Fresh, Loaded, Booting, Ready, Stopped, Faulted };
enum class Stage { Preflight, Upload, Publish, PspLoad, StopFirmware, Reset,
    InitializeMemory, Windows, ReleaseReset, WaitBoot, VerifyVersion, Complete };
struct Result { Status status; Stage stage; uint32_t reads,writes,polls,lastRegister,lastValue;
    bool stagingRetentionRequired,runtimeRetentionRequired,firmwareMayBeLoaded;
    VramTransferResult transfer; HdpResult hdp; pspring::Result psp; };
struct Access;
class State {
public:
    State():phase_(Phase::Fresh),image_{},plan_{},layout_{},memory_{},stagingRetain_(false),runtimeRetain_(false),loaded_(false) {}
    State(const State&)=delete; State& operator=(const State&)=delete;
    Phase phase() const { return phase_; } bool ready() const { return phase_==Phase::Ready; }
    bool needsStagingRetention() const { return stagingRetain_; }
    bool needsRuntimeRetention() const { return runtimeRetain_; }
    uint64_t mailboxGpuAddress() const { return memory_.gpuMcAddress+plan_.regions[1].offset; }
private:
    Phase phase_; Image image_; Plan plan_; Layout layout_; Memory memory_;
    bool stagingRetain_,runtimeRetain_,loaded_; friend struct Access;
};
// Existing PSP ring/TMR and source-selected firmware on this same epoch.
// Distinct live VRAM staging; full upload/readback/HDP and real PSP51 fence+
// zero response status. TMR/code remains owned through DMCUB stop/reset.
Status load(const uint8_t*,size_t,const VramAllocator&,VramAllocationHandle,
    const VramTransferIo&,const HdpIo&,HdpState&,const pspring::Io&,pspring::State&,
    const pspring::PollOptions&,State&,Result&);
// Caller supplies a dedicated, pinned coherent x86 runtime BO mapped through
// active MMHUB/GART (Linux DCN3.2 uses GTT). GPU MC/GART address is not a CPU
// pointer, CPU physical address, or VRAM-local offset. CPU/MC ranges must not
// alias PSP, firmware, VBIOS source, other objects or other allocations.
// Keeps PSP-installed CW0/CW1 and AXI mode3 intact; no backdoor instruction
// loading. Resets owned DMCUB using STOP_FW response+PWAIT, configures actual
// CW3..6/region5 and inbox/outboxes, releases reset, waits DAL+mailbox ready,
// then queries the executing firmware version through GPINT. Default native
// Navi31 profile: DMCU disabled, SCRATCH15=0, boot options=0, no DPIA/IPS.
// Requires exclusive display/GPINT/PSP/MC ownership, posted device, display
// clocks, D0/BME and reset/PM/removal exclusion throughout. Stops inherited
// VBIOS DMCUB only under that ownership. Pin BO BEFORE calling; a failed write
// can leave a reference. Failure is sticky and never replays load or boot.
Status boot(const Layout&,const uint8_t* rom,size_t,const ImageInfo&,
    const Memory&,const Io&,const PollOptions&,State&,Result&);
// Caller first stops all inbox/outbox/other display clients. STOP_FW+PWAIT,
// real soft reset and window disable/readback retire runtime BO only. PSP
// TMR/code retains independent ownership. Failure keeps BO pinned; retrying
// observation/stop is allowed, never rebooting a faulted State.
Status stop(const Io&,const PollOptions&,State&,Result&);
// Cold first-attach only, before loading PSP SOS. Caller owns PCI power/reset,
// GPINT and display programming exclusion and pins all inherited memory until
// success or a separately verified reset. Reads actual SOS=0 before and after
// STOP_FW/PWAIT plus soft reset/window-disable readback. This stops firmware;
// it does not stop or release existing scanout surfaces. A failed write may
// have landed; Result.writes requires retaining the whole ownership epoch.
// No firmware-loaded State is fabricated. publishMemory is unused here.
Status stopInheritedCold(const Navi31IpLayout&,const Layout&,const Io&,
    const PollOptions&,Result&);
const char* statusString(Status);
} }
#endif
