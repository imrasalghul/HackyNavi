// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_BOOT_DISPLAY_STOP_HPP
#define HACKYNAVI_BOOT_DISPLAY_STOP_HPP
#include "hackynavi/boot_display.hpp"
namespace hackynavi { namespace bootdisplaystop {
enum class Status { Ok,InvalidArgument,InvalidState,CaptureRejected,ReadFailed,
    WriteFailed,DeviceUnavailable,SnapshotChanged,PowerChanged,ClockUnavailable,
    ReadbackMismatch,DrainTimeout };
enum class Phase { Fresh,Armed,Stopped,Faulted };
struct Io { void* context;bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t);void (*delayUs)(void*,uint32_t); };
struct PollOptions { uint32_t iterations,intervalUs; }; // <=100ms per wait
struct Result { Status status;bootdisplay::Status captureStatus;
    uint32_t reads,writes,polls,lastRegister,lastValue;uint8_t pipe,blankedMask;
    bool writesMayHaveOccurred,retentionRequired; };
struct Access;
class State {
public:
    State():phase_(Phase::Fresh),layout_{},capture_{},clocks_{},blanked_(0),wrote_(false) {}
    State(const State&)=delete;State& operator=(const State&)=delete;
    Phase phase() const {return phase_;}
    bool retentionRequired() const {return phase_==Phase::Armed || phase_==Phase::Faulted;}
    const bootdisplay::Capture& capture() const {return capture_;}
private:
    Phase phase_;bootdisplay::Layout layout_;bootdisplay::Capture capture_;
    uint32_t clocks_[bootdisplay::kPipeCount];uint8_t blanked_;bool wrote_;
    friend struct Access;
};
// Perform and retain the actual cold capture while SOS is still zero. No
// caller-created success flag or snapshot is accepted. Active HUBPs also need
// HUBP_CLOCK_ENABLE; this module never changes display clocks. Caller keeps
// this same PCI/reset/power epoch and excludes ALL firmware/OS flips, mode and
// power changes through eventual stop. This does not establish that exclusion.
Status capture(const bootdisplay::Layout&,const MemoryLayout&,const Io&,State&,Result&);
// Terminal retirement of the captured HUBP memory readers only. Before any
// write, re-read all captured programming and power/request state; reject
// unknown formats, cursor/DMDATA or changed address/translation implicitly by
// exact comparison to the independently decoded capture. Never enter an OFF
// domain. Active pipes retain their clocks, pre-drain as hubp2_set_blank_regs,
// then set BLANK_EN=1 and TTU_DISABLE=0; read back and wait for actual
// NO_OUTSTANDING_REQ, then independently recheck every powered pipe drained.
// This combines source-backed DCN3.2 blank/drain operations into a terminal
// port policy. No legacy HUBP_DISABLE bit exists on DCN3.2. It neither gates
// clocks nor disables OTG/MPC/DIO/PHY; those downstream blocks do not source
// these captured scanout addresses. Cursor, DMDATA, DWB, MALL/other clients,
// DMCUB and later driver-created planes remain separate ownership domains.
// DMCUB need not be running; every competing display programmer must be stopped
// or excluded. Keep DCN/HUBP clocks and power available until this completes.
// A failed/ambiguous write or observation seals State and requires retaining
// memory until verified device reset; never blindly retry or resume scanout.
Status stop(const Io&,const PollOptions&,State&,Result&);
const char* statusString(Status);
} }
#endif
