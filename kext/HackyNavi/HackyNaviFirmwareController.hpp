// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_KERNEL_FIRMWARE_CONTROLLER_HPP
#define HACKYNAVI_KERNEL_FIRMWARE_CONTROLLER_HPP
#include "HackyNaviGARTController.hpp"
#include <hackynavi/firmware_resources.hpp>
#include <hackynavi/psp.hpp>
#include <hackynavi/psp_ring.hpp>
#include <hackynavi/psp_display_ta.hpp>
#include <hackynavi/gfx_firmware.hpp>
#include <hackynavi/gfx_init.hpp>
#include <hackynavi/smu.hpp>
#include <hackynavi/smu_clock.hpp>
#include <hackynavi/display_idle.hpp>
#include <hackynavi/dmcub_firmware.hpp>
#include <hackynavi/dmcub.hpp>

class HackyNaviDisplayController;

// Real kernel PSP13 adapter. Uses embedded AMD firmware, owned allocations in
// the GART controller's SAME arena, uncached BAR5 and actual PSP responses.
// No userspace resource service or synthetic completion path is involved.
// Hardware/allocation/submission entry points require the caller's live
// HackyNaviGARTController::Access, installed before descendant locks. Normal
// work requires Operation authority; cleanup accepts RetirementScope. Only
// CPU-only unpublished release is permitted without live hardware authority.

class HackyNaviFirmwareController : public OSObject {
    OSDeclareDefaultStructors(HackyNaviFirmwareController)
public:
    enum class State { Empty, Prepared, SosLoaded, RingReady, TmrReady, FirmwareLoaded, Quiesced,
                       FailedUnpublished, FaultedRetained, Retired };
    // Requires initialized MMHUB/HDP on this driver-owned, powered device,
    // cold PSP bootloader state, valid firmware/display/training reservations,
    // stopped CP/SDMA and caller-wide exclusion of suspend/reset/removal.
    // Factory only allocates CPU/VRAM bookkeeping; it makes no hardware writes.
    static HackyNaviFirmwareController* withController(HackyNaviGARTController*, IOReturn&);
    IOReturn loadSos(const hackynavi::psp::Options&);
    IOReturn createRing(const hackynavi::pspring::PollOptions&);
    IOReturn queryBootConfiguration(const hackynavi::pspring::PollOptions&, uint32_t&);
    // Uploads the real SOS TOC, obtains the PSP-reported TMR size, allocates
    // owned aligned VRAM and waits for SETUP_TMR's real fence and response.
    IOReturn initializeTmr(const hackynavi::pspring::PollOptions&);
    // Real ordered SMU/SDMA/GC firmware uploads and PSP authentication/load,
    // RLC autoload command and CP/bootload polling. Explicit non-MES profile
    // for revision C8. Success permits GFXHUB setup; CP queues follow later.
    IOReturn loadGraphicsFirmware(const hackynavi::pspring::PollOptions&);
    bool loadedFirmware(uint32_t pspType, hackynavi::gfxfw::LoadedSegment&);
    struct TrustedApplicationResult {
        IOReturn result;
        uint64_t generation,elapsedUs;
        hackynavi::pspta::Status parser;
        bool parserAttempted;
        hackynavi::pspring::Result ring;
        hackynavi::pspring::TaInformation application;
        hackynavi::VramTransferResult firmwareUpload,sharedUpload;
    };
    // Lazy actual HDCP/DTM TA loading on this owner's PSP ring/TMR. Exact AMD
    // embedded entry, private firmware staging and distinct 16KiB shared VRAM;
    // both uploads are readback-verified before real LOAD_TA submission. Same
    // Operation/epoch and one measured deadline cover every MMIO callback.
    // No startup caller invokes these yet. Loading creates only our own TA
    // session; it proves neither application initialization nor inherited HDCP
    // absence. Native opaque application commands are deliberately not exposed.
    IOReturn loadTrustedApplication(HackyNaviFunctionGroup::Operation&,
        hackynavi::pspta::Kind,uint32_t timeoutUs,TrustedApplicationResult&);
    // A typed display lease prevents individual unload until its own sessions
    // and topology have retired. Failed/ambiguous unload retains its VRAM.
    IOReturn unloadTrustedApplication(HackyNaviFunctionGroup::Operation&,
        hackynavi::pspta::Kind,uint32_t timeoutUs,TrustedApplicationResult&);
    struct DisplayTrustedResult {
        IOReturn result;
        uint64_t generation,elapsedUs;
        hackynavi::pspdisplayta::Result protocol;
        TrustedApplicationResult application;
        uint32_t ownedDisplays,ownedAuthentications,destroyedAuthentications,
                 removedDisplays,unloadedApplications;
        bool protocolAttempted,applicationAttempted,bindingRetired,retentionRequired;
    };
    // Kernel-owned HDMI TMDS only. Caller retains the actual Display object
    // and installs normal GART::Access before entry; these methods acquire
    // Display -> Firmware -> GART. Topology is derived from its actual owned
    // Running stream and ROM route, never caller-supplied IDs/handles. Both
    // real TAs load lazily under the same measured deadline. No startup or
    // userspace selector calls these methods. Creating a TA authentication
    // session is not a sink handshake, encryption or inherited-HDCP proof.
    IOReturn registerDisplayTrustedTopology(HackyNaviFunctionGroup::Operation&,
        HackyNaviDisplayController&,uint8_t path,uint32_t timeoutUs,DisplayTrustedResult&);
    IOReturn createDisplayAuthentication(HackyNaviFunctionGroup::Operation&,
        HackyNaviDisplayController&,uint8_t path,hackynavi::pspdisplayta::Protocol,
        hackynavi::pspdisplayta::Content,uint32_t timeoutUs,DisplayTrustedResult&);
    IOReturn queryDisplayAuthentication(HackyNaviFunctionGroup::Operation&,
        HackyNaviDisplayController&,uint8_t path,uint32_t timeoutUs,DisplayTrustedResult&);
    // These cleanup methods use only saved private tokens. A stopped stream
    // need not become Running again to destroy its owned session/topology.
    IOReturn destroyDisplayAuthentication(HackyNaviFunctionGroup::Operation&,
        HackyNaviDisplayController&,uint8_t path,uint32_t timeoutUs,DisplayTrustedResult&);
    IOReturn removeDisplayTrustedTopology(HackyNaviFunctionGroup::Operation&,
        HackyNaviDisplayController&,uint8_t path,uint32_t timeoutUs,DisplayTrustedResult&);
    // Real retirement authority only. Destroy owned auth, remove owned DTM
    // entries, release both protocol leases, then unload actual TA IDs. No
    // allocation, new topology or inherited/global command is permitted.
    // Safe before or after owned streams stop; normally called before Display
    // teardown. A completely unused TA owner is a successful no-op.
    IOReturn retireDisplayTrustedApplications(HackyNaviFunctionGroup::RetirementScope&,
        uint32_t timeoutUs,DisplayTrustedResult&);
    struct ClockResult {
        IOReturn result;
        uint64_t generation,elapsedUs;
        hackynavi::smuclock::Result bind,operation,tableClose,automatic,deepSleepRestore,dfCstateAllow;
        hackynavi::smuclock::Journal journal;
        hackynavi::smu::Result awake;
        hackynavi::displayidle::Result idle;
        // Observation only; the private Display object and its journals are
        // revalidated under Display -> Firmware -> GART, never supplied here.
        uint64_t famsSerial;
        bool runningDisplayCoordinated;
        hackynavi::dmcub::Result displayCoordination;
    };
    struct MetricsObservation {
        ClockResult operation;
        hackynavi::smuclock::Metrics metrics;
        bool valid;
    };
    // Existing Firmware/PPSMC owner only. Caller installs actual normal GART
    // Access before entering; one measured deadline spans every subprotocol.
    // Initial range programming additionally requires no queue/VM/display
    // owner, freshly idle physical pipes and disabled/reset DMCUB. ACKed soft
    // ranges are not DAL fulfillment or fixed/measured clocks.
    IOReturn requestSocFabricRange(HackyNaviFunctionGroup::Operation&,
        hackynavi::smuclock::Clock,uint16_t minMHz,uint16_t maxMHz,uint32_t timeoutUs,ClockResult&);
    // Before range programming, take a one-shot observed SOC/Fabric DS policy
    // under the same cold consumer/reset checks. Restores only captured bits
    // at retirement; DF C-state allowance remains a separate prerequisite.
    IOReturn inhibitSocFabricDeepSleep(HackyNaviFunctionGroup::Operation&,uint32_t timeoutUs,ClockResult&);
    // Same cold ownership checks, one-shot external-client disallow request.
    // Retirement deliberately ALLOWS DF C-state after dependent users stop;
    // neither command is a getter or restoration of a foreign policy.
    IOReturn disallowDfCstate(HackyNaviFunctionGroup::Operation&,uint32_t timeoutUs,ClockResult&);
    // Owns and zero-initializes a private VRAM page before real address/transfer
    // commands. Returns the exact interface0x3d metrics after ACK and ordered
    // uncached MM_DATA readback. No cached/CPU-generated metrics fallback.
    IOReturn captureSmuMetrics(HackyNaviFunctionGroup::Operation&,uint32_t timeoutUs,MetricsObservation&);
    // Terminal producer seal, after actual queue/display owners retire. Does
    // not claim that the firmware's driver-table address was cleared. Failed
    // or ambiguous transfers remain retained; no fresh DMA is submitted here.
    // Touched SOC/Fabric soft ranges return to the source's automatic policy
    // only after fresh consumer quiescence. This does not restore an unknown
    // previous owner's constraints or withdraw Display's DAL/UCLK requests.
    // Owned DF C-state returns to ALLOW, then captured SOC/Fabric DS bits
    // restore, only after the owned ranges and metrics producer retire.
    IOReturn retireClockState(HackyNaviFunctionGroup::RetirementScope&,uint32_t timeoutUs,ClockResult&);
    // Separate early-startup path, only with no queue/VM/display owner, healthy
    // FirmwareLoaded and BothEnabled GART. Fresh zero CSIB/disabled SRM and
    // physical halt/drain checks are mandatory. Keeps PSP/TMR storage pinned;
    // success is required before the later PSP retirement may destroy TMR.
    IOReturn retireAutoloadRlc(HackyNaviFunctionGroup::RetirementScope&,
        const hackynavi::gfxinit::RetirementPollOptions&);
    // Requires every GPU consumer of firmware/TMR already quiesced. Refuses
    // while the SDMA owner exists. Sends DESTROY_TMR and DESTROY_RINGS before
    // releasing their buffers. Faulted hardware needs explicit reset recovery.
    IOReturn retireAfterQuiescence(const hackynavi::pspring::PollOptions&);
    // This does not reset hardware; call only after separately verified reset
    // or power removal proves all PSP/engine references have retired.
    IOReturn releaseAfterDeviceReset();
    State state() const { return state_; }
    const hackynavi::psp::Result& lastSosResult() const { return sosResult_; }
    const hackynavi::pspring::Result& lastRingResult() const { return ringResult_; }
    const hackynavi::gfxfw::Result& lastGraphicsResult() const { return graphicsResult_; }
    const hackynavi::gfxinit::Result& lastAutoloadRetirementResult() const { return autoloadRetirementResult_; }
    const hackynavi::smu::Result& lastSmuResult() const { return smuResult_; }
    const hackynavi::dmcubfw::Result& lastDisplayFirmwareResult() const { return displayFirmwareResult_; }
    bool requiresHardwareRetirement() const { return hardwareHold_; }
protected:
    bool init() override;
    void free() override;
private:
    friend class HackyNaviGraphicsQueue;
    friend class HackyNaviDisplayController;
    friend class HackyNaviEngineController;
    enum Slot { SosStage, Ring, Command, Fence, Payload, Tmr, MetricsTable,
                HdcpStage, HdcpShared, DtmStage, DtmShared, SlotCount };
    IOReturn initialize(HackyNaviGARTController*);
    bool providerReady() const;
    // Lock order is this mutex, then GART mutex. Held for whole protocols and
    // their MM_INDEX window use; callbacks never recursively acquire either.
    IOReturn begin(State);
    IOReturn finish(IOReturn);
    IOReturn fault();
    void holdHardware();
    IOReturn releaseAllocationsLocked();
    hackynavi::psp::Io sosIo();
    hackynavi::pspring::Io ringIo();
    static bool read32(void*, uint32_t, uint32_t&);
    static bool write32(void*, uint32_t, uint32_t);
    static bool flushHdp(void*);
    static bool beforeReadback(void*);
    static void delayUs(void*, uint32_t);
    static void delayMs(void*, uint32_t);
    IOReturn beginClock(HackyNaviFunctionGroup::Operation&,uint32_t,ClockResult&,uint64_t started=0);
    IOReturn finishClock(ClockResult&,IOReturn,HackyNaviDisplayController* =nullptr);
    IOReturn bindClock(ClockResult&);
    IOReturn clockQuiescent(ClockResult&,HackyNaviDisplayController* =nullptr);
    enum class ClockPolicy { Range, InhibitDeepSleep, DisallowDfCstate };
    // A non-null Display is accepted only from its own locked entry point.
    // These acquire Firmware/GART; they never acquire Display recursively.
    IOReturn changeClockPolicy(HackyNaviFunctionGroup::Operation&,ClockPolicy,
        hackynavi::smuclock::Clock,uint16_t,uint16_t,uint32_t,ClockResult&,
        HackyNaviDisplayController* =nullptr,uint64_t started=0);
    IOReturn retireClockStateInternal(HackyNaviFunctionGroup::RetirementScope&,
        uint32_t,ClockResult&,HackyNaviDisplayController* =nullptr,uint64_t started=0);
    IOReturn prepareMetricsPage(ClockResult&);
    // Only Engine's private pre-MODE1 path may invoke this. The actual
    // Lifecycle has entered recovery but has not attempted reset. Never
    // restore normal firmware readiness after this terminal table operation.
    IOReturn prepareClockTableForReset(HackyNaviFunctionGroup::RetirementScope&,uint32_t,ClockResult&);
    static bool clockValid(void*);
    static bool clockRead(void*,uint32_t,uint32_t&);
    static bool clockWrite(void*,uint32_t,uint32_t);
    static void clockDelay(void*,uint32_t);
    static uint64_t clockTime(void*);
    static bool clockVisibility(void*);
    static void clockFault(void*);
    IOReturn beginTrustedApplication(HackyNaviFunctionGroup::Operation&,
        hackynavi::pspta::Kind,uint32_t,TrustedApplicationResult&,uint64_t started=0);
    IOReturn finishTrustedApplication(hackynavi::pspta::Kind,
        TrustedApplicationResult&,IOReturn,bool faulted=false);
    static bool taValid(void*);
    static bool taRead(void*,uint32_t,uint32_t&);
    static bool taWrite(void*,uint32_t,uint32_t);
    static bool taFlush(void*);
    static bool taVisibility(void*);
    static void taDelayUs(void*,uint32_t);
    static void taDelayMs(void*,uint32_t);
    hackynavi::pspring::Io taIo();
    static void taFault(void*);
    bool taOutputAliases(const void*,size_t,const void*,size_t,
        const HackyNaviDisplayController* =nullptr) const;
    IOReturn loadTrustedApplicationLocked(hackynavi::pspta::Kind,TrustedApplicationResult&);
    IOReturn unloadTrustedApplicationLocked(hackynavi::pspta::Kind,TrustedApplicationResult&);
    IOReturn taError(IOReturn,bool faulted=false);
    enum class DisplayTaAction { Add,Create,Query,Destroy,Remove };
    IOReturn displayTrustedOperation(HackyNaviFunctionGroup::Operation&,
        HackyNaviDisplayController&,uint8_t,DisplayTaAction,hackynavi::pspdisplayta::Protocol,
        hackynavi::pspdisplayta::Content,uint32_t,DisplayTrustedResult&);
    bool displayTopologyLocked(HackyNaviDisplayController&,uint8_t,hackynavi::pspdisplayta::Topology&) const;
    void snapshotDisplayTrusted(DisplayTrustedResult&);
    IOReturn finishDisplayTrusted(DisplayTrustedResult&,IOReturn);

    IOLock* lock_;
    HackyNaviGARTController* gart_;
    uint8_t* workspace_;
    bool lease_, hardwareHold_, rlcRetired_;
    State state_;
    hackynavi::VramAllocation slots_[SlotCount];
    hackynavi::FirmwareResource sosResource_;
    hackynavi::psp::SosImage sosImage_;
    hackynavi::psp::Layout sosLayout_;
    hackynavi::pspring::Layout ringLayout_;
    hackynavi::psp::State sos_;
    hackynavi::pspring::State ring_;
    hackynavi::psp::Result sosResult_;
    hackynavi::pspring::Result ringResult_;
    uint8_t pciRevision_;
    hackynavi::gfxfw::Files graphicsFiles_;
    hackynavi::gfxfw::Plan graphicsPlan_;
    hackynavi::gfxfw::State graphics_;
    hackynavi::gfxfw::Result graphicsResult_;
    hackynavi::gfxinit::AutoloadRetirement autoloadRetirement_;
    hackynavi::gfxinit::Result autoloadRetirementResult_;
    hackynavi::smu::Layout smuLayout_;
    hackynavi::smu::State smu_;
    hackynavi::smu::Result smuResult_;
    hackynavi::smuclock::Controller clocks_;
    HackyNaviFunctionGroup::Operation* clockScope_;
    HackyNaviFunctionGroup::RetirementScope* clockRetirementScope_;
    uint64_t clockGeneration_,clockStart_,clockLast_;
    uint32_t clockBudget_;
    IOReturn clockFailure_;
    bool clockRecovery_;
    HackyNaviFunctionGroup::Operation* taScope_;
    HackyNaviFunctionGroup::RetirementScope* taRetirementScope_;
    uint64_t taGeneration_,taStart_,taLast_;
    uint32_t taBudget_;
    IOReturn taFailure_;
    hackynavi::pspdisplayta::State displayTa_;
    struct DisplayTaRecord {
        HackyNaviDisplayController* owner;
        hackynavi::pspdisplayta::Topology topology;
        hackynavi::pspdisplayta::Display display;
        hackynavi::pspdisplayta::Authentication authentication;
        uint8_t path;
        bool topologyActive,authenticationActive;
    } displayTaRecords_[hackynavi::pspdisplayta::kDisplays];
    hackynavi::FirmwareResource displayResource_;
    hackynavi::dmcubfw::State displayFirmware_;
    hackynavi::dmcubfw::Result displayFirmwareResult_;
};
#endif
