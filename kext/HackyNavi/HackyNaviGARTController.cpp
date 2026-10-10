// SPDX-License-Identifier: MIT
#include "HackyNaviGARTController.hpp"
#include "HackyNaviVMContext.hpp"
#include "HackyNaviPciAccess.hpp"
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
bool validProvider(IOPCIDevice* device, IOService* owner, bool dmaRequired = false) {
    return nativepci::ready(device, owner, dmaRequired ?
        nativepci::BusMaster::Enabled : nativepci::BusMaster::Any);
}
}

bool HackyNaviGARTController::init() {
    access_ = nullptr; accessThread_ = nullptr;
    lock_ = nullptr; device_ = nullptr; openOwner_ = nullptr; registerMap_ = nullptr;
    registers_ = nullptr; registerBytes_ = 0;
    queueOwner_ = nullptr;
    firmwareOwner_ = nullptr; firmwareReady_ = false;
    displayOwner_ = nullptr; discovery_ = nullptr;
    for (auto& owner : vmOwners_) owner = nullptr;
    vmAdmissionClosed_ = vmRetirementActive_ = false;
    vmRetirementFailure_ = kIOReturnSuccess;
    mmioActive_ = hardwareHold_ = mmMayBeEnabled_ = gfxMayBeEnabled_ = false;
    state_ = State::Empty; config_ = Config{}; ipLayout_ = Navi31IpLayout{};
    hdpLayout_ = HdpLayout{}; table_ = VramAllocation{}; scratch_ = VramAllocation{};
    gartConfig_ = gart::Config{}; gartResult_ = gart::Result{};
    hdpResult_ = HdpResult{}; transferResult_ = VramTransferResult{};
    ownerGeneration_ = colorTopologyGeneration_ = colorTopologyDeadline_ = 0;
    colorTopologyScope_ = nullptr; colorTopologyAttempted_ = colorTopologyValid_ = false;
    colorTopologyLayout_ = {}; colorTopologySnapshot_ = {}; colorTopologyTrace_ = {};
    colorTopologyStatus_ = colortopology::Status::InvalidArgument;
    graphicsTopologyGeneration_ = graphicsTopologyDeadline_ = 0; graphicsTopologyScope_ = nullptr;
    graphicsTopologyAttempted_ = graphicsTopologyValid_ = false; graphicsTopologyPhase_ = 0;
    graphicsTopologyMetadata_ = {}; graphicsTopologyLayout_ = {}; graphicsTopologySnapshot_ = {};
    graphicsTopologyTrace_ = {}; graphicsTopologyStatus_ = graphicstopology::Status::InvalidArgument;
    for (auto& snapshot : graphicsTopologyColor_) snapshot = {};
    for (auto& trace : graphicsTopologyColorTrace_) trace = {};
    if (!OSObject::init()) return false;
    lock_ = IOLockAlloc();
    return lock_ != nullptr;
}

HackyNaviGARTController::Access::Access(HackyNaviGARTController& gart,
    HackyNaviFunctionGroup::Operation& scope):gart_(nullptr),operation_(&scope),retirement_(nullptr),thread_(current_thread()) {
    if(scope.matchesGraphics(gart.device_,gart.openOwner_))bind(gart);
}
HackyNaviGARTController::Access::Access(HackyNaviGARTController& gart,
    HackyNaviFunctionGroup::RetirementScope& scope):gart_(nullptr),operation_(nullptr),retirement_(&scope),thread_(current_thread()) {
    if(scope.matchesGraphics(gart.device_,gart.openOwner_))bind(gart);
}
HackyNaviGARTController::Access::Access(HackyNaviGARTController* gart,
    HackyNaviFunctionGroup::RetirementScope& scope):gart_(nullptr),operation_(nullptr),retirement_(&scope),thread_(current_thread()) {
    if(gart&&scope.matchesGraphics(gart->device_,gart->openOwner_))bind(*gart);
}
void HackyNaviGARTController::Access::bind(HackyNaviGARTController& gart) {
    Access* expected=nullptr;
    if(!__atomic_compare_exchange_n(&gart.access_,&expected,this,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
    gart.retain();gart_=&gart;
    __atomic_store_n(&gart.accessThread_,thread_,__ATOMIC_RELEASE);
}
HackyNaviGARTController::Access::~Access() {
    if(!gart_)return;
    if(thread_!=current_thread()||__atomic_load_n(&gart_->access_,__ATOMIC_ACQUIRE)!=this)
        panic("HackyNavi: GART access scope ownership changed");
    auto* gart=gart_;
    __atomic_store_n(&gart->accessThread_,nullptr,__ATOMIC_RELEASE);
    __atomic_store_n(&gart->access_,nullptr,__ATOMIC_RELEASE);
    gart_=nullptr;operation_=nullptr;retirement_=nullptr;gart->release();
}
bool HackyNaviGARTController::Access::valid() const {
    return gart_&&thread_==current_thread()&&
        __atomic_load_n(&gart_->accessThread_,__ATOMIC_ACQUIRE)==thread_&&
        __atomic_load_n(&gart_->access_,__ATOMIC_ACQUIRE)==this&&
        (operation_?operation_->matchesGraphics(gart_->device_,gart_->openOwner_):
            retirement_&&retirement_->matchesGraphics(gart_->device_,gart_->openOwner_));
}
bool HackyNaviGARTController::Access::normal() const {return operation_&&valid();}
bool HackyNaviGARTController::Access::retiring(const HackyNaviFunctionGroup::RetirementScope& scope) const {
    return !operation_&&retirement_==&scope&&valid();
}
bool HackyNaviGARTController::accessReady() const {
    // Check the owner thread before dereferencing a lexical guard. A foreign
    // thread can observe revocation, but never races its destructor's stack.
    if(__atomic_load_n(&accessThread_,__ATOMIC_ACQUIRE)!=current_thread())return false;
    const auto* access=__atomic_load_n(&access_,__ATOMIC_ACQUIRE);return access&&access->valid();
}
bool HackyNaviGARTController::normalAccessReady() const {
    if(__atomic_load_n(&accessThread_,__ATOMIC_ACQUIRE)!=current_thread())return false;
    const auto* access=__atomic_load_n(&access_,__ATOMIC_ACQUIRE);return access&&access->normal();
}
bool HackyNaviGARTController::retirementAccessReady(const HackyNaviFunctionGroup::RetirementScope& scope) const {
    if(__atomic_load_n(&accessThread_,__ATOMIC_ACQUIRE)!=current_thread())return false;
    const auto* access=__atomic_load_n(&access_,__ATOMIC_ACQUIRE);return access&&access->retiring(scope);
}

HackyNaviGARTController* HackyNaviGARTController::withDevice(HackyNaviFunctionGroup::Operation& scope, IOPCIDevice* device,
    IOService* owner, IOMemoryMap* map, const uint8_t* discovery, size_t discoveryBytes,
    const Config& config, const VramRange* reservations, size_t count, IOReturn& result,
    IOReturn* factoryTeardownFailure) {
    result = kIOReturnNoMemory;
    if (factoryTeardownFailure) *factoryTeardownFailure = kIOReturnSuccess;
    auto* controller = new HackyNaviGARTController;
    if (!controller) return nullptr;
    if (!controller->init()) { controller->release(); return nullptr; }
    result = controller->initialize(scope, device, owner, map, discovery, discoveryBytes,
                                    config, reservations, count);
    if (result != kIOReturnSuccess) {
        // No address or owner escapes a failed factory. Unwind explicitly so
        // the parent receives DMA cleanup errors before the object disappears.
        IOLockLock(controller->lock_);
        const IOReturn teardown = controller->releaseAllocations();
        IOLockUnlock(controller->lock_);
        if (factoryTeardownFailure) *factoryTeardownFailure = teardown;
        controller->release(); return nullptr;
    }
    return controller;
}

IOReturn HackyNaviGARTController::initialize(HackyNaviFunctionGroup::Operation& scope, IOPCIDevice* device, IOService* owner,
    IOMemoryMap* map, const uint8_t* discovery, size_t discoveryBytes,
    const Config& config, const VramRange* reservations, size_t count) {
    if (!scope.matchesGraphics(device,owner))return kIOReturnOffline;
    ownerGeneration_ = scope.generation();
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
    device_ = device; device_->retain();
    openOwner_ = owner; openOwner_->retain();
    Access access(*this,scope);
    if(!normalAccessReady())return kIOReturnOffline;
    nativepci::AssignedMemory assigned(device, kIOPCIConfigBaseAddress5);
    if(!normalAccessReady())return kIOReturnOffline;
    const UInt32 bar4 = device->configRead32(kIOPCIConfigBaseAddress4);
    if(!normalAccessReady())return kIOReturnOffline;
    const UInt32 bar5 = device->configRead32(kIOPCIConfigBaseAddress5);
    if(!normalAccessReady())return kIOReturnOffline;
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

    registerMap_ = map; registerMap_->retain();
    registers_ = reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress());
    registerBytes_ = map->getLength(); config_ = config;
    if(!normalAccessReady())return kIOReturnOffline;
    IOReturn result = storage_.allocate(device_, openOwner_, config.storage);
    if (result != kIOReturnSuccess) return result;
    if(!normalAccessReady())return kIOReturnOffline;
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
    return normalAccessReady()?kIOReturnSuccess:kIOReturnOffline;
}

bool HackyNaviGARTController::ioReady(uint32_t offset) const {
    return accessReady() && vmRetirementFailure_ == kIOReturnSuccess && mmioActive_ && device_ && openOwner_ && !device_->isInactive() &&
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
    return self->accessReady();
}
bool HackyNaviGARTController::write32(void* context, uint32_t offset, uint32_t value) {
    auto* self = static_cast<HackyNaviGARTController*>(context);
    if (!self->ioReady(offset)) return false;
    __asm__ __volatile__("" ::: "memory");
    self->registers_[offset / 4] = value;
    OSSynchronizeIO();
    __asm__ __volatile__("" ::: "memory");
    return self->accessReady();
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
    if (!normalAccessReady()) return endOperation(kIOReturnOffline);
    if (state_ != State::Prepared) return endOperation(kIOReturnNotReady);
    if (!accessReady() || !validProvider(device_, openOwner_)) return endOperation(kIOReturnOffline);
    IOReturn result = synchronizeDeviceLocked(HackyNaviGARTStorage::Region::Data);
    if (result == kIOReturnSuccess)
        result = synchronizeDeviceLocked(HackyNaviGARTStorage::Region::Writeback);
    if (result == kIOReturnSuccess)
        result = synchronizeDeviceLocked(HackyNaviGARTStorage::Region::Display);
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
    if (!normalAccessReady()) return endOperation(kIOReturnOffline);
    if (hub == gart::Hub::Gfx && firmwareOwner_ && !firmwareReady_)
        return endOperation(kIOReturnNotReady);
    if ((hub == gart::Hub::Mm && state_ != State::Uploaded) ||
        (hub == gart::Hub::Gfx && state_ != State::MmEnabled))
        return endOperation(kIOReturnNotReady);
    // MMHUB installs the owned translation while PCI DMA is disabled. This
    // sequence, including HDP visibility and invalidate ACK, uses CPU MMIO;
    // it neither submits a DMA request nor depends on bus mastering. GFXHUB
    // comes after firmware startup and retains its BME-on requirement.
    if (!accessReady() || !validProvider(device_, openOwner_, hub == gart::Hub::Gfx) ||
        (hub == gart::Hub::Mm && (device_->configRead16(kIOPCIConfigCommand) & 4)) || !accessReady())
        return hardwareHold_ ? fault() : endOperation(kIOReturnOffline);
    mmioActive_ = true;
    auto& plan = gartPlan_;
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
    // The caller holds the exclusive PCI/power epoch across all callbacks.
    // A changed/denied command read still fails while retaining published
    // storage; only Engine may subsequently enable BME with real readback.
    if (!accessReady() || !validProvider(device_, openOwner_, hub == gart::Hub::Gfx) ||
        (hub == gart::Hub::Mm && (device_->configRead16(kIOPCIConfigCommand) & 4)) || !accessReady())
        return fault();
    state_ = hub == gart::Hub::Mm ? State::MmEnabled : State::BothEnabled;
    return endOperation(kIOReturnSuccess);
}

bool HackyNaviGARTController::invalidate(gart::Hub hub) {
    auto& plan = gartPlan_;
    return gart::prepareFlush(ipLayout_, registerBytes_, hub, gartIo(), plan, gartResult_) ==
        gart::Status::Ok && gart::executePlan(plan, gartIo(), config_.poll, gartResult_) == gart::Status::Ok;
}
IOReturn HackyNaviGARTController::flush() {
    IOLockLock(lock_);
    if (!normalAccessReady()) return endOperation(kIOReturnOffline);
    if (state_ != State::MmEnabled && state_ != State::BothEnabled)
        return endOperation(kIOReturnNotReady);
    if (!accessReady() || !validProvider(device_, openOwner_, true)) return fault();
    mmioActive_ = true;
    if ((mmMayBeEnabled_ && !invalidate(gart::Hub::Mm)) ||
        (gfxMayBeEnabled_ && !invalidate(gart::Hub::Gfx))) return fault();
    return endOperation(kIOReturnSuccess);
}
bool HackyNaviGARTController::disable(gart::Hub hub) {
    auto& plan = gartPlan_;
    return gart::prepareDisable(ipLayout_, registerBytes_, hub, gartIo(), plan, gartResult_) ==
        gart::Status::Ok && gart::executePlan(plan, gartIo(), config_.poll, gartResult_) == gart::Status::Ok;
}
IOReturn HackyNaviGARTController::retireAfterQuiescence() {
    IOLockLock(lock_);
    if (hardwareHold_ && !accessReady()) return endOperation(kIOReturnOffline);
    if (vmRetirementActive_) return endOperation(kIOReturnBusy);
    if (vmRetirementFailure_ != kIOReturnSuccess) return endOperation(vmRetirementFailure_);
    if (queueOwner_ || firmwareOwner_ || displayOwner_ || hasVmOwners()) return endOperation(kIOReturnBusy);
    if (state_ == State::Retired) return endOperation(kIOReturnSuccess);
    if (state_ == State::Empty || state_ == State::FaultedRetained)
        return endOperation(kIOReturnNotReady); // Failed hardware requires an independently verified reset.
    if ((mmMayBeEnabled_ || gfxMayBeEnabled_) && (!accessReady() || !validProvider(device_, openOwner_, true)))
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
    if (result != kIOReturnSuccess) return endOperation(result);
    state_ = State::Retired;
    const bool dropHold = hardwareHold_; hardwareHold_ = false;
    endOperation(result);
    if (dropHold) release(); // May destroy this object; no access to members below.
    return result;
}
IOReturn HackyNaviGARTController::releaseAfterDeviceReset() {
    IOLockLock(lock_);
    if (hardwareHold_ && !accessReady()) return endOperation(kIOReturnOffline);
    if (vmRetirementActive_) return endOperation(kIOReturnBusy);
    if (vmRetirementFailure_ != kIOReturnSuccess) return endOperation(vmRetirementFailure_);
    if (queueOwner_ || firmwareOwner_ || displayOwner_ || hasVmOwners()) return endOperation(kIOReturnBusy);
    // The caller, not this bookkeeping path, performed and verified the reset.
    mmMayBeEnabled_ = gfxMayBeEnabled_ = false;
    hdpState_.clearAfterDeviceReset();
    const IOReturn result = releaseAllocations();
    if (result != kIOReturnSuccess) return endOperation(result);
    state_ = State::Retired;
    const bool dropHold = hardwareHold_; hardwareHold_ = false;
    endOperation(result);
    if (dropHold) release();
    return result;
}

IOReturn HackyNaviGARTController::retireVmContextsAfterVerifiedReset(
    const HackyNaviResetController::RetirementProof& proof) {
    // Reset's mutex precedes GART. The caller's common retirement gate keeps
    // the proof live through this snapshot and every context's revalidation.
    if (!accessReady()) return kIOReturnOffline;
    const IOReturn verified = proof.validateFor(device_, openOwner_, registerMap_);
    if (!accessReady()) return kIOReturnOffline;
    if (verified != kIOReturnSuccess) return verified;
    HackyNaviVMContext* owners[7] = {};
    IOLockLock(lock_);
    if (vmRetirementActive_) return endOperation(kIOReturnBusy);
    if (vmRetirementFailure_ != kIOReturnSuccess) return endOperation(vmRetirementFailure_);
    vmAdmissionClosed_ = true; vmRetirementActive_ = true;
    for (unsigned i = 0; i < 7; ++i) {
        // Registry references are strong, including never-published contexts:
        // retaining under this lock cannot resurrect an object in free().
        owners[i] = vmOwners_[i];
        if (owners[i]) owners[i]->retain();
    }
    endOperation(kIOReturnSuccess);
    IOReturn result = kIOReturnSuccess;
    for (auto* owner : owners) {
        if (!owner) continue;
        // Each owner takes Reset -> GART itself and verifies the same device,
        // PCI owner and map. Never call this while holding the GART mutex.
        if (result == kIOReturnSuccess) result = owner->releaseAfterVerifiedReset(proof);
        owner->release(); // snapshot hold; destructor can take the GART mutex
    }
    IOLockLock(lock_);
    if (result == kIOReturnSuccess && hasVmOwners()) result = kIOReturnBusy;
    // DMA teardown failures are terminal for this epoch. A later empty
    // registry must not silently turn a failed teardown into success.
    latchVmStorageFailureLocked(result);
    vmRetirementActive_ = false;
    return endOperation(vmRetirementFailure_);
}

void HackyNaviGARTController::latchVmStorageFailureLocked(IOReturn result) {
    if (result == kIOReturnSuccess) return;
    if (vmRetirementFailure_ == kIOReturnSuccess) vmRetirementFailure_ = result;
    vmAdmissionClosed_ = true;
    // Deny every later hub/firmware/queue activation, not just VMID creation.
    // Preserve existing publication holds; this is no hardware-retirement
    // proof and must never release an exposed allocation or acknowledge PM.
    state_ = State::FaultedRetained;
    firmwareReady_ = false;
    mmioActive_ = false;
}

bool HackyNaviGARTController::hasVmOwners() const {
    for (auto* owner : vmOwners_) if (owner) return true;
    return false;
}
IOReturn HackyNaviGARTController::releaseAllocations() {
    if (hasVmOwners()) return kIOReturnBusy;
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
    // The DMA members preserve individual errors, but the controller's
    // Retired shortcut and an emptied VRAM ledger must preserve them too.
    // Keep its publication hold on failure until the outer owner handles the
    // terminal cleanup error; a second retirement call cannot report success.
    latchVmStorageFailureLocked(result);
    return vmRetirementFailure_;
}
const HackyNaviDMABuffer* HackyNaviGARTController::buffer(HackyNaviGARTStorage::Region region) const {
    if (!accessReady() || state_ == State::FaultedRetained || state_ == State::Retired) return nullptr;
    return storage_.buffer(region);
}
IOReturn HackyNaviGARTController::acquireQueueStorage(OSObject* owner) {
    IOLockLock(lock_);
    if (!normalAccessReady()) return endOperation(kIOReturnOffline);
    if (!owner) return endOperation(kIOReturnBadArgument);
    if (queueOwner_) return endOperation(kIOReturnBusy);
    if (firmwareOwner_ && !firmwareReady_) return endOperation(kIOReturnNotReady);
    if (state_ != State::BothEnabled || !accessReady() || !validProvider(device_, openOwner_, true))
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
    if (!normalAccessReady()) return endOperation(kIOReturnOffline);
    if (!owner || queueOwner_ != owner || state_ != State::BothEnabled)
        return endOperation(kIOReturnNotReady);
    if (!accessReady() || !validProvider(device_, openOwner_, true)) return fault();
    IOReturn result = synchronizeDeviceLocked(HackyNaviGARTStorage::Region::Data);
    if (result == kIOReturnSuccess)
        result = synchronizeDeviceLocked(HackyNaviGARTStorage::Region::Writeback);
    if (result != kIOReturnSuccess) return endOperation(result);
    mmioActive_ = true;
    if (!hdpFlush(this)) return fault();
    return endOperation(kIOReturnSuccess);
}
bool HackyNaviGARTController::gpuAddress(HackyNaviGARTStorage::Region region, uint64_t& out) const {
    out = 0;
    if (!accessReady() || (state_ != State::MmEnabled && state_ != State::BothEnabled)) return false;
    const auto* binding = storage_.binding(region);
    if (!binding || !binding->bound) return false;
    out = config_.apertureStart + binding->firstPage * kPage;
    return true;
}
IOReturn HackyNaviGARTController::synchronizeForDevice(HackyNaviGARTStorage::Region region) {
    IOLockLock(lock_);
    if (state_ == State::FaultedRetained || state_ == State::Retired)
        return endOperation(kIOReturnNotReady);
    return endOperation(synchronizeDeviceLocked(region));
}
IOReturn HackyNaviGARTController::synchronizeForCPU(HackyNaviGARTStorage::Region region) {
    IOLockLock(lock_);
    if (state_ == State::FaultedRetained || state_ == State::Retired)
        return endOperation(kIOReturnNotReady);
    return endOperation(synchronizeCpuLocked(region));
}
IOReturn HackyNaviGARTController::synchronizeDeviceBuffer(HackyNaviDMABuffer& buffer) {
    if (!accessReady()) return kIOReturnOffline;
    const IOReturn result=buffer.synchronizeForDevice();
    return accessReady()?result:kIOReturnOffline;
}
IOReturn HackyNaviGARTController::synchronizeCpuBuffer(HackyNaviDMABuffer& buffer) {
    if (!accessReady()) return kIOReturnOffline;
    const IOReturn result=buffer.synchronizeForCPU();
    return accessReady()?result:kIOReturnOffline;
}
IOReturn HackyNaviGARTController::synchronizeDeviceLocked(HackyNaviGARTStorage::Region region) {
    if (!accessReady()) return kIOReturnOffline;
    const IOReturn result=storage_.synchronizeForDevice(region);
    return accessReady()?result:kIOReturnOffline;
}
IOReturn HackyNaviGARTController::synchronizeCpuLocked(HackyNaviGARTStorage::Region region) {
    if (!accessReady()) return kIOReturnOffline;
    const IOReturn result=storage_.synchronizeForCPU(region);
    return accessReady()?result:kIOReturnOffline;
}
void HackyNaviGARTController::free() {
    // The self-retain prevents reaching free with published hardware references.
    // Factory failure and successful retirement are both safe cleanup paths.
    if (lock_) IOLockLock(lock_);
    (void)releaseAllocations();
    if (lock_) IOLockUnlock(lock_);
    registers_ = nullptr; registerBytes_ = 0;
    if (registerMap_) { registerMap_->release(); registerMap_ = nullptr; }
    if (discovery_) { discovery_->release(); discovery_ = nullptr; }
    if (device_) { device_->release(); device_ = nullptr; }
    if (openOwner_) { openOwner_->release(); openOwner_ = nullptr; }
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
    OSObject::free();
}
