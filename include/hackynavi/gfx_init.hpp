// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_GFX_INIT_HPP
#define HACKYNAVI_GFX_INIT_HPP
#include "hackynavi/smu.hpp"
namespace hackynavi {
namespace gfxinit {
constexpr size_t kClearStateDwords=960, kClearStateBytes=kClearStateDwords*4;
enum class Status { Ok, InvalidArgument, InvalidState, UnsupportedLayout,
    OutsideBar, FirmwareRejected, InvalidAllocation, OutsideAllocation,
    ActiveEngine, UnsupportedArchitecture, ReadFailed, WriteFailed,
    DeviceUnavailable, ReadbackMismatch, TransferFailed, VisibilityFailed, QuiesceTimeout };
enum class Stage { Preflight, Guard, UploadClearState, PublishClearState,
    Halt, ProgramCounter, PipeReset, Golden, Constants, ClearStatePointer, Srm,
    RetireGuard, EnterSafeMode, DrainRlc, DisableSrm, RetireClearState, Complete };
struct Io { void* context; bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t); };
struct Result {
    Status status; Stage stage; uint32_t reads,writes,polls,lastRegister,lastValue;
    bool registerWritesMayHaveOccurred,clearStateRetentionRequired;
    VramTransferResult transfer; HdpResult hdp; HdpStatus hdpStatus;
};
class State {
public:
    State():started_(false),ready_(false),retained_(false),retirementStarted_(false),csb_{} {}
    State(const State&)=delete; State& operator=(const State&)=delete;
    bool prepared() const { return ready_; }
    bool needsClearStateRetention() const { return retained_; }
    VramAllocationHandle clearStateAllocation() const { return csb_; }
private:
    bool started_,ready_,retained_,retirementStarted_; VramAllocationHandle csb_; friend struct Access;
};
// Encodes every context extent from AMD's complete clearstate_gfx11.h plus
// preamble, context-control, tile-steering0, end and CLEAR_STATE. 960DWORDs,
// little endian. This is a real GC11 PM4 stream, not a synthesized zero state.
// Output untouched on preflight failure. Tile register comes from IP discovery.
Status encodeClearState(const Navi31IpLayout&,uint8_t*,size_t);

// PSP-loaded GC11.0.0 only, same powered reset epoch as firmware/SMU States.
// Caller has enabled VMID0 GFXHUB and excluded every queue/reset/PM/register
// selector user. MES stays disabled. Only VMID0 kernel work is configured;
// other VMIDs are required inactive. Rendering users need later VM ownership.
// Uploads the complete CSB through owned VRAM with independent readback+HDP,
// programs each RS64 PFP/ME/MEC pipe's firmware start PC, pulses pipe resets,
// sets Navi31 golden TCP_CNTL and VMID0 shader-memory policy, disables GDS/GWS/
// OA for nonzero VMIDs, publishes RLC CSIB and enables SRM. Leaves CP halted.
// Does NOT use direct-loader IC cache/TMR programming; PSP owns that setup.
// caller supplies >=4096B nonaliasing CPU workspace and a distinct owned CSB
// allocation>=4096B; cannot alias firmware, PSP control, TMR or other buffers.
// State is single use once hardware accesses start. Pin the CSB before calls;
// after pointer publication it remains device-owned until verified reset or
// retireClearState succeeds. A failed write may have landed.
// Prepared is a prerequisite for queue setup, not command execution success.
Status prepare(const Navi31IpLayout&,const gfxfw::Files&,const gfxfw::State&,
    const smu::State&,const VramAllocator&,VramAllocationHandle clearState,
    uint8_t* workspace,size_t workspaceBytes,const VramTransferIo&,const Io&,
    const HdpIo&,HdpState&,State&,Result&);
struct RetirementIo { void* context; bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t); void (*delayUs)(void*,uint32_t); };
struct RetirementPollOptions { uint32_t iterations,intervalUs; };
// Terminal teardown within the SAME powered reset epoch as prepare/SMU.
// Caller already drained and stopped every queue, holds GC selector/PM/reset
// exclusion, and pins the CSB through this call. No queue or PM actor may run
// afterward until firmware/GC reinitialization following verified reset.
// Requires the prepared CSIB still names its live owned allocation, halted
// PFP/ME/MEC/MES and inactive graphics rings. Uses Linux's RLC safe-mode ACK
// and RLC stop, then requires GPM activity clear, SRM_BUSY/BUSY_DELAY clear
// and the SRM command FIFO empty, plus RLC/MC/thread/translation busy and
// stall flags and GRBM RLC requests clear before disabling SRM/clearing CSIB.
// This explicit port retirement policy is not Linux's normal hw_fini path.
// A timeout, lost ownership or ambiguous I/O seals State and retains CSB;
// no repeat, forced reset, status substitution or CPU completion is allowed.
Status retireClearState(const Navi31IpLayout&,uint64_t barBytes,const smu::State&,
    const VramAllocator&,const RetirementIo&,const RetirementPollOptions&,State&,Result&);
const char* statusString(Status);
} // namespace gfxinit
} // namespace hackynavi
#endif
