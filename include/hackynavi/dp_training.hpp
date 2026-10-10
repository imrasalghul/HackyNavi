// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DP_TRAINING_HPP
#define HACKYNAVI_DP_TRAINING_HPP
#include "hackynavi/dp_aux.hpp"
#include "hackynavi/dio_link.hpp"
namespace hackynavi { namespace dptrain {
enum class Status { Ok, InvalidArgument, InvalidState, AuxFailed, SourceFailed,
    UnsupportedReceiver, UnsupportedTopology, LinkLimits, VerifyFailed,
    ClockRecoveryFailed, EqualizationFailed, LinkLost };
enum class Stage { Preflight, Capabilities, PowerUp, Configure, SourceEnable,
    ClockRecovery, Equalization, ExitTraining, VideoIdle, VerifyLink, Complete };
struct Limits { uint8_t maxLanes; uint32_t maxSymbolClockKHz; };
struct Config { uint8_t lanes; uint32_t symbolClockKHz; Limits source;
    bool downspread; }; // must match the already programmed source PLL
struct Caps { uint8_t raw[15],revision,maxLanes; uint32_t maxSymbolClockKHz;
    bool extended,tps3,tps4,downspread,mstCapable; uint32_t crDelayUs,eqDelayUs; };
struct Result {
    Status status; Stage stage; Caps sink;
    uint32_t auxReads,auxWrites,sourceCommands,crAttempts,eqAttempts,delayUs;
    uint32_t lastDpcd; uint8_t swing,preEmphasis,laneSet,statusBytes[6];
    dio::Pattern eqPattern;
    bool sourceEnableAttempted,sinkPowerMayBeChanged,patternMayBeActive;
    bool cleanupAttempted,sourceDisabled,sinkPatternCleared,sinkPoweredDown;
    bool recoveryRequired,trained;
};
enum class AuxStatus { Ok, Nack, Failed };
// Ordered backend protocol: reads/writes return Ok only for the exact byte
// count ACKed by the receiver. Source callbacks program the real transmitter;
// enable/disable must observe DIG_ENABLE, not just consume firmware commands.
// Each healthy callback says whether further operations on that transport are
// permitted after failure. No failed operation is automatically replayed here.
struct Io { void* context;
    AuxStatus (*readDpcd)(void*,uint32_t,uint8_t*,uint8_t);
    AuxStatus (*writeDpcd)(void*,uint32_t,const uint8_t*,uint8_t);
    bool (*sourceEnable)(void*,uint32_t);
    bool (*sourcePattern)(void*,dio::Pattern);
    bool (*sourceDrive)(void*,uint8_t,uint8_t);
    bool (*sourceVideo)(void*); bool (*sourceDisable)(void*);
    bool (*auxHealthy)(void*); bool (*sourceHealthy)(void*);
    void (*delayUs)(void*,uint32_t);
};
// One cold native SST 8b/10b training attempt, RBR/HBR/HBR2/HBR3 and 1/2/4
// lanes, uniform swing/pre-emphasis. Reads base/extended caps and topology;
// rejects branches, active MST, LTTPR, alternate scrambler, active FEC/DSC,
// unsupported rates/coding and absent enhanced framing. UHBR is never used.
// Sink power D0 is read back after 1ms. TPS1 CR is bounded to 100 total / five
// unchanged-swing replies; EQ to six replies. TPS4 keeps scrambling enabled.
// Disables sink training, programs source video idle and checks every active
// lane CR/EQ/symbol plus interlane alignment again after 5ms. Does not enable
// a pixel stream, OTG or scanout. Failure keeps its original error and attempts
// bounded sink-pattern/D3 and source-disable cleanup only on healthy transports.
// No rate fallback/retrain occurs automatically. Caller chooses bandwidth and
// explicit board/source limits, owns this connector and holds the global
// display/firmware/reset lock for the entire operation. The source is prepared
// and stream idle, source PLL/power is ready, and AUX is open for this route.
Status train(const Io&,const Config&,Result&);
struct Delay { void* context; void (*delayUs)(void*,uint32_t); };
struct HardwareResult { Result training; dpaux::Status auxStatus;
    dpaux::Result aux; dio::Status sourceStatus; dio::Result source; };
// Production binding: actual AUX packets, DCN3.2 DIO and DMCUB VSEMPH/PHY
// commands. Rejects a closed/faulted transport, mismatched DDC/HPD/lane routing,
// or an already enabled source. Leaves the caller's AUX engine open. After
// success, Link is VideoPattern; after confirmed cleanup it is Closed.
Status train(dpaux::Engine&,dio::Link&,dmcub::Inbox&,const Config&,
    const dmcub::PollOptions&,const Delay&,HardwareResult&);
const char* statusString(Status);
} }
#endif
