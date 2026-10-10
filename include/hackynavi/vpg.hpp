// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_VPG_HPP
#define HACKYNAVI_VPG_HPP
#include "discovery.hpp"
namespace hackynavi { namespace vpg {
constexpr size_t kInstanceCount=5,kPacketCount=15,kPacketWords=9;
enum class Register : uint8_t { Access,Data,FrameUpdate,ImmediateUpdate,Status,MemoryPower,Clock,Count };
constexpr size_t kRegisterCount=size_t(Register::Count);
struct Layout { bool valid;uint64_t barBytes;uint32_t registers[kInstanceCount][kRegisterCount]; };
struct Io { void* context;bool (*read32)(void*,uint32_t,uint32_t&);bool (*write32)(void*,uint32_t,uint32_t);void (*delayUs)(void*,uint32_t); };
struct Poll { uint32_t maxPolls,delayUs; };
struct Packet { uint8_t header[4],payload[32]; };
enum class Status { Ok,InvalidArgument,InvalidState,DiscoveryRejected,UnsupportedDmu,OutsideBar,
    TransportError,DeviceUnavailable,ForeignControl,VerifyFailed,PowerTimeout,UpdateTimeout,Faulted };
enum class Phase { Closed,Open,Faulted };
struct Result { Status status;uint32_t reads,writes,polls,lastRegister,lastValue,packetWords;
    bool writeAttempted,updateSubmitted,updateCompleted; };
Status buildLayout(const uint8_t*,size_t,uint64_t,Layout&);
// Programs the real DCN3.2 VPG SRAM window. Caller exclusively owns the
// associated DIG/VPG and holds display/power/reset exclusion. Its packet
// scheduler must be disabled during writes and before close; this transport
// neither enables packet transmission nor claims a video frame was sent.
// AFMT clock must already be enabled and observed on by its shared owner.
class Engine {
public:
    Engine();
    Status open(const Layout&,const Io&,uint8_t instance,const Poll&,Result&);
    Status upload(uint8_t packet,const Packet&,const Poll&,Result&);
    // Restores only the SRAM sleep controls acquired by open. A faulted
    // transport must be independently retired/reset, not silently reopened.
    Status close(const Poll&,Result&);
    Phase phase() const { return phase_; }
    bool requiresHardwareRetirement() const { return touched_; }
    void clearAfterVerifiedReset();
private:
    Engine(const Engine&)=delete;Engine& operator=(const Engine&)=delete;
    Layout layout_;Io io_;uint8_t instance_;Phase phase_;bool touched_;uint32_t savedPower_;
    Status finish(Status,Result&);
    Status read(Register,uint32_t&,Result&);
    Status write(Register,uint32_t,Result&);
    Status update(Register,uint32_t,uint32_t,Result&);
    Status wait(Register,uint32_t,uint32_t,const Poll&,Status,Result&);
    Status idle(const Poll&,Result&);
};
const char* statusString(Status);
} }
#endif
