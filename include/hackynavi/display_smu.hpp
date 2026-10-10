// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DISPLAY_SMU_HPP
#define HACKYNAVI_DISPLAY_SMU_HPP
#include "ip_layout.hpp"
#include "display_dentist.hpp"
namespace hackynavi { namespace displaysmu {
enum class Clock : uint8_t { Disp=8,Dpp=9,Dcf=11,Dtb=12 };
// PPCLK values from the pinned DCN32 SMU13 interface. These are independent
// firmware frequency tables, NOT correlated voltage states or current clocks.
enum class DpmClock : uint8_t { Soc=1,Memory=2,Fabric=3,Disp=8,Dpp=9,Dcf=11,Dtb=12 };
constexpr size_t kDpmClockCount=7,kDpmLevelCapacity=16;
struct DpmTable {
    DpmClock clock;
    uint32_t metadata;
    uint32_t rawFrequency[kDpmLevelCapacity];
    uint16_t frequencyMHz[kDpmLevelCapacity];
    uint32_t dcMaxMHz; // Raw GetDcModeMaxDpmFreq(0xC), full MHz reply.
    uint8_t levelCount;
    bool fineGrained;
};
struct DpmInventory { uint32_t firmwareVersion;DpmTable clocks[kDpmClockCount]; };
enum class Status { Ok,InvalidArgument,InvalidState,UnsupportedLayout,OutsideBar,
    ReadFailed,WriteFailed,DeviceUnavailable,Timeout,CommandFailed,UnknownCommand,
    BadPrerequisite,Busy,UnknownResponse,UnsupportedFirmware,InvalidFrequency,
    InvalidDpmInventory,InventoryCapacityExceeded,ChangedDpmInventory,ResetRequired,
    ObservationFailed,InvalidClockObservation };
enum class Stage { Preflight,PreviousResponse,ClearResponse,Argument,Message,Response,Output,HardMinimum,Complete };
struct Layout { bool valid;uint64_t barBytes;uint32_t message,argument,response; };
struct Io { void* context;bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t);void (*delayUs)(void*,uint32_t); };
// One shared response-read budget per public operation, including all hard-min
// status queries. No nested retries can multiply this bound. <=2 seconds delay.
struct Poll { uint32_t maxReads,delayUs; };
struct Result { Status status;Stage stage;uint32_t reads,writes,responseReads,delays,
    lastRegister,lastValue,message,argument,response,hardMinBits;
    bool mailboxWriteAttempted,minimumRequestAttempted; };
struct Frequency { uint32_t reportedKHz,fulfilledBits; };
// UCLK's returned argument has no unambiguous unit contract in the pinned
// DCN32 callers. Preserve it without treating it as MHz/kHz or a measurement.
struct MemoryMinimum { uint16_t requestedMHz;uint32_t rawReply,fulfilledBits; };
struct MemoryJournal {
    uint16_t minimumMHz,attemptedMHz; // minimumMHz is the last fulfilled floor.
    uint32_t rawReply,fulfilledBits;
    bool requestAttempted,commandAcknowledged,replyRead,fulfilled;
};
struct DmcubWaitJournal {
    uint32_t rawReply;
    bool requestAttempted,commandAcknowledged,replyRead;
};
// ACK-only DAL prerequisites: there is no corresponding fulfillment query.
// rawReply is uninterpreted; it is not a frequency sample or fixed-clock proof.
struct AckClockJournal {
    uint32_t requestedMHz,rawReply;
    bool requestAttempted,commandAcknowledged,replyRead;
};
enum class ColdPresetStage { Preflight,InitialObservation,IntermediateRequest,
    IntermediateObservation,FinalRequest,FinalObservation,Complete };
struct ColdPresetObservation {
    uint32_t rawPll,programmedVcoKHz,rawDentist,rawDfsDid,programmedKHz;
};
struct ColdPresetTarget { uint32_t programmedVcoKHz,programmedKHz;uint8_t dividerId; };
struct ColdPresetIo {
    void* context;
    // Actual native cold/FAMS/inbox/scope/deadline checks and a fresh stable
    // PLL/DFS/DENTIST observation. Initial target DID/kHz are zero (unspecified
    // old programming), never observed values. Other phases carry the exact
    // internally computed target; a returned true alone cannot satisfy its
    // raw-DID/change-done/frequency checks. No caller boolean grants ownership.
    bool (*observe)(void*,Clock,ColdPresetStage,const ColdPresetTarget&,ColdPresetObservation&);
};
struct ColdPresetStep {
    uint16_t requestedMHz;
    uint32_t rawReply,fulfilledBits;
    bool requestAttempted,commandAcknowledged,replyRead,fulfilled,observationValid;
    ColdPresetObservation observation;
};
struct ColdPresetJournal {
    bool started,initialObservationValid,usesIntermediate,complete;
    uint32_t programmedVcoKHz,requiredKHz;
    uint16_t lastFulfilledMHz;
    ColdPresetStage stage;
    displaydentist::Preset finalPreset;
    ColdPresetObservation initial;
    ColdPresetStep intermediate,final;
};
struct ColdPresetFrequency {
    displaydentist::Preset preset;
    Frequency accepted;
    ColdPresetObservation observation;
};
struct ColdPresetResult { Status status;ColdPresetStage stage;Result mailbox; };
Status buildLayout(const Navi31IpLayout&,uint64_t barBytes,Layout&);
// DAL mailbox C2PMSG 74/51/52, distinct from the MP1 graphics-power mailbox.
// Caller exclusively owns DAL and excludes PM/reset/removal. PMFW must already
// be running on this device/reset epoch. No DMA, watermark tables or PM policy
// are established here. Hard-min acknowledgement is not a frequency measurement.
class Controller {
public:
    Controller();
    Status initialize(const Layout&,const Io&,const Poll&,Result&);
    // Quiescent pixel pipeline only: no active pipe/DTO users during changes.
    // Caller supplies any required DMCUB/PM coordination for clock changes.
    // MHz encodes an upstream-selected clock preset, not an EDID pixel clock.
    // Nonzero requests only; lowering an active request is not allowed here.
    // At most 1950 MHz for DISP/DPP per DCN3.2 recommendation.
    // Output changes only after firmware-reported frequency and fulfillment of
    // BOTH requested clock and DPREFCLK have been checked.
    Status requestMinimum(Clock,uint16_t MHz,const Poll&,Frequency&,Result&);
    // One cold acquisition per DISP/DPP clock/reset epoch, sharing this real
    // DAL owner. Rejects an existing legacy floor and later legacy requests.
    // Exact source DID126 prelude when entering DISP DID127, then lower final
    // request; no generic lowering switch. One response budget spans both
    // commands/fulfillment queries and the native observer supplies one real
    // elapsed scope across every callback. No live FIFO/DTO transition here.
    // Raw reply and actual divider-derived frequency must agree exactly.
    // Output is cleared on failure; attempted journals survive ambiguity and
    // require verified reset, never legacy zero-MHz release or reconstruction.
    Status requestColdPreset(Clock,uint32_t programmedVcoKHz,uint32_t requiredKHz,
        const ColdPresetIo&,const Poll&,ColdPresetFrequency&,ColdPresetResult&);
    const ColdPresetJournal* coldPresetJournal(Clock) const;
    // Separate UCLK/PPCLK2 path: real input MHz and ACK, then BOTH UCLK(0x20)
    // and DPREFCLK(0x04) DAL fulfillment. Requires the same Navi31 PMFW gate
    // as display clocks. Caller owns quiescent pipes and actual DMCUB/PM
    // coordination; native initial integration requires DMCUB held in reset.
    // Nonzero monotonic floors only. This is NOT a fixed memory frequency,
    // a SOC/FCLK hold, a metrics observation or authorization for a DML plan.
    // Output is cleared on failure. Private journal changes before the first
    // possibly delivered request and survives ambiguous errors/normal stop.
    Status requestMemoryMinimum(uint16_t MHz,const Poll&,MemoryMinimum&,Result&);
    const MemoryJournal& memoryJournal() const{return memory_;}
    // Cold one-shot UCLK maximum: DAL0x0a, (PPCLK2<<16)|MHz. Caller validates
    // the actual DPM/DC inventory and owns all cold/FAMS prerequisites. Cannot
    // contradict this owner's fulfilled minimum; subsequent minima cannot
    // exceed the owned maximum. A matching minimum/maximum is still NOT a
    // clock measurement or complete fixed-clock authorization.
    Status requestMemoryMaximum(uint16_t MHz,const Poll&,Result&);
    const AckClockJournal& memoryMaximumJournal() const{return memoryMaximum_;}
    // Independent DCF deep-sleep floor: DAL0x0d, MHz without a clock selector.
    // Native caller rounds the actual calculated kHz upward. The firmware
    // parameter is 32-bit; this initial profile admits nonzero <=65535 MHz,
    // matching its DPM frequency domain and rejecting rather than truncating.
    // Each new prerequisite is attempted at most once per verified-reset epoch.
    // Journals precede possible delivery, survive faults/normal stop, and only
    // independently verified reset clears them. There is no release policy
    // or assumption about another owner's previous constraint here.
    Status requestDcfDeepSleepFloor(uint32_t MHz,const Poll&,Result&);
    const AckClockJournal& dcfDeepSleepJournal() const{return dcfDeepSleep_;}
    // DAL0x14 argument0: stop PMFW always waiting for DMCUB's MCLK response.
    // This does not disable DMCUB FAMS or memory-clock switching itself. Caller
    // owns the cold all-pipes-off transaction and subsequently sends the actual
    // FAMS-disable command. Journal precedes possible delivery and survives
    // errors/normal stop. Only independently verified reset clears ownership;
    // it does not promise a particular firmware default after that reset.
    Status disableDmcubMclkWait(const Poll&,Result&);
    const DmcubWaitJournal& dmcubWaitJournal() const{return dmcubWait_;}
    bool policyMayBeActive() const{return dmcubWait_.requestAttempted||
        memoryMaximum_.requestAttempted||dcfDeepSleep_.requestAttempted||
        coldPresets_[0].intermediate.requestAttempted||coldPresets_[0].final.requestAttempted||
        coldPresets_[1].final.requestAttempted;}
    // GetDpmFreqByIndex(0xB): query metadata at index 0xff, then each real
    // index; fine-grained tables have exactly min/max. GetDcModeMaxDpmFreq
    // (0xC) supplies the independent DC ceiling for every table. Two observations
    // must agree. One shared response budget bounds ALL table queries. The
    // explicit 16-level software capacity rejects larger tables, never truncates.
    // Success is firmware-reported inventory, not measured clocks, clock holds,
    // a DML bounding box, or a firmware watermark policy. UCLK remains MHz;
    // converting to DRAM MT/s belongs to the explicit board/model adapter.
    // Output is cleared on failure; ambiguous transport terminally faults this
    // mailbox owner. No clock or policy-setting commands are sent here.
    Status queryDpmInventory(const Poll&,DpmInventory&,Result&);
    // Legacy display-only terminal path, retained for existing callers:
    // caller has stopped all clock consumers, including display firmware,
    // under global exclusion. Sends frequency 0 for existing display minima
    // and checks ACK. The current source audit has NOT established that zero
    // proves full withdrawal; success here is the legacy command protocol,
    // not independently verified release, stopped clocks or retired DMA.
    // No resulting frequency is published; do not extend this to new owners.
    // Faulted/ambiguous owners require verified reset instead of this path.
    // Any UCLK, DCF deep-sleep or DMCUB-wait policy request makes this return
    // ResetRequired WITHOUT hardware writes:
    // pinned DCN32 source does not establish zero as UCLK withdrawal. Its
    // journals remain retained until independently verified reset. No previous
    // DMCUB-wait policy is invented or automatically restored here.
    Status releaseMinimaAfterQuiescence(const Poll&,Result&);
    bool ready() const{return ready_;}
    bool faulted() const{return faulted_;}
    bool minimumMayBeActive() const{return minimum_;}
    uint32_t firmwareVersion() const{return version_;}
    void clearAfterVerifiedReset();
private:
    Controller(const Controller&)=delete;Controller& operator=(const Controller&)=delete;
    Layout layout_;Io io_;bool ready_,faulted_,minimum_;uint32_t version_;
    uint16_t requested_[4];
    MemoryJournal memory_;
    DmcubWaitJournal dmcubWait_;
    AckClockJournal memoryMaximum_,dcfDeepSleep_;
    ColdPresetJournal coldPresets_[2];
    Status finish(Status,Result&);
    Status read(uint32_t,uint32_t&,Result&);
    Status write(uint32_t,uint32_t,Result&);
    Status response(const Poll&,Result&);
    Status send(uint32_t,uint32_t,const Poll&,uint32_t&,Result&,ColdPresetStep* = nullptr);
    Status requestColdStep(Clock,ColdPresetStep&,const Poll&,Result&);
};
} }
#endif
