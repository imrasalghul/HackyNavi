// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_GFX_FIRMWARE_HPP
#define HACKYNAVI_GFX_FIRMWARE_HPP
#include "hackynavi/firmware.hpp"
#include "hackynavi/hdp.hpp"
#include "hackynavi/psp_ring.hpp"

namespace hackynavi {
namespace gfxfw {
constexpr size_t kMaximumSegments = 24;
struct View { const uint8_t* bytes; size_t size; };
enum class File : uint8_t { Smu, Sdma, Pfp, Me, Mec, Imu, Rlc, Count };
struct Files { View smu, sdma, pfp, me, mec, imu, rlc; };
struct ImuFirmware { FirmwareHeader header; FirmwareRange instruction, data; };
struct RlcFirmware {
    FirmwareHeader header;
    // PSP enum order: CNTL,GPM,SRM,IRAM,DRAM,P,V,G. Empty optional ranges skipped.
    FirmwareRange segments[8];
};
// Exact IMU1.0/RLC2.3 layouts used by the pinned Navi31 firmware. IMU PSP
// ranges use common.payloadOffset and payloadOffset+IRAMsize, as Linux does;
// explicit IMU offsets are metadata for another loading path, not file offsets.
// RLC ranges are absolute file offsets. Optional zero-length ranges may have
// zero offsets. Nonzero ranges must be aligned, file-bounded and disjoint.
// These parsers establish bounds, not authenticity; outputs zero on failure.
hackynavi::Status parseImu(const uint8_t*,size_t,ImuFirmware&);
hackynavi::Status parseRlc(const uint8_t*,size_t,RlcFirmware&);

enum class Status { Ok, InvalidArgument, UnsupportedDevice, FirmwareRejected,
    UnsupportedFirmware, InvalidSegment, UnsupportedLayout, OutsideBar,
    InvalidAllocation, OutsideAllocation, InvalidState, ActiveMes,
    TransferFailed, VisibilityFailed, PspFailed, ReadFailed, DeviceUnavailable,
    AutoloadTimeout };
enum class Stage { Preflight, Guard, Upload, Publish, Load, WaitLoad,
    Autoload, WaitAutoload, WaitBootload, Complete };
enum class Phase { Fresh, Loading, Ready, Faulted };
struct Segment { File file; FirmwareRange range; uint32_t pspType; };
struct Plan { size_t count; uint64_t stagingBytes; Segment segments[kMaximumSegments]; };
struct LoadedSegment { uint32_t pspType, bytes; uint64_t tmrMcAddress; };
struct Result {
    Status status; Stage stage; File file; hackynavi::Status parserStatus;
    size_t segment, verifiedSegments, acceptedSegments;
    uint32_t readAttempts,polls,lastRegister,lastValue;
    bool stagingRetentionRequired, firmwareMayBeLoaded, autoloadSubmitted;
    VramTransferResult transfer; HdpResult hdp; HdpStatus hdpStatus;
    pspring::Result psp;
};
struct Access;
class State {
public:
    State() : phase_(Phase::Fresh), count_(0), loaded_{}, stagingRetain_(false) {}
    State(const State&)=delete; State& operator=(const State&)=delete;
    Phase phase() const { return phase_; }
    bool needsStagingRetention() const { return stagingRetain_; }
    size_t loadedCount() const { return count_; }
    const LoadedSegment* loaded(size_t index) const { return index < count_ ? &loaded_[index] : nullptr; }
    // Ready means PSP acceptance plus actual CP_STAT/BOOTLOAD completion only.
    // It does not establish initialized CP queues, rendering or Metal support.
    bool autoloadComplete() const { return phase_ == Phase::Ready; }
private:
    Phase phase_; size_t count_; LoadedSegment loaded_[kMaximumSegments];
    bool stagingRetain_; friend struct Access;
};

// Bounded cold-start profile for the user's 1002:744c revision C8, GC11.0.0,
// MP0/MP1 13.0.0 and SDMA6.0.0. Requires deliberately disabled MES; the load
// entrypoint verifies CP_MES_CNTL halted/inactive, never silently enables MES.
// Source-selected authentic files must remain immutable. No firmware execution
// or allocation occurs in plan. All seven files are checked before any upload.
Status plan(const Files&,const Navi31IpLayout&,uint8_t pciRevision,Plan&,Result&);

// Real PSP path: TMR must already be set up on this same reset epoch. Caller
// exclusively owns and powers the device, excludes reset/suspend/removal and
// all other MM-window/PSP/GC users, and keeps files and objects nonoverlapping.
// Staging must be a distinct live allocation (not PSP ring/cmd/fence/TMR). Each
// full segment is uploaded, independently verified, HDP-published, submitted,
// and retired through PSP fence+response before staging is reused. SMU first,
// then SDMA, RS64 instructions/stacks, IMU, RLC components, RLC_G last; then
// AUTOLOAD_RLC and actual CP_STAT==0/BOOTLOAD_COMPLETE polling. No direct IMU
// RAM writes, IMU start/reset toggles or RLC-backdoor sequence is substituted.
// Poll bound applies separately to each PSP command and the final bootload.
// On failure state seals; if submission may be outstanding preserve staging
// until independently retired or reset. TMR/firmware remain owned through GPU
// quiescence/reset regardless of staging retirement. Reconstruct State only
// after a verified new reset epoch; successful completion cannot be replayed.
Status load(const Files&,const Navi31IpLayout&,uint8_t pciRevision,
    const VramAllocator&,VramAllocationHandle staging,const VramTransferIo&,
    const HdpIo&,HdpState&,const pspring::Io&,pspring::State&,
    const pspring::PollOptions&,State&,Result&);
const char* statusString(Status);
} // namespace gfxfw
} // namespace hackynavi
#endif
