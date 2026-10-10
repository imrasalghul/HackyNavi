// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_SMU_HPP
#define HACKYNAVI_SMU_HPP
#include "hackynavi/gfx_firmware.hpp"
namespace hackynavi {
namespace smu {
enum class Status { Ok, InvalidArgument, InvalidState, UnsupportedLayout, OutsideBar,
    ReadFailed, WriteFailed, DeviceUnavailable, ResponseTimeout, CommandFailed,
    UnknownCommand, BadPrerequisite, Busy, UnknownResponse, InvalidVersion, AwakeTimeout };
enum class Stage { Preflight, PreviousResponse, ClearResponse, Argument, Message,
    Response, Output, ResetRelease, Bootload, Complete };
enum class Phase { Fresh, Ready, Awake, Faulted };
struct Layout {
    bool valid; uint64_t barBytes;
    uint32_t message,argument,response,imuReset,cpStatus,bootloadStatus;
};
struct Io {
    void* context;
    bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t); // False can have reached hardware.
    void (*delayUs)(void*,uint32_t);
};
struct PollOptions { uint32_t maxPolls,delayUs; }; // 1..1M,1..1000; product<=10s.
struct Result {
    Status status; Stage stage;
    uint32_t reads,writes,polls,lastRegister,lastValue,message,argument,response;
    uint32_t smuVersion,interfaceVersion,imuReset,cpStatus,bootloadStatus;
    bool mailboxWritesMayHaveOccurred,commandMayHaveOccurred,gfxOffDisabled;
};
struct Access;
class State {
public:
    State():phase_(Phase::Fresh),layout_{},smuVersion_(0),interfaceVersion_(0),disabled_(false) {}
    State(const State&)=delete; State& operator=(const State&)=delete;
    State(State&&)=delete; State& operator=(State&&)=delete;
    Phase phase() const { return phase_; }
    bool awake() const { return phase_==Phase::Awake; }
    bool gfxOffDisabled() const { return disabled_; }
    uint32_t smuVersion() const { return smuVersion_; }
    uint32_t interfaceVersion() const { return interfaceVersion_; }
private:
    Phase phase_; Layout layout_; uint32_t smuVersion_,interfaceVersion_; bool disabled_;
    friend struct Access;
};
Status buildLayout(const Navi31IpLayout&,uint64_t barBytes,Layout&);
// Requires this driver's completed PSP firmware/autoload State on the same
// powered reset epoch. The first GetSmuVersion follows Linux SMU_FW_INIT and
// skips the pre-poll only once; every later command waits for the prior
// completion before clear-response/argument/message writes. Replies must be1.
// These are actual SMU-reported versions; no baked-in success/version fallback.
Status initialize(const Layout&,const gfxfw::State& firmware,const Io&,
                  const PollOptions&,State&,Result&);
// Sends actual PPSMC13.0.0 DisallowGfxOff(0x29,param0), waits for success1,
// then verifies reset release and completed RLC bootload before permitting
// GFXHUB/CP/SDMA access. Linux's discrete13.0.0 path uses the mailbox ACK;
// the reset/CP/bootload readback is an additional conservative cold-start
// requirement in this port, using documented GC11 fields and Linux checks.
// Requires engines still quiescent (CP_STAT==0); no IMU reset/start writes.
Status disableGfxOff(const Io&,const PollOptions&,State&,Result&);
// Read-only revalidation while GFXOFF remains disabled; intended immediately
// before cold-start GFXHUB/CP programming, not while CP is executing jobs.
Status verifyAwake(const Io&,const PollOptions&,State&,Result&);
// Explicit retirement/power-management operation only AFTER every GFX/SDMA
// user is quiescent and mappings are no longer being changed. Success merely
// permits firmware GFXOFF policy; it does not assert the device powered off.
Status allowGfxOffAfterQuiescence(const Io&,const PollOptions&,State&,Result&);
// Caller serializes the full protocols with all MP1 mailbox users and excludes
// suspend/reset/removal. Ordered uncached BAR reads/writes only. State is bound
// to this device/reset epoch; any hardware failure seals it. No implicit retry,
// fallback, reset or DMA retirement. Reconstruct only after verified reset.
const char* statusString(Status);
} // namespace smu
} // namespace hackynavi
#endif
