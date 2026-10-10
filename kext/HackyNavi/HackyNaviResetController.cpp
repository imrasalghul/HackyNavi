// SPDX-License-Identifier: MIT
// Native binding to mode1_reset.cpp's pinned AMD protocol. PCI shadow semantics
// checked against Apple IOPCIFamily-681.140.3 IOPCIDevice::saveDeviceState,
// IOPCIBridge::saveDeviceState/setDevicePowerState/_restoreDeviceState.
#include "HackyNaviResetController.hpp"
#include "HackyNaviAtomController.hpp"
#include "HackyNaviPciAccess.hpp"
#include "HackyNaviRecoveryEvidence.hpp"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/debug.h>
#include <libkern/OSAtomic.h>
#include <libkern/libkern.h>
#if !defined(__x86_64__)
#error HackyNaviResetController requires native x86_64 ordered PCI MMIO
#endif
OSDefineMetaClassAndStructors(HackyNaviResetController,OSObject)
using namespace hackynavi;
namespace {
constexpr uint32_t kIdentity=0x744c1002;
// smu_v13_0_check_fw_status: MP1_Public | smnMP1_FIRMWARE_FLAGS.
constexpr uint32_t kSmuFirmwareFlags=0x03b00000U|0x03010024U;
constexpr uint16_t kBusMaster=4,kTransactionsPending=0x20;
constexpr uint8_t kControlOffsets[4]={8,16,40,48};
// Device-control FLR and link-control retrain are self-clearing requests.
constexpr uint16_t kControlMasks[4]={0x7fff,0xffdf,0xffff,0xffff};
}
bool HackyNaviResetController::init() {
    lock_=nullptr;device_=nullptr;openOwner_=nullptr;registerMap_=nullptr;
    access_=nullptr;accessThread_=nullptr;accessSerial_=proofAccessSerial_=0;
    registers_=nullptr;registerBytes_=retirementDeadline_=0;discoveryBytes_=romBytes_=nullptr;
    active_=prepared_=hardwareHold_=permanentShadow_=shadowSaved_=false;
    triggerAttempted_=responseAttempted_=blackout_=blackoutFinished_=false;
    state_=State::Empty;stage_=Stage::None;result_=kIOReturnSuccess;
    lastRegister_=lastValue_=scratch3_=scratch7_=debugMessage_=debugResponse_=0;
    discoveryStatus_=DeviceDiscoveryStatus::InvalidArgument;
    ipStatus_=IpLayoutStatus::InvalidLayout;firmwareStatus_=Status::NotFound;
    discovery_={};romLayout_={};rom_={};ip_={};hdp_={};nbio_={};psp_={};
    firmwareInfo_={};protocol_={};saved_={};observations_={};
    proof_.issuer_=this;retirement_={};retirementMemory_={};
    preparation_={};preparationEvidence_=nullptr;preparationRecording_=false;
    if(!OSObject::init())return false;
    lock_=IOLockAlloc();return lock_!=nullptr;
}
HackyNaviResetController::Access::Access(HackyNaviResetController* controller,
    HackyNaviFunctionGroup::Operation& scope):controller_(nullptr),operation_(&scope),retirement_(nullptr),
    device_(nullptr),owner_(nullptr),map_(nullptr),thread_(current_thread()),serial_(0) {bind(controller);}
HackyNaviResetController::Access::Access(HackyNaviResetController* controller,
    HackyNaviFunctionGroup::RetirementScope& scope):controller_(nullptr),operation_(nullptr),retirement_(&scope),
    device_(nullptr),owner_(nullptr),map_(nullptr),thread_(current_thread()),serial_(0) {bind(controller);}
void HackyNaviResetController::Access::bind(HackyNaviResetController* controller) {
    if(!controller||!controller->lock_)return;
    IOLockLock(controller->lock_);
    if(!controller->access_&&controller->accessSerial_!=UINT64_MAX&&
        controller->device_&&controller->openOwner_&&controller->registerMap_) {
        controller_=controller;device_=controller->device_;owner_=controller->openOwner_;map_=controller->registerMap_;
        serial_=++controller->accessSerial_;
        controller->retain();controller->access_=this;
        __atomic_store_n(&controller->accessThread_,thread_,__ATOMIC_RELEASE);
    }
    IOLockUnlock(controller->lock_);
}
HackyNaviResetController::Access::~Access() {
    if(!controller_)return;
    if(thread_!=current_thread())panic("HackyNavi: reset access scope changed thread");
    auto* controller=controller_;IOLockLock(controller->lock_);
    if(controller->access_!=this)panic("HackyNavi: reset access binding changed");
    __atomic_store_n(&controller->accessThread_,static_cast<thread_t>(nullptr),__ATOMIC_RELEASE);
    controller->access_=nullptr;controller_=nullptr;
    IOLockUnlock(controller->lock_);controller->release();
}
bool HackyNaviResetController::Access::valid() const {
    return controller_&&controller_->currentAccess()==this&&controller_->accessReady();
}
const HackyNaviResetController::Access* HackyNaviResetController::currentAccess() const {
    // A different thread never dereferences the stack pointer. The creating
    // thread cannot concurrently destroy its guard while making this call.
    if(__atomic_load_n(&accessThread_,__ATOMIC_ACQUIRE)!=current_thread())return nullptr;
    const auto* access=access_;
    return access&&access->controller_==this&&access->thread_==current_thread()&&
        access->serial_==accessSerial_&&matchesDevice(access->device_,access->owner_,access->map_)?access:nullptr;
}
bool HackyNaviResetController::accessReady() const {
    const auto* access=currentAccess();
    return access&&!blackout_&&(access->operation_?access->operation_->matchesGraphics(device_,openOwner_):
        access->retirement_&&access->retirement_->matchesGraphics(device_,openOwner_));
}
bool HackyNaviResetController::recoveryAccessReady() const {
    const auto* access=currentAccess();
    return access&&!blackout_&&access->retirement_&&access->retirement_->permitsPciRecovery()&&
        access->retirement_->matchesGraphics(device_,openOwner_);
}
bool HackyNaviResetController::blackoutAccessReady() const {
    const auto* access=currentAccess();
    // No live scope.valid()/matchesGraphics here: they inspect PCI while the
    // mandatory MODE1 blackout prohibits that access. The group supplies only
    // its atomic worker/thread/generation/deadline/recovery-reason predicate.
    return access&&access->retirement_&&access->retirement_->recoveryWindowOpen();
}
HackyNaviResetController* HackyNaviResetController::withDevice(IOPCIDevice* device,
    IOService* owner,IOMemoryMap* map,IOReturn& result) {
    result=kIOReturnNoMemory;auto* self=new HackyNaviResetController;
    if(!self)return nullptr;
    if(!self->init()) {self->release();return nullptr;}
    result=self->initialize(device,owner,map);
    if(result!=kIOReturnSuccess) {self->release();return nullptr;}
    return self;
}
IOReturn HackyNaviResetController::initialize(IOPCIDevice* device,IOService* owner,IOMemoryMap* map) {
    if(!device || !owner || !map || !map->getVirtualAddress() || (map->getVirtualAddress()&3) ||
        map->getLength()<0x80000 || map->getLength()>16U*1024*1024 ||
        (map->getMapOptions()&kIOMapCacheMask)!=kIOMapInhibitCache)return kIOReturnBadArgument;
    device_=device;device_->retain();openOwner_=owner;openOwner_->retain();
    // Factory/setup has no Access yet and performs identity/resource reads
    // only. All later hardware entry points require the real stack binding.
    if(!nativepci::ready(device_,openOwner_))return kIOReturnOffline;
    const UInt32 bar4=device_->configRead32(kIOPCIConfigBaseAddress4);
    const UInt32 bar5=device_->configRead32(kIOPCIConfigBaseAddress5);
    nativepci::AssignedMemory resource(device_,kIOPCIConfigBaseAddress5);
    if(!resource || bar4==UINT32_MAX || (!(bar4&1) && (bar4&6)==4) ||
        bar5==UINT32_MAX || (bar5&7) || !(bar5&~UInt32(15)) ||
        resource->getPhysicalAddress()!=(bar5&~UInt32(15)) ||
        map->getPhysicalAddress()!=resource->getPhysicalAddress() || map->getLength()!=resource->getLength())
        return kIOReturnUnsupported;
    registerMap_=map;map->retain();registerBytes_=map->getLength();
    registers_=reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress());
    discoveryBytes_=static_cast<uint8_t*>(IOMalloc(kDeviceDiscoveryMaxBytes));
    romBytes_=static_cast<uint8_t*>(IOMalloc(runtimerom::kRecommendedCapacity));
    if(!discoveryBytes_ || !romBytes_)return kIOReturnNoMemory;
    state_=State::Attached;return kIOReturnSuccess;
}
bool HackyNaviResetController::providerReady() const {
    if(!accessReady())return false;
    return nativepci::ready(device_,openOwner_,
        (state_==State::VerifyingRetirement || state_==State::RetirementVerified) ?
        nativepci::BusMaster::Disabled : nativepci::BusMaster::Any)&&accessReady();
}
bool HackyNaviResetController::validOffset(uint32_t offset) const {
    return registers_ && !(offset&3) && registerBytes_>=4 && offset<=registerBytes_-4;
}
bool HackyNaviResetController::retirementDeadlineReady() {
    if(state_!=State::VerifyingRetirement)return true;
    uint64_t now=0;clock_get_uptime(&now);
    if(now>=retirementDeadline_) {retirement_.deadlineExpired=true;return false;}
    return true;
}
bool HackyNaviResetController::read32(void* context,uint32_t offset,uint32_t& value) {
    auto* self=static_cast<HackyNaviResetController*>(context);
    self->lastRegister_=offset;
    if(!self->active_ || self->blackout_ || !self->validOffset(offset) ||
        !self->retirementDeadlineReady() || !self->providerReady())return false;
    __asm__ __volatile__("" ::: "memory");value=self->registers_[offset/4];
    OSSynchronizeIO();__asm__ __volatile__("" ::: "memory");self->lastValue_=value;
    return self->accessReady()&&self->retirementDeadlineReady();
}
bool HackyNaviResetController::write32(void* context,uint32_t offset,uint32_t value) {
    auto* self=static_cast<HackyNaviResetController*>(context);
    self->lastRegister_=offset;self->lastValue_=value;
    if(!self->active_ || !self->validOffset(offset) || !self->retirementDeadlineReady())return false;
    // The response write is the second half of the upstream trigger. It is
    // the sole allowed access after DEBUG_MSG until the 500ms wait completes.
    const bool response=self->blackout_ && self->triggerAttempted_ && !self->responseAttempted_ &&
        offset==self->debugResponse_ && value==0;
    if(self->blackout_ && !response)return false;
    if(response?!self->blackoutAccessReady():!self->providerReady())return false;
    if(self->state_==State::Attached) {
        if(offset!=0 && offset!=0x18 && offset!=self->romLayout_.romIndex &&
            offset!=self->romLayout_.rsmuIndex)return false;
    } else if(self->state_==State::Prepared) {
        if(offset!=self->nbio_.rsmuIndexRegister)return false;
    } else if((self->state_!=State::Resetting && self->state_!=State::VerifyingRetirement) || !self->hardwareHold_)return false;
    if(offset==self->debugMessage_ && self->state_==State::Resetting) {
        if(value!=2 || !self->shadowSaved_ || self->triggerAttempted_)return false;
        self->triggerAttempted_=self->blackout_=true;
    }
    if(response)self->responseAttempted_=true;
    __asm__ __volatile__("" ::: "memory");self->registers_[offset/4]=value;
    OSSynchronizeIO();__asm__ __volatile__("" ::: "memory");
    return self->blackout_?self->blackoutAccessReady():self->accessReady();
}
bool HackyNaviResetController::readPci32(void* context,uint32_t offset,uint32_t& value) {
    auto* self=static_cast<HackyNaviResetController*>(context);
    if(!self->active_ || self->blackout_ || (offset&3) || offset>0xfc ||
        !self->accessReady() || !self->device_ || self->device_->isInactive() ||
        !self->device_->isOpen(self->openOwner_))return false;
    value=self->device_->configRead32(UInt8(offset));return self->accessReady();
}
bool HackyNaviResetController::writePci16(void* context,uint32_t offset,uint16_t value) {
    auto* self=static_cast<HackyNaviResetController*>(context);
    if(!self->active_ || self->blackout_ || self->state_!=State::Resetting || !self->hardwareHold_ ||
        offset!=kIOPCIConfigCommand || (value&kBusMaster) || !self->recoveryAccessReady() ||
        !self->providerReady())return false;
    self->device_->configWrite16(kIOPCIConfigCommand,value);return self->recoveryAccessReady();
}
void HackyNaviResetController::delayMs(void* context,uint32_t ms) {
    auto* self=static_cast<HackyNaviResetController*>(context);
    // IOSleep is used in the required sleepable context. No config/MMIO, log
    // callbacks, provider checks or ACK polls occur during this interval.
    if(self->blackout_) {
        const bool admitted=self->blackoutAccessReady();
        // Even revocation cannot shorten the hardware's mandatory quiet time.
        // Leave blackout latched on failed authority so no later PCI access
        // or successful restore can be inferred from the void delay callback.
        IOSleep(ms);
        if(admitted&&ms>=500&&self->blackoutAccessReady()) {
            self->blackout_=false;self->blackoutFinished_=true;
        }
        return;
    }
    if(!self->active_||!self->accessReady())return;
    IOSleep(ms);
}
void HackyNaviResetController::delayUs(void* context,uint32_t us) {
    auto* self=static_cast<HackyNaviResetController*>(context);
    if(!self->active_||!self->accessReady()||!self->retirementDeadlineReady())return;
    if(us>=1000)IOSleep((us+999)/1000);else IODelay(us);
}
bool HackyNaviResetController::snapshotPci(PciSnapshot& out) const {
    out={};if(!providerReady())return false;
    out.identity=device_->configRead32(0);out.revisionClass=device_->configRead32(8);
    out.subsystem=device_->configRead32(0x2c);out.header=device_->configRead32(0x0c);
    out.command=device_->configRead16(kIOPCIConfigCommand);out.rom=device_->configRead32(0x30);
    if(out.identity!=kIdentity || out.subsystem==UINT32_MAX || out.header==UINT32_MAX ||
        out.rom==UINT32_MAX || (out.header&0x7f0000))return false;
    for(unsigned i=0;i<6;++i) {
        out.bars[i]=device_->configRead32(UInt8(0x10+i*4));
        if(out.bars[i]==UINT32_MAX)return false;
    }
    UInt16 pmHeader=0,expressHeader=0;
    if(!nativepci::readCapability(device_,1,out.pmOffset,pmHeader) || out.pmOffset>0xf8 ||
        !nativepci::readCapability(device_,0x10,out.expressOffset,expressHeader) || out.expressOffset>0xec)return false;
    out.expressCapabilities=device_->configRead16(out.expressOffset+2);
    const uint16_t version=out.expressCapabilities&15;
    if(out.expressCapabilities==0xffff || !version || version>2 ||
        (version==2 && out.expressOffset>0xc8))return false;
    out.controlCount=version==2?4:2;
    for(unsigned i=0;i<out.controlCount;++i) {
        out.controls[i]=device_->configRead16(UInt8(out.expressOffset+kControlOffsets[i]));
        if(out.controls[i]==0xffff)return false;
    }
    return device_->configRead16(out.pmOffset)==pmHeader &&
        device_->configRead16(out.expressOffset)==expressHeader &&
        device_->configRead16(out.expressOffset+2)==out.expressCapabilities &&
        device_->configRead32(0)==out.identity && device_->configRead32(8)==out.revisionClass &&
        device_->configRead32(0x2c)==out.subsystem &&
        device_->configRead8(kIOPCIConfigHeaderType)==UInt8(out.header>>16) &&
        device_->configRead16(kIOPCIConfigCommand)==out.command && providerReady();
}
bool HackyNaviResetController::verifyPci(const PciSnapshot& expected) const {
    PciSnapshot now={};if(!snapshotPci(now))return false;
    if(now.identity!=expected.identity || now.revisionClass!=expected.revisionClass ||
        now.subsystem!=expected.subsystem || ((now.header^expected.header)&0x00ff0000) ||
        now.command!=expected.command || now.rom!=expected.rom || now.pmOffset!=expected.pmOffset ||
        now.expressOffset!=expected.expressOffset || now.expressCapabilities!=expected.expressCapabilities ||
        now.controlCount!=expected.controlCount)return false;
    for(unsigned i=0;i<6;++i)if(now.bars[i]!=expected.bars[i])return false;
    for(unsigned i=0;i<now.controlCount;++i)
        if((now.controls[i]^expected.controls[i])&kControlMasks[i])return false;
    nativepci::AssignedMemory resource(device_,kIOPCIConfigBaseAddress5);
    return resource && registerMap_ && !(now.bars[5]&7) &&
        resource->getPhysicalAddress()==(now.bars[5]&~UInt32(15)) &&
        registerMap_->getPhysicalAddress()==resource->getPhysicalAddress() &&
        registerMap_->getLength()==resource->getLength();
}
bool HackyNaviResetController::savePciState(void* context) {
    auto* self=static_cast<HackyNaviResetController*>(context);
    if(self->blackout_ || !self->hardwareHold_ || self->shadowSaved_ ||
        !self->recoveryAccessReady() ||
        !self->snapshotPci(self->saved_) || (self->saved_.command&kBusMaster))return false;
    // Mark persistence before calling: a failing native call can still have
    // changed the shadow flags. Permanent forces recapture of a valid shadow
    // and prevents IOPCIBridge's ordinary save path from entering D3.
    self->permanentShadow_=true;
    if(!self->recoveryAccessReady() ||
        self->device_->saveDeviceState(kIOPCIConfigShadowPermanent)!=kIOReturnSuccess ||
        !self->recoveryAccessReady() ||
        !self->verifyPci(self->saved_))return false;
    self->shadowSaved_=true;return true;
}
bool HackyNaviResetController::restorePciState(void* context) {
    auto* self=static_cast<HackyNaviResetController*>(context);
    if(self->blackout_ || !self->blackoutFinished_ || !self->shadowSaved_ ||
        !self->recoveryAccessReady() ||
        (self->saved_.command&kBusMaster) || self->device_->isInactive() ||
        !self->device_->isOpen(self->openOwner_))return false;
    self->stage_=Stage::VerifyRestore;
    // The native bridge returns success even for certain inner restore
    // failures; independent identity/BAR/command/D0/control checks are vital.
    if(self->device_->restoreDeviceState()!=kIOReturnSuccess||!self->recoveryAccessReady())return false;
    return self->verifyPci(self->saved_);
}
bool HackyNaviResetController::readSmuFirmware(uint32_t& value) {
    uint32_t high=0,posting=0;
    if(!read32(this,nbio_.rsmuIndexHiRegister,high) || high ||
        !write32(this,nbio_.rsmuIndexRegister,kSmuFirmwareFlags) ||
        !read32(this,nbio_.rsmuIndexRegister,posting) || posting!=kSmuFirmwareFlags ||
        !read32(this,nbio_.rsmuDataRegister,value) || value==UINT32_MAX)return false;
    return true;
}
void HackyNaviResetController::holdHardware() {
    if(!hardwareHold_) {retain();hardwareHold_=true;}
}
IOReturn HackyNaviResetController::finish(IOReturn result) {
    if(preparationRecording_) {capturePreparationEvidence(result);preparationRecording_=false;}
    result_=result;active_=false;IOLockUnlock(lock_);return result;
}
void HackyNaviResetController::preparationStep(PreparationStep step) {
    preparation_.step=step;preparation_.attempted|=uint32_t(1)<<unsigned(step);
}
void HackyNaviResetController::preparationPassed() {
    preparation_.completed|=uint32_t(1)<<unsigned(preparation_.step);
}
OSDictionary* HackyNaviResetController::copyPreparationEvidence() {
    IOLockLock(lock_);auto* out=preparationEvidence_;if(out)out->retain();IOLockUnlock(lock_);return out;
}
void HackyNaviResetController::capturePreparationEvidence(IOReturn result) {
    using namespace recoveryevidence;
    auto* out=OSDictionary::withCapacity(56);if(!out)return;
    const auto& p=preparation_;
    bool ok=historical(out)&&number(out,"Result",uint32_t(result))&&number(out,"StateBefore",unsigned(p.stateBefore))&&
        number(out,"StateAfter",unsigned(state_))&&number(out,"Step",unsigned(p.step))&&
        number(out,"AttemptedSteps",p.attempted)&&number(out,"CompletedSteps",p.completed)&&
        number(out,"LastRegister",lastRegister_)&&number(out,"LastValue",lastValue_)&&
        flag(out,"PreparedAfterCall",prepared_)&&flag(out,"FinalSosSampleAttempted",false)&&
        number(out,"Bar5PhysicalAddress",registerMap_->getPhysicalAddress())&&number(out,"Bar5Bytes",registerBytes_)&&
        number(out,"RawAttemptedMask",p.rawAttempted)&&number(out,"RawReadSucceededMask",p.rawReadSucceeded)&&
        number(out,"RawValidMask",p.rawValid);
    const uint32_t offsets[3]={psp_.heartbeat,scratch3_,scratch7_};
    const uint32_t values[3]={observations_.sosBefore,observations_.scratch3Before,observations_.scratch7Before};
    ok=words(out,"RawOffsetsSOS_Scratch3_Scratch7",offsets,3)&&
        words(out,"RawValuesSOS_Scratch3_Scratch7",values,3)&&ok;
    if(p.attempted&(1U<<unsigned(PreparationStep::Discovery))) {
        ok=text(out,"DiscoveryStatus",deviceDiscoveryStatusString(discoveryStatus_))&&
            number(out,"DiscoveryBytesCopied",discovery_.bytesCopied)&&number(out,"DiscoveryBinaryBytes",discovery_.binarySize)&&
            number(out,"DiscoveryReadyPolls",discovery_.readyPolls)&&number(out,"DiscoverySourceOffset",discovery_.sourceOffset)&&
            number(out,"DiscoveryRegionBytes",discovery_.regionSize)&&flag(out,"DiscoveryUsedScratch",discovery_.usedScratch)&&
            flag(out,"DiscoverySelectorsAttempted",discovery_.selectorWritesAttempted)&&ok;
    }
    if(p.attempted&(1U<<unsigned(PreparationStep::IpLayout)))
        ok=text(out,"IpLayoutStatus",ipLayoutStatusString(ipStatus_))&&
            number(out,"IpDiagnosticDiscoveryStatus",unsigned(p.diagnostic.discoveryStatus))&&
            number(out,"IpDiagnosticBlock",unsigned(p.diagnostic.block))&&
            number(out,"IpDiagnosticDie",p.diagnostic.dieId)&&number(out,"IpDiagnosticInstance",p.diagnostic.instance)&&ok;
    if(p.attempted&(1U<<unsigned(PreparationStep::RomLayout)))
        ok=text(out,"RomStatus",runtimerom::statusString(rom_.status))&&number(out,"RomBytesRead",rom_.bytesRead)&&
            number(out,"RomBytesRequired",rom_.bytesRequired)&&number(out,"RomReads",rom_.reads)&&number(out,"RomWrites",rom_.writes)&&
            number(out,"RomLastRegister",rom_.lastRegister)&&number(out,"RomLastValue",rom_.lastValue)&&
            number(out,"RomBankByteOffset",rom_.bankByteOffset)&&flag(out,"RomSelectorsAttempted",rom_.selectorWritesMayHaveOccurred)&&ok;
    if(p.attempted&(1U<<unsigned(PreparationStep::Subsystem)))ok=number(out,"PciSubsystem",p.subsystem)&&ok;
    if(p.attempted&(1U<<unsigned(PreparationStep::Firmware)))
        ok=text(out,"FirmwareInfoStatus",statusString(firmwareStatus_))&&number(out,"BiosScratchBaseDwords",firmwareInfo_.biosScratchRegisterDword)&&ok;
    if(p.attempted&(1U<<unsigned(PreparationStep::Hdp)))ok=number(out,"HdpLayoutStatus",unsigned(p.hdpStatus))&&ok;
    if(p.attempted&(1U<<unsigned(PreparationStep::Nbio)))ok=number(out,"NbioLayoutStatus",unsigned(p.nbioStatus))&&ok;
    if(p.attempted&(1U<<unsigned(PreparationStep::Psp)))ok=number(out,"PspLayoutStatus",unsigned(p.pspStatus))&&ok;
    if(p.attempted&(1U<<unsigned(PreparationStep::DebugMessage)))
        ok=number(out,"DebugMessageLayoutStatus",unsigned(p.messageStatus))&&number(out,"DebugMessageRegister",debugMessage_)&&ok;
    if(p.attempted&(1U<<unsigned(PreparationStep::DebugResponse)))
        ok=number(out,"DebugResponseLayoutStatus",unsigned(p.responseStatus))&&number(out,"DebugResponseRegister",debugResponse_)&&ok;
    // A partial native snapshot has no per-field validity contract. Only a
    // completed snapshot is serialized; its independent verification has its
    // own attempted/completed step bits and is never implied by these values.
    if(p.completed&(1U<<unsigned(PreparationStep::PciSnapshot))) {
        auto* pci=OSDictionary::withCapacity(15);
        if(!pci)ok=false;
        else {
            const auto& s=p.pci;
            uint32_t controls[4]={s.controls[0],s.controls[1],s.controls[2],s.controls[3]};
            const bool complete=number(pci,"Identity",s.identity)&&number(pci,"RevisionClass",s.revisionClass)&&
                number(pci,"Subsystem",s.subsystem)&&number(pci,"Header",s.header)&&number(pci,"Command",s.command)&&
                number(pci,"Rom",s.rom)&&words(pci,"Bars",s.bars,6)&&number(pci,"PmOffset",s.pmOffset)&&
                number(pci,"ExpressOffset",s.expressOffset)&&number(pci,"ExpressCapabilities",s.expressCapabilities)&&
                number(pci,"ControlCount",s.controlCount)&&words(pci,"Controls",controls,s.controlCount);
            ok=complete&&out->setObject("PciSnapshot",pci)&&ok;pci->release();
        }
    }
    if(ok)preparationEvidence_=out;else out->release();
}
IOReturn HackyNaviResetController::fail(IOReturn result) {
    state_=hardwareHold_?State::FaultedRetained:State::FailedUnpublished;
    IOLog("HackyNavi reset: stage=%u result=0x%x protocol=%s step=%u register=0x%x value=0x%x retained=%u shadow=%u\n",
        unsigned(stage_),unsigned(result),mode1::statusString(protocol_.status),unsigned(protocol_.stage),
        lastRegister_,lastValue_,unsigned(hardwareHold_),unsigned(permanentShadow_));
    if(stage_==Stage::Discovery || stage_==Stage::Rom || stage_==Stage::Layout)
        IOLog("HackyNavi reset capture: discovery=%s IP=%s ROM=%s firmware=%s\n",
            deviceDiscoveryStatusString(discoveryStatus_),ipLayoutStatusString(ipStatus_),
            runtimerom::statusString(rom_.status),statusString(firmwareStatus_));
    if(stage_>=Stage::VerifyPost)
        IOLog("HackyNavi reset retirement: POST=%s CP/SDMA=%s DMUB=%s scanout=%s media=%s IH/WB/MALL=%s RLC attempted=%u check=%u reg=0x%x value=0x%x deadline=%u\n",
            (retirement_.attemptedChecks&1)?bootstrap::statusString(retirement_.posted.status):"Unattempted",
            (retirement_.attemptedChecks&2)?bootstrap::statusString(retirement_.cpSdma.status):"Unattempted",
            (retirement_.attemptedChecks&4)?dmcubfw::statusString(retirement_.displayFirmware.status):"Unattempted",
            (retirement_.attemptedChecks&8)?bootdisplaystop::statusString(retirement_.scanout.status):"Unattempted",
            (retirement_.attemptedChecks&16)?coldaux::statusString(retirement_.auxiliary.status):"Unattempted",
            (retirement_.attemptedChecks&32)?colddma::statusString(retirement_.otherDma.status):"Unattempted",
            unsigned(bool(retirement_.attemptedChecks&64)),unsigned(retirement_.rlcCheck),
            retirement_.rlcLastRegister,retirement_.rlcLastValue,unsigned(retirement_.deadlineExpired));
    return finish(result);
}
IOReturn HackyNaviResetController::prepare() {
    IOLockLock(lock_);
    if(preparationEvidence_){preparationEvidence_->release();preparationEvidence_=nullptr;}
    preparation_={};preparation_.stateBefore=state_;preparationRecording_=true;
    preparationStep(PreparationStep::Admission);
    if(state_!=State::Attached)return finish(kIOReturnNotReady);
    if(!providerReady())return finish(kIOReturnOffline);
    preparationPassed();
    active_=true;stage_=Stage::Discovery;
    preparationStep(PreparationStep::Discovery);
    discoveryStatus_=acquireDeviceDiscovery({this,read32,write32,delayUs},{2000,1000},
        discoveryBytes_,kDeviceDiscoveryMaxBytes,discovery_);
    if(discoveryStatus_!=DeviceDiscoveryStatus::Ok)return fail(kIOReturnIOError);
    preparationPassed();preparationStep(PreparationStep::IpLayout);
    ipStatus_=buildNavi31IpLayout(discoveryBytes_,discovery_.binarySize,ip_,preparation_.diagnostic);
    if(ipStatus_!=IpLayoutStatus::Ok)return fail(kIOReturnUnsupported);
    preparationPassed();preparationStep(PreparationStep::RomLayout);
    stage_=Stage::Rom;
    rom_.status=runtimerom::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,romLayout_);
    if(rom_.status!=runtimerom::Status::Ok)return fail(kIOReturnUnsupported);
    preparationPassed();preparationStep(PreparationStep::RomCapture);
    if(runtimerom::acquire(romLayout_,{this,read32,write32},romBytes_,runtimerom::kRecommendedCapacity,rom_)!=runtimerom::Status::Ok)
        return fail(kIOReturnUnsupported);
    preparationPassed();preparationStep(PreparationStep::Subsystem);
    if(!accessReady())return fail(kIOReturnOffline);
    const UInt32 subsystem=device_->configRead32(kIOPCIConfigSubSystemVendorID);
    preparation_.subsystem=subsystem;
    // Preserve the original single subsystem read and final scope admission.
    preparationStep(PreparationStep::Firmware);
    firmwareStatus_=parseFirmwareInfo(romBytes_,rom_.bytesRead,rom_.image,firmwareInfo_);
    if(subsystem==UINT32_MAX || UInt16(subsystem)!=rom_.atom.subsystemVendorId ||
        UInt16(subsystem>>16)!=rom_.atom.subsystemDeviceId || firmwareStatus_!=Status::Ok || !accessReady())
        return fail(kIOReturnUnsupported);
    preparation_.completed|=1U<<unsigned(PreparationStep::Subsystem);
    preparationPassed();
    stage_=Stage::Layout;
    preparationStep(PreparationStep::Hdp);
    preparation_.hdpStatus=buildHdpLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,hdp_);
    if(preparation_.hdpStatus!=HdpStatus::Ok)return fail(kIOReturnUnsupported);
    preparationPassed();preparationStep(PreparationStep::Nbio);
    preparation_.nbioStatus=buildNbioLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,nbio_);
    if(preparation_.nbioStatus!=NbioStatus::Ok)return fail(kIOReturnUnsupported);
    preparationPassed();preparationStep(PreparationStep::Psp);
    preparation_.pspStatus=psp::buildLayout(ip_,registerBytes_,psp_);
    if(preparation_.pspStatus!=psp::Status::Ok)return fail(kIOReturnUnsupported);
    preparationPassed();preparationStep(PreparationStep::DebugMessage);
    ResolvedRegister message={},response={};
    preparation_.messageStatus=resolveNavi31Register(ip_,Navi31Block::Mp1,0,0x28b,registerBytes_,message);
    if(preparation_.messageStatus!=IpLayoutStatus::Ok)return fail(kIOReturnUnsupported);
    debugMessage_=message.mmioByteOffset;preparationPassed();preparationStep(PreparationStep::DebugResponse);
    preparation_.responseStatus=resolveNavi31Register(ip_,Navi31Block::Mp1,0,0x276,registerBytes_,response);
    if(preparation_.responseStatus!=IpLayoutStatus::Ok)return fail(kIOReturnUnsupported);
    debugMessage_=message.mmioByteOffset;debugResponse_=response.mmioByteOffset;
    preparationPassed();preparationStep(PreparationStep::ScratchLayout);
    const uint64_t scratch=(uint64_t(firmwareInfo_.biosScratchRegisterDword)+7)*4;
    if(!firmwareInfo_.biosScratchRegisterDword || scratch>UINT32_MAX || !validOffset(uint32_t(scratch)) ||
        (scratch>=0x7f000 && scratch<0x80000))return fail(kIOReturnUnsupported);
    scratch7_=uint32_t(scratch);scratch3_=scratch7_-16;
    if(!validOffset(scratch3_) || (scratch3_>=0x7f000 && scratch3_<0x80000) ||
        scratch3_==debugMessage_ || scratch3_==debugResponse_)return fail(kIOReturnUnsupported);
    preparationPassed();preparationStep(PreparationStep::PciSnapshot);stage_=Stage::Snapshot;
    if(!snapshotPci(preparation_.pci))return fail(kIOReturnOffline);
    preparationPassed();preparationStep(PreparationStep::PciVerify);
    if(!verifyPci(preparation_.pci))return fail(kIOReturnOffline);
    preparationPassed();
    const uint32_t offsets[3]={psp_.heartbeat,scratch3_,scratch7_};
    uint32_t* const values[3]={&observations_.sosBefore,&observations_.scratch3Before,&observations_.scratch7Before};
    for(unsigned i=0;i<3;++i) {
        preparationStep(PreparationStep(unsigned(PreparationStep::Sos)+i));preparation_.rawAttempted|=1U<<i;
        if(!read32(this,offsets[i],*values[i]))return fail(kIOReturnIOError);
        preparation_.rawReadSucceeded|=1U<<i;
        if(*values[i]==UINT32_MAX)return fail(kIOReturnIOError);
        preparation_.rawValid|=1U<<i;preparationPassed();
    }
    preparationStep(PreparationStep::Complete);preparationPassed();
    prepared_=true;state_=State::Prepared;stage_=Stage::Complete;return finish(kIOReturnSuccess);
}
bool HackyNaviResetController::observeAfterReset() {
    if(!recoveryAccessReady())return false;
    observations_.restoredPciVerified=verifyPci(saved_);
    if(!observations_.restoredPciVerified)return false;
    observations_.pciCommand=device_->configRead16(kIOPCIConfigCommand);
    observations_.powerControlStatus=saved_.pmOffset?device_->configRead16(saved_.pmOffset+4):0;
    // Apple's own PCI function-reset path polls Transactions Pending for up
    // to 50ms. This is a bounded observation, not a substitute reset ACK and
    // not proof that posted DMA or internal VRAM clients have been retired.
    for(unsigned i=0;i<51;++i) {
        if(!recoveryAccessReady())return false;
        ++observations_.transactionPolls;
        observations_.expressDeviceStatus=device_->configRead16(saved_.expressOffset+10);
        if(observations_.expressDeviceStatus==0xffff||!recoveryAccessReady())return false;
        if(!(observations_.expressDeviceStatus&kTransactionsPending)) {
            observations_.transactionsPendingClear=true;break;
        }
        if(i<50)IOSleep(1);
    }
    if(!read32(this,psp_.heartbeat,observations_.sosAfter) || observations_.sosAfter==UINT32_MAX ||
        !read32(this,scratch3_,observations_.scratch3After) || observations_.scratch3After==UINT32_MAX ||
        !read32(this,scratch7_,observations_.scratch7After) || observations_.scratch7After==UINT32_MAX)return false;
    observations_.resetRegistersObserved=true;
    observations_.engineHungAnnotationCleared=(observations_.scratch3After&0x20000000U)==0;
    // SOS and scratch values are reported verbatim. Linux's MP0 13.0.0
    // bootloader-steady-state wrapper is a no-op; inventing an exact SOS or
    // scratch completion predicate here would reject valid MODE1 resets.
    return true;
}
IOReturn HackyNaviResetController::reset() {
    IOLockLock(lock_);if(state_!=State::Prepared || !prepared_)return finish(kIOReturnNotReady);
    if(!recoveryAccessReady()||!providerReady())return finish(kIOReturnOffline);
    active_=true;observations_={};stage_=Stage::SmuFirmware;
    if(!readSmuFirmware(observations_.smuFirmwareFlags) || !(observations_.smuFirmwareFlags&1) ||
        !read32(this,psp_.heartbeat,observations_.sosBefore) || observations_.sosBefore==UINT32_MAX ||
        !read32(this,scratch3_,observations_.scratch3Before) || observations_.scratch3Before==UINT32_MAX ||
        !read32(this,scratch7_,observations_.scratch7Before) || observations_.scratch7Before==UINT32_MAX)
        return fail(kIOReturnNotReady);
    state_=State::Resetting;stage_=Stage::Reset;holdHardware();
    const mode1::Io io={this,read32,write32,readPci32,writePci16,savePciState,restorePciState,delayMs,delayUs};
    if(mode1::reset(ip_,hdp_,io,{100000,10,firmwareInfo_.biosScratchRegisterDword},protocol_)!=mode1::Status::Ok)
        return fail(kIOReturnIOError);
    observations_.resetProtocolCompleted=true;stage_=Stage::ObserveReset;
    if(!observeAfterReset())return fail(kIOReturnIOError);
    state_=State::ProtocolCompleteRetained;stage_=Stage::Complete;
    IOLog("HackyNavi MODE1: protocol complete; BME=%u pending-clear=%u SOS=0x%x->0x%x scratch7=0x%x->0x%x; ASIC_Init/recovery required\n",
        unsigned((observations_.pciCommand&kBusMaster)!=0),unsigned(observations_.transactionsPendingClear),
        observations_.sosBefore,observations_.sosAfter,observations_.scratch7Before,observations_.scratch7After);
    return finish(kIOReturnSuccess);
}
bool HackyNaviResetController::captureRetirementMemory() {
    const uint32_t relative[]={0x8ec,0x8ed,0x8d7};uint32_t values[3]={},sizeMiB=0;
    for(unsigned pass=0;pass<2;++pass) {
        uint32_t size=0;if(!read32(this,hdp_.memorySizeRegister,size) || !size || size==UINT32_MAX)return false;
        if(pass && size!=sizeMiB)return false;sizeMiB=size;
        for(unsigned i=0;i<3;++i) {
            ResolvedRegister reg={};uint32_t value=0;
            if(resolveNavi31Register(ip_,Navi31Block::Mmhub,0,relative[i],registerBytes_,reg)!=IpLayoutStatus::Ok ||
                !read32(this,reg.mmioByteOffset,value) || value==UINT32_MAX || (pass && value!=values[i]))return false;
            values[i]=value;
        }
    }
    if(!recoveryAccessReady())return false;
    const UInt32 low=device_->configRead32(kIOPCIConfigBaseAddress0);
    if(low==UINT32_MAX || (low&1) || ((low&6)!=0 && (low&6)!=4))return false;
    uint64_t address=low&~UInt32(15);
    if((low&6)==4) {
        const UInt32 high=device_->configRead32(kIOPCIConfigBaseAddress1);
        if(high==UINT32_MAX)return false;address|=uint64_t(high)<<32;
    }
    nativepci::AssignedMemory resource(device_,kIOPCIConfigBaseAddress0);
    if(!resource || resource->getPhysicalAddress()!=address)return false;
    return deriveMemoryLayout({values[0],values[1],values[2],uint64_t(sizeMiB)<<20,address,resource->getLength()},
        retirementMemory_)==MemoryLayoutStatus::Ok&&recoveryAccessReady();
}
bool HackyNaviResetController::verifyPspStopped() {
    // Linux Navi31 first-init admission is SOS==0. The extra bootloader READY
    // observation is the actual C2PMSG35 idle/completion state used by PSP13
    // memory training and firmware loading, not an invented MODE1 ACK.
    for(unsigned pass=0;pass<2;++pass) {
        if(!read32(this,psp_.heartbeat,retirement_.sos) || retirement_.sos ||
            !read32(this,psp_.command,retirement_.bootloaderCommand) ||
            retirement_.bootloaderCommand!=0x80000000U)return false;
    }
    return read32(this,psp_.heartbeat,retirement_.sos) && !retirement_.sos;
}
bool HackyNaviResetController::verifyRlcStopped() {
    // Same GC11 hardware drain signals as gfx_init::retireClearState, after
    // the full ASIC reset instead of that owner's firmware-state-dependent
    // safe-mode sequence. RLC and SRM must already be disabled; no successful
    // reset/POST result is substituted for these actual observations.
    const uint32_t raw[]={0x4c00,0x4c80,0x4e6b,0x4c9b,0x4c88,0x4c04,0x4cb6,0xda2,0xf40,0x1f40,0x1f41};
    const uint32_t masks[]={1,3,0xff01ffe1U,3,3,0xff,0x1ef,0x04004000,UINT32_MAX,1,1};
    const uint32_t wanted[]={0,0,0,0,1,0,0,0,0,0,0};
    uint32_t registers[sizeof(raw)/sizeof(raw[0])]={};
    for(unsigned i=0;i<sizeof(raw)/sizeof(raw[0]);++i) {
        ResolvedRegister reg={};
        if(resolveNavi31Register(ip_,Navi31Block::Gc,i<7?1U:0U,raw[i],registerBytes_,reg)!=IpLayoutStatus::Ok)return false;
        registers[i]=reg.mmioByteOffset;
    }
    for(unsigned poll=0;poll<100000;++poll) {
        bool idle=true;
        for(unsigned i=0;i<sizeof(raw)/sizeof(raw[0]);++i) {
            uint32_t value=0;++retirement_.rlcReads;
            retirement_.rlcCheck=uint8_t(i);retirement_.rlcLastRegister=registers[i];
            if(!read32(this,registers[i],value) || value==UINT32_MAX)return false;
            retirement_.rlcLastValue=value;
            if((value&masks[i])!=wanted[i]) {
                // Enabled firmware/rings are unexpected after this reset and
                // cannot be justified by waiting. Busy drain signals may clear.
                if(i<2 || i>=9)return false;
                idle=false;break;
            }
        }
        if(idle) {retirement_.rlcStopped=true;return true;}
        if(poll+1<100000)delayUs(this,10);
    }
    return false;
}
IOReturn HackyNaviResetController::verifyRetirement(HackyNaviAtomController* atom) {
    IOLockLock(lock_);
    if(state_!=State::ProtocolCompleteRetained || !hardwareHold_ || !atom || !atom->matchesReset(this) ||
        atom->state()!=HackyNaviAtomController::State::InitializedRetained ||
        !atom->observations().tableExecuted || !atom->observations().firmwareWaitSatisfied ||
        !atom->observations().scratchMarkerSet || !protocol_.blackoutCompleted || !protocol_.pciStateRestored ||
        !observations_.resetProtocolCompleted || !observations_.restoredPciVerified ||
        !observations_.transactionsPendingClear)return finish(kIOReturnNotReady);
    if(!recoveryAccessReady() || !providerReady() ||
        (device_->configRead16(kIOPCIConfigCommand)&kBusMaster))return finish(kIOReturnOffline);
    retirement_={};retirement_.atomCompleted=true;state_=State::VerifyingRetirement;active_=true;
    clock_interval_to_deadline(20,kSecondScale,&retirementDeadline_);
    // Architectural basis: amdgpu_device_reinit_after_reset at pinned Linux
    // 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0 treats MODE1 as full_reset, posts
    // through ASIC_Init, then recreates IP/firmware/VM/ring state and detects
    // VRAM loss. This owner likewise discards the old epoch, but first checks
    // every admitted memory client stopped while BME stays disabled. It never
    // uses old queue fences or CPU firmware flags as post-reset evidence.
    stage_=Stage::VerifyPost;retirement_.attemptedChecks|=1;
    if(bootstrap::checkPostedColdState(ip_,registerBytes_,firmwareInfo_.biosScratchRegisterDword,
        {this,read32,nullptr,nullptr},retirement_.posted)!=bootstrap::Status::Ok || !verifyPspStopped())
        return fail(kIOReturnNotReady);
    stage_=Stage::StopCpSdma;retirement_.attemptedChecks|=2;
    if(bootstrap::quiesceCpSdma(ip_,registerBytes_,{this,read32,write32,delayUs},{100000,10},
        retirement_.cpSdma)!=bootstrap::Status::Ok)return fail(kIOReturnIOError);
    stage_=Stage::StopDisplayFirmware;retirement_.attemptedChecks|=4;dmcubfw::Layout dmub={};
    retirement_.displayFirmware.status=dmcubfw::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,dmub);
    if(retirement_.displayFirmware.status!=dmcubfw::Status::Ok ||
        dmcubfw::stopInheritedCold(ip_,dmub,{this,read32,write32,nullptr,delayUs},{100000,10},
            retirement_.displayFirmware)!=dmcubfw::Status::Ok)return fail(kIOReturnIOError);
    stage_=Stage::CaptureMemory;
    if(!captureRetirementMemory())return fail(kIOReturnUnsupported);
    stage_=Stage::StopScanout;retirement_.attemptedChecks|=8;bootdisplay::Layout display={};
    if(bootdisplay::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,display)!=bootdisplay::Status::Ok ||
        bootdisplaystop::capture(display,retirementMemory_,{this,read32,nullptr,nullptr},retirementScanout_,
            retirement_.scanout)!=bootdisplaystop::Status::Ok ||
        bootdisplaystop::stop({this,read32,write32,delayUs},{100000,1},retirementScanout_,
            retirement_.scanout)!=bootdisplaystop::Status::Ok)return fail(kIOReturnIOError);
    stage_=Stage::Auxiliary;retirement_.attemptedChecks|=16;coldaux::Layout auxiliary={};
    retirement_.auxiliary.status=coldaux::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,auxiliary);
    if(retirement_.auxiliary.status!=coldaux::Status::Ok ||
        coldaux::checkStopped(auxiliary,{this,read32},retirement_.auxiliary)!=coldaux::Status::Ok)
        return fail(kIOReturnNotReady);
    stage_=Stage::InterruptWriteback;retirement_.attemptedChecks|=32;colddma::Layout other={};
    retirement_.otherDma.status=colddma::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,other);
    if(retirement_.otherDma.status!=colddma::Status::Ok ||
        colddma::checkStopped(other,{this,read32},retirement_.otherDma)!=colddma::Status::Ok)
        return fail(kIOReturnNotReady);
    stage_=Stage::RlcDrain;retirement_.attemptedChecks|=64;if(!verifyRlcStopped())return fail(kIOReturnNotReady);
    stage_=Stage::PspStopped;if(!verifyPspStopped())return fail(kIOReturnNotReady);
    stage_=Stage::FinalRetirement;
    // Recheck producer controls after all display/firmware operations. Shared
    // outer epoch serialization keeps them disabled through token consumption.
    if(bootstrap::quiesceCpSdma(ip_,registerBytes_,{this,read32,write32,delayUs},{100000,10},
        retirement_.cpSdma)!=bootstrap::Status::Ok ||
        coldaux::checkStopped(auxiliary,{this,read32},retirement_.auxiliary)!=coldaux::Status::Ok ||
        colddma::checkStopped(other,{this,read32},retirement_.otherDma)!=colddma::Status::Ok ||
        !verifyRlcStopped() || !verifyPspStopped() || !verifyPci(saved_))return fail(kIOReturnIOError);
    const uint16_t pending=device_->configRead16(saved_.expressOffset+10);
    if(pending==0xffff || (pending&kTransactionsPending) || !retirementDeadlineReady() ||
        !recoveryAccessReady())return fail(kIOReturnNotReady);
    proofAccessSerial_=currentAccess()->serial_;
    retirement_.verified=true;state_=State::RetirementVerified;stage_=Stage::Complete;
    IOLog("HackyNavi reset: MODE1/ASIC_Init and stopped memory clients verified; old epoch may be released under device lock\n");
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviResetController::RetirementProof::validateFor(IOPCIDevice* device,IOService* owner,IOMemoryMap* map) const {
    return issuer_?issuer_->validateRetirement(device,owner,map):kIOReturnNotReady;
}
IOReturn HackyNaviResetController::validateRetirement(IOPCIDevice* device,IOService* owner,IOMemoryMap* map) {
    IOLockLock(lock_);
    const bool valid=state_==State::RetirementVerified && retirement_.verified && hardwareHold_ &&
        recoveryAccessReady() && proofAccessSerial_==currentAccess()->serial_ &&
        matchesDevice(device,owner,map) && providerReady() && verifyPci(saved_);
    return finish(valid?kIOReturnSuccess:kIOReturnNotReady);
}
IOReturn HackyNaviResetController::finishRetirementTransfer(const RetirementProof& proof) {
    IOLockLock(lock_);
    if(&proof!=&proof_ || !retirement_.verified || state_!=State::RetirementVerified || !hardwareHold_ ||
        !recoveryAccessReady() || proofAccessSerial_!=currentAccess()->serial_ ||
        !providerReady() || !verifyPci(saved_))return finish(kIOReturnNotReady);
    retirement_.verified=false;proofAccessSerial_=0;state_=State::RetirementTransferred;
    hardwareHold_=false;finish(kIOReturnSuccess);release();return kIOReturnSuccess;
}
IOReturn HackyNaviResetController::refreshPermanentPciState(HackyNaviFunctionGroup::RetirementScope& scope) {
    IOLockLock(lock_);
    const auto* access=currentAccess();
    if(!access||access->retirement_!=&scope||!recoveryAccessReady())return finish(kIOReturnOffline);
    if(!permanentShadow_ || blackout_ || state_==State::Resetting || state_==State::VerifyingRetirement ||
        state_==State::RetirementVerified || (!hardwareHold_ && state_!=State::RetirementTransferred))
        return finish(kIOReturnNotReady);
    if(!providerReady())return finish(kIOReturnOffline);
    stage_=Stage::RefreshPciShadow;PciSnapshot now={};
    if(!snapshotPci(now) || !recoveryAccessReady() ||
        device_->saveDeviceState(kIOPCIConfigShadowPermanent)!=kIOReturnSuccess ||
        !recoveryAccessReady() || !verifyPci(now))return fail(kIOReturnIOError);
    // The recovery owner controls this new snapshot. It is never reused as
    // this object's BME-off reset snapshot; reset() is deliberately one-shot.
    stage_=Stage::Complete;return finish(kIOReturnSuccess);
}
void HackyNaviResetController::free() {
    if(preparationEvidence_)preparationEvidence_->release();
    if(discoveryBytes_)IOFree(discoveryBytes_,kDeviceDiscoveryMaxBytes);
    if(romBytes_)IOFree(romBytes_,runtimerom::kRecommendedCapacity);
    registers_=nullptr;
    if(registerMap_)registerMap_->release();if(device_)device_->release();if(openOwner_)openOwner_->release();
    if(lock_)IOLockFree(lock_);OSObject::free();
}
