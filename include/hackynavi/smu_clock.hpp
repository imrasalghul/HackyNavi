// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_SMU_CLOCK_HPP
#define HACKYNAVI_SMU_CLOCK_HPP
#include "smu.hpp"
#include "vram_transfer.hpp"
namespace hackynavi { namespace smuclock {
constexpr uint32_t kInterfaceVersion=0x3d;
constexpr size_t kMetricsBytes=392,kClockCount=13;
enum class Clock : uint8_t { Soc=1,Fabric=3 };
enum class Status { Ok,InvalidArgument,InvalidState,UnsupportedLayout,UnsupportedInterface,
    OwnerUnavailable,Timeout,ClockWentBackwards,ReadFailed,WriteFailed,DeviceUnavailable,
    CommandFailed,UnknownCommand,BadPrerequisite,Busy,UnknownResponse,ChangedFirmware,
    DisabledFeature,ChangedFeatures,InvalidFrequency,InvalidAllocation,VisibilityFailed,
    TransferFailed,InvalidMetrics,SerialExhausted };
enum class Stage { Preflight,PreviousResponse,ClearResponse,Argument,Message,Response,Output,
    Features,Bounds,Maximum,Minimum,AddressHigh,AddressLow,Transfer,Visibility,Readback,Complete };
struct Poll { uint32_t timeoutUs,maxResponseReads,delayUs; };
struct RangeJournal {
    uint16_t requestedMinMHz,requestedMaxMHz,acknowledgedMinMHz,acknowledgedMaxMHz;
    bool maximumAttempted,maximumAcknowledged,minimumAttempted,minimumAcknowledged;
    bool automaticMaximumAttempted,automaticMaximumAcknowledged,automaticMinimumAttempted,automaticMinimumAcknowledged;
};
struct TransportJournal {
    uint64_t requestSerial,lastMessageSerial,settledMessageSerial;
    uint32_t requestMessage,requestArgument,lastMessage,lastArgument,lastResponse;
    Stage stage;
    bool previousResponseReadValid,responseClearAttempted,responseClearCompleted;
    bool argumentAttempted,argumentCompleted,messageAttempted,messageCallbackSucceeded;
    bool responseReadValid,responseSettled;
};
struct DeepSleepJournal {
    uint64_t originalFeatures,heldFeatures;
    uint32_t changedBits;
    bool captured,disableAttempted,disableAcknowledged,inhibitReadbackComplete,inhibited;
    bool restoreAttempted,restoreAcknowledged,restoreReadbackComplete,released;
};
struct DfCstateJournal {
    uint64_t disallowFeatures,allowFeatures;
    bool disallowAttempted,disallowAcknowledged,disallowFeaturesVerified,disallowed;
    bool allowAttempted,allowAcknowledged,allowFeaturesVerified,released;
};
struct Journal {
    RangeJournal soc,fabric;
    DeepSleepJournal deepSleep;
    DfCstateJournal dfCstate;
    TransportJournal transport;
    uint64_t tableGpuAddress,transferSerial,tableAttemptSerial,completedTableAttemptSerial;
    bool addressHighAttempted,addressHighAcknowledged,addressLowAttempted,addressLowAcknowledged;
    bool transferAttempted,transferAcknowledged,cpuVisible,readComplete,ownerFaultNotified,tableSealed,rangesSealed;
    uint32_t reconciliationPriorResponse;
    bool reconciliationAttempted,reconciliationPriorSettled,reconciliationCompleted;
    Status faultStatus;
    Stage faultStage;
};
struct Metrics {
    uint8_t raw[kMetricsBytes];
    uint32_t currentClockMHz[kClockCount],counter;
    uint64_t transferSerial;
    // Point-in-time comparison only. No DAL fulfillment bits exist for these
    // soft ranges, and this does not prove future clocks or deep-sleep policy.
    bool socRangeObserved,fabricRangeObserved;
};
struct Result {
    Status status;Stage stage;
    uint32_t reads,writes,responseReads,delays,lastRegister,lastValue,message,argument,response;
    uint64_t elapsedUs,runningFeatures;
    bool mailboxWriteAttempted;
    VramTransferResult transfer;
};
struct Io {
    void* context;
    // Native implementation must validate the actual normal/retirement scope,
    // installed GART access, common Firmware owner, and retained provider.
    bool (*valid)(void*);
    bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write32)(void*,uint32_t,uint32_t); // false may still have landed
    void (*delayUs)(void*,uint32_t);
    uint64_t (*timeUs)(void*);
    // Real ordering before uncached indirect VRAM readback. HDP6 has no
    // invalidate callback in pinned Linux; never invent an invalidate write.
    bool (*beforeCpuRead)(void*);
    // REQUIRED: seal the common native Firmware owner on protocol ambiguity,
    // so existing PPSMC entrypoints cannot continue using its old Awake state.
    // Must not reenter this object or acquire its already-held owner locks.
    void (*faultOwner)(void*);
};
// Companion of the EXISTING smu::State, not a second power/mailbox authority.
// Native factory must retain the containing Firmware/GART objects and serialize
// all PPSMC users with the same Firmware -> GART locks and actual group scope.
// The borrowed state, allocator and callbacks must outlive this object. No
// first-message skip, SMU initialization, GFXOFF operation or CP_STAT polling.
// All methods are externally serialized; inputs/results must not alias state.
class Controller {
public:
    Controller();
    Status bind(const Navi31IpLayout&,uint64_t barBytes,const smu::State&,
        const Io&,const Poll&,Result&);
    // Actual running DPM feature gates and queried min/max/DC ceilings;
    // maximum uses source-required MHz+1, then minimum uses integer MHz.
    // Only ACKs are claimed. Native caller owns any required memory/display
    // quiescence and deep-sleep policy; this does not manufacture fixed clocks.
    Status requestSoftRange(Clock,uint16_t minMHz,uint16_t maxMHz,const Poll&,Result&);
    // One-shot, exact SMU13.0.0 DS_SOCCLK/DS_FCLK policy. Capture the actual
    // stable enabled mask, disable only its bits 12/13, and verify the exact
    // resulting full mask. No DF C-state allowance or fixed-clock claim.
    // Native caller must own the global policy and prove cold quiescence.
    Status inhibitSocFabricDeepSleep(const Poll&,Result&);
    // After dependent consumers quiesce and this owner's ranges are released,
    // restore ONLY the captured bits. Any ambiguity or unexpected feature
    // change seals the owner and retains the private policy journal.
    Status restoreSocFabricDeepSleepAfterQuiescence(const Poll&,Result&);
    // Own the external-client DF C-state policy through PPSMC 0x3b,0.
    // ACK establishes the request, not a sampled DF state or prior allowance.
    // Native caller proves cold quiescence and serializes all policy users.
    Status disallowDfCstate(const Poll&,Result&);
    // Terminal owned policy: PPSMC 0x3b,1 after all consumers, metrics and
    // ranges quiesce. This explicitly ALLOWS C-state; it does not restore an
    // unknown previous allowance. Ambiguity retains ownership without retry.
    Status allowDfCstateAfterQuiescence(const Poll&,Result&);
    // Dedicated actual allocation, no caller-supplied GPU address or bytes.
    // GPU address derives through allocator.addresses on the full live handle.
    // Address publication is persistent; no rebinding/zero-address withdrawal.
    Status publishMetrics(const VramAllocator&,VramAllocationHandle,const Poll&,Result&);
    // One actual table5 SMU->VRAM transfer, ACK, real visibility callback, and
    // readVram through the retained allocation/MM_INDEX window. No cache reuse.
    // Exact interface0x3d layout; output cleared on any failure. Zero counters
    // and counter wrap are valid; no unsupported freshness semantics invented.
    Status captureMetrics(const Poll&,Metrics&,Result&);
    // Terminal local producer close, never a firmware pointer-clear command.
    // Requires the last actual transfer ACK, visibility and complete readback.
    // Caller excludes EVERY other driver-table producer with the same actual
    // Firmware/GART owner locks and generation-bound scope, and permanently
    // prevents later external transfers before releasing this exact allocation.
    // Failure/ambiguous publication cannot be retired through this method.
    // Clock ranges remain owned independently after the scratch producer closes.
    Status sealMetricsAfterCompletedTransfer(const Poll&,Result&);
    // One terminal attempt for this FAULTED owner's still-retained same table.
    // Native Io.valid MUST bind a real retirement scope, same pre-reset epoch,
    // provider, allocator and mailbox owner; no MODE1/power-loss/rebind allowed.
    // Known prior replies settle the mailbox, never prove transfer success.
    // Rechecks version/interface, ACKs BOTH address halves and a fresh table5
    // transfer, performs ordered full readback, then closes the table producer.
    // Never clears the common fault or restores ready(); ranges stay retained.
    Status reconcileMetricsForRetirement(const Poll&,Result&);
    // Terminal policy change, for this owner's successfully requested ranges
    // only: source automatic maximum 0xffff followed by minimum zero. This
    // does NOT restore a foreign policy or claim a measured clock value.
    // Native caller proves all dependent consumers quiescent under its actual
    // retirement scope before/after, and owns the global SOC/Fabric policy.
    // Partial/failed ranges cannot use this path. Any ambiguity retains them.
    // Successful completion irreversibly forbids later range requests.
    Status returnRangesToAutomaticAfterQuiescence(const Poll&,Result&);
    bool ready() const{return initialized_&&!faulted_&&!terminalOnly_;}
    bool faulted() const{return faulted_;}
    bool retentionRequired() const;
    bool tableRetentionRequired() const;
    bool rangeRetentionRequired() const;
    bool deepSleepRetentionRequired() const;
    bool dfCstateRetentionRequired() const;
    const Journal& journal() const{return journal_;}
    // No generic reset-clear or foreign-range restoration API. Existing MODE1
    // proof alone must not be treated as an observed SMU table pointer clear.
private:
    Controller(const Controller&)=delete;Controller& operator=(const Controller&)=delete;
    const smu::State* owner_;smu::Layout layout_;Io io_;
    bool bound_,initialized_,faulted_,terminalOnly_;uint32_t version_;
    const VramAllocator* allocator_;VramAllocationHandle allocation_;VramRange range_;
    Journal journal_;Result* active_;Poll poll_;uint64_t start_,last_;
    Status begin(const Poll&,Result&);
    Status end(Status);
    bool guard();
    Status read(uint32_t,uint32_t&);
    Status write(uint32_t,uint32_t);
    Status delay();
    Status response(bool acceptKnownTerminal=false);
    Status send(uint32_t,uint32_t,bool,uint32_t&,bool acceptKnownPrevious=false);
    Status features(uint64_t&);
    Status setDfCstate(bool allow,const Poll&,Result&);
    bool allocationValid() const;
    static bool transferRead(void*,uint32_t,uint32_t&);
    static bool transferWrite(void*,uint32_t,uint32_t);
};
} }
#endif
