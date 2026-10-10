// SPDX-License-Identifier: MIT
#include "HackyNaviEngineController.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
#include <libkern/libkern.h>

#if !defined(__x86_64__)
#error HackyNaviEngineController requires native coherent x86_64 DMA
#endif
OSDefineMetaClassAndStructors(HackyNaviEngineController,OSObject)
using namespace hackynavi;
namespace {
constexpr uint32_t kIdentity=0x744c1002;
const bootstrap::PollOptions kBootstrapPoll={100000,10};
const psp::Options kSosPoll={10000,10000,1000};
const pspring::PollOptions kPspPoll={10000,100};
const gfxqueue::PollOptions kGraphicsPoll={100000,10};
const gfxinit::RetirementPollOptions kRlcPoll={100000,10};
const dmcubfw::PollOptions kDisplayPoll={100000,10};
const bootdisplaystop::PollOptions kScanoutPoll={100000,1};
}
bool HackyNaviEngineController::init() {
    lock_=nullptr;device_=nullptr;openOwner_=nullptr;registerMap_=nullptr;
    registers_=nullptr;registerBytes_=0;discoveryBytes_=romBytes_=nullptr;
    mmioActive_=hardwareHold_=controlWritesStarted_=busMasterChanged_=false;
    state_=State::Empty;stage_=Stage::None;result_=kIOReturnSuccess;
    lastRegister_=lastValue_=0;discoveryStatus_=DeviceDiscoveryStatus::InvalidArgument;
    ipStatus_=IpLayoutStatus::InvalidLayout;memoryStatus_=MemoryLayoutStatus::InvalidArgument;
    firmwareInfoStatus_=Status::NotFound;reservationStatus_=VramReservationStatus::MissingEvidence;
    nbioStatus_=NbioStatus::InvalidState;
    discovery_={};rom_={};ip_={};memory_={};firmwareInfo_={};usage_={};
    usageStatus_=Status::NotFound;bootScanoutResult_={};auxiliary_={};coldDma_={};bootstrap_={};
    inheritedDisplay_={};reservations_={};nbioResult_={};bootConfiguration_=0;writeTest_={};shaderTest_={};
    gart_=nullptr;firmware_=nullptr;graphics_=nullptr;display_=nullptr;
    if(!OSObject::init())return false;
    lock_=IOLockAlloc();return lock_!=nullptr;
}
HackyNaviEngineController* HackyNaviEngineController::withDevice(IOPCIDevice* device,
    IOService* owner,IOMemoryMap* map,IOReturn& result) {
    result=kIOReturnNoMemory;auto* self=new HackyNaviEngineController;
    if(!self)return nullptr;
    if(!self->init()) { self->release();return nullptr; }
    result=self->initialize(device,owner,map);
    if(result!=kIOReturnSuccess) { self->release();return nullptr; }
    return self;
}
IOReturn HackyNaviEngineController::initialize(IOPCIDevice* device,IOService* owner,IOMemoryMap* map) {
    if(!device || !owner || !map || !map->getVirtualAddress() || (map->getVirtualAddress()&3) ||
        map->getLength()<0x80000 || map->getLength()>16U*1024*1024 ||
        (map->getMapOptions()&kIOMapCacheMask)!=kIOMapInhibitCache)return kIOReturnBadArgument;
    device_=device;device_->retain();openOwner_=owner;openOwner_->retain();
    if(!providerReady())return kIOReturnOffline;
    const UInt32 bar4=device_->configRead32(kIOPCIConfigBaseAddress4);
    const UInt32 bar5=device_->configRead32(kIOPCIConfigBaseAddress5);
    IODeviceMemory* resource=device_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
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
    state_=State::Prepared;return kIOReturnSuccess;
}
bool HackyNaviEngineController::providerReady(bool dma) const {
    if(!device_ || !openOwner_ || device_->isInactive() || !device_->isOpen(openOwner_) ||
        device_->configRead32(0)!=kIdentity)return false;
    const UInt16 command=device_->configRead16(kIOPCIConfigCommand);
    const UInt32 revision=device_->configRead32(kIOPCIConfigRevisionID);
    if(command==0xffff || !(command&2) || (dma && !(command&4)) ||
        (revision&0xff)!=0xc8 || (revision>>24)!=3)return false;
    UInt8 offset=0;
    if(device_->findPCICapability(1,&offset)) {
        if(offset<0x40 || offset>0xf8 || (offset&3))return false;
        const UInt16 power=device_->configRead16(offset+4);
        if(power==0xffff || (power&3))return false;
    }
    return true;
}
bool HackyNaviEngineController::read32(void* context,uint32_t offset,uint32_t& value) {
    auto* self=static_cast<HackyNaviEngineController*>(context);
    self->lastRegister_=offset;
    if(!self->mmioActive_ || !self->registers_ || (offset&3) ||
        offset>self->registerBytes_-4 || !self->providerReady())return false;
    __asm__ __volatile__("" ::: "memory");value=self->registers_[offset/4];
    OSSynchronizeIO();__asm__ __volatile__("" ::: "memory");self->lastValue_=value;return true;
}
bool HackyNaviEngineController::write32(void* context,uint32_t offset,uint32_t value) {
    auto* self=static_cast<HackyNaviEngineController*>(context);
    self->lastRegister_=offset;self->lastValue_=value;
    if(!self->hardwareHold_ || !self->mmioActive_ || !self->registers_ || (offset&3) ||
        offset>self->registerBytes_-4 || !self->providerReady())return false;
    __asm__ __volatile__("" ::: "memory");self->registers_[offset/4]=value;
    OSSynchronizeIO();__asm__ __volatile__("" ::: "memory");return true;
}
void HackyNaviEngineController::delayUs(void*,uint32_t us) {
    if(us>=1000)IOSleep((us+999)/1000);else IODelay(us);
}
void HackyNaviEngineController::holdHardware() {
    if(!hardwareHold_) { retain();hardwareHold_=true; }
}
IOReturn HackyNaviEngineController::finish(IOReturn result) {
    result_=result;mmioActive_=false;IOLockUnlock(lock_);return result;
}
IOReturn HackyNaviEngineController::fail(IOReturn result) {
    result_=result;logFailure();
    // MM_INDEX/ROM/RSMU selector writes from acquisition publish no allocation
    // and do not enable an engine. Every later possibly landed control write
    // seals this epoch, including failure before subowners have been created.
    state_=controlWritesStarted_?State::FaultedRetained:State::FailedUnpublished;
    const bool drop=hardwareHold_ && !controlWritesStarted_;
    if(drop)hardwareHold_=false;
    finish(result);if(drop)release();return result;
}
bool HackyNaviEngineController::captureMemory() {
    const uint32_t relative[]={0x8ec,0x8ed,0x8d7};uint32_t values[3]={};
    for(unsigned pass=0;pass<2;++pass)for(unsigned i=0;i<3;++i) {
        ResolvedRegister r={};uint32_t value=0;
        if(resolveNavi31Register(ip_,Navi31Block::Mmhub,0,relative[i],registerBytes_,r)!=IpLayoutStatus::Ok ||
            !read32(this,r.mmioByteOffset,value) || value==UINT32_MAX)return false;
        if(pass && value!=values[i])return false;values[i]=value;
    }
    const UInt32 bar0=device_->configRead32(kIOPCIConfigBaseAddress0);
    if(bar0==UINT32_MAX || (bar0&1) || ((bar0&6)!=0 && (bar0&6)!=4))return false;
    uint64_t address=bar0&~UInt32(15);
    if((bar0&6)==4) {
        const UInt32 high=device_->configRead32(kIOPCIConfigBaseAddress1);
        if(high==UINT32_MAX)return false;address|=uint64_t(high)<<32;
    }
    IODeviceMemory* resource=device_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if(!resource || resource->getPhysicalAddress()!=address)return false;
    memoryStatus_=deriveMemoryLayout({values[0],values[1],values[2],discovery_.vramBytes,
        address,resource->getLength()},memory_);
    return memoryStatus_==MemoryLayoutStatus::Ok;
}
bool HackyNaviEngineController::checkTrainingIdle() {
    psp::Layout layout={};
    if(psp::buildLayout(ip_,registerBytes_,layout)!=psp::Status::Ok)return false;
    // PSP13 training waits on C2PMSG35 bit31; requiring the exact successful
    // bootloader completion value also rejects every pending/error command.
    // SOS is bracketed so this proof cannot come from a prior driver epoch.
    for(unsigned i=0;i<2;++i) {
        uint32_t sos=0,command=0;
        if(!read32(this,layout.heartbeat,sos) || sos ||
            !read32(this,layout.command,command) || command!=0x80000000U)return false;
    }
    uint32_t sos=0;return read32(this,layout.heartbeat,sos) && !sos;
}
bool HackyNaviEngineController::makeReservations() {
    const auto& displayCapture=bootScanout_.capture();
    VramRange planes[bootdisplay::kPipeCount]={};
    for(size_t i=0;i<displayCapture.planeCount;++i)planes[i]=displayCapture.planes[i].reservation;
    VramReservationInput input={};input.memory=memory_;
    input.firmwareStatus=firmwareInfoStatus_;input.firmware=firmwareInfo_;
    input.firmwareEvidence=VramReservationEvidence::RomImage;
    input.usageStatus=usageStatus_;input.usage=usage_;
    input.usageEvidence=VramReservationEvidence::RomImage;
    input.preOsFramebuffers=planes;input.preOsFramebufferCount=displayCapture.planeCount;
    input.preOsFramebufferInventoryComplete=displayCapture.complete;
    input.discoveryCaptureStatus=discoveryStatus_;
    input.discoveryBinaryStatus=DiscoveryStatus::Ok;input.discovery=discovery_;
    // Set only along the successful cold protocol path. No active PSP/TMR or
    // prior queues exist: SOS=0, actual CP/SDMA stop, media/LSDMA cold checks,
    // inherited DMCUB STOP/PWAIT/reset, and actual IH/DWB/MCIF/MALL stopped
    // observations have all completed in this epoch. The global owner keeps
    // every admitted producer stopped; discovery/ROM alone is not this proof.
    input.additionalReservationInventoryComplete=state_==State::Starting && stage_==Stage::Reservations &&
        bootScanout_.phase()==bootdisplaystop::Phase::Armed && bootstrap_.status==bootstrap::Status::Ok &&
        auxiliary_.status==coldaux::Status::Ok && coldDma_.status==colddma::Status::Ok &&
        inheritedDisplay_.status==dmcubfw::Status::Ok && inheritedDisplay_.stage==dmcubfw::Stage::Complete;
    input.memoryTrainingInactive=checkTrainingIdle();
    reservationStatus_=buildVramReservations(input,reservations_);
    return reservationStatus_==VramReservationStatus::Ok && reservations_.complete;
}
IOReturn HackyNaviEngineController::start() {
    IOLockLock(lock_);
    if(state_!=State::Prepared)return finish(kIOReturnNotReady);
    if(!providerReady())return finish(kIOReturnOffline);
    state_=State::Starting;holdHardware();mmioActive_=true;
    stage_=Stage::Discovery;
    discoveryStatus_=acquireDeviceDiscovery({this,read32,write32,delayUs},{2000,1000},discoveryBytes_,
        kDeviceDiscoveryMaxBytes,discovery_);
    if(discoveryStatus_!=DeviceDiscoveryStatus::Ok)return fail(kIOReturnIOError);
    IpLayoutDiagnostic diagnostic={};
    ipStatus_=buildNavi31IpLayout(discoveryBytes_,discovery_.binarySize,ip_,diagnostic);
    if(ipStatus_!=IpLayoutStatus::Ok)
        return fail(kIOReturnUnsupported);
    stage_=Stage::Rom;runtimerom::Layout romLayout={};
    rom_.status=runtimerom::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,romLayout);
    if(rom_.status!=runtimerom::Status::Ok ||
        runtimerom::acquire(romLayout,{this,read32,write32},romBytes_,runtimerom::kRecommendedCapacity,rom_)!=runtimerom::Status::Ok)
        return fail(kIOReturnUnsupported);
    const UInt32 subsystem=device_->configRead32(kIOPCIConfigSubSystemVendorID);
    firmwareInfoStatus_=parseFirmwareInfo(romBytes_,rom_.bytesRead,rom_.image,firmwareInfo_);
    if(subsystem==UINT32_MAX || UInt16(subsystem)!=rom_.atom.subsystemVendorId ||
        UInt16(subsystem>>16)!=rom_.atom.subsystemDeviceId || firmwareInfoStatus_!=Status::Ok)
        return fail(kIOReturnUnsupported);
    usageStatus_=parseVramUsage(romBytes_,rom_.bytesRead,rom_.image,usage_);
    if(usageStatus_!=Status::Ok && usageStatus_!=Status::NotFound)return fail(kIOReturnUnsupported);
    stage_=Stage::Memory;if(!captureMemory())return fail(kIOReturnUnsupported);
    stage_=Stage::Posted;
    if(bootstrap::checkPostedColdState(ip_,registerBytes_,firmwareInfo_.biosScratchRegisterDword,
        {this,read32,nullptr,nullptr},bootstrap_)!=bootstrap::Status::Ok)return fail(kIOReturnNotReady);
    stage_=Stage::Training;if(!checkTrainingIdle())return fail(kIOReturnNotReady);
    stage_=Stage::Auxiliary;coldaux::Layout auxLayout={};
    auxiliary_.status=coldaux::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,auxLayout);
    if(auxiliary_.status!=coldaux::Status::Ok ||
        coldaux::checkStopped(auxLayout,{this,read32},auxiliary_)!=coldaux::Status::Ok)return fail(kIOReturnNotReady);
    stage_=Stage::StopInheritedDisplayFirmware;dmcubfw::Layout inherited={};
    inheritedDisplay_.status=dmcubfw::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,inherited);
    if(inheritedDisplay_.status!=dmcubfw::Status::Ok)
        return fail(kIOReturnUnsupported);
    controlWritesStarted_=true;
    if(dmcubfw::stopInheritedCold(ip_,inherited,{this,read32,write32,nullptr,delayUs},kDisplayPoll,
        inheritedDisplay_)!=dmcubfw::Status::Ok)return fail(kIOReturnIOError);
    stage_=Stage::BootDisplay;bootdisplay::Layout displayLayout={};
    bootScanoutResult_.captureStatus=bootdisplay::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,displayLayout);
    if(bootScanoutResult_.captureStatus!=bootdisplay::Status::Ok) {
        bootScanoutResult_.status=bootdisplaystop::Status::CaptureRejected;return fail(kIOReturnUnsupported);
    }
    if(
        bootdisplaystop::capture(displayLayout,memory_,{this,read32,nullptr,nullptr},
            bootScanout_,bootScanoutResult_)!=bootdisplaystop::Status::Ok)
        return fail(kIOReturnUnsupported);
    stage_=Stage::StopEngines;
    if(bootstrap::quiesceCpSdma(ip_,registerBytes_,{this,read32,write32,delayUs},kBootstrapPoll,
        bootstrap_)!=bootstrap::Status::Ok)return fail(kIOReturnIOError);
    stage_=Stage::Auxiliary;
    if(coldaux::checkStopped(auxLayout,{this,read32},auxiliary_)!=coldaux::Status::Ok)return fail(kIOReturnNotReady);
    stage_=Stage::ColdDma;colddma::Layout dmaLayout={};
    coldDma_.status=colddma::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,dmaLayout);
    if(coldDma_.status!=colddma::Status::Ok ||
        colddma::checkStopped(dmaLayout,{this,read32},coldDma_)!=colddma::Status::Ok)return fail(kIOReturnNotReady);
    stage_=Stage::Reservations;if(!makeReservations())return fail(kIOReturnUnsupported);
    stage_=Stage::Nbio;NbioLayout nbioLayout={};
    nbioStatus_=buildNbioLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,nbioLayout);
    if(nbioStatus_!=NbioStatus::Ok)return fail(kIOReturnUnsupported);
    nbioStatus_=initializeNbioRegisters(nbioLayout,{this,read32,write32},nbio_,nbioResult_);
    if(nbioStatus_!=NbioStatus::Ok)
        return fail(kIOReturnIOError);
    HackyNaviGARTController::Config config={};config.memory=memory_;
    config.ownedArena={0,memory_.vramBytes};config.storage=HackyNaviGARTStorage::defaultOptions();
    config.poll={100000,10};
    // Lowest disjoint 4GiB-aligned MC aperture after the entire framebuffer
    // window. If the upper address space is exhausted, try the low aperture.
    const uint64_t align=UINT64_C(1)<<32,limit=UINT64_C(1)<<47;
    if(memory_.mcWindowEndInclusive<limit-align)
        config.apertureStart=(memory_.mcWindowEndInclusive+align)&~(align-1);
    else if(memory_.mcWindowStart>=config.storage.apertureBytes)config.apertureStart=0;
    else return fail(kIOReturnNoSpace);
    stage_=Stage::AllocateGart;IOReturn result=kIOReturnSuccess;
    gart_=HackyNaviGARTController::withDevice(device_,openOwner_,registerMap_,discoveryBytes_,
        discovery_.binarySize,config,reservations_.ranges,reservations_.count,result);
    if(!gart_)return fail(result);
    stage_=Stage::UploadGart;if((result=gart_->upload())!=kIOReturnSuccess)return fail(result);
    stage_=Stage::BusMaster;
    // IOPCIDevice returns the PREVIOUS enable state, not a success result.
    // Record ownership before the write and verify actual command readback.
    const UInt16 command=device_->configRead16(kIOPCIConfigCommand);
    if(command==0xffff || !(command&2))return fail(kIOReturnOffline);
    if(!(command&4)) { busMasterChanged_=true;(void)device_->setBusMasterEnable(true); }
    if(!providerReady(true))return fail(kIOReturnOffline);
    stage_=Stage::Mmhub;if((result=gart_->enableMmhub())!=kIOReturnSuccess)return fail(result);
    stage_=Stage::Doorbell;
    nbioStatus_=setNbioDoorbellAperture({this,read32,write32},nbio_,true,nbioResult_);
    if(nbioStatus_!=NbioStatus::Ok)
        return fail(kIOReturnIOError);
    stage_=Stage::FirmwareOwner;firmware_=HackyNaviFirmwareController::withController(gart_,result);
    if(!firmware_)return fail(result);
    stage_=Stage::Sos;if((result=firmware_->loadSos(kSosPoll))!=kIOReturnSuccess)return fail(result);
    stage_=Stage::PspRing;if((result=firmware_->createRing(kPspPoll))!=kIOReturnSuccess)return fail(result);
    stage_=Stage::BootConfig;
    if((result=firmware_->queryBootConfiguration(kPspPoll,bootConfiguration_))!=kIOReturnSuccess)return fail(result);
    stage_=Stage::Tmr;if((result=firmware_->initializeTmr(kPspPoll))!=kIOReturnSuccess)return fail(result);
    stage_=Stage::GraphicsFirmware;
    if((result=firmware_->loadGraphicsFirmware(kPspPoll))!=kIOReturnSuccess)return fail(result);
    stage_=Stage::Gfxhub;if((result=gart_->enableGfxhub())!=kIOReturnSuccess)return fail(result);
    stage_=Stage::GraphicsOwner;graphics_=HackyNaviGraphicsQueue::withFirmware(firmware_,result);
    if(!graphics_)return fail(result);
    stage_=Stage::GraphicsPrepare;if((result=graphics_->prepare())!=kIOReturnSuccess)return fail(result);
    stage_=Stage::GraphicsStart;if((result=graphics_->start(kGraphicsPoll))!=kIOReturnSuccess)return fail(result);
    stage_=Stage::GraphicsVerify;
    if((result=graphics_->runWriteTest(kGraphicsPoll,writeTest_))!=kIOReturnSuccess || !writeTest_.completed)
        return fail(result==kIOReturnSuccess?kIOReturnIOError:result);
    stage_=Stage::ShaderVerify;
    if((result=graphics_->runShaderTest(kGraphicsPoll,shaderTest_))!=kIOReturnSuccess || !shaderTest_.completed)
        return fail(result==kIOReturnSuccess?kIOReturnIOError:result);
    stage_=Stage::DisplayOwner;
    display_=HackyNaviDisplayController::withFirmware(firmware_,romBytes_,rom_.bytesRead,rom_.image,result);
    if(!display_)return fail(result);
    stage_=Stage::DisplayBoot;if((result=display_->boot(kDisplayPoll))!=kIOReturnSuccess)return fail(result);
    stage_=Stage::Complete;state_=State::Ready;
    IOLog("HackyNavi engines: GPU CP/shader verification completed (%u/%u words); DAL DMCUB booted\n",
        writeTest_.wordsCompared,shaderTest_.wordsCompared);
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviEngineController::stop() {
    IOLockLock(lock_);
    if(state_==State::Retired)return finish(kIOReturnSuccess);
    if(state_==State::Prepared || state_==State::FailedUnpublished) {
        state_=State::Retired;return finish(kIOReturnSuccess);
    }
    // A failed control write may have landed in any subowner. Neither caller
    // release nor a partial successful cleanup supplies a reset guarantee.
    if(state_!=State::Ready)return finish(kIOReturnNotReady);
    if(!providerReady(true))return fail(kIOReturnOffline);
    mmioActive_=true;IOReturn result=kIOReturnSuccess;
    // Withdraw every inherited memory reader before stopping the new display
    // firmware. Downstream OTG/DIO still clock pixels but no longer fetch the
    // captured surfaces; this controller created no additional streams.
    stage_=Stage::StopBootScanout;
    if(bootdisplaystop::stop({this,read32,write32,delayUs},kScanoutPoll,bootScanout_,bootScanoutResult_)!=bootdisplaystop::Status::Ok)
        return fail(kIOReturnIOError);
    stage_=Stage::StopDisplay;if((result=display_->stop(kDisplayPoll))!=kIOReturnSuccess)return fail(result);
    stage_=Stage::StopGraphics;if((result=graphics_->stop(kGraphicsPoll))!=kIOReturnSuccess)return fail(result);
    stage_=Stage::RetireRlc;
    if((result=graphics_->retireAfterQuiescence(kRlcPoll))!=kIOReturnSuccess)return fail(result);
    stage_=Stage::RetireFirmware;
    if((result=firmware_->retireAfterQuiescence(kPspPoll))!=kIOReturnSuccess)return fail(result);
    stage_=Stage::RetireGart;if((result=gart_->retireAfterQuiescence())!=kIOReturnSuccess)return fail(result);
    stage_=Stage::RetireNbio;
    nbioStatus_=disableNbioApertures({this,read32,write32},nbio_,nbioResult_);
    if(nbioStatus_!=NbioStatus::Ok)return fail(kIOReturnIOError);
    if(busMasterChanged_) {
        (void)device_->setBusMasterEnable(false);
        const UInt16 command=device_->configRead16(kIOPCIConfigCommand);
        if(command==0xffff || (command&4))return fail(kIOReturnIOError);
        busMasterChanged_=false;
    }
    state_=State::Retired;const bool drop=hardwareHold_;hardwareHold_=false;
    releaseOwners();finish(kIOReturnSuccess);if(drop)release();return kIOReturnSuccess;
}
IOReturn HackyNaviEngineController::releaseAfterVerifiedReset(const HackyNaviResetController::RetirementProof& proof) {
    IOLockLock(lock_);stage_=Stage::VerifiedReset;IOReturn result=kIOReturnSuccess;
    result=proof.validateFor(device_,openOwner_,registerMap_);
    if(result!=kIOReturnSuccess)return finish(result);
    // The native reset owner checked these exact retained provider resources.
    // Release precisely the subowners that actually exist, including partial
    // startup states, while the outer epoch excludes every new producer.
    if(display_ && (result=display_->releaseAfterDeviceReset())!=kIOReturnSuccess)return fail(result);
    if(graphics_ && (result=graphics_->releaseAfterDeviceReset())!=kIOReturnSuccess)return fail(result);
    if(firmware_ && (result=firmware_->releaseAfterDeviceReset())!=kIOReturnSuccess)return fail(result);
    if(gart_ && (result=gart_->releaseAfterDeviceReset())!=kIOReturnSuccess)return fail(result);
    nbio_.clearAfterDeviceReset();busMasterChanged_=false;state_=State::Retired;
    const bool drop=hardwareHold_;hardwareHold_=false;releaseOwners();
    finish(kIOReturnSuccess);if(drop)release();return kIOReturnSuccess;
}
void HackyNaviEngineController::releaseOwners() {
    if(display_) { display_->release();display_=nullptr; }
    if(graphics_) { graphics_->release();graphics_=nullptr; }
    if(firmware_) { firmware_->release();firmware_=nullptr; }
    if(gart_) { gart_->release();gart_=nullptr; }
}
const char* HackyNaviEngineController::stageString(Stage stage) {
    switch(stage) {
#define NAME(x) case Stage::x:return #x
    NAME(None);NAME(Discovery);NAME(Rom);NAME(Memory);NAME(Posted);NAME(Training);
    NAME(Auxiliary);NAME(StopInheritedDisplayFirmware);NAME(BootDisplay);NAME(StopEngines);NAME(ColdDma);
    NAME(Reservations);NAME(Nbio);NAME(AllocateGart);NAME(UploadGart);NAME(BusMaster);
    NAME(Mmhub);NAME(Doorbell);NAME(FirmwareOwner);NAME(Sos);NAME(PspRing);NAME(BootConfig);
    NAME(Tmr);NAME(GraphicsFirmware);NAME(Gfxhub);NAME(GraphicsOwner);NAME(GraphicsPrepare);
    NAME(GraphicsStart);NAME(GraphicsVerify);NAME(ShaderVerify);NAME(DisplayOwner);NAME(DisplayBoot);NAME(Complete);
    NAME(StopDisplay);NAME(StopGraphics);NAME(RetireRlc);NAME(RetireFirmware);NAME(StopBootScanout);
    NAME(RetireGart);NAME(RetireNbio);NAME(VerifiedReset);
#undef NAME
    }
    return "Unknown";
}
void HackyNaviEngineController::logFailure() const {
    IOLog("HackyNavi engines: stage=%s result=0x%x retain=%u last BAR5[0x%x]=0x%x\n",
        stageString(stage_),unsigned(result_),unsigned(controlWritesStarted_),lastRegister_,lastValue_);
    switch(stage_) {
    case Stage::Discovery:
        IOLog("HackyNavi discovery: acquisition=%s layout=%s bytes=%llu\n",
            deviceDiscoveryStatusString(discoveryStatus_),ipLayoutStatusString(ipStatus_),
            static_cast<unsigned long long>(discovery_.bytesCopied));break;
    case Stage::Rom:
        IOLog("HackyNavi ROM: %s FirmwareInfo=%s usage=%s bank=0x%x\n",
            runtimerom::statusString(rom_.status),statusString(firmwareInfoStatus_),statusString(usageStatus_),rom_.bankByteOffset);break;
    case Stage::Memory:IOLog("HackyNavi memory: %s\n",memoryLayoutStatusString(memoryStatus_));break;
    case Stage::Posted:case Stage::StopEngines:
        IOLog("HackyNavi bootstrap: %s step=%u register=0x%x value=0x%x writes=%u\n",
            bootstrap::statusString(bootstrap_.status),unsigned(bootstrap_.stage),bootstrap_.lastOffset,
            bootstrap_.lastValue,bootstrap_.writeAttempts);break;
    case Stage::Training:
        IOLog("HackyNavi training admission requires cold SOS=0 and C2PMSG35=0x80000000\n");break;
    case Stage::Auxiliary:
        IOLog("HackyNavi media/LSDMA: %s register=0x%x value=0x%x mask=0x%x expected=0x%x\n",
            coldaux::statusString(auxiliary_.status),auxiliary_.lastRegister,auxiliary_.lastValue,
            auxiliary_.mask,auxiliary_.expected);break;
    case Stage::ColdDma:
        IOLog("HackyNavi IH/writeback/MALL: %s register=0x%x value=0x%x mask=0x%x expected=0x%x pass=%u pipe=%u\n",
            colddma::statusString(coldDma_.status),coldDma_.lastRegister,coldDma_.lastValue,
            coldDma_.mask,coldDma_.expected,coldDma_.pass,coldDma_.pipe);break;
    case Stage::StopInheritedDisplayFirmware:
        IOLog("HackyNavi inherited DMCUB: %s step=%u register=0x%x value=0x%x writes=%u\n",
            dmcubfw::statusString(inheritedDisplay_.status),unsigned(inheritedDisplay_.stage),
            inheritedDisplay_.lastRegister,inheritedDisplay_.lastValue,inheritedDisplay_.writes);break;
    case Stage::BootDisplay:case Stage::StopBootScanout:
        IOLog("HackyNavi boot scanout: %s capture=%s pipe=%u register=0x%x value=0x%x writes=%u\n",
            bootdisplaystop::statusString(bootScanoutResult_.status),bootdisplay::statusString(bootScanoutResult_.captureStatus),
            bootScanoutResult_.pipe,bootScanoutResult_.lastRegister,bootScanoutResult_.lastValue,bootScanoutResult_.writes);break;
    case Stage::Reservations:
        IOLog("HackyNavi reservations: %s missing=0x%x count=%llu\n",
            vramReservationStatusString(reservationStatus_),reservations_.missingEvidence,
            static_cast<unsigned long long>(reservations_.count));break;
    case Stage::Nbio:case Stage::Doorbell:case Stage::RetireNbio:
        IOLog("HackyNavi NBIO: %s control writes=%u selector writes=%u\n",nbioStatusString(nbioStatus_),
            nbioResult_.controlWritesAttempted,nbioResult_.selectorWritesAttempted);break;
    default:break;
    }
    if(gart_ && (stage_==Stage::Mmhub || stage_==Stage::Gfxhub || stage_==Stage::RetireGart || stage_==Stage::UploadGart)) {
        const auto& r=gart_->lastGartResult();const auto& t=gart_->lastTransferResult();
        IOLog("HackyNavi GART: %s step=%u register=0x%x value=0x%x; transfer=%s\n",
            gart::statusString(r.status),unsigned(r.stage),r.lastOffset,r.lastValue,vramTransferStatusString(t.status));
    }
    if(firmware_ && (stage_==Stage::Sos || stage_==Stage::PspRing || stage_==Stage::BootConfig ||
        stage_==Stage::Tmr || stage_==Stage::GraphicsFirmware || stage_==Stage::RetireFirmware ||
        stage_==Stage::DisplayBoot || stage_==Stage::StopDisplay)) {
        const auto& s=firmware_->lastSosResult();const auto& r=firmware_->lastRingResult();
        const auto& d=firmware_->lastDisplayFirmwareResult();
        IOLog("HackyNavi firmware: SOS=%s step=%u register=0x%x value=0x%x; PSP=%s command=0x%x response=0x%x fence=%u/%u\n",
            psp::statusString(s.status),unsigned(s.stage),s.lastRegister,s.lastValue,pspring::statusString(r.status),
            r.commandId,r.responseStatus,r.observedFence,r.fenceSequence);
        IOLog("HackyNavi firmware: graphics=%s SMU=%s DMCUB=%s step=%u register=0x%x value=0x%x\n",
            gfxfw::statusString(firmware_->lastGraphicsResult().status),smu::statusString(firmware_->lastSmuResult().status),
            dmcubfw::statusString(d.status),unsigned(d.stage),d.lastRegister,d.lastValue);
    }
    if(graphics_ && (stage_==Stage::GraphicsPrepare || stage_==Stage::GraphicsStart || stage_==Stage::GraphicsVerify || stage_==Stage::ShaderVerify ||
        stage_==Stage::StopGraphics || stage_==Stage::RetireRlc)) {
        const auto& i=graphics_->lastInitResult();const auto& q=graphics_->lastQueueResult();
        IOLog("HackyNavi graphics: init=%s step=%u register=0x%x value=0x%x; queue=%s step=%u register=0x%x value=0x%x fence=%u RPTR=%u\n",
            gfxinit::statusString(i.status),unsigned(i.stage),i.lastRegister,i.lastValue,gfxqueue::statusString(q.status),
            unsigned(q.stage),q.lastRegister,q.lastValue,q.fenceValue,q.observedRptr);
        if(stage_==Stage::ShaderVerify)
            IOLog("HackyNavi shader: encode=%s dispatches=%u words=%u mismatch=%u expected=0x%x observed=0x%x\n",
                gfxcompute::statusString(graphics_->lastComputeStatus()),shaderTest_.dispatchesCompleted,
                shaderTest_.wordsCompared,shaderTest_.firstMismatch,shaderTest_.expected,shaderTest_.observed);
    }
}
void HackyNaviEngineController::free() {
    releaseOwners();
    if(discoveryBytes_)IOFree(discoveryBytes_,kDeviceDiscoveryMaxBytes);
    if(romBytes_)IOFree(romBytes_,runtimerom::kRecommendedCapacity);
    registers_=nullptr;
    if(registerMap_)registerMap_->release();if(device_)device_->release();if(openOwner_)openOwner_->release();
    if(lock_)IOLockFree(lock_);OSObject::free();
}
