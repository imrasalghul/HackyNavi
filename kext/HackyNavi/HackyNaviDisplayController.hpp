// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_KERNEL_DISPLAY_CONTROLLER_HPP
#define HACKYNAVI_KERNEL_DISPLAY_CONTROLLER_HPP
#include "HackyNaviFirmwareController.hpp"
#include <hackynavi/dmcub.hpp>
#include <hackynavi/dp_aux.hpp>
#include <hackynavi/hdmi_ddc.hpp>
#include <hackynavi/hdmi_link.hpp>
#include <hackynavi/dp_training.hpp>
#include <hackynavi/edid_acquire.hpp>
#include <hackynavi/hdmi_stream.hpp>
#include <hackynavi/display_smu.hpp>
#include <hackynavi/display_model_input.hpp>
#include <hackynavi/display_dentist.hpp>
#include <hackynavi/display_clock.hpp>
#include <hackynavi/display_idle.hpp>
#include <hackynavi/display_power.hpp>
class HackyNaviScanoutSurface;

// Kernel owner of real DMCUB runtime DMA and physical display transports.
// Caller holds a real FunctionGroup scope and installs GART::Access before
// any descendant owner mutex. Normal operations require normal access;
// stop/disable/reset cleanup accepts the actual retirement access too.
// Caller establishes
// DCN clocks, and withdraws inherited display clients before boot. No user
// client is exposed here. Firmware command consumption is not a modeset fence.
class HackyNaviDisplayController : public OSObject {
    OSDeclareDefaultStructors(HackyNaviDisplayController)
public:
    enum class State { Empty, Prepared, Ready, FaultedRetained, Retired, StoppedRetained };
    // Copies and revalidates the selected legacy image and its PCI subsystem
    // identity, builds connector routes, leases dedicated Display GART storage.
    // No hardware writes. Firmware must have completed PSP DMUB51 loading.
    static HackyNaviDisplayController* withFirmware(HackyNaviFirmwareController*,
        const uint8_t* rom, size_t bytes, const hackynavi::ImageInfo&, IOReturn&);
    IOReturn boot(const hackynavi::dmcubfw::PollOptions&);
    // Required if a pre-boot UCLK floor is retained: bind its actual epoch.
    IOReturn boot(HackyNaviFunctionGroup::Operation&, const hackynavi::dmcubfw::PollOptions&);
    struct ClockResult {
        hackynavi::displayidle::Result idle;
        hackynavi::displaysmu::Result smu;
        hackynavi::displayclock::Result pipe;
    };
    struct DpmCaptureResult {
        IOReturn result;
        uint64_t elapsedUs, serial;
        hackynavi::displayidle::Result idle;
        hackynavi::displaysmu::Result initialize, query;
        hackynavi::displaymodel::Status boardStatus;
        hackynavi::displaymodel::Result references;
    };
    struct DpmObservation {
        uint64_t deviceGeneration, serial;
        hackynavi::displaysmu::DpmInventory inventory;
        hackynavi::displaymodel::BoardInfo board;
        hackynavi::displaymodel::ReferenceInfo references;
    };
    // Cold, exclusive DAL observation under the caller's actual normal scope
    // and installed GART::Access. A single measured deadline covers preflight,
    // initialization, both inventory passes, and stable reference registers.
    // Board fields come from this owner's selected retained ROM. Does not
    // change clock minima. Reference/VCO values are derived from ROM and live
    // programming, not independently measured running clock counters.
    // Private origin/generation is retained here; copied observations are not
    // authority to program bandwidth, a DML model, or measured current clocks.
    IOReturn captureDpmInventory(HackyNaviFunctionGroup::Operation&, uint32_t timeoutUs,
        DpmCaptureResult&);
    IOReturn copyDpmObservation(HackyNaviFunctionGroup::Operation&, DpmObservation&);
    struct ProgrammedClocksResult {
        IOReturn result;
        uint64_t deviceGeneration, inventorySerial, elapsedUs;
        hackynavi::displayidle::Result idle;
        hackynavi::displaydentist::Result capture;
        hackynavi::displaydentist::Observation observation;
    };
    // Read-only, same retained board/reference/DPM generation. Real stable
    // DFS programming is distinct from a frequency counter or enduring hold.
    // Does not authorize a modeset or alter any clock request.
    IOReturn captureProgrammedClocks(HackyNaviFunctionGroup::Operation&,
        uint32_t requiredClockMask,uint32_t timeoutUs,ProgrammedClocksResult&);
    struct GlobalClockResult {
        IOReturn result;
        uint64_t elapsedUs,deviceGeneration,serial,inventorySerial;
        hackynavi::displayidle::Result idle;
        hackynavi::displaydentist::Result capture;
        hackynavi::displaysmu::ColdPresetResult transition;
        hackynavi::displaysmu::ColdPresetJournal journal;
        hackynavi::displaysmu::ColdPresetFrequency frequency;
    };
    // One cold acquisition of DISP or DPP from this owner's real DPM/reference
    // inventory. The same Operation, FAMS/DAL and owned inbox must remain valid
    // across each actual mailbox phase and stable PLL/DFS/DENTIST readback.
    // No software queue, VM, surface, link, stream or DTO consumer is admitted.
    // Private completed references alone can authorize later DTO/scanout use;
    // these result copies are diagnostic. No startup callsite yet.
    IOReturn requestGlobalClockPreset(HackyNaviFunctionGroup::Operation&,
        hackynavi::displaysmu::Clock,uint32_t requiredKHz,uint32_t timeoutUs,GlobalClockResult&);
    struct AckClockPolicyResult {
        IOReturn result;
        uint64_t elapsedUs,deviceGeneration,serial;
        hackynavi::displaysmu::AckClockJournal journal;
        hackynavi::displaysmu::Result initialize,inventory,request;
        hackynavi::displayidle::Result idle;
    };
    // Exact, one-shot DAL hard-maximum / DCF deep-sleep-floor requests.
    // Actual DPM/DC bounds, cold pipes and same-owner DMCUB/FAMS coordination
    // are checked under a single elapsed deadline. ACK is not measurement.
    // Policy and resources remain owned until verified reset, including failure.
    IOReturn requestMemoryMaximum(HackyNaviFunctionGroup::Operation&,
        uint16_t MHz,uint32_t timeoutUs,AckClockPolicyResult&);
    IOReturn requestDcfDeepSleepFloor(HackyNaviFunctionGroup::Operation&,
        uint16_t MHz,uint32_t timeoutUs,AckClockPolicyResult&);
    struct MemoryMinimumResult {
        IOReturn result;
        uint64_t elapsedUs,deviceGeneration,serial;
        uint32_t dmcubControl,dmcubControl2;
        uint64_t famsSerial;
        bool runningFirmwareCoordinated;
        hackynavi::dmcub::Result firmwareCoordination;
        hackynavi::displayidle::Result idle;
        hackynavi::displaysmu::Result initialize,inventory,minimum;
        hackynavi::displaysmu::MemoryJournal journal;
    };
    // Cold UCLK floor. Prepared requires actual DMCUB disabled/in-reset
    // readbacks. Ready instead requires this owner's completed FAMS-disable
    // and DAL coordination journal in the same generation, plus fresh owned
    // inbox/firmware checks before and after the minimum request. Both require
    // actual Operation + normal GART::Access, fresh physical pipe idle and no
    // local DTO/link users. No inherited DTO/PHY readback is claimed.
    // Queries the real UCLK table/DC ceiling under the same elapsed deadline
    // and rejects a floor absent from that table (or fine-grained interval).
    // Owner retains the possible constraint through boot, failures and stop.
    // Only existing verified full-device reset retirement can discard it.
    // A fulfilled floor is not fixed UCLK, current metrics, a SOC/FCLK hold,
    // or DML/modeset authorization.
    IOReturn requestMemoryMinimum(HackyNaviFunctionGroup::Operation&, uint16_t MHz,
        uint32_t timeoutUs, MemoryMinimumResult&);
    struct FamsJournal {
        uint64_t deviceGeneration, serial;
        bool started, dalAcknowledged, commandWritten, commandSubmitted,
             commandConsumed, complete;
    };
    struct FamsResult {
        IOReturn result;
        uint64_t elapsedUs;
        hackynavi::displayidle::Result idle;
        hackynavi::displaysmu::Result initialize, dal;
        hackynavi::dmcub::Result preflight, inbox;
        FamsJournal journal;
    };
    // Real cold FAMS handoff for the future fixed-clock modeset transaction.
    // Requires this owner's running, version-verified DMCUB, normal Operation
    // and GART access, all physical pipes idle and all local consumers absent.
    // Orders DAL0x14/0 ACK before type76/subtype0 consumption, matching the
    // non-FAMS dcn32 bandwidth-preparation ordering. One elapsed deadline
    // spans every callback. A completed journal proves those commands only;
    // fixed clocks, DML/bandwidth and display activation remain separate.
    // No automatic startup callsite. Possible policy ownership survives stop
    // and ambiguous failures until independently verified full-device reset.
    IOReturn disableFamsForColdClockChanges(HackyNaviFunctionGroup::Operation&,
        uint32_t timeoutUs, FamsResult&);
    // Coordinated cold Firmware/PPSMC policy entry points. The same retained
    // Ready Display must own completed FAMS/DAL journals in this generation.
    // Queue/VM owners and every local display consumer remain excluded;
    // physical pipe idle and actual inbox ownership are freshly checked on
    // both sides. One deadline spans Display -> Firmware -> GART and all I/O.
    // These are policy requests, not fixed/measured clock or modeset proof.
    IOReturn requestSocFabricRange(HackyNaviFunctionGroup::Operation&,
        hackynavi::smuclock::Clock,uint16_t minMHz,uint16_t maxMHz,uint32_t timeoutUs,
        HackyNaviFirmwareController::ClockResult&);
    IOReturn inhibitSocFabricDeepSleep(HackyNaviFunctionGroup::Operation&,uint32_t timeoutUs,
        HackyNaviFirmwareController::ClockResult&);
    IOReturn disallowDfCstate(HackyNaviFunctionGroup::Operation&,uint32_t timeoutUs,
        HackyNaviFirmwareController::ClockResult&);
    // Genuine retirement scope only. While this exact Display remains Ready
    // and retained, seal metrics, return ranges to automatic, ALLOW DF, then
    // restore only captured DS bits. Does not release Display/FAMS/DAL/UCLK
    // ownership, stop DMCUB, or manufacture a reset proof. Ambiguity retains
    // both native owners and their journals; no normal work is reopened.
    IOReturn retireFirmwareClockState(HackyNaviFunctionGroup::RetirementScope&,
        uint32_t timeoutUs,HackyNaviFirmwareController::ClockResult&);
    // Cold modeset clock acquisition only. All local links/streams/DTOs must
    // be closed and every physical pipe must pass fresh hardware idle checks.
    // Caller owns global display/PM exclusion and supplies a bandwidth/preset
    // derived MHz request. Result is the PMFW-reported accepted frequency.
    IOReturn requestClockMinimum(hackynavi::displaysmu::Clock, uint16_t MHz,
        const hackynavi::displaysmu::Poll&, hackynavi::displaysmu::Frequency&, ClockResult&);
    // Requires the private completed global DPP transaction in this generation
    // and fresh matching hardware programming before using its DTO reference.
    IOReturn configureDppClock(HackyNaviFunctionGroup::Operation&,uint8_t pipe,
        uint32_t requiredKHz,uint8_t k1Divisor,uint8_t k2Divisor,uint32_t timeoutUs,ClockResult&);
    IOReturn stopDppClock(uint8_t pipe, ClockResult&);
    // Owns DOMAIN0..3 through real request/power-state handshakes. Waking a
    // domain does not enable HUBP clocks or establish its requestor/timing.
    // Caller excludes autonomous firmware/PM users of IP_REQUEST_EN too.
    IOReturn powerOnPipe(uint8_t pipe, const hackynavi::displaypower::Poll&,
        hackynavi::displaypower::Result&);
    // Requires stopped/disconnected pipeline, clocks/DTO off and fetch clients
    // retired. A failed transition retains this owner's display-storage lease.
    IOReturn powerOffPipe(uint8_t pipe, const hackynavi::displaypower::Poll&,
        hackynavi::displaypower::Result&);
    bool route(uint8_t path, hackynavi::dmcub::Route&);
    uint8_t pathCount() const { return paths_; }
    IOReturn pixelClock(uint8_t path, const hackynavi::dmcub::PixelClock&,
        const hackynavi::dmcub::PollOptions&);
    IOReturn setupEncoder(uint8_t path, const hackynavi::dmcub::Encoder&,
        const hackynavi::dmcub::PollOptions&);
    // Source PLL and DCN clocks must already be programmed by the modeset
    // owner. Opens actual ROM-selected AUX, prepares DIO and trains against
    // real receiver lane status. Success leaves a trained idle DP link;
    // pixel stream/OTG/scanout activation follows in the modeset owner.
    IOReturn trainDp(uint8_t path, uint8_t frontend, const hackynavi::dptrain::Config&,
        const hackynavi::dmcub::PollOptions&, hackynavi::dptrain::HardwareResult&);
    // The modeset owner first stops the pixel stream. Powers down the DP sink
    // where reachable, disables the source and confirms DIG_ENABLE is clear.
    IOReturn disableDp(uint8_t path, const hackynavi::dmcub::PollOptions&);
    // The modeset owner programs PLL/depth, DCN clocks and stream timing
    // first, with this frontend's FIFO and OTG/pixel stream stopped. Actual
    // ROM-routed DDC/SCDC, encoder setup and PHY enable/readback follow here.
    // Config contains validated EDID/board limits; clock is the board's ATOM
    // I2C reference. Success leaves AVMUTE asserted and FIFO off, not scanout.
    IOReturn enableHdmi(uint8_t path, uint8_t frontend,
        const hackynavi::hdmiddc::Clock&, const hackynavi::hdmitmds::Config&,
        const hackynavi::dmcub::PollOptions&, hackynavi::hdmitmds::HardwareResult&);
    // Stop stream/OTG/FIFO first. Uses the saved route/DDC clock and SCDC
    // capability from enableHdmi; an unreachable sink is reported in out,
    // independently of confirmed local PHY retirement. No new EDID guesses.
    IOReturn disableHdmi(uint8_t path, const hackynavi::dmcub::PollOptions&,
        hackynavi::hdmitmds::HardwareResult&);
    // Requires this owner's enabled HDMI link and an exclusively programmed
    // pixel/scanout pipeline. Uses the actual saved frontend/ROM PHY route;
    // callers cannot substitute a different hardware encoder. Sink comes from
    // validated EDID; AVI encoding must match enableHdmi's pixel encoding.
    IOReturn prepareHdmiStream(uint8_t path, uint8_t pipe, const hackynavi::avi::Config&,
        const hackynavi::edid::Monitor&, const hackynavi::dmcub::PollOptions&,
        hackynavi::hdmistream::Result&);
    IOReturn startHdmiStream(uint8_t path, const hackynavi::dmcub::PollOptions&,
        hackynavi::hdmistream::Result&);
    IOReturn stopHdmiStream(uint8_t path, const hackynavi::dmcub::PollOptions&,
        hackynavi::hdmistream::Result&);
    // One serialized batch on the ROM-selected AUX engine. The final request
    // must end any MOT chain; real STOP/release follows even a protocol NACK.
    // Results retain the actual wire reply; ambiguous I/O retains ownership.
    IOReturn transferAux(uint8_t path, const hackynavi::dpaux::Request*, size_t count,
        hackynavi::dpaux::Result* results);
    // Clock.crystalKHz must come from this board's ATOM clock table. Complete
    // repeated-START messages and final STOP use the actual shared I2C engine.
    IOReturn transferHdmi(uint8_t path, const hackynavi::hdmiddc::Clock&,
        const hackynavi::hdmiddc::Message*, size_t count, hackynavi::hdmiddc::Result&);
    IOReturn readHdmiEdid(uint8_t path, const hackynavi::hdmiddc::Clock&, uint8_t block,
        hackynavi::hdmiddc::Result&);
    // Reads and rechecks every effective EDID block through this ROM path's
    // physical DDC or I2C-over-AUX engine. The HDMI clock is ignored for DP.
    // raw must initially be null; on success the caller owns the returned
    // OSData reference. raw/monitor stay unchanged on any error, including
    // failed pin release. No partial capture is published as complete EDID.
    IOReturn readEdid(uint8_t path, const hackynavi::hdmiddc::Clock&,
        OSData*& raw, hackynavi::edid::Monitor&, hackynavi::edidacq::HardwareResult&);
    struct StopResult {
        IOReturn result, clockRetirementResult;
        uint64_t elapsedUs;
        bool consumersStopped, physicalIdleChecked, firmwareStopped,
             runtimeRetained, policyRetained, leaseReleased,
             coordinatedClockRetirementAttempted;
        hackynavi::displayidle::Result idle;
        hackynavi::dmcubfw::Result firmware;
        hackynavi::displaysmu::Result minima;
    };
    // Genuine retirement scope and installed retirement GART access. The
    // native stream/surface owner must already have retired its own objects.
    // Closes owned transports/links/DTOs/power domains and freshly verifies
    // physical idle where owned clocks/domains/policy require it, while leaving DMCUB Ready, its inbox and every policy
    // journal retained. No cached consumer-stopped bit is used as authority.
    // Prepared without a hardware hold has no I/O to retire and stays Prepared.
    IOReturn stopConsumers(HackyNaviFunctionGroup::RetirementScope&,uint32_t timeoutUs,
        const hackynavi::dmcubfw::PollOptions&,StopResult&);
    // After real graphics/VM/RLC retirement. Rechecks the consumers and uses
    // this owner's actual FAMS journal to retire coordinated Firmware clocks
    // before stopping DMCUB. Otherwise Engine retires Firmware clocks after
    // this method releases Display. STOP_FW/PWAIT/reset retires runtime DMA
    // only: reset-only FAMS/DAL policy yields Busy + StoppedRetained, retaining
    // the lease and journals. FaultedRetained is never rehabilitated here.
    IOReturn stopFirmware(HackyNaviFunctionGroup::RetirementScope&,uint32_t timeoutUs,
        const hackynavi::dmcubfw::PollOptions&,StopResult&);
    // The modeset owner first stops every driver-created pixel stream. This
    // closes owned AUX/DDC and disables owned DIO links before retiring DMCUB.
    // Inherited boot scanout must be blanked/drained by its separate owner;
    // downstream inherited OTG/PHY state is not a DMCUB DMA reference.
    // Stops local transports, requests STOP_FW, observes PWAIT, asserts reset
    // and disables runtime windows before releasing the Display lease.
    IOReturn stop(const hackynavi::dmcubfw::PollOptions&);
    // The genuine reset proof retires memory clients, not downstream display
    // programming. Keep the same actual retirement Access/scope through proof
    // consumption. Retained links/streams/DTOs/power/pin owners reject without
    // changing journals or dropping the hardware hold; they need an additional
    // real downstream retirement protocol. DMCUB-only owners can be released.
    IOReturn releaseAfterVerifiedReset(HackyNaviFunctionGroup::RetirementScope&,
        const HackyNaviResetController::RetirementProof&);
    State state() const { return state_; }
    bool requiresHardwareRetirement() const { return hardwareHold_; }
    const hackynavi::dmcub::Result& lastInboxResult() const { return inboxResult_; }
protected:
    bool init() override;
    void free() override;
private:
    friend class HackyNaviScanoutSurface;
    friend class HackyNaviFirmwareController;
    IOReturn initialize(HackyNaviFirmwareController*, const uint8_t*, size_t, const hackynavi::ImageInfo&);
    IOReturn bootInternal(HackyNaviFunctionGroup::Operation*, const hackynavi::dmcubfw::PollOptions&);
    void lockOwners();
    IOReturn finish(IOReturn);
    IOReturn fault();
    bool providerReady(bool retiring = false) const;
    bool validPath(uint8_t, uint8_t connector = 0) const;
    IOReturn beginPath(uint8_t, uint8_t connector = 0, bool retiring = false);
    IOReturn inboxStatus(hackynavi::dmcub::Status);
    IOReturn hdmiOperation(uint8_t, const hackynavi::hdmiddc::Clock&,
        const hackynavi::hdmiddc::Message*, size_t, bool edid, uint8_t block,
        hackynavi::hdmiddc::Result&);
    IOReturn releaseLeaseLocked();
    IOReturn stopConsumersLocked(const hackynavi::dmcubfw::PollOptions&,StopResult&,bool physical);
    bool physicalStopRequiredLocked() const;
    IOReturn verifyStoppedConsumersLocked(StopResult&,bool physical);
    IOReturn stopFirmwareLocked(const hackynavi::dmcubfw::PollOptions&,StopResult&);
    void snapshotStopLocked(StopResult&);
    IOReturn finishScopedStop(StopResult&,DpmCaptureResult&,IOReturn);
    IOReturn disableDpLocked(uint8_t, const hackynavi::dmcub::PollOptions&);
    IOReturn disableHdmiLocked(uint8_t, const hackynavi::dmcub::PollOptions&,
        hackynavi::hdmitmds::HardwareResult&);
    void clearLinkMetadata(uint8_t);
    IOReturn stopHdmiStreamLocked(uint8_t, const hackynavi::dmcub::PollOptions&,
        hackynavi::hdmistream::Result&);
    IOReturn checkClockIdleLocked(ClockResult&);
    void invalidateGlobalClockReferences();
    IOReturn globalClockColdLocked(HackyNaviFunctionGroup::Operation&,hackynavi::displayidle::Result&);
    static bool observeGlobalClock(void*,hackynavi::displaysmu::Clock,
        hackynavi::displaysmu::ColdPresetStage,const hackynavi::displaysmu::ColdPresetTarget&,
        hackynavi::displaysmu::ColdPresetObservation&);
    // Caller already holds all owner locks. Installs/clears its own bounded
    // read guard without unlocking them or mutating clock hardware.
    IOReturn verifyGlobalClockReferenceLocked(HackyNaviFunctionGroup::Operation&,
        hackynavi::displaysmu::Clock,uint64_t startedUs,uint32_t timeoutUs,ProgrammedClocksResult&);
    IOReturn requestAckClockPolicy(HackyNaviFunctionGroup::Operation&,
        bool memoryMaximum,uint16_t MHz,uint32_t timeoutUs,AckClockPolicyResult&);
    bool memoryFirmwareStoppedLocked(MemoryMinimumResult&);
    IOReturn memoryClockReadyLocked(HackyNaviFunctionGroup::Operation&, MemoryMinimumResult&);
    IOReturn firmwareClockQuiescentLocked(HackyNaviFirmwareController::ClockResult&);
    bool dpmCaptureReady();
    static bool referenceValid(void*);
    static uint64_t referenceTimeUs(void*);
    IOReturn finishDpmCapture(IOReturn);
    void invalidateDpmInventory();
    IOReturn stopDppClockLocked(uint8_t, ClockResult&);
    IOReturn powerOffPipeLocked(uint8_t, const hackynavi::displaypower::Poll&,
        hackynavi::displaypower::Result&);
    static bool read32(void*, uint32_t, uint32_t&);
    static bool write32(void*, uint32_t, uint32_t);
    static bool readMemory(void*, uint64_t, uint8_t*, size_t);
    static bool writeMemory(void*, uint64_t, const uint8_t*, size_t);
    static bool publish(void*);
    static void delayUs(void*, uint32_t);
    bool memoryRange(uint64_t, size_t) const;
    hackynavi::dmcubfw::Io firmwareIo();
    hackynavi::dmcub::Io inboxIo();
    IOLock* lock_;
    HackyNaviFirmwareController* firmware_;
    HackyNaviGARTController* gart_;
    // Borrowed, exclusively leased by a native surface owner retaining us.
    // Parent retirement cannot bypass its allocation/pixel/HUBP lifetime.
    HackyNaviScanoutSurface* scanoutOwner_;
    OSData* rom_;
    bool lease_, hardwareHold_;
    State state_;
    uint8_t paths_;
    hackynavi::ImageInfo image_;
    hackynavi::dmcub::Route routes_[hackynavi::kMaxDisplayPaths];
    bool routeValid_[hackynavi::kMaxDisplayPaths];
    hackynavi::dmcubfw::Layout firmwareLayout_;
    hackynavi::dmcubfw::Memory memory_;
    hackynavi::dmcub::Layout inboxLayout_;
    hackynavi::dmcub::Inbox inbox_;
    hackynavi::dmcub::Result inboxResult_;
    hackynavi::dpaux::Layout auxLayout_;
    hackynavi::dpaux::Engine aux_;
    hackynavi::hdmiddc::Layout ddcLayout_;
    hackynavi::hdmiddc::Engine ddc_;
    hackynavi::dio::Layout dioLayout_;
    hackynavi::dio::Link links_[hackynavi::kMaxDisplayPaths];
    hackynavi::dio::Result linkResult_;
    enum class LinkKind : uint8_t { None, Dp, Hdmi };
    LinkKind linkKinds_[hackynavi::kMaxDisplayPaths];
    hackynavi::hdmiddc::Clock hdmiClocks_[hackynavi::kMaxDisplayPaths];
    bool hdmiScdc_[hackynavi::kMaxDisplayPaths];
    hackynavi::hdmitmds::Encoding hdmiEncoding_[hackynavi::kMaxDisplayPaths];
    uint8_t streamPipe_[hackynavi::kMaxDisplayPaths];
    hackynavi::hdmistream::Layout streamLayout_;
    hackynavi::hdmistream::Controller streams_[hackynavi::kMaxDisplayPaths];
    hackynavi::displayidle::Layout idleLayout_;
    hackynavi::displaypower::Layout powerLayout_;
    hackynavi::displaypower::Controller pipePowers_[hackynavi::displaypower::kPipeCount];
    hackynavi::displayclock::Layout pipeClockLayout_;
    hackynavi::displayclock::Controller pipeClocks_[hackynavi::displayclock::kPipeCount];
    hackynavi::displaysmu::Layout clockSmuLayout_;
    hackynavi::displaysmu::Controller clockSmu_;
    uint32_t dppReferenceKHz_,dispReferenceKHz_;
    struct GlobalClockReference {
        bool valid;
        uint64_t generation,serial,inventorySerial,famsSerial;
        hackynavi::displaymodel::BoardInfo board;
        hackynavi::displaymodel::ReferenceInfo references;
        hackynavi::displaydentist::Layout layout;
        hackynavi::displaysmu::ColdPresetFrequency frequency;
    } globalClockReferences_[2];
    uint64_t globalClockSerial_;
    // Borrowed only inside the actual locked request; never caller authority.
    GlobalClockReference* globalClockAttempt_;
    GlobalClockResult* globalClockReport_;
    uint32_t globalClockCeilingKHz_;
    uint64_t memoryGeneration_,memorySerial_;
    FamsJournal fams_;
    hackynavi::displaysmu::DpmInventory dpmInventory_;
    hackynavi::displaymodel::BoardInfo dpmBoard_;
    hackynavi::displaymodel::ReferenceInfo dpmReferences_;
    bool dpmInventoryValid_;
    uint64_t dpmGeneration_,dpmSerial_,dpmStart_,dpmLast_;
    uint32_t dpmBudget_;
    // Borrowed only while capture owns Display -> Firmware -> GART locks.
    HackyNaviFunctionGroup::Operation* dpmScope_;
    HackyNaviFunctionGroup::RetirementScope* dpmRetirementScope_;
    DpmCaptureResult* dpmReport_;
};
#endif
