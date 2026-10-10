// SPDX-License-Identifier: MIT
#include "HackyNaviDisplayController.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSData.h>
#if !defined(__x86_64__)
#error HackyNaviDisplayController requires coherent x86_64 DMA and uncached MMIO
#endif
OSDefineMetaClassAndStructors(HackyNaviDisplayController, OSObject)
using namespace hackynavi;
namespace {
using Region = HackyNaviGARTStorage::Region;
bool validPoll(const dmcubfw::PollOptions& p) {
    return p.iterations && p.iterations <= 1000000 && p.intervalUs && p.intervalUs <= 1000 &&
        uint64_t(p.iterations) * p.intervalUs <= 1000000;
}
bool validPoll(const dmcub::PollOptions& p) {
    return p.maxPolls && p.maxPolls <= 1000000 && p.delayUs && p.delayUs <= 100 &&
        uint64_t(p.maxPolls) * p.delayUs <= 2000000;
}
}
bool HackyNaviDisplayController::init() {
    lock_ = nullptr; firmware_ = nullptr; gart_ = nullptr; rom_ = nullptr;
    lease_ = hardwareHold_ = false; state_ = State::Empty; paths_ = 0;
    image_ = ImageInfo{}; memory_ = dmcubfw::Memory{};
    firmwareLayout_ = dmcubfw::Layout{}; inboxLayout_ = dmcub::Layout{};
    inboxResult_ = dmcub::Result{}; auxLayout_ = dpaux::Layout{}; ddcLayout_ = hdmiddc::Layout{};
    dioLayout_ = dio::Layout{}; linkResult_ = dio::Result{};
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
    if (gart_->displayOwner_ || !gart_->discovery_ || !firmware_->providerReady() ||
        !gart_->firmwareReady_ || firmware_->state_ != HackyNaviFirmwareController::State::FirmwareLoaded ||
        firmware_->displayFirmware_.phase() != dmcubfw::Phase::Loaded) return finish(kIOReturnNotReady);
    const uint32_t subsystem = gart_->device_->configRead32(0x2c);
    if (subsystem == UINT32_MAX || uint16_t(subsystem) != atom.subsystemVendorId ||
        uint16_t(subsystem >> 16) != atom.subsystemDeviceId) return finish(kIOReturnUnsupported);
    const auto* discovery = static_cast<const uint8_t*>(gart_->discovery_->getBytesNoCopy());
    const size_t discoveryBytes = gart_->discovery_->getLength();
    if (dmcubfw::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, firmwareLayout_) != dmcubfw::Status::Ok ||
        dmcub::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, inboxLayout_) != dmcub::Status::Ok ||
        dpaux::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, auxLayout_) != dpaux::Status::Ok ||
        hdmiddc::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, ddcLayout_) != hdmiddc::Status::Ok ||
        dio::buildLayout(discovery, discoveryBytes, gart_->registerBytes_, dioLayout_) != dio::Status::Ok)
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
    return lease_ && gart_->displayOwner_ == this && firmware_->providerReady() &&
        (firmware_->state_ == HackyNaviFirmwareController::State::FirmwareLoaded ||
         (retiring && firmware_->state_ == HackyNaviFirmwareController::State::Quiesced)) &&
        (retiring || gart_->firmwareReady_);
}
IOReturn HackyNaviDisplayController::fault() {
    state_ = State::FaultedRetained;
    IOLog("HackyNavi: display fault, DMUB stage=%u status=%s register=0x%x; owners retained\n",
        unsigned(firmware_->displayFirmwareResult_.stage), dmcubfw::statusString(firmware_->displayFirmwareResult_.status),
        firmware_->displayFirmwareResult_.lastRegister);
    return kIOReturnIOError;
}
bool HackyNaviDisplayController::read32(void* context, uint32_t offset, uint32_t& value) {
    return HackyNaviGARTController::read32(static_cast<HackyNaviDisplayController*>(context)->gart_, offset, value);
}
bool HackyNaviDisplayController::write32(void* context, uint32_t offset, uint32_t value) {
    return HackyNaviGARTController::write32(static_cast<HackyNaviDisplayController*>(context)->gart_, offset, value);
}
bool HackyNaviDisplayController::memoryRange(uint64_t address, size_t bytes) const {
    return memory_.cpu && bytes && address >= memory_.gpuMcAddress &&
        address - memory_.gpuMcAddress < memory_.bytes &&
        bytes <= memory_.bytes - size_t(address - memory_.gpuMcAddress);
}
bool HackyNaviDisplayController::readMemory(void* context, uint64_t address, uint8_t* data, size_t bytes) {
    auto* self = static_cast<HackyNaviDisplayController*>(context);
    if (!self->gart_->mmioActive_ || !data || !self->memoryRange(address, bytes) ||
        self->gart_->storage_.synchronizeForCPU(Region::Display) != kIOReturnSuccess) return false;
    memmove(data, self->memory_.cpu + size_t(address - self->memory_.gpuMcAddress), bytes); return true;
}
bool HackyNaviDisplayController::writeMemory(void* context, uint64_t address, const uint8_t* data, size_t bytes) {
    auto* self = static_cast<HackyNaviDisplayController*>(context);
    if (!self->gart_->mmioActive_ || !data || !self->memoryRange(address, bytes)) return false;
    memmove(self->memory_.cpu + size_t(address - self->memory_.gpuMcAddress), data, bytes); return true;
}
bool HackyNaviDisplayController::publish(void* context) {
    auto* self = static_cast<HackyNaviDisplayController*>(context);
    return self->gart_->storage_.synchronizeForDevice(Region::Display) == kIOReturnSuccess &&
        HackyNaviGARTController::hdpFlush(self->gart_);
}
void HackyNaviDisplayController::delayUs(void*, uint32_t us) {
    if (us >= 1000) IOSleep((us + 999) / 1000); else IODelay(us);
}
dmcubfw::Io HackyNaviDisplayController::firmwareIo() { return {this, read32, write32, publish, delayUs}; }
dmcub::Io HackyNaviDisplayController::inboxIo() { return {this, read32, write32, readMemory, writeMemory, publish, delayUs}; }
IOReturn HackyNaviDisplayController::boot(const dmcubfw::PollOptions& poll) {
    if (!validPoll(poll)) return kIOReturnBadArgument;
    lockOwners();
    if (state_ != State::Prepared) return finish(kIOReturnNotReady);
    if (!providerReady()) return finish(kIOReturnOffline);
    retain(); hardwareHold_ = true; gart_->mmioActive_ = true;
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
void HackyNaviDisplayController::clearLinkMetadata(uint8_t path) {
    linkKinds_[path] = LinkKind::None;
    hdmiClocks_[path] = hdmiddc::Clock{}; hdmiScdc_[path] = false;
}
bool HackyNaviDisplayController::route(uint8_t path, dmcub::Route& out) {
    out = dmcub::Route{}; IOLockLock(lock_);
    const bool valid = state_ != State::Retired && validPath(path);
    if (valid) out = routes_[path]; IOLockUnlock(lock_); return valid;
}
IOReturn HackyNaviDisplayController::beginPath(uint8_t path, uint8_t connector) {
    lockOwners();
    if (!validPath(path, connector)) return finish(kIOReturnBadArgument);
    if (state_ != State::Ready) return finish(kIOReturnNotReady);
    if (!providerReady()) return finish(fault());
    gart_->mmioActive_ = true; return kIOReturnSuccess;
}
IOReturn HackyNaviDisplayController::inboxStatus(dmcub::Status status) {
    if (status == dmcub::Status::Ok) return kIOReturnSuccess;
    return inbox_.needsRecovery() ? fault() : kIOReturnBadArgument;
}
IOReturn HackyNaviDisplayController::pixelClock(uint8_t path, const dmcub::PixelClock& clock,
    const dmcub::PollOptions& poll) {
    const auto result = beginPath(path); if (result != kIOReturnSuccess) return result;
    return finish(inboxStatus(inbox_.pixelClock(routes_[path], clock, poll, inboxResult_)));
}
IOReturn HackyNaviDisplayController::setupEncoder(uint8_t path, const dmcub::Encoder& encoder,
    const dmcub::PollOptions& poll) {
    const auto result = beginPath(path); if (result != kIOReturnSuccess) return result;
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
    const auto result = beginPath(path, 0x13); if (result != kIOReturnSuccess) return result;
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
    const auto result = beginPath(path, 0x0c);
    if (result != kIOReturnSuccess) {
        if (result != kIOReturnBadArgument) out.link.status = hdmitmds::Status::InvalidState;
        return result;
    }
    const auto retired = disableHdmiLocked(path, poll, out);
    return finish(retired != kIOReturnSuccess ? retired :
        out.link.status == hdmitmds::Status::Ok ? kIOReturnSuccess : kIOReturnIOError);
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
    auto* bytes = static_cast<uint8_t*>(IOMalloc(capacity));
    if (!bytes) return kIOReturnNoMemory;
    const auto started = beginPath(path);
    if (started != kIOReturnSuccess) {
        IOFree(bytes, capacity);
        if (started != kIOReturnBadArgument) out.capture.status = edidacq::Status::InvalidState;
        return started;
    }
    const uint8_t connector = uint8_t(routes_[path].connectorObject);
    if (connector != 0x0c && connector != 0x13) {
        IOFree(bytes, capacity); return finish(kIOReturnUnsupported);
    }
    if (aux_.ownsPins() || ddc_.ownsController()) {
        out.capture.status = edidacq::Status::InvalidState;
        IOFree(bytes, capacity); return finish(fault());
    }
    edid::Monitor parsed{};
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
    IOFree(bytes, capacity); return finish(result);
}
IOReturn HackyNaviDisplayController::releaseLeaseLocked() {
    if (lease_) {
        if (gart_->displayOwner_ != this) return kIOReturnInternalError;
        gart_->displayOwner_ = nullptr; lease_ = false;
    }
    return kIOReturnSuccess;
}
IOReturn HackyNaviDisplayController::stop(const dmcubfw::PollOptions& poll) {
    if (!validPoll(poll)) return kIOReturnBadArgument;
    lockOwners();
    if (state_ == State::Retired) return finish(kIOReturnSuccess);
    if (hardwareHold_) {
        if (!providerReady(true)) return finish(fault());
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
            if (retired != kIOReturnSuccess) return finish(fault());
        }
        if (aux_.isOpen()) (void)aux_.close(ar);
        if (ddc_.isOpen()) (void)ddc_.close(dr);
        if (aux_.ownsPins() || ddc_.ownsController()) return finish(fault());
        // The display lock excludes every local command producer. STOP_FW and
        // PWAIT/reset provide the retirement proof even for a timed-out inbox.
        if (firmware_->displayFirmware_.needsRuntimeRetention() &&
            dmcubfw::stop(firmwareIo(), poll, firmware_->displayFirmware_, firmware_->displayFirmwareResult_) != dmcubfw::Status::Ok)
            return finish(fault());
        inbox_.clearAfterVerifiedReset();
    }
    const auto result = releaseLeaseLocked(); if (result != kIOReturnSuccess) return finish(result);
    state_ = State::Retired; const bool drop = hardwareHold_; hardwareHold_ = false;
    finish(kIOReturnSuccess); if (drop) release(); return kIOReturnSuccess;
}
IOReturn HackyNaviDisplayController::releaseAfterDeviceReset() {
    lockOwners();
    inbox_.clearAfterVerifiedReset(); aux_.clearAfterVerifiedReset(); ddc_.clearAfterVerifiedReset();
    for (uint8_t path = 0; path < paths_; ++path) {
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
