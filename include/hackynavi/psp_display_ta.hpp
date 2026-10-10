// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_PSP_DISPLAY_TA_HPP
#define HACKYNAVI_PSP_DISPLAY_TA_HPP
#include "hackynavi/psp_ring.hpp"
namespace hackynavi { namespace pspdisplayta {
constexpr size_t kDisplays=4;
enum class Status { Ok, InvalidArgument, InvalidState, InvalidAllocation, AliasedStorage,
    StaleToken, Busy, Exhausted, GuardFailed, TransferFailed, VisibilityFailed,
    PspFailed, ApplicationRejected, ResponseCorrupt };
enum class Phase { Fresh, Bound, FaultedRetained, Retired };
enum class Protocol : uint8_t { Hdcp1, Hdcp2 };
enum class Content : uint8_t { Type0=1, Type1=2, Maximum=3 };
// DCN3.2 direct DIG, HDMI TMDS or DP8b/10b SST only. Caller must derive these
// indices from its owned live stream/connector. DDC/controller +1 translation,
// matching PHY/link/DIO index and max HDCP2.3 follow the pinned Linux path.
// No FRL/HPO, MST, eDP/ASSR, Miracast or USB4/DPIA topology is implied.
struct Topology { uint8_t otg,ddc,streamEncoder,transmitter; bool hdcpCapable; };
class State;
struct Access;
class Display {
public: Display():owner_(nullptr),serial_(0),slot_(0){}
private:
    const State* owner_; uint32_t serial_; uint8_t slot_;
    friend struct Access;
};
class Authentication {
public: Authentication():owner_(nullptr),serial_(0),slot_(0){}
private:
    const State* owner_; uint32_t serial_; uint8_t slot_;
    friend struct Access;
};
struct Io {
    pspring::Io ring;
    // Same native Operation/GART Access plus measured deadline for every I/O.
    bool (*valid)(void*);
    // Seal the actual native owner, retaining hardware/storage after ambiguity.
    void (*fault)(void*);
};
struct Result {
    Status status; pspring::Result psp; VramTransferResult transfer;
    uint32_t command,applicationStatus,responseId,sessionId,protectionLevel,contentType;
    bool sharedWriteAttempted,invokeAttempted,pspAcknowledged,applicationAcknowledged,
         sessionValid,protectionValid,retentionRequired;
    uint8_t an[8],aksv[5],ainfo; // HDCP1 create output; never synthesized.
};
// Nontransferable lease over this ring's actual loaded HDCP/DTM records and
// allocator-owned 16KiB shared BOs. Ring scratch must provide >=16KiB; it is
// reused only after the preceding PSP command has fully completed. The native
// owner preserves this object, ring, scratch and callbacks until retire/reset.
// Scope/lock ownership remains with the native adapter; this is not a source
// of authority to adopt inherited display or authentication sessions.
class State {
public:
    State();
    State(const State&)=delete;State& operator=(const State&)=delete;
    State(State&&)=delete;State& operator=(State&&)=delete;
    Phase phase() const{return phase_;}
    bool retentionRequired() const{return phase_==Phase::Bound||phase_==Phase::FaultedRetained;}
private:
    struct Record {
        Topology topology; uint32_t displaySerial,authSerial,session;
        Protocol protocol; bool topologyMayExist,topologyActive,sessionMayExist,sessionValid;
    };
    Phase phase_; pspring::State* ring_; Io io_; uint32_t nextSerial_; bool guardLost_;
    Record records_[kDisplays];
    friend struct Access;
};
Status bind(pspring::State&,const Io&,State&,Result&);
Status addDisplay(State&,const Topology&,const pspring::PollOptions&,Display&,Result&);
Status removeDisplay(State&,const Display&,const pspring::PollOptions&,Result&);
Status createAuthentication(State&,const Display&,Protocol,Content,const pspring::PollOptions&,Authentication&,Result&);
Status queryAuthentication(State&,const Authentication&,const pspring::PollOptions&,Result&);
Status destroyAuthentication(State&,const Authentication&,const pspring::PollOptions&,Result&);
// Releases both ring-private leases only after all actual owned auth destroys
// and topology removals were acknowledged. Faulted states cannot retire.
Status retire(State&,Result&);
const char* statusString(Status);
} }
#endif
