// SPDX-License-Identifier: MIT
#include "HackyNaviDisplayController.hpp"
#include "HackyNaviScanoutSurface.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#if !defined(__x86_64__)
#error HackyNaviDisplayController requires coherent x86_64 DMA and uncached MMIO
#endif
OSDefineMetaClassAndStructors(HackyNaviDisplayController, OSObject)
using namespace hackynavi;
namespace {
using Region = HackyNaviGARTStorage::Region;
uint64_t timeUs() {
    uint64_t ticks = 0, ns = 0;
    clock_get_uptime(&ticks); absolutetime_to_nanoseconds(ticks, &ns); return ns / 1000;
}
bool validPoll(const dmcubfw::PollOptions& p) {
    return p.iterations && p.iterations <= 1000000 && p.intervalUs && p.intervalUs <= 1000 &&
        uint64_t(p.iterations) * p.intervalUs <= 1000000;
}
bool validPoll(const dmcub::PollOptions& p) {
    return p.maxPolls && p.maxPolls <= 1000000 && p.delayUs && p.delayUs <= 100 &&
        uint64_t(p.maxPolls) * p.delayUs <= 2000000;
}
bool validPoll(const displaypower::Poll& p) {
    return p.iterations && p.iterations <= 100000 && p.intervalUs && p.intervalUs <= 1000 &&
        uint64_t(p.iterations) * p.intervalUs <= 100000;
}
}
bool HackyNaviDisplayController::init() {
    lock_ = nullptr; firmware_ = nullptr; gart_ = nullptr; rom_ = nullptr; scanoutOwner_ = nullptr;
    lease_ = hardwareHold_ = false; state_ = State::Empty; paths_ = 0;
    image_ = ImageInfo{}; memory_ = dmcubfw::Memory{};
    firmwareLayout_ = dmcubfw::Layout{}; inboxLayout_ = dmcub::Layout{};
    inboxResult_ = dmcub::Result{}; auxLayout_ = dpaux::Layout{}; ddcLayout_ = hdmiddc::Layout{};
    dioLayout_ = dio::Layout{}; linkResult_ = dio::Result{};
    streamLayout_ = hdmistream::Layout{};
    idleLayout_ = displayidle::Layout{}; pipeClockLayout_ = displayclock::Layout{};
    powerLayout_ = displaypower::Layout{};
    clockSmuLayout_ = displaysmu::Layout{}; dppReferenceKHz_ = dispReferenceKHz_ = 0;
    globalClockReferences_[0] = {}; globalClockReferences_[1] = {}; globalClockSerial_ = 0;
    globalClockAttempt_ = nullptr; globalClockReport_ = nullptr; globalClockCeilingKHz_ = 0;
    memoryGeneration_ = memorySerial_ = 0;
    fams_ = {};
    dpmInventory_ = {}; dpmBoard_ = {}; dpmReferences_ = {}; dpmInventoryValid_ = false;
    dpmGeneration_ = dpmSerial_ = dpmStart_ = dpmLast_ = 0; dpmBudget_ = 0;
    dpmScope_ = nullptr; dpmRetirementScope_ = nullptr; dpmReport_ = nullptr;
    for (uint8_t i = 0; i < kMaxDisplayPaths; ++i) {
        routes_[i] = dmcub::Route{}; routeValid_[i] = false; clearLinkMetadata(i);
    }
    if (!OSObject::init()) return false;
    lock_ = IOLockAlloc(); return lock_ != nullptr;
}
HackyNaviDisplayController* HackyNaviDisplayController::withFirmware(
    HackyNaviFirmwareController* firmware, const uint8_t* rom, size_t bytes,
    const ImageInfo& image, IOReturn& result) {
    result = kIOReturnNoMemory;
    auto* self = new HackyNaviDisplayController;
    if (!self) return nullptr;
    if (!self->init()) { self->release(); return nullptr; }
    result = self->initialize(firmware, rom, bytes, image);
    if (result != kIOReturnSuccess) { self->release(); return nullptr; }
    return self;
}
IOReturn HackyNaviDisplayController::initialize(HackyNaviFirmwareController* firmware,
    const uint8_t* rom, size_t bytes, const ImageInfo& image) {
    if (!firmware || !firmware->gart_ || !rom) return kIOReturnBadArgument;
    ImageInfo checked{};
    if (parseImage(rom, bytes, image.offset, checked) != Status::Ok || checked.codeType ||
        checked.vendorId != 0x1002 || checked.deviceId != 0x744c || !checked.checksumValid ||
        !checked.size || checked.size > 1024 * 1024) return kIOReturnBadArgument;
    rom_ = OSData::withBytes(rom + checked.offset, static_cast<unsigned>(checked.size));
    if (!rom_) return kIOReturnNoMemory;
    const auto* data = static_cast<const uint8_t*>(rom_->getBytesNoCopy());
    AtomInfo atom{}; DisplayInfo display{};
    if (parseImage(data, rom_->getLength(), 0, image_) != Status::Ok || !image_.checksumValid ||
        image_.vendorId != 0x1002 || image_.deviceId != 0x744c || image_.codeType ||
        parseAtom(data, rom_->getLength(), image_, atom) != Status::Ok ||
        parseDisplay(data, rom_->getLength(), image_, display) != Status::Ok || !display.pathCount)
        return kIOReturnBadArgument;
    firmware_ = firmware; firmware_->retain(); gart_ = firmware_->gart_; gart_->retain();
    lockOwners();
    if (!gart_->normalAccessReady() || gart_->displayOwner_ || !gart_->discovery_ || !firmware_->providerReady() ||
        !gart_->firmwareReady_ || firmware_->state_ != HackyNaviFirmwareController::State::FirmwareLoaded ||
        firmware_->displayFirmware_.phase() != dmcubfw::Phase::Loaded) return finish(kIOReturnNotReady);
    const uint32_t subsystem = gart_->device_->configRead32(0x2c);
    if (!gart_->normalAccessReady()) return finish(kIOReturnOffline);
    if (subsystem == UINT32_MAX || uint16_t(subsystem) != atom.subsystemVendorId ||
        uint16_t(subsystem >> 16) != atom.subsystemDeviceId) return finish(kIOReturnUnsupported);
    const auto* discovery = static_cast<const uint8_t*>(gart_->discovery_->getBytesNoCopy());
    const size_t discoveryBytes = gart_->discovery_->getLength();
    if (dmcubfw::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, firmwareLayout_) != dmcubfw::Status::Ok ||
        dmcub::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, inboxLayout_) != dmcub::Status::Ok ||
        dpaux::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, auxLayout_) != dpaux::Status::Ok ||
        hdmiddc::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, ddcLayout_) != hdmiddc::Status::Ok ||
        dio::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, dioLayout_) != dio::Status::Ok ||
        hdmistream::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, streamLayout_) != hdmistream::Status::Ok ||
        displayidle::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, idleLayout_) != displayidle::Status::Ok ||
        displaypower::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, powerLayout_) != displaypower::Status::Ok ||
        displayclock::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, pipeClockLayout_) != displayclock::Status::Ok ||
        displaysmu::buildLayout(gart_->ipLayout_, gart_->registerBytes_, clockSmuLayout_) != displaysmu::Status::Ok)
        return finish(kIOReturnUnsupported);
    paths_ = display.pathCount;
    for (uint8_t i = 0; i < paths_; ++i) {
        routeValid_[i] = dmcub::parseRoute(data, rom_->getLength(), image_, inboxLayout_, i, routes_[i]) == dmcub::Status::Ok;
        // The same physical pins/PHY cannot be independently owned by two
        // native routes. Leave unsupported routes unavailable to callers.
        if (routeValid_[i]) for (uint8_t j = 0; j < i; ++j) if (routeValid_[j] &&
            (routes_[i].phy == routes_[j].phy || routes_[i].ddc == routes_[j].ddc))
            return finish(kIOReturnUnsupported);
    }
    const auto* buffer = gart_->storage_.buffer(Region::Display);
    dmcubfw::Image fwImage{}; dmcubfw::Plan plan{};
    if (!buffer || !buffer->cpuAddress() || buffer->size() > SIZE_MAX ||
        !gart_->gpuAddress(Region::Display, memory_.gpuMcAddress) ||
        dmcubfw::parse(firmware_->displayResource_.data, firmware_->displayResource_.bytes, fwImage) != dmcubfw::Status::Ok ||
        dmcubfw::plan(fwImage, uint32_t(image_.size), plan) != dmcubfw::Status::Ok ||
        buffer->size() < plan.totalBytes) return finish(kIOReturnNoSpace);
    memory_.cpu = static_cast<uint8_t*>(buffer->cpuAddress()); memory_.bytes = size_t(buffer->size());
    gart_->displayOwner_ = this; lease_ = true; state_ = State::Prepared;
    return finish(kIOReturnSuccess);
}
void HackyNaviDisplayController::lockOwners() {
    IOLockLock(lock_); IOLockLock(firmware_->lock_); IOLockLock(gart_->lock_);
}
IOReturn HackyNaviDisplayController::finish(IOReturn result) {
    gart_->mmioActive_ = false;
    IOLockUnlock(gart_->lock_); IOLockUnlock(firmware_->lock_); IOLockUnlock(lock_);
    return result;
}
bool HackyNaviDisplayController::providerReady(bool retiring) const {
    return (retiring ? gart_->accessReady() : gart_->normalAccessReady()) &&
        lease_ && gart_->displayOwner_ == this && firmware_->providerReady() &&
        (firmware_->state_ == HackyNaviFirmwareController::State::FirmwareLoaded ||
         (retiring && firmware_->state_ == HackyNaviFirmwareController::State::Quiesced)) &&
        (retiring || gart_->firmwareReady_) &&
        (retiring ? gart_->accessReady() : gart_->normalAccessReady());
}
IOReturn HackyNaviDisplayController::fault() {
    invalidateGlobalClockReferences();
    state_ = State::FaultedRetained;
    IOLog("HackyNavi: display fault, DMUB stage=%u status=%s register=0x%x; owners retained\n",
        unsigned(firmware_->displayFirmwareResult_.stage), dmcubfw::statusString(firmware_->displayFirmwareResult_.status),
        firmware_->displayFirmwareResult_.lastRegister);
    return kIOReturnIOError;
}
bool HackyNaviDisplayController::read32(void* context, uint32_t offset, uint32_t& value) {
    auto* self = static_cast<HackyNaviDisplayController*>(context);
    return self->dpmCaptureReady() && HackyNaviGARTController::read32(self->gart_, offset, value) &&
        self->dpmCaptureReady();
}
bool HackyNaviDisplayController::write32(void* context, uint32_t offset, uint32_t value) {
    auto* self = static_cast<HackyNaviDisplayController*>(context);
    return self->dpmCaptureReady() && HackyNaviGARTController::write32(self->gart_, offset, value) &&
        self->dpmCaptureReady();
}
bool HackyNaviDisplayController::memoryRange(uint64_t address, size_t bytes) const {
    return memory_.cpu && bytes && address >= memory_.gpuMcAddress &&
        address - memory_.gpuMcAddress < memory_.bytes &&
        bytes <= memory_.bytes - size_t(address - memory_.gpuMcAddress);
}
bool HackyNaviDisplayController::readMemory(void* context, uint64_t address, uint8_t* data, size_t bytes) {
    auto* self = static_cast<HackyNaviDisplayController*>(context);
    if (!self->dpmCaptureReady() || !self->gart_->mmioActive_ || !data || !self->memoryRange(address, bytes) ||
        self->gart_->synchronizeCpuLocked(Region::Display) != kIOReturnSuccess) return false;
    if (!self->dpmCaptureReady()) return false;
    memmove(data, self->memory_.cpu + size_t(address - self->memory_.gpuMcAddress), bytes);
    return self->dpmCaptureReady();
}
bool HackyNaviDisplayController::writeMemory(void* context, uint64_t address, const uint8_t* data, size_t bytes) {
    auto* self = static_cast<HackyNaviDisplayController*>(context);
    if (!self->dpmCaptureReady() || !self->gart_->mmioActive_ || !data || !self->memoryRange(address, bytes)) return false;
    memmove(self->memory_.cpu + size_t(address - self->memory_.gpuMcAddress), data, bytes);
    return self->dpmCaptureReady();
}
bool HackyNaviDisplayController::publish(void* context) {
    auto* self = static_cast<HackyNaviDisplayController*>(context);
    return self->dpmCaptureReady() && self->gart_->mmioActive_ &&
        self->gart_->synchronizeDeviceLocked(Region::Display) == kIOReturnSuccess &&
        self->dpmCaptureReady() &&
        HackyNaviGARTController::hdpFlush(self->gart_) && self->dpmCaptureReady();
}
void HackyNaviDisplayController::delayUs(void* context, uint32_t us) {
    auto* self = static_cast<HackyNaviDisplayController*>(context);
    if (!self->dpmCaptureReady() || us > 100000) return;
    if ((self->dpmScope_ || self->dpmRetirementScope_) &&
        us >= self->dpmBudget_ - (self->dpmLast_ - self->dpmStart_)) {
        self->dpmReport_->result = kIOReturnTimeout; return;
    }
    if (us >= 1000) IOSleep((us + 999) / 1000); else IODelay(us);
    (void)self->dpmCaptureReady();
}
dmcubfw::Io HackyNaviDisplayController::firmwareIo() { return {this, read32, write32, publish, delayUs}; }
dmcub::Io HackyNaviDisplayController::inboxIo() { return {this, read32, write32, readMemory, writeMemory, publish, delayUs}; }
IOReturn HackyNaviDisplayController::boot(const dmcubfw::PollOptions& poll) {
    return bootInternal(nullptr, poll);
}
IOReturn HackyNaviDisplayController::boot(HackyNaviFunctionGroup::Operation& scope,
    const dmcubfw::PollOptions& poll) {
    return bootInternal(&scope, poll);
}
IOReturn HackyNaviDisplayController::bootInternal(HackyNaviFunctionGroup::Operation* scope,
    const dmcubfw::PollOptions& poll) {
    if (!validPoll(poll)) return kIOReturnBadArgument;
    lockOwners();
    if (state_ != State::Prepared) return finish(kIOReturnNotReady);
    if (!providerReady()) return finish(kIOReturnOffline);
    if (scope && !scope->matchesGraphics(gart_->device_, gart_->openOwner_)) return finish(kIOReturnOffline);
    const auto& maximum = clockSmu_.memoryMaximumJournal();
    const auto& deepSleep = clockSmu_.dcfDeepSleepJournal();
    if ((clockSmu_.memoryJournal().requestAttempted || maximum.requestAttempted || deepSleep.requestAttempted) &&
        (!scope || !memoryGeneration_ || scope->generation() != memoryGeneration_ || !clockSmu_.ready() ||
         (clockSmu_.memoryJournal().requestAttempted && !clockSmu_.memoryJournal().fulfilled) ||
         (maximum.requestAttempted && (!maximum.commandAcknowledged || !maximum.replyRead)) ||
         (deepSleep.requestAttempted && (!deepSleep.commandAcknowledged || !deepSleep.replyRead))))
        return finish(kIOReturnNotReady);
    // A pre-boot UCLK request already retains the same hardware owner.
    if (!hardwareHold_) { retain(); hardwareHold_ = true; }
    gart_->mmioActive_ = true;
    if (dmcubfw::boot(firmwareLayout_, static_cast<const uint8_t*>(rom_->getBytesNoCopy()),
        rom_->getLength(), image_, memory_, firmwareIo(), poll, firmware_->displayFirmware_,
        firmware_->displayFirmwareResult_) != dmcubfw::Status::Ok) return finish(fault());
    if (inbox_.open(inboxLayout_, inboxIo(), firmware_->displayFirmware_.mailboxGpuAddress(), inboxResult_) != dmcub::Status::Ok)
        return finish(fault());
    state_ = State::Ready;
    IOLog("HackyNavi: DMCUB firmware version verified; real GART mailbox opened\n");
    return finish(kIOReturnSuccess);
}
bool HackyNaviDisplayController::validPath(uint8_t path, uint8_t connector) const {
    return path < paths_ && routeValid_[path] && (!connector || uint8_t(routes_[path].connectorObject) == connector);
}
IOReturn HackyNaviDisplayController::checkClockIdleLocked(ClockResult& out) {
    const displayidle::Io io = {this, read32};
    const auto status = displayidle::verify(idleLayout_, io, out.idle);
    if (status == displayidle::Status::Ok) return kIOReturnSuccess;
    IOLog("HackyNavi: clock admission pipe=%u status=%u register=0x%x value=0x%x\n",
        unsigned(out.idle.pipe), unsigned(status), out.idle.lastRegister, out.idle.lastValue);
    if (status == displayidle::Status::ReadFailed || status == displayidle::Status::DeviceUnavailable)
        return fault();
    return kIOReturnBusy;
}
void HackyNaviDisplayController::invalidateDpmInventory() {
    dpmInventoryValid_ = false; dpmGeneration_ = 0; dpmInventory_ = {}; dpmBoard_ = {}; dpmReferences_ = {};
}
bool HackyNaviDisplayController::referenceValid(void* context) {
    return static_cast<HackyNaviDisplayController*>(context)->dpmCaptureReady();
}
uint64_t HackyNaviDisplayController::referenceTimeUs(void*) { return timeUs(); }
bool HackyNaviDisplayController::dpmCaptureReady() {
    if (!dpmScope_ && !dpmRetirementScope_) return gart_->accessReady();
    if (!dpmReport_ || dpmReport_->result != kIOReturnSuccess) return false;
    const bool normal = dpmScope_ && !dpmRetirementScope_ && gart_->normalAccessReady() &&
        dpmScope_->matchesGraphics(gart_->device_, gart_->openOwner_);
    const bool retiring = dpmRetirementScope_ && !dpmScope_ && gart_->accessReady() &&
        dpmRetirementScope_->matchesGraphics(gart_->device_, gart_->openOwner_);
    if (!normal && !retiring) {
        dpmReport_->result = kIOReturnOffline; return false;
    }
    const uint64_t now = timeUs();
    if (now < dpmLast_) { dpmReport_->result = kIOReturnError; return false; }
    dpmLast_ = now; dpmReport_->elapsedUs = now - dpmStart_;
    if (now - dpmStart_ >= dpmBudget_) { dpmReport_->result = kIOReturnTimeout; return false; }
    return true;
}
IOReturn HackyNaviDisplayController::finishDpmCapture(IOReturn result) {
    if (dpmReport_->result == kIOReturnSuccess) dpmReport_->result = result;
    result = dpmReport_->result;
    if (result != kIOReturnSuccess) invalidateDpmInventory();
    dpmScope_ = nullptr; dpmRetirementScope_ = nullptr; dpmReport_ = nullptr; dpmBudget_ = 0;
    return finish(result);
}
IOReturn HackyNaviDisplayController::captureDpmInventory(HackyNaviFunctionGroup::Operation& scope,
    uint32_t timeoutUs, DpmCaptureResult& out) {
    out = {}; out.result = kIOReturnBadArgument;
    if (!timeoutUs || timeoutUs > 2000000) return out.result;
    if (!gart_ || !gart_->normalAccessReady() || !scope.matchesGraphics(gart_->device_, gart_->openOwner_))
        return out.result = kIOReturnOffline;
    const uint64_t started = timeUs();
    lockOwners(); invalidateDpmInventory();
    dpmScope_ = &scope; dpmReport_ = &out; dpmStart_ = dpmLast_ = started;
    dpmBudget_ = timeoutUs; out.result = kIOReturnSuccess;
    if (!dpmCaptureReady()) return finishDpmCapture(out.result);
    if (state_ != State::Ready || !providerReady()) return finishDpmCapture(kIOReturnNotReady);
    out.boardStatus = displaymodel::parseBoard(static_cast<const uint8_t*>(rom_->getBytesNoCopy()),
        rom_->getLength(), image_, dpmBoard_);
    if (out.boardStatus != displaymodel::Status::Ok) return finishDpmCapture(kIOReturnUnsupported);
    if (dpmSerial_ == UINT64_MAX) return finishDpmCapture(kIOReturnNoResources);
    if (scanoutOwner_ && scanoutOwner_->requiresHardwareRetirement()) return finishDpmCapture(kIOReturnBusy);
    for (uint8_t path = 0; path < paths_; ++path)
        if (links_[path].phase() != dio::Phase::Closed || streams_[path].requiresHardwareRetirement())
            return finishDpmCapture(kIOReturnBusy);
    for (size_t pipe = 0; pipe < displayclock::kPipeCount; ++pipe)
        if (pipeClocks_[pipe].requiresHardwareRetirement()) return finishDpmCapture(kIOReturnBusy);
    gart_->mmioActive_ = true;
    ClockResult idle{}; const auto admission = checkClockIdleLocked(idle); out.idle = idle.idle;
    if (admission != kIOReturnSuccess) return finishDpmCapture(admission);
    // Delay/read budget is finite in addition to the measured deadline above.
    // No query index/count controls a retry loop or caller-provided allocation.
    const displaysmu::Poll poll = {100000, 10};
    const displaysmu::Io io = {this, read32, write32, delayUs};
    if (!clockSmu_.ready() && clockSmu_.initialize(clockSmuLayout_, io, poll, out.initialize) != displaysmu::Status::Ok)
        return finishDpmCapture(fault());
    if (clockSmu_.queryDpmInventory(poll, dpmInventory_, out.query) != displaysmu::Status::Ok || !dpmCaptureReady())
        return finishDpmCapture(fault());
    const displaymodel::Io referenceIo = {this, referenceValid, read32, referenceTimeUs};
    if (displaymodel::captureReferences(static_cast<const uint8_t*>(gart_->discovery_->getBytesNoCopy()),
        gart_->discovery_->getLength(), gart_->registerBytes_, dpmBoard_, referenceIo,
        uint32_t(dpmBudget_ - (dpmLast_ - dpmStart_)), dpmReferences_, out.references) != displaymodel::Status::Ok ||
        !dpmCaptureReady()) return finishDpmCapture(fault());
    dpmGeneration_ = scope.generation(); ++dpmSerial_; dpmInventoryValid_ = true; out.serial = dpmSerial_;
    return finishDpmCapture(kIOReturnSuccess);
}
IOReturn HackyNaviDisplayController::copyDpmObservation(HackyNaviFunctionGroup::Operation& scope,
    DpmObservation& out) {
    out = {};
    if (!gart_ || !gart_->normalAccessReady() || !scope.matchesGraphics(gart_->device_, gart_->openOwner_))
        return kIOReturnOffline;
    lockOwners();
    if (!dpmInventoryValid_ || state_ != State::Ready || !providerReady() || dpmGeneration_ != scope.generation())
        return finish(kIOReturnNotReady);
    out = {dpmGeneration_, dpmSerial_, dpmInventory_, dpmBoard_, dpmReferences_};
    if (!gart_->normalAccessReady() || !scope.matchesGraphics(gart_->device_, gart_->openOwner_)) {
        out = {}; invalidateDpmInventory(); return finish(kIOReturnOffline);
    }
    return finish(kIOReturnSuccess);
}
bool HackyNaviDisplayController::memoryFirmwareStoppedLocked(MemoryMinimumResult& out) {
    // Same DMCUB_CNTL.ENABLE / CNTL2.SOFT_RESET fields used by the real
    // dmcubfw STOP_FW/PWAIT/reset protocol. Do not infer this from Loaded.
    uint32_t after = 0;
    const auto control = inboxLayout_.registers[size_t(dmcub::Register::Control)];
    const auto reset = inboxLayout_.registers[size_t(dmcub::Register::Control2)];
    return read32(this, reset, out.dmcubControl2) && out.dmcubControl2 != UINT32_MAX &&
        (out.dmcubControl2 & 1u) && read32(this, control, out.dmcubControl) &&
        out.dmcubControl != UINT32_MAX && !(out.dmcubControl & 0x10000u) &&
        read32(this, reset, after) && after == out.dmcubControl2 && dpmCaptureReady();
}
IOReturn HackyNaviDisplayController::memoryClockReadyLocked(
    HackyNaviFunctionGroup::Operation& scope, MemoryMinimumResult& out) {
    if (state_ == State::Prepared) {
        if (firmware_->displayFirmware_.phase() != dmcubfw::Phase::Loaded)
            return kIOReturnNotReady;
        return memoryFirmwareStoppedLocked(out) ? kIOReturnSuccess : kIOReturnNotReady;
    }
    // Only this owner can submit FAMS configuration; it exposes no raw inbox
    // writer. A copied journal or a foreign/generation-stale observation is
    // never accepted. checkIdle rechecks real firmware and ring ownership.
    const auto& policy = clockSmu_.dmcubWaitJournal();
    if (state_ != State::Ready || !firmware_->displayFirmware_.ready() ||
        !clockSmu_.ready() || !fams_.started || !fams_.complete ||
        !fams_.serial || fams_.deviceGeneration != scope.generation() ||
        !fams_.dalAcknowledged || !fams_.commandWritten || !fams_.commandSubmitted ||
        !fams_.commandConsumed || !policy.requestAttempted ||
        !policy.commandAcknowledged || !policy.replyRead || !inbox_.isOpen() || inbox_.pending())
        return kIOReturnNotReady;
    if (inbox_.checkIdle(out.firmwareCoordination) != dmcub::Status::Ok || !dpmCaptureReady())
        return fault();
    out.runningFirmwareCoordinated = true;
    out.famsSerial = fams_.serial;
    return kIOReturnSuccess;
}
IOReturn HackyNaviDisplayController::requestMemoryMinimum(HackyNaviFunctionGroup::Operation& scope,
    uint16_t mhz, uint32_t timeoutUs, MemoryMinimumResult& out) {
    out = {}; out.result = kIOReturnBadArgument;
    if (!mhz || !timeoutUs || timeoutUs > 2000000) return out.result;
    if (!gart_ || !gart_->normalAccessReady() || !scope.matchesGraphics(gart_->device_, gart_->openOwner_))
        return out.result = kIOReturnOffline;
    // Reuse the native deadline/guard callbacks while holding their exact
    // owner locks. No Operation is acquired recursively or retained over IPC.
    const uint64_t started = timeUs(); DpmCaptureResult guard{};
    lockOwners(); dpmScope_ = &scope; dpmReport_ = &guard;
    dpmStart_ = dpmLast_ = started; dpmBudget_ = timeoutUs; guard.result = kIOReturnSuccess;
    const auto done = [&](IOReturn result) {
        if (guard.result == kIOReturnSuccess) guard.result = result;
        out.result = guard.result; out.elapsedUs = guard.elapsedUs;
        out.deviceGeneration = memoryGeneration_; out.serial = memorySerial_;
        out.journal = clockSmu_.memoryJournal();
        return finishDpmCapture(out.result);
    };
    if (!dpmCaptureReady()) return done(guard.result);
    if ((state_ != State::Prepared && state_ != State::Ready) || !providerReady())
        return done(kIOReturnNotReady);
    if (memoryGeneration_ && memoryGeneration_ != scope.generation()) return done(kIOReturnOffline);
    if (clockSmu_.memoryMaximumJournal().requestAttempted &&
        mhz > clockSmu_.memoryMaximumJournal().requestedMHz) return done(kIOReturnBadArgument);
    if (memorySerial_ == UINT64_MAX) return done(kIOReturnNoResources);
    if (scanoutOwner_) return done(kIOReturnBusy);
    for (uint8_t path = 0; path < paths_; ++path)
        if (links_[path].phase() != dio::Phase::Closed || streams_[path].requiresHardwareRetirement())
            return done(kIOReturnBusy);
    for (size_t pipe = 0; pipe < displayclock::kPipeCount; ++pipe)
        if (pipeClocks_[pipe].requiresHardwareRetirement()) return done(kIOReturnBusy);
    gart_->mmioActive_ = true;
    const auto coordinated = memoryClockReadyLocked(scope, out);
    if (coordinated != kIOReturnSuccess) return done(coordinated);
    ClockResult idle{}; const auto admission = checkClockIdleLocked(idle); out.idle = idle.idle;
    if (admission != kIOReturnSuccess) return done(admission);
    // Before any DAL write, pin this owner and all its existing firmware/GART
    // leases. Even an unsuccessful callback may have delivered a command.
    if (!hardwareHold_) { retain(); hardwareHold_ = true; }
    invalidateDpmInventory();
    const displaysmu::Poll poll = {100000, 10};
    const displaysmu::Io io = {this, read32, write32, delayUs};
    if (!clockSmu_.ready() && clockSmu_.initialize(clockSmuLayout_, io, poll, out.initialize) != displaysmu::Status::Ok)
        return done(fault());
    if (clockSmu_.queryDpmInventory(poll, dpmInventory_, out.inventory) != displaysmu::Status::Ok || !dpmCaptureReady())
        return done(fault());
    const auto& memory = dpmInventory_.clocks[1];
    bool supported = memory.clock == displaysmu::DpmClock::Memory && mhz <= memory.dcMaxMHz;
    if (supported && memory.fineGrained)
        supported = mhz >= memory.frequencyMHz[0] && mhz <= memory.frequencyMHz[1];
    else if (supported) {
        supported = false;
        for (uint8_t level = 0; level < memory.levelCount; ++level)
            if (mhz == memory.frequencyMHz[level]) supported = true;
    }
    invalidateDpmInventory();
    if (!supported || mhz < clockSmu_.memoryJournal().minimumMHz) return done(kIOReturnBadArgument);
    if (memoryClockReadyLocked(scope, out) != kIOReturnSuccess) return done(fault());
    memoryGeneration_ = scope.generation(); ++memorySerial_;
    displaysmu::MemoryMinimum accepted{};
    if (clockSmu_.requestMemoryMinimum(mhz, poll, accepted, out.minimum) != displaysmu::Status::Ok ||
        !dpmCaptureReady()) return done(fault());
    // Recheck the cold admission while retaining the same global exclusion.
    if (memoryClockReadyLocked(scope, out) != kIOReturnSuccess) return done(fault());
    const auto finalIdle = checkClockIdleLocked(idle); out.idle = idle.idle;
    if (finalIdle != kIOReturnSuccess || !dpmCaptureReady()) return done(fault());
    return done(kIOReturnSuccess);
}
IOReturn HackyNaviDisplayController::disableFamsForColdClockChanges(
    HackyNaviFunctionGroup::Operation& scope, uint32_t timeoutUs, FamsResult& out) {
    out = {}; out.result = kIOReturnBadArgument;
    if (!timeoutUs || timeoutUs > 2000000) return out.result;
    if (!gart_ || !gart_->normalAccessReady() || !scope.matchesGraphics(gart_->device_, gart_->openOwner_))
        return out.result = kIOReturnOffline;
    const uint64_t started = timeUs(); DpmCaptureResult guard{};
    lockOwners(); dpmScope_ = &scope; dpmReport_ = &guard;
    dpmStart_ = dpmLast_ = started; dpmBudget_ = timeoutUs; guard.result = kIOReturnSuccess;
    const auto done = [&](IOReturn result) {
        if (guard.result == kIOReturnSuccess) guard.result = result;
        out.result = guard.result; out.elapsedUs = guard.elapsedUs; out.journal = fams_;
        return finishDpmCapture(out.result);
    };
    if (!dpmCaptureReady()) return done(guard.result);
    if (state_ != State::Ready || !providerReady() || !inbox_.isOpen() || inbox_.pending())
        return done(kIOReturnNotReady);
    if (fams_.deviceGeneration && fams_.deviceGeneration != scope.generation()) return done(kIOReturnOffline);
    if (fams_.serial == UINT64_MAX) return done(kIOReturnNoResources);
    if (scanoutOwner_) return done(kIOReturnBusy);
    for (uint8_t path = 0; path < paths_; ++path)
        if (links_[path].phase() != dio::Phase::Closed || streams_[path].requiresHardwareRetirement())
            return done(kIOReturnBusy);
    for (size_t pipe = 0; pipe < displayclock::kPipeCount; ++pipe)
        if (pipeClocks_[pipe].requiresHardwareRetirement()) return done(kIOReturnBusy);
    gart_->mmioActive_ = true;
    ClockResult idle{}; const auto admission = checkClockIdleLocked(idle); out.idle = idle.idle;
    if (admission != kIOReturnSuccess || !dpmCaptureReady()) return done(admission);
    if (inbox_.checkIdle(out.preflight) != dmcub::Status::Ok || !dpmCaptureReady()) return done(fault());
    if (!hardwareHold_) { retain(); hardwareHold_ = true; }
    invalidateDpmInventory();
    const displaysmu::Poll dalPoll = {100000, 10};
    const displaysmu::Io io = {this, read32, write32, delayUs};
    if (!clockSmu_.ready() && clockSmu_.initialize(clockSmuLayout_, io, dalPoll, out.initialize) != displaysmu::Status::Ok)
        return done(fault());
    if (!dpmCaptureReady()) return done(fault());
    // This owner was constructed/booted with the pinned DCN3.2 firmware.
    // No public arbitrary command API can enable FAMS concurrently. Hold the
    // same normal hardware scope and Display/Firmware/GART locks throughout.
    const uint64_t serial = fams_.serial + 1;
    fams_ = {scope.generation(), serial, true, false, false, false, false, false};
    const auto dalStatus = clockSmu_.disableDmcubMclkWait(dalPoll, out.dal);
    fams_.dalAcknowledged = clockSmu_.dmcubWaitJournal().commandAcknowledged;
    if (dalStatus != displaysmu::Status::Ok || !dpmCaptureReady()) return done(fault());
    const dmcub::PollOptions inboxPoll = {100000, 10};
    const auto inboxStatus = inbox_.disableFams(inboxPoll, out.inbox);
    fams_.commandWritten = out.inbox.slotWritten;
    fams_.commandSubmitted = out.inbox.submitted;
    fams_.commandConsumed = out.inbox.consumed;
    if (inboxStatus != dmcub::Status::Ok || !out.inbox.consumed || !dpmCaptureReady()) return done(fault());
    const auto finalIdle = checkClockIdleLocked(idle); out.idle = idle.idle;
    if (finalIdle != kIOReturnSuccess || !dpmCaptureReady()) return done(fault());
    fams_.complete = true;
    return done(kIOReturnSuccess);
}
IOReturn HackyNaviDisplayController::requestClockMinimum(displaysmu::Clock clock, uint16_t mhz,
    const displaysmu::Poll& poll, displaysmu::Frequency& frequency, ClockResult& out) {
    out = ClockResult{};
    if (!mhz || (clock != displaysmu::Clock::Disp && clock != displaysmu::Clock::Dpp &&
        clock != displaysmu::Clock::Dcf && clock != displaysmu::Clock::Dtb) ||
        ((clock == displaysmu::Clock::Disp || clock == displaysmu::Clock::Dpp) && mhz > 1950) ||
        !poll.maxReads || poll.maxReads > 1000000 || !poll.delayUs || poll.delayUs > 1000 ||
        uint64_t(poll.maxReads) * poll.delayUs > 2000000) return kIOReturnBadArgument;
    lockOwners();
    if (state_ != State::Ready) return finish(kIOReturnNotReady);
    if (scanoutOwner_ && scanoutOwner_->requiresHardwareRetirement()) return finish(kIOReturnBusy);
    if (!providerReady()) return finish(fault());
    for (uint8_t path = 0; path < paths_; ++path)
        if (links_[path].phase() != dio::Phase::Closed || streams_[path].requiresHardwareRetirement())
            return finish(kIOReturnBusy);
    for (size_t pipe = 0; pipe < displayclock::kPipeCount; ++pipe)
        if (pipeClocks_[pipe].requiresHardwareRetirement()) return finish(kIOReturnBusy);
    gart_->mmioActive_ = true;
    const auto idle = checkClockIdleLocked(out); if (idle != kIOReturnSuccess) return finish(idle);
    invalidateGlobalClockReferences();
    invalidateDpmInventory();
    const displaysmu::Io io = {this, read32, write32, delayUs};
    if (!clockSmu_.ready() && clockSmu_.initialize(clockSmuLayout_, io, poll, out.smu) != displaysmu::Status::Ok)
        return finish(fault());
    displaysmu::Frequency accepted{};
    const auto status = clockSmu_.requestMinimum(clock, mhz, poll, accepted, out.smu);
    if (status != displaysmu::Status::Ok) {
        IOLog("HackyNavi: display clock request status=%u stage=%u response=0x%x; ownership retained\n",
            unsigned(status), unsigned(out.smu.stage), out.smu.response);
        return finish(clockSmu_.faulted() ? fault() : kIOReturnBadArgument);
    }
    // Legacy transport replies never publish a verified global reference.
    frequency = accepted; return finish(kIOReturnSuccess);
}
IOReturn HackyNaviDisplayController::stopDppClockLocked(uint8_t pipe, ClockResult& out) {
    if (scanoutOwner_ && scanoutOwner_->requiresHardwareRetirement()) return kIOReturnBusy;
    if (!pipeClocks_[pipe].requiresHardwareRetirement()) return kIOReturnSuccess;
    const auto idle = checkClockIdleLocked(out); if (idle != kIOReturnSuccess) return idle;
    if (out.idle.poweredOffMask & (1u << pipe)) return fault();
    const auto status = pipeClocks_[pipe].stop(out.pipe);
    return status == displayclock::Status::Ok ? kIOReturnSuccess : fault();
}
IOReturn HackyNaviDisplayController::stopDppClock(uint8_t pipe, ClockResult& out) {
    out = ClockResult{}; if (pipe >= displayclock::kPipeCount) return kIOReturnBadArgument;
    lockOwners();
    if (state_ != State::Ready && state_ != State::FaultedRetained) return finish(kIOReturnNotReady);
    if (!providerReady(true)) return finish(fault());
    gart_->mmioActive_ = true; return finish(stopDppClockLocked(pipe, out));
}
IOReturn HackyNaviDisplayController::powerOnPipe(uint8_t pipe, const displaypower::Poll& poll,
    displaypower::Result& out) {
    out = displaypower::Result{}; out.status = displaypower::Status::InvalidArgument;
    if (pipe >= displaypower::kPipeCount || !validPoll(poll)) return kIOReturnBadArgument;
    lockOwners();
    if (state_ != State::Ready) return finish(kIOReturnNotReady);
    if (scanoutOwner_ && scanoutOwner_->requiresHardwareRetirement()) return finish(kIOReturnBusy);
    if (!providerReady()) return finish(fault());
    // Cold acquisition while no local output is active. A future multi-output
    // modeset owner can replace this with route-specific, shared-resource leases.
    for (uint8_t path = 0; path < paths_; ++path)
        if (links_[path].phase() != dio::Phase::Closed || streams_[path].requiresHardwareRetirement())
            return finish(kIOReturnBusy);
    gart_->mmioActive_ = true;
    const displaypower::Io io = {this, read32, write32, delayUs};
    const auto status = pipePowers_[pipe].powerOn(powerLayout_, io, pipe, poll, out);
    if (status == displaypower::Status::Ok) return finish(kIOReturnSuccess);
    IOLog("HackyNavi: pipe %u power-on status=%u stage=%u register=0x%x\n",
        unsigned(pipe), unsigned(status), unsigned(out.stage), out.lastRegister);
    return finish(pipePowers_[pipe].faulted() || status == displaypower::Status::TransportError ||
        status == displaypower::Status::DeviceUnavailable ? fault() : kIOReturnBusy);
}
IOReturn HackyNaviDisplayController::powerOffPipeLocked(uint8_t pipe, const displaypower::Poll& poll,
    displaypower::Result& out) {
    out = displaypower::Result{};
    if (scanoutOwner_ && scanoutOwner_->requiresHardwareRetirement()) return kIOReturnBusy;
    if (!pipePowers_[pipe].requiresHardwareRetirement()) return kIOReturnSuccess;
    if (pipeClocks_[pipe].requiresHardwareRetirement()) return kIOReturnBusy;
    for (uint8_t path = 0; path < paths_; ++path)
        if (links_[path].phase() != dio::Phase::Closed || streams_[path].requiresHardwareRetirement())
            return kIOReturnBusy;
    const auto status = pipePowers_[pipe].powerOff(poll, out);
    if (status == displaypower::Status::Ok) return kIOReturnSuccess;
    IOLog("HackyNavi: pipe %u power-off status=%u stage=%u register=0x%x; owner retained\n",
        unsigned(pipe), unsigned(status), unsigned(out.stage), out.lastRegister);
    return pipePowers_[pipe].faulted() ? fault() : kIOReturnBusy;
}
IOReturn HackyNaviDisplayController::powerOffPipe(uint8_t pipe, const displaypower::Poll& poll,
    displaypower::Result& out) {
    out = displaypower::Result{}; out.status = displaypower::Status::InvalidArgument;
    if (pipe >= displaypower::kPipeCount || !validPoll(poll)) return kIOReturnBadArgument;
    lockOwners();
    if (state_ != State::Ready && state_ != State::FaultedRetained) return finish(kIOReturnNotReady);
    if (!providerReady(true)) return finish(fault());
    gart_->mmioActive_ = true; return finish(powerOffPipeLocked(pipe, poll, out));
}
void HackyNaviDisplayController::clearLinkMetadata(uint8_t path) {
    linkKinds_[path] = LinkKind::None;
    hdmiClocks_[path] = hdmiddc::Clock{}; hdmiScdc_[path] = false;
    hdmiEncoding_[path] = hdmitmds::Encoding::Rgb; streamPipe_[path] = 0xff;
}
bool HackyNaviDisplayController::route(uint8_t path, dmcub::Route& out) {
    out = dmcub::Route{}; IOLockLock(lock_);
    const bool valid = state_ != State::Retired && validPath(path);
    if (valid) out = routes_[path]; IOLockUnlock(lock_); return valid;
}
IOReturn HackyNaviDisplayController::beginPath(uint8_t path, uint8_t connector, bool retiring) {
    lockOwners();
    if (!validPath(path, connector)) return finish(kIOReturnBadArgument);
    if (state_ != State::Ready) return finish(kIOReturnNotReady);
    if (!providerReady(retiring)) return finish(fault());
    gart_->mmioActive_ = true; return kIOReturnSuccess;
}
IOReturn HackyNaviDisplayController::inboxStatus(dmcub::Status status) {
    if (status == dmcub::Status::Ok) return kIOReturnSuccess;
    return inbox_.needsRecovery() ? fault() : kIOReturnBadArgument;
}
IOReturn HackyNaviDisplayController::pixelClock(uint8_t path, const dmcub::PixelClock& clock,
    const dmcub::PollOptions& poll) {
    const auto result = beginPath(path); if (result != kIOReturnSuccess) return result;
    // Until the modeset owner tracks shared PLL leases, do not retune a clock
    // under any prepared/running stream, including a different connector.
    for (uint8_t i = 0; i < paths_; ++i)
        if (streams_[i].requiresHardwareRetirement()) return finish(kIOReturnBusy);
    return finish(inboxStatus(inbox_.pixelClock(routes_[path], clock, poll, inboxResult_)));
}
IOReturn HackyNaviDisplayController::setupEncoder(uint8_t path, const dmcub::Encoder& encoder,
    const dmcub::PollOptions& poll) {
    const auto result = beginPath(path); if (result != kIOReturnSuccess) return result;
    for (uint8_t i = 0; i < paths_; ++i)
        if (streams_[i].requiresHardwareRetirement() && links_[i].frontendIndex() == encoder.engine)
            return finish(kIOReturnBusy);
    return finish(inboxStatus(inbox_.setupEncoder(routes_[path], encoder, poll, inboxResult_)));
}
IOReturn HackyNaviDisplayController::transferAux(uint8_t path, const dpaux::Request* requests,
    size_t count, dpaux::Result* results) {
    if (!requests || !results || !count || count > 32 || requests[count - 1].mot) return kIOReturnBadArgument;
    for (size_t i = 0; i < count; ++i) results[i] = dpaux::Result{};
    const auto result = beginPath(path, 0x13); if (result != kIOReturnSuccess) return result;
    const dpaux::Io io = {this, read32, write32, delayUs}; dpaux::Result lifecycle{};
    auto status = aux_.open(auxLayout_, io, routes_[path].ddc, routes_[path].hpd, lifecycle);
    for (size_t i = 0; status == dpaux::Status::Ok && i < count; ++i)
        status = aux_.transferWithRetries(requests[i], results[i]);
    if (aux_.isOpen()) {
        const auto closed = aux_.close(lifecycle); if (closed != dpaux::Status::Ok) status = closed;
    }
    if (aux_.ownsPins()) return finish(fault());
    return finish(status == dpaux::Status::Ok ? kIOReturnSuccess : kIOReturnIOError);
}
IOReturn HackyNaviDisplayController::trainDp(uint8_t path, uint8_t frontend,
    const dptrain::Config& config, const dmcub::PollOptions& poll, dptrain::HardwareResult& out) {
    out = dptrain::HardwareResult{};
    out.training.status = dptrain::Status::InvalidArgument;
    if (frontend >= 5 || !validPoll(poll)) return kIOReturnBadArgument;
    const auto result = beginPath(path, 0x13);
    if (result != kIOReturnSuccess) {
        if (result != kIOReturnBadArgument) out.training.status = dptrain::Status::InvalidState;
        return result;
    }
    out.training.status = dptrain::Status::InvalidState;
    if (links_[path].phase() != dio::Phase::Closed || linkKinds_[path] != LinkKind::None) return finish(kIOReturnBusy);
    if (aux_.ownsPins() || ddc_.ownsController()) {
        out.training.recoveryRequired = true; return finish(fault());
    }
    const dpaux::Io auxIo = {this, read32, write32, delayUs};
    const dio::Io linkIo = {this, read32, write32, delayUs};
    dpaux::Result lifecycle{};
    const auto opened = aux_.open(auxLayout_, auxIo, routes_[path].ddc, routes_[path].hpd, lifecycle);
    out.auxStatus = opened; out.aux = lifecycle;
    out.training.status = dptrain::Status::AuxFailed;
    IOReturn operation = kIOReturnIOError;
    if (opened == dpaux::Status::Ok) {
        // Record ownership kind before the first possibly landed DIO write.
        linkKinds_[path] = LinkKind::Dp;
        const auto prepared = links_[path].prepare(dioLayout_, linkIo, routes_[path], frontend, config.lanes, linkResult_);
        out.sourceStatus = prepared; out.source = linkResult_;
        out.training.status = dptrain::Status::SourceFailed;
        if (prepared == dio::Status::Ok) {
            const dptrain::Delay delay = {this, delayUs};
            const auto trained = dptrain::train(aux_, links_[path], inbox_, config, poll, delay, out);
            if (trained == dptrain::Status::Ok) operation = kIOReturnSuccess;
            // A capabilities/preflight rejection changes no sink state and
            // leaves only our idle DIO routing prepared. Retire that routing.
            if (trained != dptrain::Status::Ok && links_[path].phase() == dio::Phase::Prepared) {
                out.sourceStatus = links_[path].disable(inbox_, poll, linkResult_);
                out.source = linkResult_;
            }
        }
    }
    if (aux_.isOpen()) {
        const auto closed = aux_.close(lifecycle);
        if (closed != dpaux::Status::Ok) {
            out.auxStatus = closed; out.aux = lifecycle;
            if (out.training.status == dptrain::Status::Ok) out.training.status = dptrain::Status::AuxFailed;
            operation = kIOReturnIOError;
        }
    }
    if (links_[path].phase() == dio::Phase::Closed) clearLinkMetadata(path);
    if (aux_.ownsPins() || links_[path].needsRecovery() || inbox_.needsRecovery()) {
        out.training.recoveryRequired = true;
        return finish(fault());
    }
    return finish(operation);
}
IOReturn HackyNaviDisplayController::disableDpLocked(uint8_t path, const dmcub::PollOptions& poll) {
    if (links_[path].phase() == dio::Phase::Closed) { clearLinkMetadata(path); return kIOReturnSuccess; }
    if (linkKinds_[path] != LinkKind::Dp || !validPath(path, 0x13)) return fault();
    if (links_[path].needsRecovery() || !inbox_.isOpen()) return fault();
    const dpaux::Io io = {this, read32, write32, delayUs}; dpaux::Result ar{};
    if (links_[path].phase() != dio::Phase::Prepared) {
        // Receiver power-down is attempted while the physical link exists.
        // A disconnected receiver cannot ACK; that alone cannot keep the
        // local transmitter or this driver's DMA allocations alive.
        if (!aux_.ownsPins() && !ddc_.ownsController() &&
            aux_.open(auxLayout_, io, routes_[path].ddc, routes_[path].hpd, ar) == dpaux::Status::Ok) {
            const dpaux::Request read = {dpaux::Operation::NativeRead, 0x600, 1, false, nullptr};
            if (aux_.transferWithRetries(read, ar) == dpaux::Status::Ok && ar.length == 1) {
                const uint8_t power = uint8_t((ar.data[0] & ~7U) | 2U);
                const dpaux::Request write = {dpaux::Operation::NativeWrite, 0x600, 1, false, &power};
                (void)aux_.transferWithRetries(write, ar);
            }
        }
        if (aux_.isOpen()) (void)aux_.close(ar);
    }
    // Even an AUX fault must not prevent stopping a still-usable transmitter.
    const auto disabled = links_[path].disable(inbox_, poll, linkResult_);
    if (links_[path].phase() == dio::Phase::Closed) clearLinkMetadata(path);
    if (disabled != dio::Status::Ok || aux_.ownsPins()) return fault();
    return kIOReturnSuccess;
}
IOReturn HackyNaviDisplayController::disableDp(uint8_t path, const dmcub::PollOptions& poll) {
    if (!validPoll(poll)) return kIOReturnBadArgument;
    const auto result = beginPath(path, 0x13, true); if (result != kIOReturnSuccess) return result;
    return finish(disableDpLocked(path, poll));
}
IOReturn HackyNaviDisplayController::enableHdmi(uint8_t path, uint8_t frontend,
    const hdmiddc::Clock& clock, const hdmitmds::Config& config,
    const dmcub::PollOptions& poll, hdmitmds::HardwareResult& out) {
    out = hdmitmds::HardwareResult{};
    out.link.status = hdmitmds::Status::InvalidArgument;
    if (frontend >= dio::kLinkCount || !validPoll(poll)) return kIOReturnBadArgument;
    const auto planned = hdmitmds::plan(config, out.link.configuration);
    out.link.status = planned;
    if (planned != hdmitmds::Status::Ok)
        return planned == hdmitmds::Status::InvalidArgument ? kIOReturnBadArgument : kIOReturnUnsupported;
    const auto result = beginPath(path, 0x0c);
    if (result != kIOReturnSuccess) {
        out.link.status = result == kIOReturnBadArgument ? hdmitmds::Status::InvalidArgument : hdmitmds::Status::InvalidState;
        return result;
    }
    out.link.status = hdmitmds::Status::InvalidState;
    if (links_[path].phase() != dio::Phase::Closed || linkKinds_[path] != LinkKind::None)
        return finish(kIOReturnBusy);
    if (aux_.ownsPins() || ddc_.ownsController()) {
        out.link.recoveryRequired = true; return finish(fault());
    }
    const hdmiddc::Io ddcIo = {this, read32, write32, delayUs};
    const dio::Io linkIo = {this, read32, write32, delayUs};
    const hdmitmds::Delay delay = {this, delayUs};
    hdmiddc::Result lifecycle{};
    const auto opened = ddc_.open(ddcLayout_, ddcIo, routes_[path].ddc, clock, lifecycle);
    out.ddcStatus = opened; out.ddc = lifecycle;
    out.link.status = hdmitmds::Status::ScdcFailed;
    IOReturn operation = kIOReturnIOError;
    if (opened == hdmiddc::Status::Ok) {
        // Clock is stored only after the real engine accepted it. It is reused
        // for teardown so no unverified reference clock is invented at stop.
        hdmiClocks_[path] = clock; hdmiScdc_[path] = config.sink.scdc;
        hdmiEncoding_[path] = config.encoding;
        linkKinds_[path] = LinkKind::Hdmi;
        const auto prepared = links_[path].prepare(dioLayout_, linkIo, routes_[path], frontend, 4, linkResult_);
        out.sourceStatus = prepared; out.source = linkResult_;
        out.link.status = hdmitmds::Status::SourceFailed;
        if (prepared == dio::Status::Ok) {
            const auto activated = hdmitmds::activate(ddc_, links_[path], inbox_, config, poll, delay, out);
            if (activated == hdmitmds::Status::Ok) operation = kIOReturnSuccess;
            // Early receiver admission rejection has not touched its SCDC
            // state; unwind our prepared DIO route without enabling a PHY.
            if (activated != hdmitmds::Status::Ok && links_[path].phase() == dio::Phase::Prepared) {
                out.link.cleanupAttempted = true;
                out.sourceStatus = links_[path].disable(inbox_, poll, linkResult_);
                out.source = linkResult_;
                out.link.sourceDisabled = out.sourceStatus == dio::Status::Ok;
            }
        }
    }
    if (ddc_.isOpen()) {
        const auto closed = ddc_.close(lifecycle);
        if (closed != hdmiddc::Status::Ok) {
            out.ddcStatus = closed; out.ddc = lifecycle;
            if (out.link.status == hdmitmds::Status::Ok) out.link.status = hdmitmds::Status::ScdcFailed;
            operation = kIOReturnIOError;
        }
    }
    if (operation != kIOReturnSuccess) {
        out.link.activated = false;
        // GPIO release can fail after a successful PHY enable. Still stop the
        // local source while its firmware transport is usable, then retain the
        // Display owner if the shared I2C controller/pins remain ambiguous.
        if (links_[path].phase() != dio::Phase::Closed && !links_[path].needsRecovery() && inbox_.isOpen()) {
            out.link.cleanupAttempted = true;
            out.sourceStatus = links_[path].disable(inbox_, poll, linkResult_);
            out.source = linkResult_;
            out.link.sourceDisabled = out.sourceStatus == dio::Status::Ok;
        }
    }
    if (links_[path].phase() == dio::Phase::Closed) clearLinkMetadata(path);
    if (ddc_.ownsController() || aux_.ownsPins() || links_[path].needsRecovery() || inbox_.needsRecovery()) {
        out.link.activated = false; out.link.recoveryRequired = true;
        IOLog("HackyNavi: HDMI path=%u stage=%u status=%s DDC=%s source=%s; owners retained\n",
            unsigned(path), unsigned(out.link.stage), hdmitmds::statusString(out.link.status),
            hdmiddc::statusString(out.ddcStatus), dio::statusString(out.sourceStatus));
        return finish(fault());
    }
    return finish(operation);
}
IOReturn HackyNaviDisplayController::disableHdmiLocked(uint8_t path, const dmcub::PollOptions& poll,
    hdmitmds::HardwareResult& out) {
    out = hdmitmds::HardwareResult{};
    hdmistream::Result streamResult{};
    if (stopHdmiStreamLocked(path, poll, streamResult) != kIOReturnSuccess) {
        out.link.status = hdmitmds::Status::SourceFailed; out.link.recoveryRequired = true;
        return fault();
    }
    if (links_[path].phase() == dio::Phase::Closed) {
        clearLinkMetadata(path); out.link.sourceDisabled = true; out.link.stage = hdmitmds::Stage::Complete;
        return kIOReturnSuccess;
    }
    if (linkKinds_[path] != LinkKind::Hdmi || !validPath(path, 0x0c) ||
        links_[path].needsRecovery() || !inbox_.isOpen()) {
        out.link.status = hdmitmds::Status::InvalidState; out.link.recoveryRequired = true; return fault();
    }
    hdmiddc::Result lifecycle{};
    const hdmiddc::Io io = {this, read32, write32, delayUs};
    const hdmitmds::Delay delay = {this, delayUs};
    // A faulted AUX/DDC owner may still hold the shared GPIO route. Do not
    // steal/reset it to reach a sink; local PHY retirement is independent.
    hdmiddc::Status opened = hdmiddc::Status::Closed;
    if (hdmiScdc_[path] && !aux_.ownsPins() && !ddc_.ownsController())
        opened = ddc_.open(ddcLayout_, io, routes_[path].ddc, hdmiClocks_[path], lifecycle);
    if (ddc_.isOpen() && ddc_.portIndex() != routes_[path].ddc) {
        out.link.stage = hdmitmds::Stage::SourceDisable;
        out.sourceStatus = links_[path].disable(inbox_, poll, out.source);
        out.link.sourceDisabled = out.sourceStatus == dio::Status::Ok;
        out.link.status = out.link.sourceDisabled ? hdmitmds::Status::ScdcFailed : hdmitmds::Status::SourceFailed;
    } else {
        (void)hdmitmds::deactivate(ddc_, links_[path], inbox_, hdmiScdc_[path], poll, delay, out);
    }
    if (opened != hdmiddc::Status::Ok && hdmiScdc_[path] && !ddc_.isOpen()) {
        out.ddcStatus = opened; out.ddc = lifecycle;
    }
    if (ddc_.isOpen()) {
        const auto closed = ddc_.close(lifecycle);
        if (closed != hdmiddc::Status::Ok) {
            out.ddcStatus = closed; out.ddc = lifecycle;
            if (out.link.status == hdmitmds::Status::Ok) out.link.status = hdmitmds::Status::ScdcFailed;
        }
    }
    const bool retired = links_[path].phase() == dio::Phase::Closed;
    if (retired) clearLinkMetadata(path);
    if (!retired || aux_.ownsPins() || ddc_.ownsController() || inbox_.needsRecovery()) {
        out.link.recoveryRequired = true;
        IOLog("HackyNavi: HDMI retirement path=%u status=%s DDC=%s source=%s; owners retained\n",
            unsigned(path), hdmitmds::statusString(out.link.status), hdmiddc::statusString(out.ddcStatus),
            dio::statusString(out.sourceStatus));
        return fault();
    }
    // A receiver NACK or clean DDC-open rejection cannot keep local DMA alive
    // after verified source retirement and GPIO release. Keep its wire error
    // in out for the caller, but allow the ordered device stop to continue.
    out.link.recoveryRequired = false;
    return kIOReturnSuccess;
}
IOReturn HackyNaviDisplayController::disableHdmi(uint8_t path, const dmcub::PollOptions& poll,
    hdmitmds::HardwareResult& out) {
    out = hdmitmds::HardwareResult{}; out.link.status = hdmitmds::Status::InvalidArgument;
    if (!validPoll(poll)) return kIOReturnBadArgument;
    const auto result = beginPath(path, 0x0c, true);
    if (result != kIOReturnSuccess) {
        if (result != kIOReturnBadArgument) out.link.status = hdmitmds::Status::InvalidState;
        return result;
    }
    const auto retired = disableHdmiLocked(path, poll, out);
    return finish(retired != kIOReturnSuccess ? retired :
        out.link.status == hdmitmds::Status::Ok ? kIOReturnSuccess : kIOReturnIOError);
}
IOReturn HackyNaviDisplayController::prepareHdmiStream(uint8_t path, uint8_t pipe,
    const avi::Config& aviConfig, const edid::Monitor& monitor, const dmcub::PollOptions& poll,
    hdmistream::Result& out) {
    out = hdmistream::Result{}; out.status = hdmistream::Status::InvalidArgument;
    if (pipe >= otg::kPipeCount || !validPoll(poll)) return kIOReturnBadArgument;
    const auto begun = beginPath(path, 0x0c); if (begun != kIOReturnSuccess) return begun;
    if (linkKinds_[path] != LinkKind::Hdmi || links_[path].phase() != dio::Phase::Enabled)
        return finish(kIOReturnNotReady);
    if (streams_[path].phase() != hdmistream::Phase::Closed && streams_[path].phase() != hdmistream::Phase::Stopped)
        return finish(kIOReturnBusy);
    const auto encoding = aviConfig.encoding == avi::Encoding::Rgb ? hdmitmds::Encoding::Rgb :
        aviConfig.encoding == avi::Encoding::YCbCr444 ? hdmitmds::Encoding::YCbCr444 : hdmitmds::Encoding::YCbCr420;
    if (encoding != hdmiEncoding_[path]) return finish(kIOReturnBadArgument);
    for (uint8_t i = 0; i < paths_; ++i) if (i != path && streams_[i].requiresHardwareRetirement() && streamPipe_[i] == pipe)
        return finish(kIOReturnBusy);
    const hdmistream::Config config = {links_[path].frontendIndex(), routes_[path].phy, pipe, aviConfig};
    const vpg::Io io = {this, read32, write32, delayUs}; const vpg::Poll bounds = {poll.maxPolls, poll.delayUs};
    const auto status = streams_[path].prepare(streamLayout_, io, config, monitor, bounds, out);
    if (streams_[path].requiresHardwareRetirement()) streamPipe_[path] = pipe;
    if (status == hdmistream::Status::Ok) return finish(kIOReturnSuccess);
    return finish(streams_[path].phase() == hdmistream::Phase::Faulted ? fault() : kIOReturnIOError);
}
IOReturn HackyNaviDisplayController::startHdmiStream(uint8_t path, const dmcub::PollOptions& poll,
    hdmistream::Result& out) {
    out = hdmistream::Result{}; out.status = hdmistream::Status::InvalidArgument;
    if (!validPoll(poll)) return kIOReturnBadArgument;
    const auto begun = beginPath(path, 0x0c); if (begun != kIOReturnSuccess) return begun;
    if (linkKinds_[path] != LinkKind::Hdmi || links_[path].phase() != dio::Phase::Enabled)
        return finish(kIOReturnNotReady);
    const vpg::Poll bounds = {poll.maxPolls, poll.delayUs};
    const auto status = streams_[path].start(bounds, out);
    if (status == hdmistream::Status::Ok) return finish(kIOReturnSuccess);
    return finish(streams_[path].phase() == hdmistream::Phase::Faulted ? fault() : kIOReturnIOError);
}
IOReturn HackyNaviDisplayController::stopHdmiStreamLocked(uint8_t path, const dmcub::PollOptions& poll,
    hdmistream::Result& out) {
    out = hdmistream::Result{};
    if (!streams_[path].requiresHardwareRetirement()) return kIOReturnSuccess;
    const vpg::Poll bounds = {poll.maxPolls, poll.delayUs};
    const auto status = streams_[path].stop(bounds, out);
    if (status != hdmistream::Status::Ok) {
        IOLog("HackyNavi: HDMI stream stop path=%u status=%u register=0x%x packet=%s; owners retained\n",
            unsigned(path), unsigned(status), out.lastRegister, vpg::statusString(out.packet.status));
        return fault();
    }
    streamPipe_[path] = 0xff; return kIOReturnSuccess;
}
IOReturn HackyNaviDisplayController::stopHdmiStream(uint8_t path, const dmcub::PollOptions& poll,
    hdmistream::Result& out) {
    out = hdmistream::Result{}; out.status = hdmistream::Status::InvalidArgument;
    if (!validPoll(poll)) return kIOReturnBadArgument;
    const auto begun = beginPath(path, 0x0c, true); if (begun != kIOReturnSuccess) return begun;
    return finish(stopHdmiStreamLocked(path, poll, out));
}
IOReturn HackyNaviDisplayController::hdmiOperation(uint8_t path, const hdmiddc::Clock& clock,
    const hdmiddc::Message* messages, size_t count, bool edid, uint8_t block, hdmiddc::Result& out) {
    out = hdmiddc::Result{};
    if (!edid && (!messages || !count || count > hdmiddc::kMaxMessages)) return kIOReturnBadArgument;
    const auto result = beginPath(path, 0x0c); if (result != kIOReturnSuccess) return result;
    const hdmiddc::Io io = {this, read32, write32, delayUs}; hdmiddc::Result lifecycle{};
    auto status = ddc_.open(ddcLayout_, io, routes_[path].ddc, clock, lifecycle);
    if (status == hdmiddc::Status::Ok) status = edid ? ddc_.readEdidBlock(block, out) : ddc_.transfer(messages, count, out);
    if (ddc_.isOpen()) {
        const auto closed = ddc_.close(lifecycle); if (closed != hdmiddc::Status::Ok) status = closed;
    }
    if (ddc_.ownsController()) return finish(fault());
    return finish(status == hdmiddc::Status::Ok ? kIOReturnSuccess : kIOReturnIOError);
}
IOReturn HackyNaviDisplayController::transferHdmi(uint8_t path, const hdmiddc::Clock& clock,
    const hdmiddc::Message* messages, size_t count, hdmiddc::Result& out) {
    return hdmiOperation(path, clock, messages, count, false, 0, out);
}
IOReturn HackyNaviDisplayController::readHdmiEdid(uint8_t path, const hdmiddc::Clock& clock,
    uint8_t block, hdmiddc::Result& out) { return hdmiOperation(path, clock, nullptr, 0, true, block, out); }
IOReturn HackyNaviDisplayController::readEdid(uint8_t path, const hdmiddc::Clock& clock,
    OSData*& raw, edid::Monitor& monitor, edidacq::HardwareResult& out) {
    out = edidacq::HardwareResult{}; out.capture.status = edidacq::Status::InvalidArgument;
    if (raw) return kIOReturnBadArgument;
    constexpr size_t capacity = edid::kBlockBytes * edid::kMaxBlocks;
    // The bounded64-mode Monitor alone exceeds two kernel-stack KiB. Keep it
    // with the owned capture bytes rather than on the sleepable worker stack.
    struct Scratch { uint8_t bytes[capacity]; edid::Monitor monitor; };
    auto* scratch = static_cast<Scratch*>(IOMalloc(sizeof(Scratch)));
    if (!scratch) return kIOReturnNoMemory;
    auto* bytes = scratch->bytes;
    auto& parsed = scratch->monitor;
    bzero(&parsed, sizeof(parsed));
    const auto started = beginPath(path);
    if (started != kIOReturnSuccess) {
        IOFree(scratch, sizeof(Scratch));
        if (started != kIOReturnBadArgument) out.capture.status = edidacq::Status::InvalidState;
        return started;
    }
    const uint8_t connector = uint8_t(routes_[path].connectorObject);
    if (connector != 0x0c && connector != 0x13) {
        IOFree(scratch, sizeof(Scratch)); return finish(kIOReturnUnsupported);
    }
    if (aux_.ownsPins() || ddc_.ownsController()) {
        out.capture.status = edidacq::Status::InvalidState;
        IOFree(scratch, sizeof(Scratch)); return finish(fault());
    }
    out.capture.status = edidacq::Status::TransportFailed;
    if (connector == 0x13) {
        const dpaux::Io io = {this, read32, write32, delayUs};
        out.auxStatus = aux_.open(auxLayout_, io, routes_[path].ddc, routes_[path].hpd, out.aux);
        if (out.auxStatus == dpaux::Status::Ok)
            (void)edidacq::capture(aux_, bytes, capacity, parsed, out);
        if (aux_.isOpen()) {
            dpaux::Result closeResult{};
            const auto closed = aux_.close(closeResult);
            if (closed != dpaux::Status::Ok) {
                out.auxStatus = closed; out.aux = closeResult;
                out.capture.status = edidacq::Status::TransportFailed;
                out.capture.stage = edidacq::Stage::ReleaseTransport;
                out.capture.complete = false;
            }
        }
    } else {
        const hdmiddc::Io io = {this, read32, write32, delayUs};
        out.ddcStatus = ddc_.open(ddcLayout_, io, routes_[path].ddc, clock, out.ddc);
        if (out.ddcStatus == hdmiddc::Status::Ok)
            (void)edidacq::capture(ddc_, bytes, capacity, parsed, out);
        if (ddc_.isOpen()) {
            hdmiddc::Result closeResult{};
            const auto closed = ddc_.close(closeResult);
            if (closed != hdmiddc::Status::Ok) {
                out.ddcStatus = closed; out.ddc = closeResult;
                out.capture.status = edidacq::Status::TransportFailed;
                out.capture.stage = edidacq::Stage::ReleaseTransport;
                out.capture.complete = false;
            }
        }
    }
    IOReturn result = kIOReturnIOError;
    if (aux_.ownsPins() || ddc_.ownsController()) {
        out.capture.complete = false;
        IOLog("HackyNavi: EDID path=%u stage=%u status=%s AUX=%s DDC=%s; owners retained\n",
            unsigned(path), unsigned(out.capture.stage), edidacq::statusString(out.capture.status),
            dpaux::statusString(out.auxStatus), hdmiddc::statusString(out.ddcStatus));
        result = fault();
    } else if (out.capture.complete) {
        auto* data = OSData::withBytes(bytes, static_cast<unsigned>(out.capture.bytes));
        if (data) { raw = data; monitor = parsed; result = kIOReturnSuccess; }
        else { out.capture.complete = false; result = kIOReturnNoMemory; }
    }
    IOFree(scratch, sizeof(Scratch)); return finish(result);
}
IOReturn HackyNaviDisplayController::releaseLeaseLocked() {
    if (scanoutOwner_) return kIOReturnBusy;
    if (clockSmu_.minimumMayBeActive() || clockSmu_.policyMayBeActive() ||
        clockSmu_.faulted() || fams_.started) return kIOReturnBusy;
    for (size_t pipe = 0; pipe < displayclock::kPipeCount; ++pipe)
        if (pipeClocks_[pipe].requiresHardwareRetirement() || pipePowers_[pipe].requiresHardwareRetirement())
            return kIOReturnBusy;
    for (uint8_t path = 0; path < paths_; ++path)
        if (streams_[path].requiresHardwareRetirement()) return kIOReturnBusy;
    if (lease_) {
        if (gart_->displayOwner_ != this) return kIOReturnInternalError;
        gart_->displayOwner_ = nullptr; lease_ = false;
    }
    return kIOReturnSuccess;
}
void HackyNaviDisplayController::snapshotStopLocked(StopResult& out) {
    out.firmwareStopped = firmware_->displayFirmware_.phase() == dmcubfw::Phase::Stopped;
    out.runtimeRetained = firmware_->displayFirmware_.needsRuntimeRetention();
    out.policyRetained = clockSmu_.minimumMayBeActive() || clockSmu_.policyMayBeActive() ||
        clockSmu_.faulted() || fams_.started;
    out.leaseReleased = !lease_;
    out.firmware = firmware_->displayFirmwareResult_;
}
bool HackyNaviDisplayController::physicalStopRequiredLocked() const {
    if (fams_.started || clockSmu_.minimumMayBeActive() || clockSmu_.policyMayBeActive()) return true;
    for (size_t pipe = 0; pipe < displayclock::kPipeCount; ++pipe)
        if (pipeClocks_[pipe].requiresHardwareRetirement() || pipePowers_[pipe].requiresHardwareRetirement()) return true;
    // With only DMCUB runtime owned, inherited downstream OTG/MPC/DIO is
    // not its DMA client. Preserve the existing no-policy STOP contract.
    return false;
}
IOReturn HackyNaviDisplayController::verifyStoppedConsumersLocked(StopResult& out,bool physical) {
    // Re-read native ownership each time; an earlier successful consumer phase
    // is a report, never authority for later clock or firmware retirement.
    if (scanoutOwner_ || aux_.ownsPins() || ddc_.ownsController()) return kIOReturnBusy;
    for (uint8_t path = 0; path < paths_; ++path)
        if (links_[path].phase() != dio::Phase::Closed || streams_[path].requiresHardwareRetirement())
            return kIOReturnBusy;
    for (size_t pipe = 0; pipe < displayclock::kPipeCount; ++pipe)
        if (pipeClocks_[pipe].requiresHardwareRetirement() || pipePowers_[pipe].requiresHardwareRetirement())
            return kIOReturnBusy;
    if (physical) {
        ClockResult observed{};
        const auto result = checkClockIdleLocked(observed); out.idle = observed.idle;
        if (result != kIOReturnSuccess) return result;
        out.physicalIdleChecked = true;
    }
    out.consumersStopped = true; return kIOReturnSuccess;
}
IOReturn HackyNaviDisplayController::stopConsumersLocked(
    const dmcubfw::PollOptions& poll,StopResult& out,bool physical) {
    invalidateGlobalClockReferences();
    if (scanoutOwner_) return kIOReturnBusy;
    // A stream/surface has its own blank/drain and DMA retirement protocol.
    // Do not disable its link underneath it or fabricate its completion.
    for (uint8_t path = 0; path < paths_; ++path)
        if (streams_[path].requiresHardwareRetirement()) return kIOReturnBusy;
    if (hardwareHold_) {
        if (!providerReady(true)) return fault();
        gart_->mmioActive_ = true;
        const dmcub::PollOptions linkPoll = {poll.iterations, poll.intervalUs > 100 ? 100 : poll.intervalUs};
        dpaux::Result ar{}; hdmiddc::Result dr{};
        if (aux_.isOpen()) (void)aux_.close(ar);
        if (ddc_.isOpen()) (void)ddc_.close(dr);
        for (uint8_t path = 0; path < paths_; ++path) if (links_[path].phase() != dio::Phase::Closed) {
            IOReturn retired = kIOReturnUnsupported;
            if (linkKinds_[path] == LinkKind::Dp) retired = disableDpLocked(path, linkPoll);
            else if (linkKinds_[path] == LinkKind::Hdmi) {
                hdmitmds::HardwareResult hdmi{}; retired = disableHdmiLocked(path, linkPoll, hdmi);
            }
            if (retired != kIOReturnSuccess) return fault();
        }
        if (aux_.isOpen()) (void)aux_.close(ar);
        if (ddc_.isOpen()) (void)ddc_.close(dr);
        if (aux_.ownsPins() || ddc_.ownsController()) return fault();
        ClockResult clockResult{};
        for (uint8_t pipe = 0; pipe < displayclock::kPipeCount; ++pipe)
            if (stopDppClockLocked(pipe, clockResult) != kIOReturnSuccess) return fault();
        // Upstream DCN3.2 uses a 1us interval with 1000 polls for PGFSM.
        const displaypower::Poll powerPoll = {1000, 1}; displaypower::Result powerResult{};
        for (uint8_t pipe = 0; pipe < displaypower::kPipeCount; ++pipe)
            if (powerOffPipeLocked(pipe, powerPoll, powerResult) != kIOReturnSuccess) return fault();
    }
    return verifyStoppedConsumersLocked(out, physical && hardwareHold_);
}
IOReturn HackyNaviDisplayController::stopFirmwareLocked(const dmcubfw::PollOptions& poll,StopResult& out) {
    if (hardwareHold_) {
        if (!providerReady(true)) return fault();
        gart_->mmioActive_ = true;
        // The display lock excludes every local command producer. STOP_FW and
        // PWAIT/reset provide the retirement proof even for a timed-out inbox.
        if (firmware_->displayFirmware_.needsRuntimeRetention() &&
            dmcubfw::stop(firmwareIo(), poll, firmware_->displayFirmware_, firmware_->displayFirmwareResult_) != dmcubfw::Status::Ok)
            return fault();
        inbox_.clearAfterVerifiedReset();
        if (clockSmu_.minimumMayBeActive() || clockSmu_.policyMayBeActive()) {
            ClockResult clockResult{};
            if (checkClockIdleLocked(clockResult) != kIOReturnSuccess) return fault();
            const displaysmu::Poll clockPoll = {poll.iterations, poll.intervalUs};
            const auto released = clockSmu_.releaseMinimaAfterQuiescence(clockPoll, out.minima);
            if (released == displaysmu::Status::ResetRequired) {
                // Actual DMCUB runtime has stopped, but DAL/FAMS policy is an
                // independent retained owner. Neither zero requests nor a
                // local DMCUB reset is a full-device policy retirement proof.
                state_ = State::StoppedRetained; return kIOReturnBusy;
            }
            if (released != displaysmu::Status::Ok) return fault();
            dppReferenceKHz_ = dispReferenceKHz_ = 0;
        }
    }
    if (!dpmCaptureReady()) return fault();
    const auto result = releaseLeaseLocked();
    if (result != kIOReturnSuccess) {
        if (result == kIOReturnBusy && !firmware_->displayFirmware_.needsRuntimeRetention() &&
            (clockSmu_.minimumMayBeActive() || clockSmu_.policyMayBeActive() || fams_.started))
            state_ = State::StoppedRetained;
        return result;
    }
    state_ = State::Retired; return kIOReturnSuccess;
}
IOReturn HackyNaviDisplayController::finishScopedStop(StopResult& out,DpmCaptureResult& guard,IOReturn result) {
    if (!dpmCaptureReady() && hardwareHold_) (void)fault();
    if (guard.result != kIOReturnSuccess) result = guard.result;
    out.elapsedUs = guard.elapsedUs; snapshotStopLocked(out);
    const bool drop = result == kIOReturnSuccess && state_ == State::Retired && hardwareHold_;
    if (drop) hardwareHold_ = false;
    out.result = finishDpmCapture(result);
    if (drop) release();
    return out.result;
}
IOReturn HackyNaviDisplayController::stopConsumers(HackyNaviFunctionGroup::RetirementScope& scope,
    uint32_t timeoutUs,const dmcubfw::PollOptions& poll,StopResult& out) {
    out = {}; out.result = kIOReturnBadArgument; out.clockRetirementResult = kIOReturnNotReady;
    if (!validPoll(poll) || !timeoutUs || timeoutUs > 2000000) return out.result;
    if (!gart_ || !gart_->accessReady() || !scope.matchesGraphics(gart_->device_,gart_->openOwner_))
        return out.result = kIOReturnOffline;
    const auto started = timeUs(); lockOwners();
    DpmCaptureResult guard{}; guard.result = kIOReturnSuccess;
    dpmScope_ = nullptr; dpmRetirementScope_ = &scope; dpmReport_ = &guard;
    dpmStart_ = dpmLast_ = started; dpmBudget_ = timeoutUs;
    if (!dpmCaptureReady()) return finishScopedStop(out,guard,guard.result);
    if (state_ != State::Prepared && state_ != State::Ready && state_ != State::StoppedRetained && state_ != State::Retired)
        return finishScopedStop(out,guard,kIOReturnNotReady);
    invalidateDpmInventory();
    const auto result = stopConsumersLocked(poll,out,physicalStopRequiredLocked());
    return finishScopedStop(out,guard,result);
}
IOReturn HackyNaviDisplayController::stopFirmware(HackyNaviFunctionGroup::RetirementScope& scope,
    uint32_t timeoutUs,const dmcubfw::PollOptions& poll,StopResult& out) {
    out = {}; out.result = kIOReturnBadArgument; out.clockRetirementResult = kIOReturnNotReady;
    if (!validPoll(poll) || !timeoutUs || timeoutUs > 2000000) return out.result;
    if (!gart_ || !gart_->accessReady() || !scope.matchesGraphics(gart_->device_,gart_->openOwner_))
        return out.result = kIOReturnOffline;
    const auto started = timeUs(); lockOwners();
    DpmCaptureResult guard{}; guard.result = kIOReturnSuccess;
    dpmScope_ = nullptr; dpmRetirementScope_ = &scope; dpmReport_ = &guard;
    dpmStart_ = dpmLast_ = started; dpmBudget_ = timeoutUs;
    if (!dpmCaptureReady()) return finishScopedStop(out,guard,guard.result);
    if (state_ != State::Prepared && state_ != State::Ready && state_ != State::StoppedRetained && state_ != State::Retired)
        return finishScopedStop(out,guard,kIOReturnNotReady);
    if (gart_->queueOwner_ || gart_->hasVmOwners()) return finishScopedStop(out,guard,kIOReturnBusy);
    if (state_ != State::Retired && !providerReady(true)) return finishScopedStop(out,guard,kIOReturnNotReady);
    invalidateDpmInventory(); gart_->mmioActive_ = hardwareHold_;
    auto result = verifyStoppedConsumersLocked(out,hardwareHold_ && physicalStopRequiredLocked());
    if (result != kIOReturnSuccess) return finishScopedStop(out,guard,result);
    if (fams_.started) {
        // Keep Display locked across the real clock owner call, which acquires
        // Firmware -> GART itself. Clear only lexical guards before handing
        // those locks over; the original elapsed budget spans both phases.
        const auto observed = dpmLast_;
        dpmScope_ = nullptr; dpmRetirementScope_ = nullptr; dpmReport_ = nullptr; dpmBudget_ = 0;
        gart_->mmioActive_ = false;
        IOLockUnlock(gart_->lock_); IOLockUnlock(firmware_->lock_);
        HackyNaviFirmwareController::ClockResult clock{};
        out.coordinatedClockRetirementAttempted = true;
        out.clockRetirementResult = firmware_->retireClockStateInternal(scope,timeoutUs,clock,this,started);
        IOLockLock(firmware_->lock_); IOLockLock(gart_->lock_);
        dpmRetirementScope_ = &scope; dpmReport_ = &guard; dpmBudget_ = timeoutUs;
        dpmStart_ = started; dpmLast_ = observed;
        if (clock.elapsedUs > observed - started) dpmLast_ = started + clock.elapsedUs;
        if (!dpmCaptureReady()) return finishScopedStop(out,guard,guard.result);
        if (out.clockRetirementResult != kIOReturnSuccess)
            return finishScopedStop(out,guard,out.clockRetirementResult);
        gart_->mmioActive_ = hardwareHold_;
        result = verifyStoppedConsumersLocked(out,hardwareHold_ && physicalStopRequiredLocked());
        if (result != kIOReturnSuccess) return finishScopedStop(out,guard,result);
    }
    result = stopFirmwareLocked(poll,out);
    return finishScopedStop(out,guard,result);
}
IOReturn HackyNaviDisplayController::stop(const dmcubfw::PollOptions& poll) {
    if (!validPoll(poll)) return kIOReturnBadArgument;
    lockOwners();
    if (!gart_->accessReady()) return finish(kIOReturnOffline);
    // Coordinated policies need the genuine scoped path while DMCUB still
    // runs. Do not let this legacy convenience entry point destroy that
    // retirement prerequisite. The existing no-policy behavior is unchanged.
    if (fams_.started) return finish(kIOReturnNotPermitted);
    invalidateDpmInventory();
    if (state_ == State::Retired) return finish(kIOReturnSuccess);
    StopResult out{};
    auto result = stopConsumersLocked(poll,out,false);
    if (result == kIOReturnSuccess) result = stopFirmwareLocked(poll,out);
    const bool drop = result == kIOReturnSuccess && state_ == State::Retired && hardwareHold_;
    if (drop) hardwareHold_ = false;
    finish(result); if (drop) release(); return result;
}
IOReturn HackyNaviDisplayController::releaseAfterVerifiedReset(
    HackyNaviFunctionGroup::RetirementScope& scope,
    const HackyNaviResetController::RetirementProof& proof) {
    if (!gart_ || !gart_->retirementAccessReady(scope) ||
        !scope.matchesGraphics(gart_->device_, gart_->openOwner_)) return kIOReturnOffline;
    // Reset takes its own mutex. Validate before Display -> Firmware -> GART;
    // the outer retirement scope remains held throughout the entire transfer.
    const auto verified = proof.validateFor(gart_->device_, gart_->openOwner_, gart_->registerMap_);
    if (verified != kIOReturnSuccess) return verified;
    lockOwners();
    if (!gart_->retirementAccessReady(scope) ||
        !scope.matchesGraphics(gart_->device_, gart_->openOwner_)) return finish(kIOReturnOffline);
    // MODE1/ASIC_Init plus stopped memory clients is not a fresh observation
    // that an owned physical stream, link, clock or GPIO engine has retired.
    // Refuse before clearing ANY state: an ambiguous downstream owner must
    // retain every journal, the Display/GART lease and this hardware self-hold.
    if (scanoutOwner_ || aux_.ownsPins() || ddc_.ownsController()) return finish(kIOReturnBusy);
    for (uint8_t path = 0; path < paths_; ++path)
        if (links_[path].phase() != dio::Phase::Closed || streams_[path].requiresHardwareRetirement())
            return finish(kIOReturnBusy);
    for (size_t pipe = 0; pipe < displayclock::kPipeCount; ++pipe)
        if (pipeClocks_[pipe].requiresHardwareRetirement() || pipePowers_[pipe].requiresHardwareRetirement())
            return finish(kIOReturnBusy);
    invalidateDpmInventory();
    inbox_.clearAfterVerifiedReset(); aux_.clearAfterVerifiedReset(); ddc_.clearAfterVerifiedReset();
    clockSmu_.clearAfterVerifiedReset(); invalidateGlobalClockReferences();
    globalClockReferences_[0] = {}; globalClockReferences_[1] = {};
    memoryGeneration_ = 0;
    fams_ = {};
    for (size_t pipe = 0; pipe < displayclock::kPipeCount; ++pipe) {
        pipeClocks_[pipe].clearAfterVerifiedReset(); pipePowers_[pipe].clearAfterVerifiedReset();
    }
    for (uint8_t path = 0; path < paths_; ++path) {
        streams_[path].clearAfterVerifiedReset();
        links_[path].clearAfterVerifiedReset(); clearLinkMetadata(path);
    }
    const auto result = releaseLeaseLocked(); if (result != kIOReturnSuccess) return finish(result);
    state_ = State::Retired; const bool drop = hardwareHold_; hardwareHold_ = false;
    finish(kIOReturnSuccess); if (drop) release(); return kIOReturnSuccess;
}
void HackyNaviDisplayController::free() {
    // Published DMA references hold this object; free only before publication
    // or after successful stop / independently verified full device reset.
    if (gart_) {
        IOLockLock(gart_->lock_); (void)releaseLeaseLocked(); IOLockUnlock(gart_->lock_);
        gart_->release(); gart_ = nullptr;
    }
    if (firmware_) { firmware_->release(); firmware_ = nullptr; }
    if (rom_) { rom_->release(); rom_ = nullptr; }
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    OSObject::free();
}
