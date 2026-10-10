// SPDX-License-Identifier: MIT
#include "HackyNaviFirmwareController.hpp"
#include "HackyNaviDisplayController.hpp"
#include "HackyNaviPciAccess.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>

#if !defined(__x86_64__)
#error HackyNaviFirmwareController requires x86_64 ordered uncached MMIO
#endif

OSDefineMetaClassAndStructors(HackyNaviFirmwareController, OSObject)
using namespace hackynavi;
namespace {
constexpr uint64_t kPayloadBytes = 2U * 1024U * 1024U;
constexpr uint64_t kTmrAlignment = 1024U * 1024U;
bool validPoll(const pspring::PollOptions& p) {
    return p.maxPolls && p.maxPolls <= 1000000 && p.delayUs && p.delayUs <= 1000 &&
        uint64_t(p.maxPolls) * p.delayUs <= 10000000;
}
}

bool HackyNaviFirmwareController::init() {
    lock_ = nullptr; gart_ = nullptr; workspace_ = nullptr;
    lease_ = hardwareHold_ = rlcRetired_ = false; state_ = State::Empty;
    for (auto& slot : slots_) slot = VramAllocation{};
    sosResource_ = FirmwareResource{}; sosImage_ = psp::SosImage{};
    sosLayout_ = psp::Layout{}; ringLayout_ = pspring::Layout{};
    sosResult_ = psp::Result{}; ringResult_ = pspring::Result{};
    pciRevision_ = 0; graphicsFiles_ = gfxfw::Files{};
    graphicsPlan_ = gfxfw::Plan{}; graphicsResult_ = gfxfw::Result{};
    autoloadRetirementResult_ = gfxinit::Result{};
    smuLayout_ = smu::Layout{}; smuResult_ = smu::Result{};
    clockScope_ = nullptr; clockRetirementScope_ = nullptr;
    clockGeneration_ = clockStart_ = clockLast_ = 0;
    clockBudget_ = 0; clockFailure_ = kIOReturnSuccess; clockRecovery_ = false;
    taScope_ = nullptr; taRetirementScope_ = nullptr; taGeneration_ = taStart_ = taLast_ = 0;
    taBudget_ = 0; taFailure_ = kIOReturnSuccess;
    for (auto& record : displayTaRecords_) record = DisplayTaRecord{};
    displayResource_ = FirmwareResource{}; displayFirmwareResult_ = dmcubfw::Result{};
    if (!OSObject::init()) return false;
    lock_ = IOLockAlloc();
    return lock_ != nullptr;
}
HackyNaviFirmwareController* HackyNaviFirmwareController::withController(
    HackyNaviGARTController* gart, IOReturn& result) {
    result = kIOReturnNoMemory;
    auto* self = new HackyNaviFirmwareController;
    if (!self) return nullptr;
    if (!self->init()) { self->release(); return nullptr; }
    result = self->initialize(gart);
    if (result != kIOReturnSuccess) { self->release(); return nullptr; }
    return self;
}
IOReturn HackyNaviFirmwareController::initialize(HackyNaviGARTController* gart) {
    if (!gart) return kIOReturnBadArgument;
    if (!gart->normalAccessReady()) return kIOReturnOffline;
    if (!getFirmwareResource(FirmwareResourceId::PspSos, sosResource_) ||
        psp::parseSosImage(sosResource_.data, sosResource_.bytes, sosImage_) != psp::Status::Ok ||
        !sosImage_.toc.size || (sosImage_.toc.size & 3) || sosImage_.toc.size > kPayloadBytes)
        return kIOReturnUnsupported;
    if ((sosImage_.registerList.size & 3) || sosImage_.registerList.size > kPayloadBytes)
        return kIOReturnUnsupported;
    dmcubfw::Image displayImage{};
    if (!getFirmwareResource(FirmwareResourceId::Dmcub, displayResource_) ||
        dmcubfw::parse(displayResource_.data, displayResource_.bytes, displayImage) != dmcubfw::Status::Ok ||
        displayImage.pspImage.size > kPayloadBytes) return kIOReturnUnsupported;
    workspace_ = static_cast<uint8_t*>(IOMalloc(psp::kStagingBytes));
    if (!workspace_) return kIOReturnNoMemory;
    gart_ = gart; gart_->retain();
    IOLockLock(lock_); IOLockLock(gart_->lock_);
    if (gart_->firmwareOwner_ || gart_->queueOwner_ || gart_->displayOwner_) return finish(kIOReturnBusy);
    if (gart_->state_ != HackyNaviGARTController::State::MmEnabled ||
        !gart_->hdpState_.configured()) return finish(kIOReturnNotReady);
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    pciRevision_ = gart_->device_->configRead8(0x08);
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    const FirmwareResourceId ids[] = {FirmwareResourceId::Smu, FirmwareResourceId::Sdma,
        FirmwareResourceId::GcPfp, FirmwareResourceId::GcMe, FirmwareResourceId::GcMec,
        FirmwareResourceId::GcImu, FirmwareResourceId::GcRlc};
    gfxfw::View* views[] = {&graphicsFiles_.smu, &graphicsFiles_.sdma, &graphicsFiles_.pfp,
        &graphicsFiles_.me, &graphicsFiles_.mec, &graphicsFiles_.imu, &graphicsFiles_.rlc};
    for (unsigned i = 0; i < 7; ++i) {
        FirmwareResource resource{};
        if (!getFirmwareResource(ids[i], resource)) return finish(kIOReturnNotFound);
        *views[i] = {resource.data, resource.bytes};
    }
    if (gfxfw::plan(graphicsFiles_, gart_->ipLayout_, pciRevision_, graphicsPlan_, graphicsResult_) !=
        gfxfw::Status::Ok || graphicsPlan_.stagingBytes > kPayloadBytes)
        return finish(kIOReturnUnsupported);
    if (psp::buildLayout(gart_->ipLayout_, gart_->registerBytes_, sosLayout_) != psp::Status::Ok ||
        pspring::buildLayout(gart_->ipLayout_, gart_->registerBytes_, ringLayout_) != pspring::Status::Ok ||
        smu::buildLayout(gart_->ipLayout_, gart_->registerBytes_, smuLayout_) != smu::Status::Ok)
        return finish(kIOReturnUnsupported);
    gart_->firmwareOwner_ = this; gart_->firmwareReady_ = false; lease_ = true;
    const uint64_t bytes[Payload + 1] = {psp::kStagingBytes, 4096, 4096, 4096, kPayloadBytes};
    for (unsigned i = SosStage; i <= Payload; ++i) {
        const uint64_t alignment = i == SosStage ? psp::kStagingBytes : 4096;
        if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
        if (gart_->allocator_.allocate(bytes[i], alignment, slots_[i]) != VramAllocatorStatus::Ok)
            return finish(kIOReturnNoSpace);
    }
    state_ = State::Prepared;
    return finish(gart_->normalAccessReady()?kIOReturnSuccess:kIOReturnOffline);
}
bool HackyNaviFirmwareController::providerReady() const {
    if (!lease_ || !gart_ || !gart_->accessReady() || gart_->firmwareOwner_ != this || !gart_->device_ ||
        !gart_->openOwner_ || !gart_->registers_ || !gart_->hdpState_.configured() ||
        (gart_->state_ != HackyNaviGARTController::State::MmEnabled &&
         gart_->state_ != HackyNaviGARTController::State::BothEnabled)) return false;
    return nativepci::ready(gart_->device_, gart_->openOwner_, nativepci::BusMaster::Enabled) &&
        gart_->device_->configRead8(0x08) == pciRevision_ && gart_->accessReady();
}
IOReturn HackyNaviFirmwareController::begin(State expected) {
    IOLockLock(lock_);
    if (!gart_) { IOLockUnlock(lock_); return kIOReturnNotReady; }
    IOLockLock(gart_->lock_);
    if (state_ != expected) return finish(kIOReturnNotReady);
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    if (!providerReady()) return finish(kIOReturnOffline);
    gart_->mmioActive_ = true;
    return kIOReturnSuccess; // Both locks remain held.
}
IOReturn HackyNaviFirmwareController::finish(IOReturn result) {
    gart_->mmioActive_ = false;
    IOLockUnlock(gart_->lock_); IOLockUnlock(lock_);
    return result;
}
IOReturn HackyNaviFirmwareController::fault() {
    IOLog("HackyNavi: firmware failure state=%u SOS=%s PSP=%s GFX=%s SMU=%s DMUB=%s; resources retained\n",
        static_cast<unsigned>(state_), psp::statusString(sosResult_.status),
        pspring::statusString(ringResult_.status), gfxfw::statusString(graphicsResult_.status),
        smu::statusString(smuResult_.status), dmcubfw::statusString(displayFirmwareResult_.status));
    gart_->firmwareReady_ = false; state_ = State::FaultedRetained;
    return finish(kIOReturnIOError);
}
void HackyNaviFirmwareController::holdHardware() {
    if (!hardwareHold_) { retain(); hardwareHold_ = true; }
}
bool HackyNaviFirmwareController::read32(void* context, uint32_t offset, uint32_t& value) {
    auto* self = static_cast<HackyNaviFirmwareController*>(context);
    return HackyNaviGARTController::read32(self->gart_, offset, value);
}
bool HackyNaviFirmwareController::write32(void* context, uint32_t offset, uint32_t value) {
    auto* self = static_cast<HackyNaviFirmwareController*>(context);
    return HackyNaviGARTController::write32(self->gart_, offset, value);
}
bool HackyNaviFirmwareController::flushHdp(void* context) {
    auto* self = static_cast<HackyNaviFirmwareController*>(context);
    return HackyNaviGARTController::hdpFlush(self->gart_);
}
bool HackyNaviFirmwareController::beforeReadback(void* context) {
    auto* self = static_cast<HackyNaviFirmwareController*>(context);
    if (!self->gart_->ioReady(4)) return false;
    // PSP writes are read back through uncached MM_DATA, not a CPU-cached BAR
    // mapping. Serialize the CPU before that ordered native read sequence.
    __asm__ __volatile__("mfence" ::: "memory");
    OSSynchronizeIO();
    return self->gart_->accessReady();
}
void HackyNaviFirmwareController::delayUs(void*, uint32_t us) { IODelay(us); }
void HackyNaviFirmwareController::delayMs(void*, uint32_t ms) { IOSleep(ms); }
psp::Io HackyNaviFirmwareController::sosIo() {
    return {this, read32, write32, flushHdp, delayUs, delayMs};
}
pspring::Io HackyNaviFirmwareController::ringIo() {
    return {this, read32, write32, flushHdp, beforeReadback, delayUs, delayMs};
}
IOReturn HackyNaviFirmwareController::loadSos(const psp::Options& options) {
    IOReturn result = begin(State::Prepared);
    if (result != kIOReturnSuccess) return result;
    holdHardware();
    IOLog("HackyNavi: loading embedded PSP13 SOS through bootloader\n");
    const auto status = psp::loadSos(sosLayout_, gart_->allocator_, slots_[SosStage].handle,
        sosResource_.data, sosResource_.bytes, workspace_, psp::kStagingBytes,
        sosIo(), options, sos_, sosResult_);
    if (status != psp::Status::Ok) {
        if (!sosResult_.mailboxWritesMayHaveOccurred) {
            // Upload/selector writes alone cannot publish a PSP reference.
            state_ = State::FailedUnpublished; hardwareHold_ = false;
            finish(status == psp::Status::AlreadyRunning ? kIOReturnBusy : kIOReturnIOError);
            release();
            return status == psp::Status::AlreadyRunning ? kIOReturnBusy : kIOReturnIOError;
        }
        return fault();
    }
    state_ = State::SosLoaded;
    IOLog("HackyNavi: PSP SOS heartbeat changed; reported version=0x%x\n", sosResult_.sosVersion);
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviFirmwareController::createRing(const pspring::PollOptions& poll) {
    if (!validPoll(poll)) return kIOReturnBadArgument;
    const IOReturn result = begin(State::SosLoaded);
    if (result != kIOReturnSuccess) return result;
    const pspring::Buffers buffers = {slots_[Ring].handle, slots_[Command].handle,
        slots_[Fence].handle, workspace_, psp::kStagingBytes};
    IOLog("HackyNavi: creating PSP KM ring\n");
    if (pspring::create(ringLayout_, sos_, gart_->allocator_, buffers, ringIo(), poll,
                        ring_, ringResult_) != pspring::Status::Ok) return fault();
    state_ = State::RingReady;
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviFirmwareController::queryBootConfiguration(
    const pspring::PollOptions& poll, uint32_t& config) {
    config = 0;
    if (!validPoll(poll)) return kIOReturnBadArgument;
    const IOReturn result = begin(State::RingReady);
    if (result != kIOReturnSuccess) return result;
    if (pspring::submitBootConfig(ringIo(), ring_, ringResult_) != pspring::Status::Ok ||
        pspring::wait(ringIo(), ring_, poll, ringResult_) != pspring::Status::Ok) return fault();
    config = ringResult_.bootConfig;
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviFirmwareController::initializeTmr(const pspring::PollOptions& poll) {
    if (!validPoll(poll)) return kIOReturnBadArgument;
    const IOReturn result = begin(State::RingReady);
    if (result != kIOReturnSuccess) return result;
    if (!ring_.requiredTmrBytes()) {
        if (uploadVram(gart_->allocator_, slots_[Payload].handle, 0,
                       sosResource_.data + sosImage_.toc.offset, sosImage_.toc.size,
                       gart_->transferIo(), gart_->transferResult_) != VramTransferStatus::Ok ||
            !flushHdp(this) ||
            pspring::submitLoadToc(ringIo(), ring_, slots_[Payload].handle, 0,
                sosImage_.toc.size, ringResult_) != pspring::Status::Ok ||
            pspring::wait(ringIo(), ring_, poll, ringResult_) != pspring::Status::Ok) return fault();
    } // An earlier allocation failure can retry without replaying LOAD_TOC.
    const uint64_t requested = ring_.requiredTmrBytes();
    IOLog("HackyNavi: PSP TOC requested TMR bytes=%u\n", static_cast<unsigned>(requested));
    if (!requested || requested > (64U << 20) || (requested & 4095)) return fault();
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    if (gart_->allocator_.allocate(requested, kTmrAlignment, slots_[Tmr]) != VramAllocatorStatus::Ok)
        return finish(kIOReturnNoSpace); // Ring is idle; no TMR address was published.
    if (pspring::submitSetupTmr(ringIo(), ring_, slots_[Tmr].handle, ringResult_) != pspring::Status::Ok ||
        pspring::wait(ringIo(), ring_, poll, ringResult_) != pspring::Status::Ok || !ring_.tmrReady())
        return fault();
    state_ = State::TmrReady;
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviFirmwareController::releaseAllocationsLocked() {
    // Neither PSP teardown nor the older MODE1 call contract establishes
    // retirement of a separately published SMU table or clock constraint.
    if (clocks_.retentionRequired()) return kIOReturnBusy;
    for (unsigned index = SlotCount; index > 0; --index) {
        auto& slot = slots_[index - 1];
        if (slot.handle.owner) {
            if (gart_->allocator_.release(slot.handle) != VramAllocatorStatus::Ok)
                return kIOReturnInternalError;
            slot = VramAllocation{};
        }
    }
    if (lease_) {
        if (gart_->firmwareOwner_ != this) return kIOReturnInternalError;
        gart_->firmwareOwner_ = nullptr; gart_->firmwareReady_ = false; lease_ = false;
    }
    return kIOReturnSuccess;
}
IOReturn HackyNaviFirmwareController::loadGraphicsFirmware(const pspring::PollOptions& poll) {
    if (!validPoll(poll)) return kIOReturnBadArgument;
    const IOReturn result = begin(State::TmrReady);
    if (result != kIOReturnSuccess) return result;
    IOLog("HackyNavi: loading %u graphics firmware segments through PSP\n",
        static_cast<unsigned>(graphicsPlan_.count));
    if (gfxfw::load(graphicsFiles_, gart_->ipLayout_, pciRevision_, gart_->allocator_,
        slots_[Payload].handle, gart_->transferIo(), gart_->hdpIo(), gart_->hdpState_,
        ringIo(), ring_, poll, graphics_, graphicsResult_) != gfxfw::Status::Ok ||
        !graphics_.autoloadComplete()) return fault();
    // Linux's non-PSP enum puts DMUB after RLC_G (which triggers autoload),
    // and before psp_rl_load. Authenticate/load the real DMUB image here;
    // display boot later binds the dedicated GART runtime BO and inbox.
    if (dmcubfw::load(displayResource_.data, displayResource_.bytes, gart_->allocator_,
        slots_[Payload].handle, gart_->transferIo(), gart_->hdpIo(), gart_->hdpState_,
        ringIo(), ring_, poll, displayFirmware_, displayFirmwareResult_) != dmcubfw::Status::Ok)
        return fault();
    // Linux psp_rl_load follows non-PSP firmware/autoload. The actual Navi31
    // SOS container supplies this REG_LIST image; it is not a bootloader step.
    if (sosImage_.registerList.size) {
        if (uploadVram(gart_->allocator_, slots_[Payload].handle, 0,
            sosResource_.data + sosImage_.registerList.offset, sosImage_.registerList.size,
            gart_->transferIo(), gart_->transferResult_) != VramTransferStatus::Ok ||
            !flushHdp(this) || pspring::submitLoadIp(ringIo(), ring_, slots_[Payload].handle,
                0, sosImage_.registerList.size, 67, ringResult_) != pspring::Status::Ok ||
            pspring::wait(ringIo(), ring_, poll, ringResult_) != pspring::Status::Ok) return fault();
    }
    const smu::Io powerIo = {this, read32, write32, delayUs};
    const smu::PollOptions powerPoll = {poll.maxPolls, poll.delayUs};
    if (smu::initialize(smuLayout_, graphics_, powerIo, powerPoll, smu_, smuResult_) != smu::Status::Ok ||
        smu::disableGfxOff(powerIo, powerPoll, smu_, smuResult_) != smu::Status::Ok || !smu_.awake())
        return fault();
    gart_->firmwareReady_ = true; state_ = State::FirmwareLoaded;
    IOLog("HackyNavi: PSP firmware loads and RLC bootload completed\n");
    IOLog("HackyNavi: SMU version=0x%x interface=%u; GFXOFF disabled and awake state checked\n",
        smu_.smuVersion(), smu_.interfaceVersion());
    return finish(kIOReturnSuccess);
}
bool HackyNaviFirmwareController::loadedFirmware(uint32_t type, gfxfw::LoadedSegment& out) {
    out = gfxfw::LoadedSegment{};
    IOLockLock(lock_);
    if (state_ == State::FirmwareLoaded) {
        for (size_t i = 0; i < graphics_.loadedCount(); ++i) {
            const auto* item = graphics_.loaded(i);
            if (item && item->pspType == type) {
                out = *item; IOLockUnlock(lock_); return true;
            }
        }
    }
    IOLockUnlock(lock_);
    return false;
}
IOReturn HackyNaviFirmwareController::retireAutoloadRlc(
    HackyNaviFunctionGroup::RetirementScope& scope,const gfxinit::RetirementPollOptions& poll) {
    if (!poll.iterations || poll.iterations > 1000000 || !poll.intervalUs || poll.intervalUs > 1000 ||
        uint64_t(poll.iterations) * poll.intervalUs > 10000000) return kIOReturnBadArgument;
    IOLockLock(lock_);
    if (!gart_) { IOLockUnlock(lock_); return kIOReturnNotReady; }
    IOLockLock(gart_->lock_);
    if (state_ != State::FirmwareLoaded || !hardwareHold_ || !lease_ || rlcRetired_ ||
        autoloadRetirement_.attempted() || !graphics_.autoloadComplete() || !smu_.awake() ||
        gart_->state_ != HackyNaviGARTController::State::BothEnabled || !gart_->firmwareReady_)
        return finish(kIOReturnNotReady);
    if (gart_->queueOwner_ || gart_->displayOwner_ || gart_->hasVmOwners() ||
        gart_->vmRetirementActive_ || gart_->vmRetirementFailure_ != kIOReturnSuccess ||
        displayFirmware_.needsRuntimeRetention()) return finish(kIOReturnBusy);
    if (gart_->normalAccessReady() || !scope.matchesGraphics(gart_->device_,gart_->openOwner_) ||
        !providerReady()) return finish(kIOReturnOffline);
    if (ring_.phase() != pspring::Phase::Running || ring_.pending() || !ring_.tmrReady() ||
        !ring_.needsRingRetention() || !ring_.needsTmrRetention()) return finish(kIOReturnNotReady);
    // The real allocator/PSP bindings remain live throughout the physical
    // retirement. No caller-supplied handles or CPU state reconstruct them.
    for (unsigned i = SosStage; i <= Tmr; ++i) {
        VramRange range{};
        if (gart_->allocator_.lookup(slots_[i].handle,range) != VramAllocatorStatus::Ok ||
            range.offset != slots_[i].range.offset || range.bytes != slots_[i].range.bytes || !range.bytes)
            return finish(kIOReturnNotReady);
    }
    gart_->firmwareReady_ = false;
    gart_->mmioActive_ = true;
    const gfxinit::RetirementIo io = {this,read32,write32,delayUs};
    const auto status = gfxinit::retireAutoloadRlc(gart_->ipLayout_,gart_->registerBytes_,graphics_,smu_,
        io,poll,autoloadRetirement_,autoloadRetirementResult_);
    IOLog("HackyNavi: no-queue RLC retirement status=%s stage=%u reads=%u writes=%u attempted=%u complete=%u\n",
        gfxinit::statusString(status),static_cast<unsigned>(autoloadRetirementResult_.stage),
        autoloadRetirementResult_.reads,autoloadRetirementResult_.writes,
        autoloadRetirement_.attempted(),autoloadRetirement_.complete());
    if (status != gfxinit::Status::Ok || !autoloadRetirement_.complete() ||
        !scope.matchesGraphics(gart_->device_,gart_->openOwner_) || !providerReady()) return fault();
    rlcRetired_ = true; state_ = State::Quiesced;
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviFirmwareController::retireAfterQuiescence(const pspring::PollOptions& poll) {
    if (!validPoll(poll)) return kIOReturnBadArgument;
    IOLockLock(lock_);
    if (!gart_) { IOLockUnlock(lock_); return kIOReturnNotReady; }
    IOLockLock(gart_->lock_);
    if (state_ == State::Retired) return finish(kIOReturnSuccess);
    // The explicit scoped TA retirement must finish before generic PSP/TMR
    // teardown. Never bypass a typed lease or its retained native Display.
    if (displayTa_.retentionRequired()) return finish(kIOReturnBusy);
    for (const auto& record : displayTaRecords_) if (record.owner) return finish(kIOReturnBusy);
    if (gart_->queueOwner_ || gart_->displayOwner_ || displayFirmware_.needsRuntimeRetention())
        return finish(kIOReturnBusy);
    gart_->firmwareReady_ = false;
    if (hardwareHold_) {
        if (state_ == State::FaultedRetained || state_ == State::SosLoaded ||
            !providerReady()) return finish(kIOReturnNotReady);
        // Quiesced can also be reached by clock retirement; only completed
        // physical RLC retirement permits destruction of loaded firmware/TMR.
        if (graphics_.autoloadComplete() && !rlcRetired_) return finish(kIOReturnNotReady);
        gart_->mmioActive_ = true;
        // Every typed display lease and queue/display consumer has retired.
        // A standalone loaded TA still retires only its actual returned ID.
        // Ambiguous loads/unloads remain sealed for reset recovery.
        const pspta::Kind kinds[] = {pspta::Kind::Hdcp,pspta::Kind::Dtm};
        for (const auto kind : kinds) {
            pspring::TaInformation ta{};
            if (!ring_.taInformation(kind,ta)) return fault();
            if (!ta.retained) continue;
            if (ta.phase != pspring::TaPhase::Loaded || !ta.sessionValid ||
                pspring::submitUnloadTa(ringIo(),ring_,kind,ringResult_) != pspring::Status::Ok ||
                pspring::wait(ringIo(),ring_,poll,ringResult_) != pspring::Status::Ok ||
                !ring_.taInformation(kind,ta) || ta.phase != pspring::TaPhase::Retired ||
                ta.retained || !ta.unloadAcknowledged)
                return fault();
        }
        if (ring_.needsTmrRetention()) {
            if (!ring_.tmrReady() ||
                pspring::submitDestroyTmr(ringIo(), ring_, ringResult_) != pspring::Status::Ok ||
                pspring::wait(ringIo(), ring_, poll, ringResult_) != pspring::Status::Ok)
                return fault();
        }
        if (pspring::stop(ringIo(), ring_, poll, ringResult_) != pspring::Status::Ok ||
            ring_.needsRingRetention() || ring_.needsTmrRetention() || ring_.needsTaRetention()) return fault();
    }
    const IOReturn result = releaseAllocationsLocked();
    if (result != kIOReturnSuccess) return finish(result);
    state_ = State::Retired;
    const bool dropHold = hardwareHold_; hardwareHold_ = false;
    finish(kIOReturnSuccess);
    if (dropHold) release();
    return kIOReturnSuccess;
}
IOReturn HackyNaviFirmwareController::releaseAfterDeviceReset() {
    IOLockLock(lock_);
    if (!gart_) { IOLockUnlock(lock_); return kIOReturnNotReady; }
    IOLockLock(gart_->lock_);
    if (hardwareHold_ && !gart_->accessReady()) return finish(kIOReturnOffline);
    if (gart_->queueOwner_ || gart_->displayOwner_) return finish(kIOReturnBusy);
    const IOReturn result = releaseAllocationsLocked();
    if (result != kIOReturnSuccess) return finish(result);
    // Portable states deliberately stay sealed; this owner cannot be reused
    // across reset epochs. Create a new owner for subsequent startup.
    state_ = State::Retired;
    const bool dropHold = hardwareHold_; hardwareHold_ = false;
    HackyNaviDisplayController* dropDisplays[pspdisplayta::kDisplays]{};
    for (size_t i = 0; i < pspdisplayta::kDisplays; ++i) {
        dropDisplays[i] = displayTaRecords_[i].owner;
        displayTaRecords_[i] = DisplayTaRecord{};
    }
    finish(kIOReturnSuccess);
    // Display reset retirement already cleared its GART lease. Never release
    // these retained objects under Firmware/GART locks: their final release
    // may enter descendant teardown. The portable State stays sealed.
    for (auto* display : dropDisplays) if (display) display->release();
    if (dropHold) release();
    return kIOReturnSuccess;
}
void HackyNaviFirmwareController::free() {
    // Published PSP references keep the self-hold until actual retirement or
    // reset recovery. Factory failures have never published these buffers.
    if (gart_) {
        IOLockLock(gart_->lock_);
        (void)releaseAllocationsLocked();
        IOLockUnlock(gart_->lock_);
        gart_->release(); gart_ = nullptr;
    }
    if (workspace_) { IOFree(workspace_, psp::kStagingBytes); workspace_ = nullptr; }
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    OSObject::free();
}
