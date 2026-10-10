// SPDX-License-Identifier: MIT
#include "HackyNaviGARTController.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSData.h>

#if !defined(__x86_64__)
#error HackyNaviGARTController requires coherent x86_64 DMA and uncached MMIO
#endif

OSDefineMetaClassAndStructors(HackyNaviGARTController, OSObject)

using namespace hackynavi;
namespace {
constexpr uint64_t kPage = 4096;
constexpr uint64_t kMaxBar = 16U << 20;
constexpr uint32_t kIdentity = 0x744c1002;
bool validProvider(IOPCIDevice* device, IOService* owner, bool dmaRequired = false) {
    if (!device || !owner || device->isInactive() || !device->isOpen(owner) ||
        device->configRead32(0) != kIdentity) return false;
    const UInt16 command = device->configRead16(kIOPCIConfigCommand);
    if (command == 0xffff || !(command & 2) || (dmaRequired && !(command & 4))) return false;
    UInt8 offset = 0;
    if (device->findPCICapability(1, &offset)) {
        if (offset < 0x40 || offset > 0xf8 || (offset & 3)) return false;
        const UInt16 power = device->configRead16(offset + 4);
        if (power == 0xffff || (power & 3)) return false;
    }
    return true;
}
}

bool HackyNaviGARTController::init() {
    lock_ = nullptr; device_ = nullptr; openOwner_ = nullptr; registerMap_ = nullptr;
    registers_ = nullptr; registerBytes_ = 0;
    queueOwner_ = nullptr;
    firmwareOwner_ = nullptr; firmwareReady_ = false;
    displayOwner_ = nullptr; discovery_ = nullptr;
    mmioActive_ = hardwareHold_ = mmMayBeEnabled_ = gfxMayBeEnabled_ = false;
    state_ = State::Empty; config_ = Config{}; ipLayout_ = Navi31IpLayout{};
    hdpLayout_ = HdpLayout{}; table_ = VramAllocation{}; scratch_ = VramAllocation{};
    gartConfig_ = gart::Config{}; gartResult_ = gart::Result{};
    hdpResult_ = HdpResult{}; transferResult_ = VramTransferResult{};
    if (!OSObject::init()) return false;
    lock_ = IOLockAlloc();
    return lock_ != nullptr;
}

HackyNaviGARTController* HackyNaviGARTController::withDevice(IOPCIDevice* device,
    IOService* owner, IOMemoryMap* map, const uint8_t* discovery, size_t discoveryBytes,
    const Config& config, const VramRange* reservations, size_t count, IOReturn& result) {
    result = kIOReturnNoMemory;
    auto* controller = new HackyNaviGARTController;
    if (!controller) return nullptr;
    if (!controller->init()) { controller->release(); return nullptr; }
    result = controller->initialize(device, owner, map, discovery, discoveryBytes,
                                    config, reservations, count);
    if (result != kIOReturnSuccess) { controller->release(); return nullptr; }
    return controller;
}

IOReturn HackyNaviGARTController::initialize(IOPCIDevice* device, IOService* owner,
    IOMemoryMap* map, const uint8_t* discovery, size_t discoveryBytes,
    const Config& config, const VramRange* reservations, size_t count) {
    if (!discovery || !discoveryBytes || discoveryBytes > 65536 ||
        !validProvider(device, owner) || !map || !map->getVirtualAddress() ||
        (map->getVirtualAddress() & 3) || map->getLength() < 0x80000 ||
        map->getLength() > kMaxBar ||
        (map->getMapOptions() & kIOMapCacheMask) != kIOMapInhibitCache ||
        !config.poll.maxPolls || config.poll.maxPolls > 100000 ||
        !config.poll.delayUs || config.poll.delayUs > 10 ||
        (config.apertureStart & ((1ULL << 32) - 1)) ||
        config.apertureStart >= (1ULL << 47) ||
        config.storage.apertureBytes > (1ULL << 47) - config.apertureStart)
        return kIOReturnBadArgument;
    IODeviceMemory* assigned = device->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    const UInt32 bar4 = device->configRead32(kIOPCIConfigBaseAddress4);
    const UInt32 bar5 = device->configRead32(kIOPCIConfigBaseAddress5);
    if (!assigned || bar4 == UINT32_MAX || (!(bar4 & 1) && (bar4 & 6) == 4) ||
        bar5 == UINT32_MAX || (bar5 & 7) || !(bar5 & ~UInt32(15)) ||
        assigned->getPhysicalAddress() != (bar5 & ~UInt32(15)) ||
        map->getPhysicalAddress() != assigned->getPhysicalAddress() ||
        map->getLength() != assigned->getLength()) return kIOReturnUnsupported;
    IpLayoutDiagnostic diagnostic{};
    if (buildNavi31IpLayout(discovery, discoveryBytes, ipLayout_, diagnostic) != IpLayoutStatus::Ok ||
        buildHdpLayout(discovery, discoveryBytes, map->getLength(), hdpLayout_) != HdpStatus::Ok)
        return kIOReturnUnsupported;
    discovery_ = OSData::withBytes(discovery, static_cast<unsigned>(discoveryBytes));
    if (!discovery_) return kIOReturnNoMemory;
    if (allocator_.configure(config.memory, config.ownedArena, reservations, count) !=
        VramAllocatorStatus::Ok) return kIOReturnBadArgument;
    // Check complete MC placement before allocating any DMA storage.
    const uint64_t end = config.apertureStart + config.storage.apertureBytes;
    if (!config.storage.apertureBytes ||
        config.memory.mcWindowEndInclusive == UINT64_MAX ||
        (config.apertureStart <= config.memory.mcWindowEndInclusive &&
         config.memory.mcWindowStart < end)) return kIOReturnBadArgument;

    device_ = device; device_->retain();
    openOwner_ = owner; openOwner_->retain();
    registerMap_ = map; registerMap_->retain();
    registers_ = reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress());
    registerBytes_ = map->getLength(); config_ = config;
    IOReturn result = storage_.allocate(device_, openOwner_, config.storage);
    if (result != kIOReturnSuccess) return result;
    result = storage_.bind(HackyNaviGARTStorage::Region::Data, 1);
    if (result == kIOReturnSuccess)
        result = storage_.bind(HackyNaviGARTStorage::Region::Writeback,
            static_cast<size_t>(config.storage.dataBytes / kPage) + 3);
    if (result == kIOReturnSuccess)
        result = storage_.bind(HackyNaviGARTStorage::Region::Display,
            static_cast<size_t>((config.storage.dataBytes + config.storage.writebackBytes) / kPage) + 5);
    if (result != kIOReturnSuccess) return result;
    if (allocator_.allocate(storage_.tableByteCount(), kPage, table_) != VramAllocatorStatus::Ok ||
        allocator_.allocate(kPage, kPage, scratch_) != VramAllocatorStatus::Ok)
        return kIOReturnNoSpace;
    uint64_t unusedMc = 0, tablePhysical = 0, scratchPhysical = 0, fallbackDma = 0;
    if (allocator_.addresses(table_.handle, unusedMc, tablePhysical) != VramAllocatorStatus::Ok ||
        allocator_.addresses(scratch_.handle, unusedMc, scratchPhysical) != VramAllocatorStatus::Ok ||
        !storage_.dummyDmaAddress(fallbackDma)) return kIOReturnInternalError;
    gartConfig_ = {config.memory, tablePhysical, table_.range.bytes,
        config.apertureStart, config.storage.apertureBytes, scratchPhysical, fallbackDma, false};
    state_ = State::Prepared;
    return kIOReturnSuccess;
}

bool HackyNaviGARTController::ioReady(uint32_t offset) const {
    return mmioActive_ && device_ && openOwner_ && !device_->isInactive() &&
        device_->isOpen(openOwner_) && registers_ && !(offset & 3) &&
        registerBytes_ >= 4 && offset <= registerBytes_ - 4;
}
bool HackyNaviGARTController::read32(void* context, uint32_t offset, uint32_t& value) {
    auto* self = static_cast<HackyNaviGARTController*>(context);
    if (!self->ioReady(offset)) return false;
    __asm__ __volatile__("" ::: "memory");
    value = self->registers_[offset / 4];
    OSSynchronizeIO();
    __asm__ __volatile__("" ::: "memory");
    return true;
}
bool HackyNaviGARTController::write32(void* context, uint32_t offset, uint32_t value) {
    auto* self = static_cast<HackyNaviGARTController*>(context);
    if (!self->ioReady(offset)) return false;
    __asm__ __volatile__("" ::: "memory");
    self->registers_[offset / 4] = value;
    OSSynchronizeIO();
    __asm__ __volatile__("" ::: "memory");
    return true;
}
void HackyNaviGARTController::delayUs(void*, uint32_t us) { IODelay(us); }
bool HackyNaviGARTController::hdpFlush(void* context) {
    auto* self = static_cast<HackyNaviGARTController*>(context);
    return flushHdp(self->hdpIo(), self->hdpState_, self->hdpResult_) == HdpStatus::Ok;
}
gart::Io HackyNaviGARTController::gartIo() { return {this, read32, write32, delayUs, hdpFlush}; }
HdpIo HackyNaviGARTController::hdpIo() { return {this, read32, write32}; }
VramTransferIo HackyNaviGARTController::transferIo() { return {this, registerBytes_, read32, write32}; }

IOReturn HackyNaviGARTController::endOperation(IOReturn result) {
    mmioActive_ = false; IOLockUnlock(lock_); return result;
}
void HackyNaviGARTController::holdHardware() {
    if (!hardwareHold_) { retain(); hardwareHold_ = true; }
}
IOReturn HackyNaviGARTController::fault() {
    state_ = State::FaultedRetained;
    return endOperation(kIOReturnIOError);
}

IOReturn HackyNaviGARTController::upload() {
    IOLockLock(lock_);
    if (state_ != State::Prepared) return endOperation(kIOReturnNotReady);
    if (!validProvider(device_, openOwner_)) return endOperation(kIOReturnOffline);
    IOReturn result = storage_.synchronizeForDevice(HackyNaviGARTStorage::Region::Data);
    if (result == kIOReturnSuccess)
        result = storage_.synchronizeForDevice(HackyNaviGARTStorage::Region::Writeback);
    if (result == kIOReturnSuccess)
        result = storage_.synchronizeForDevice(HackyNaviGARTStorage::Region::Display);
    if (result != kIOReturnSuccess) return endOperation(result);
    auto* zero = static_cast<uint8_t*>(IOMalloc(kPage));
    if (!zero) return endOperation(kIOReturnNoMemory);
    bzero(zero, kPage); mmioActive_ = true;
    auto transferred = uploadVram(allocator_, scratch_.handle, 0, zero, kPage,
                                   transferIo(), transferResult_);
    IOFree(zero, kPage);
    if (transferred == VramTransferStatus::Ok)
        transferred = uploadVram(allocator_, table_.handle, 0, storage_.tableBytes(),
                                  storage_.tableByteCount(), transferIo(), transferResult_);
    // No hub has received these addresses yet. A failed upload stays Prepared
    // so a full rewrite or ordinary pre-publication cleanup remains possible.
    if (transferred != VramTransferStatus::Ok) return endOperation(kIOReturnIOError);
    state_ = State::Uploaded;
    return endOperation(kIOReturnSuccess);
}

IOReturn HackyNaviGARTController::enableMmhub() { return enable(gart::Hub::Mm); }
IOReturn HackyNaviGARTController::enableGfxhub() { return enable(gart::Hub::Gfx); }
IOReturn HackyNaviGARTController::enable(gart::Hub hub) {
    IOLockLock(lock_);
    if (hub == gart::Hub::Gfx && firmwareOwner_ && !firmwareReady_)
        return endOperation(kIOReturnNotReady);
    if ((hub == gart::Hub::Mm && state_ != State::Uploaded) ||
        (hub == gart::Hub::Gfx && state_ != State::MmEnabled))
        return endOperation(kIOReturnNotReady);
    if (!validProvider(device_, openOwner_, true))
        return hardwareHold_ ? fault() : endOperation(kIOReturnOffline);
    mmioActive_ = true;
    gart::Plan plan;
    if (gart::prepareEnable(ipLayout_, registerBytes_, hub, gartConfig_, gartIo(),
                            plan, gartResult_) != gart::Status::Ok)
        return hardwareHold_ ? fault() : endOperation(kIOReturnIOError);
    // Pin BEFORE any callback that can change programming, including callbacks
    // returning false after a write reached hardware.
    holdHardware();
    if (hub == gart::Hub::Mm &&
        configureHdp(hdpLayout_, hdpIo(), hdpState_, hdpResult_) != HdpStatus::Ok)
        return fault();
    // Table data must be visible before publishing its address to either hub.
    if (!hdpFlush(this)) return fault();
    if (hub == gart::Hub::Mm) mmMayBeEnabled_ = true; else gfxMayBeEnabled_ = true;
    if (gart::executePlan(plan, gartIo(), config_.poll, gartResult_) != gart::Status::Ok)
        return fault();
    state_ = hub == gart::Hub::Mm ? State::MmEnabled : State::BothEnabled;
    return endOperation(kIOReturnSuccess);
}

bool HackyNaviGARTController::invalidate(gart::Hub hub) {
    gart::Plan plan;
    return gart::prepareFlush(ipLayout_, registerBytes_, hub, gartIo(), plan, gartResult_) ==
        gart::Status::Ok && gart::executePlan(plan, gartIo(), config_.poll, gartResult_) == gart::Status::Ok;
}
IOReturn HackyNaviGARTController::flush() {
    IOLockLock(lock_);
    if (state_ != State::MmEnabled && state_ != State::BothEnabled)
        return endOperation(kIOReturnNotReady);
    if (!validProvider(device_, openOwner_, true)) return fault();
    mmioActive_ = true;
    if ((mmMayBeEnabled_ && !invalidate(gart::Hub::Mm)) ||
        (gfxMayBeEnabled_ && !invalidate(gart::Hub::Gfx))) return fault();
    return endOperation(kIOReturnSuccess);
}
bool HackyNaviGARTController::disable(gart::Hub hub) {
    gart::Plan plan;
    return gart::prepareDisable(ipLayout_, registerBytes_, hub, gartIo(), plan, gartResult_) ==
        gart::Status::Ok && gart::executePlan(plan, gartIo(), config_.poll, gartResult_) == gart::Status::Ok;
}
IOReturn HackyNaviGARTController::retireAfterQuiescence() {
    IOLockLock(lock_);
    if (queueOwner_ || firmwareOwner_ || displayOwner_) return endOperation(kIOReturnBusy);
    if (state_ == State::Retired) return endOperation(kIOReturnSuccess);
    if (state_ == State::Empty || state_ == State::FaultedRetained)
        return endOperation(kIOReturnNotReady); // Failed hardware requires an independently verified reset.
    if ((mmMayBeEnabled_ || gfxMayBeEnabled_) && !validProvider(device_, openOwner_, true))
        return fault();
    mmioActive_ = true;
    if (gfxMayBeEnabled_) {
        if (!disable(gart::Hub::Gfx)) return fault();
        gfxMayBeEnabled_ = false;
    }
    if (mmMayBeEnabled_) {
        if (!disable(gart::Hub::Mm)) return fault();
        mmMayBeEnabled_ = false;
    }
    const IOReturn result = releaseAllocations();
    state_ = State::Retired;
    const bool dropHold = hardwareHold_; hardwareHold_ = false;
    endOperation(result);
    if (dropHold) release(); // May destroy this object; no access to members below.
    return result;
}
IOReturn HackyNaviGARTController::releaseAfterDeviceReset() {
    IOLockLock(lock_);
    if (queueOwner_ || firmwareOwner_ || displayOwner_) return endOperation(kIOReturnBusy);
    // The caller, not this bookkeeping path, performed and verified the reset.
    mmMayBeEnabled_ = gfxMayBeEnabled_ = false;
    hdpState_.clearAfterDeviceReset();
    const IOReturn result = releaseAllocations();
    state_ = State::Retired;
    const bool dropHold = hardwareHold_; hardwareHold_ = false;
    endOperation(result);
    if (dropHold) release();
    return result;
}

IOReturn HackyNaviGARTController::releaseAllocations() {
    IOReturn result = storage_.reset();
    if (scratch_.handle.owner) {
        if (allocator_.release(scratch_.handle) != VramAllocatorStatus::Ok && result == kIOReturnSuccess)
            result = kIOReturnInternalError;
        scratch_ = VramAllocation{};
    }
    if (table_.handle.owner) {
        if (allocator_.release(table_.handle) != VramAllocatorStatus::Ok && result == kIOReturnSuccess)
            result = kIOReturnInternalError;
        table_ = VramAllocation{};
    }
    return result;
}
const HackyNaviDMABuffer* HackyNaviGARTController::buffer(HackyNaviGARTStorage::Region region) const {
    if (state_ == State::FaultedRetained || state_ == State::Retired) return nullptr;
    return storage_.buffer(region);
}
IOReturn HackyNaviGARTController::acquireQueueStorage(OSObject* owner) {
    IOLockLock(lock_);
    if (!owner) return endOperation(kIOReturnBadArgument);
    if (queueOwner_) return endOperation(kIOReturnBusy);
    if (firmwareOwner_ && !firmwareReady_) return endOperation(kIOReturnNotReady);
    if (state_ != State::BothEnabled || !validProvider(device_, openOwner_, true))
        return endOperation(kIOReturnNotReady);
    queueOwner_ = owner;
    return endOperation(kIOReturnSuccess);
}
IOReturn HackyNaviGARTController::releaseQueueStorage(OSObject* owner) {
    IOLockLock(lock_);
    if (!owner || queueOwner_ != owner) return endOperation(kIOReturnBadArgument);
    queueOwner_ = nullptr;
    return endOperation(kIOReturnSuccess);
}
IOReturn HackyNaviGARTController::publishQueueWrites(OSObject* owner) {
    IOLockLock(lock_);
    if (!owner || queueOwner_ != owner || state_ != State::BothEnabled)
        return endOperation(kIOReturnNotReady);
    if (!validProvider(device_, openOwner_, true)) return fault();
    IOReturn result = storage_.synchronizeForDevice(HackyNaviGARTStorage::Region::Data);
    if (result == kIOReturnSuccess)
        result = storage_.synchronizeForDevice(HackyNaviGARTStorage::Region::Writeback);
    if (result != kIOReturnSuccess) return endOperation(result);
    mmioActive_ = true;
    if (!hdpFlush(this)) return fault();
    return endOperation(kIOReturnSuccess);
}
bool HackyNaviGARTController::gpuAddress(HackyNaviGARTStorage::Region region, uint64_t& out) const {
    out = 0;
    if (state_ != State::MmEnabled && state_ != State::BothEnabled) return false;
    const auto* binding = storage_.binding(region);
    if (!binding || !binding->bound) return false;
    out = config_.apertureStart + binding->firstPage * kPage;
    return true;
}
IOReturn HackyNaviGARTController::synchronizeForDevice(HackyNaviGARTStorage::Region region) {
    IOLockLock(lock_);
    if (state_ == State::FaultedRetained || state_ == State::Retired)
        return endOperation(kIOReturnNotReady);
    return endOperation(storage_.synchronizeForDevice(region));
}
IOReturn HackyNaviGARTController::synchronizeForCPU(HackyNaviGARTStorage::Region region) {
    IOLockLock(lock_);
    if (state_ == State::FaultedRetained || state_ == State::Retired)
        return endOperation(kIOReturnNotReady);
    return endOperation(storage_.synchronizeForCPU(region));
}
void HackyNaviGARTController::free() {
    // The self-retain prevents reaching free with published hardware references.
    // Factory failure and successful retirement are both safe cleanup paths.
    (void)releaseAllocations();
    registers_ = nullptr; registerBytes_ = 0;
    if (registerMap_) { registerMap_->release(); registerMap_ = nullptr; }
    if (discovery_) { discovery_->release(); discovery_ = nullptr; }
    if (device_) { device_->release(); device_ = nullptr; }
    if (openOwner_) { openOwner_->release(); openOwner_ = nullptr; }
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    OSObject::free();
}
