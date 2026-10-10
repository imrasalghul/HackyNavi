// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_HDMI_FRL_HPP
#define HACKYNAVI_HDMI_FRL_HPP
#include <stddef.h>
#include <stdint.h>
namespace hackynavi { namespace hdmifrl {
enum class Status { Ok,InvalidArgument,InvalidState,AdmissionFailed,Revoked,
    ClockFailed,ScdcFailed,SourceFailed,VerifyFailed,UnsupportedReceiver,
    UnsupportedTestRequest,ReadyTimeout,TrainingTimeout,StartTimeout,
    LowerRateRequired,RetrainRequired,InvalidPattern,FfeExhausted };
enum class Phase { Closed,Training,Trained,Packets,Started,Faulted };
enum class Operation { Train,StartPackets,Stop };
struct Config {
    uint8_t rateCode,sourceMaxRateCode,sinkMaxRateCode,maxFfe;
    // Identity of the retained, calculated mode/capacity plan and physical
    // route. These numbers are NOT locks, capability validation, or admission.
    uint64_t modePlanIdentity,routeGeneration;
};
struct Io {
    void* context;
    // Acquire the shared sleepable operational gate and check the exact
    // retained mode plan, rate, source/EDID bounds, clock/power/route ownership,
    // inactive stream and reset/PM/removal exclusion. False performs no I/O.
    // Every subordinate source/DDC operation must observe immediate revocation.
    bool (*enter)(void*,Operation,const Config&);
    void (*leave)(void*);
    bool (*valid)(void*);
    bool (*nowUs)(void*,uint64_t&); // measured monotonic time, never a counter
    void (*delayUs)(void*,uint32_t);
    bool (*readScdc)(void*,uint8_t,uint8_t*,size_t);
    bool (*writeScdc)(void*,uint8_t,uint8_t); // false may have landed
    bool (*ddcHealthy)(void*); // actual DDC engine completion/ownership state
    // Implement actual DCCG/HPO/PHY programming and verify completion; merely
    // accepting a firmware queue entry is insufficient. No default callbacks.
    bool (*prepareSource)(void*,const Config&);
    bool (*drive)(void*,uint8_t ffe);
    // Four encoded HPO patterns0..7; 0xff means leave that lane unchanged.
    bool (*patterns)(void*,const uint8_t[4]);
    // Switch trained HPO link to packet mode with stream still blank/disabled.
    bool (*packetMode)(void*);
    // Quiesce/verify source off. Does not unconditionally reset shared DCN.
    bool (*stopSource)(void*);
};
struct Result {
    Status status;Phase phase;
    uint32_t reads,writes,sourceCommands,polls;
    uint8_t lastOffset,lastValue,ffe;
    uint64_t elapsedUs;
    bool sourceMayBeChanged,sinkMayBeChanged,sourceOff,sinkRateOff;
    bool trainingObserved,startObserved,ddcRecoveryRequired,retentionRequired;
};
// CPU protocol state, not a native ownership primitive. Methods are serialized
// by the native owner; source/DDC callbacks perform real work behind Io::enter.
// Single selected rate1..6 (HDMI2.1 FRL). Never silently downrates/retries a mode.
class Link {
public:
    Link();
    // Bounded FLT_READY then FLT_UPDATE/LTP processing, 210ms each with2ms
    // polling, matching pinned AMD's105 polls. Timers include I/O time. Sink
    // compliance test modes are rejected, never made into timeout successes.
    Status train(const Config&,const Io&,Result&);
    // Separate 200ms FRL_START handshake after packet-mode entry. A new
    // FLT_UPDATE requires caller revalidation/retraining, never success.
    // Does not start/unblank an OTG/HPO stream or claim displayed pixels.
    Status startPackets(Result&);
    // Source-off proof precedes receiver FRL_RATE=0. Ambiguous cleanup keeps
    // state/resources owned. Revocation prevents further hardware I/O.
    Status stop(Result&);
    Phase phase() const {return phase_;}
    bool requiresHardwareRetirement() const {return sourceDirty_||sinkDirty_||ddcDirty_;}
    const Config& configuration() const {return config_;}
    // Only after independent reset/power-loss proof and all users withdrawn.
    void clearAfterVerifiedReset();
private:
    Config config_;Io io_;Phase phase_;
    bool sourceDirty_,sinkDirty_,ddcDirty_,trainingObserved_,startObserved_;
    uint8_t ffe_;uint64_t lastTime_;
    Status finish(Status,Result&);
    Status clock(uint64_t&);
    Status read(uint8_t,uint8_t*,size_t,Result&);
    Status write(uint8_t,uint8_t,Result&);
    Status source(bool (*)(void*),Result&);
    Status cleanup(Result&);
    Status failed(Status,Result&);
    Status waitTick(uint64_t start,uint32_t budget,Status timeout,Result&);
    Status training(Result&);
    Link(const Link&)=delete;Link& operator=(const Link&)=delete;
};
const char* statusString(Status);
} }
#endif
