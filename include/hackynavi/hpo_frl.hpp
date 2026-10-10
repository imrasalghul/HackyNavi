// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_HPO_FRL_HPP
#define HACKYNAVI_HPO_FRL_HPP
#include "hackynavi/discovery.hpp"
namespace hackynavi { namespace hpofrl {
enum class Register : size_t { Control,Clock,Config,Config2,Memory,StreamClock,StreamFifo,Tribyte,Count };
constexpr size_t kRegisters=size_t(Register::Count);
struct Layout {bool valid;uint64_t barBytes;uint32_t registers[kRegisters];};
enum class Status {Ok,InvalidArgument,InvalidState,DiscoveryRejected,UnsupportedDmu,OutsideBar,
    Revoked,TransportError,DeviceUnavailable,ForeignControl,StreamActive,MemoryAsleep,
    VerifyFailed,ClockTimeout,Faulted};
enum class Phase {Closed,Training,Packets,Faulted};
struct Poll {uint32_t maxPolls,delayUs;};
struct Io {
    void* context;
    // Called BEFORE every access. The native caller already holds the shared
    // operational gate used by FRL protocol, DDC, DMCUB and display ownership.
    // Confirms immediate power/PM/reset revocation and retained HPO ownership.
    // No nested gate acquisition: usable inside hdmifrl::Io source callbacks.
    bool (*valid)(void*);
    bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t); // false may have landed
    void (*delayUs)(void*,uint32_t);
};
struct Result {Status status;uint32_t reads,writes,polls,lastRegister,lastValue;
    bool writeAttempted,retentionRequired;};
Status buildLayout(const uint8_t*,size_t,uint64_t,Layout&);
// The one DCN3.2 HPO FRL link, generated BASE_IDX3 registers. Not a PHY/PLL,
// DMCUB transmitter, HPO stream/DSC encoder, or receiver-training controller.
// Caller proves HPO access power/clocks, owned route, stream blank/drained and
// no competing display users under the actual operational gate before entry.
// Gated stream registers are never read as false proof of an idle stream.
class Link {
public:
    Link();
    Status prepare(const Layout&,const Io&,uint8_t lanes,const Poll&,Result&);
    // Hardware encoding0..7; 0xff preserves a lane (zero receiver LTP request).
    Status patterns(const uint8_t[4],Result&);
    // Caller has observed real all-lane training completion; only disables
    // HPO training patterns. Receiver FRL_START remains a separate handshake.
    Status packetMode(Result&);
    // Requires stream blank/drained and PHY output stopped by its real owner.
    // Leaves global clocks/PHY/memory alone; disables only this link clock.
    // Can retry retained local retirement after a recoverable I/O error.
    Status disable(const Poll&,Result&);
    Phase phase() const {return phase_;}
    bool requiresHardwareRetirement() const {return touched_;}
    void clearAfterVerifiedReset();
private:
    Layout layout_;Io io_;Phase phase_;uint8_t lanes_;bool touched_;
    Status finish(Status,Result&);
    Status read(Register,uint32_t&,Result&);
    Status update(Register,uint32_t,uint32_t,Result&);
    Status streamIdle(Result&);
    Status guard(Result&);
    Status clockWait(bool,const Poll&,Result&);
    Link(const Link&)=delete;Link& operator=(const Link&)=delete;
};
const char* statusString(Status);
} }
#endif
