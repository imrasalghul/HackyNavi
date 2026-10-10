// SPDX-License-Identifier: MIT
#include "HackyNaviEngineController.hpp"
#include "HackyNaviClientSession.hpp"
#include "HackyNaviPciAccess.hpp"
#include "HackyNaviReadCapture.hpp"
#include <hackynavi/boot_display_capture.hpp>
#include <hackynavi/boot_display_stop_capture.hpp>
#include <hackynavi/cold_dma_capture.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

#if !defined(__x86_64__)
#error HackyNaviEngineController requires native coherent x86_64 DMA
#endif
OSDefineMetaClassAndStructors(HackyNaviEngineController,OSObject)
using namespace hackynavi;
namespace {
const bootstrap::PollOptions kBootstrapPoll={100000,10};
const psp::Options kSosPoll={10000,10000,1000};
const pspring::PollOptions kPspPoll={10000,100};
const gfxqueue::PollOptions kGraphicsPoll={100000,10};
const gfxinit::RetirementPollOptions kRlcPoll={100000,10};
const dmcubfw::PollOptions kDisplayPoll={100000,10};
const bootdisplaystop::PollOptions kScanoutPoll={100000,1};
bool evidenceNumber(OSDictionary* d,const char* key,uint64_t value,unsigned bits=32) {
    auto* n=OSNumber::withNumber(value,bits);if(!n)return false;
    const bool ok=d->setObject(key,n);n->release();return ok;
}
bool evidenceString(OSDictionary* d,const char* key,const char* value) {
    auto* s=OSString::withCString(value);if(!s)return false;
    const bool ok=d->setObject(key,s);s->release();return ok;
}
bool evidenceBool(OSDictionary* d,const char* key,bool value) {
    return d->setObject(key,value?kOSBooleanTrue:kOSBooleanFalse);
}
template<class Trace> void publishReadEvidence(IOService* owner,const char* key,
    const HackyNaviReadCapture<Trace>& trace,const char* status) {
    auto* report=OSDictionary::withCapacity(4);
    const bool ok=report&&evidenceString(report,"ProtocolStatus",status)&&
        evidenceBool(report,"HistoricalEvidenceOnly",true)&&evidenceBool(report,"CurrentHardwareAuthority",false)&&
        trace.publish(report,"RawReadJournal")&&owner->setProperty(key,report);
    if(report)report->release();
    if(!ok)owner->setProperty("EngineReadEvidencePublicationIncomplete",kOSBooleanTrue);
}
void recordDisplayStop(const HackyNaviDisplayController::StopResult& result,
    HackyNaviEngineController::DisplayStopObservation& out) {
    out={result.result,result.clockRetirementResult,result.elapsedUs,true,
        result.consumersStopped,result.physicalIdleChecked,result.firmwareStopped,
        result.runtimeRetained,result.policyRetained,result.leaseReleased,
        result.coordinatedClockRetirementAttempted};
    IOLog("HackyNavi display retirement: result=0x%x consumers=%u physical-idle=%u firmware-stopped=%u "
        "runtime-retained=%u policy-retained=%u lease-released=%u clock-attempted=%u clock-result=0x%x elapsed-us=%llu\n",
        unsigned(out.result),unsigned(out.consumersStopped),unsigned(out.physicalIdleChecked),
        unsigned(out.firmwareStopped),unsigned(out.runtimeRetained),unsigned(out.policyRetained),
        unsigned(out.leaseReleased),unsigned(out.clockRetirementAttempted),unsigned(out.clockRetirementResult),
        static_cast<unsigned long long>(out.elapsedUs));
}
void recordDisplayTaRetirement(const HackyNaviFirmwareController::DisplayTrustedResult& result,
    HackyNaviEngineController::DisplayTaRetirementObservation& out) {
    out={result.result,result.generation,result.elapsedUs,
        uint32_t(result.protocol.status),result.protocol.command,result.protocol.applicationStatus,
        uint32_t(result.protocol.psp.status),uint32_t(result.application.ring.status),
        result.destroyedAuthentications,result.removedDisplays,result.unloadedApplications,
        true,result.protocolAttempted,result.applicationAttempted,result.bindingRetired,result.retentionRequired};
    IOLog("HackyNavi display TA retirement: result=0x%x protocol-attempted=%u application-attempted=%u "
        "protocol=%u command=%u application-status=0x%x auth-destroyed=%u topology-removed=%u "
        "TA-unloaded=%u binding-retired=%u retained=%u elapsed-us=%llu\n",
        unsigned(out.result),unsigned(out.protocolAttempted),unsigned(out.applicationAttempted),
        out.protocolStatus,out.command,out.applicationStatus,out.authenticationsDestroyed,
        out.topologiesRemoved,out.applicationsUnloaded,unsigned(out.protocolRetired),
        unsigned(out.retentionRequired),static_cast<unsigned long long>(out.elapsedUs));
}
static_assert(sizeof(HackyNaviEngineController::Observation)<=1024,"bounded native engine observation");
}
IOReturn HackyNaviEngineController::createClientSession(HackyNaviFunctionGroup* group,
    HackyNaviFunctionGroup::Operation& scope,HackyNaviClientSession*& out) {
    out=nullptr;IOLockLock(lock_);IOReturn result=kIOReturnNotReady;
    if(state_==State::Ready&&gart_&&graphics_&&scope.belongsTo(group)&&scope.matchesGraphics(device_,openOwner_)) {
        HackyNaviGARTController::Access access(*gart_,scope);
        if(access.normal())out=HackyNaviClientSession::withOwners(group,scope,gart_,graphics_,
            openOwner_->getRegistryEntryID(),memory_,result);
        else result=kIOReturnOffline;
    }
    IOLockUnlock(lock_);return result;
}
bool HackyNaviEngineController::init() {
    lock_=nullptr;device_=nullptr;openOwner_=nullptr;registerMap_=nullptr;
    registers_=nullptr;registerBytes_=0;discoveryBytes_=romBytes_=nullptr;
    mmioActive_=hardwareHold_=controlWritesStarted_=busMasterChanged_=false;
    resetPreparationAttempted_=stopAttempted_=stageRecorded_=false;
    startupScope_=nullptr;retirementScope_=nullptr;firstAttach_=nullptr;
    state_=State::Empty;stage_=Stage::None;result_=kIOReturnSuccess;
    factoryTeardownFailure_=kIOReturnSuccess;
    lastRegister_=lastValue_=0;observation_={};discoveryStatus_=DeviceDiscoveryStatus::InvalidArgument;
    prerequisiteEvidence_=nullptr;stageEvidence_=nullptr;
    ipStatus_=IpLayoutStatus::InvalidLayout;memoryStatus_=MemoryLayoutStatus::InvalidArgument;
    firmwareInfoStatus_=Status::NotFound;reservationStatus_=VramReservationStatus::MissingEvidence;
    nbioStatus_=NbioStatus::InvalidState;
    discovery_={};rom_={};ip_={};memory_={};firmwareInfo_={};usage_={};
    usageStatus_=Status::NotFound;bootScanoutResult_={};coldDma_={};bootstrap_={};
    auxiliaryLayoutStatus_=coldaux::Status::InvalidArgument;lsdma_={};
    lsdma_.status=coldaux::LsdmaStatus::InvalidArgument;audioStop_={};
    bootConsole_={};bootConsole_.result=kIOReturnNotReady;
    inheritedDisplay_={};reservations_={};nbioResult_={};bootConfiguration_=0;writeTest_={};shaderTest_={};reuseTest_={};
    gart_=nullptr;firmware_=nullptr;graphics_=nullptr;display_=nullptr;bootBackend_=nullptr;
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
    prerequisiteEvidence_=OSDictionary::withCapacity(12);stageEvidence_=OSArray::withCapacity(80);
    if(!discoveryBytes_ || !romBytes_ || !prerequisiteEvidence_ || !stageEvidence_)return kIOReturnNoMemory;
    state_=State::Prepared;return kIOReturnSuccess;
}
bool HackyNaviEngineController::providerReady(bool dma) const {
    if(startupScope_&&!startupScope_->matchesGraphics(device_,openOwner_))return false;
    if(retirementScope_&&!retirementScope_->matchesGraphics(device_,openOwner_))return false;
    if(!nativepci::ready(device_,openOwner_,dma?nativepci::BusMaster::Enabled:nativepci::BusMaster::Any))
        return false;
    return (!startupScope_||startupScope_->matchesGraphics(device_,openOwner_))&&
        (!retirementScope_||retirementScope_->matchesGraphics(device_,openOwner_));
}
IOReturn HackyNaviEngineController::programBusMaster(bool enabled) {
    if(!hardwareHold_ || !providerReady())return kIOReturnOffline;
    const UInt16 command=device_->configRead16(kIOPCIConfigCommand);
    if(command==0xffff || !(command&2))return kIOReturnOffline;
    const UInt16 requested=enabled?UInt16(command|4):UInt16(command&~UInt16(4));
    // A possibly landed PCI write belongs to this epoch even if readback
    // fails. Use the 16-bit command access: a 32-bit RMW also writes the PCI
    // status W1C bits, and setBusMasterEnable returns the previous bit value.
    controlWritesStarted_=busMasterChanged_=true;
    device_->configWrite16(kIOPCIConfigCommand,requested);
    if(device_->configRead16(kIOPCIConfigCommand)!=requested)return kIOReturnIOError;
    return providerReady(enabled)?kIOReturnSuccess:kIOReturnOffline;
}
IOReturn HackyNaviEngineController::startupResult(IOReturn result) const {
    if(result!=kIOReturnSuccess)return result;
    return startupScope_&&startupScope_->matchesGraphics(device_,openOwner_)?kIOReturnSuccess:kIOReturnOffline;
}
bool HackyNaviEngineController::read32(void* context,uint32_t offset,uint32_t& value) {
    auto* self=static_cast<HackyNaviEngineController*>(context);
    self->lastRegister_=offset;
    if(!self->mmioActive_ || !self->registers_ || (offset&3) ||
        offset>self->registerBytes_-4 || !self->providerReady())return false;
    __asm__ __volatile__("" ::: "memory");value=self->registers_[offset/4];
    OSSynchronizeIO();__asm__ __volatile__("" ::: "memory");self->lastValue_=value;
    return (!self->startupScope_||self->startupScope_->matchesGraphics(self->device_,self->openOwner_))&&
        (!self->retirementScope_||self->retirementScope_->matchesGraphics(self->device_,self->openOwner_));
}
bool HackyNaviEngineController::write32(void* context,uint32_t offset,uint32_t value) {
    auto* self=static_cast<HackyNaviEngineController*>(context);
    self->lastRegister_=offset;self->lastValue_=value;
    if(!self->hardwareHold_ || !self->mmioActive_ || !self->registers_ || (offset&3) ||
        offset>self->registerBytes_-4 || !self->providerReady())return false;
    __asm__ __volatile__("" ::: "memory");self->registers_[offset/4]=value;
    OSSynchronizeIO();__asm__ __volatile__("" ::: "memory");
    return (!self->startupScope_||self->startupScope_->matchesGraphics(self->device_,self->openOwner_))&&
        (!self->retirementScope_||self->retirementScope_->matchesGraphics(self->device_,self->openOwner_));
}
void HackyNaviEngineController::delayUs(void*,uint32_t us) {
    if(us>=1000)IOSleep((us+999)/1000);else IODelay(us);
}
void HackyNaviEngineController::holdHardware() {
    if(!hardwareHold_) { retain();hardwareHold_=true; }
}
void HackyNaviEngineController::enterStage(Stage next) {
    if(stage_!=Stage::None&&!stageRecorded_)recordStageEvidence(kIOReturnSuccess,true);
    stage_=next;
    stageRecorded_=false;
    IOLog("HackyNavi engines: enter=%s control-writes-started=%u\n",
        stageString(next),unsigned(controlWritesStarted_));
}
void HackyNaviEngineController::snapshotObservation(Observation& out) {
    IOLockLock(lock_);out=observation_;out.controlWritesStarted=controlWritesStarted_;
    out.scanout=bootScanoutResult_;out.console=bootConsole_;out.cp=writeTest_;out.shader=shaderTest_;out.reuse=reuseTest_;
    IOLockUnlock(lock_);
}
IOReturn HackyNaviEngineController::finish(IOReturn result) {
    if(stage_!=Stage::None&&!stageRecorded_)recordStageEvidence(result,false);
    result_=result;mmioActive_=false;startupScope_=nullptr;retirementScope_=nullptr;firstAttach_=nullptr;
    IOLockUnlock(lock_);return result;
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
    nativepci::AssignedMemory resource(device_,kIOPCIConfigBaseAddress0);
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
bool HackyNaviEngineController::preflightAccessValid(void* context) {
    auto* self=static_cast<HackyNaviEngineController*>(context);
    return self->startupScope_&&self->mmioActive_&&!self->controlWritesStarted_&&self->providerReady();
}
bool HackyNaviEngineController::captureRawEvidence(const char* key,const uint32_t* offsets,size_t count,uint32_t zeroGuard) {
    // CPU-only immutable field records. A semantic value never cuts short the
    // remaining fields; transport/all-ones stops this dependent register bank.
    auto* report=OSDictionary::withCapacity(6);auto* fields=OSArray::withCapacity(unsigned(count));
    if(!report||!fields){if(report)report->release();if(fields)fields->release();
        (void)evidenceBool(prerequisiteEvidence_,"PublicationIncomplete",true);return false;}
    bool complete=true,published=true;size_t readCount=0;const char* stop="Complete";
    for(size_t i=0;i<count;++i) {
        auto* field=OSDictionary::withCapacity(6);
        if(!field){complete=published=false;stop="NoMemory";break;}
        uint32_t value=0;
        const bool attempted=preflightAccessValid(this);
        const bool succeeded=attempted&&read32(this,offsets[i],value);
        const bool valid=succeeded&&value!=UINT32_MAX;
        if(attempted)++readCount;
        published=evidenceNumber(field,"Index",i)&&evidenceNumber(field,"Register",offsets[i])&&
            evidenceNumber(field,"Value",value)&&evidenceBool(field,"Attempted",attempted)&&
            evidenceBool(field,"ReadSucceeded",succeeded)&&evidenceBool(field,"Valid",valid)&&
            fields->setObject(field)&&published;
        field->release();
        // A zero-SOS prerequisite is access policy, not an ordinary engine
        // idle mismatch: never enter its dependent bank in a prior PSP epoch.
        if(!valid||(zeroGuard&&offsets[i]==zeroGuard&&value)) {
            complete=false;stop=!attempted?"AccessRevoked":!succeeded?"ReadFailed":
                value==UINT32_MAX?"AllOnes":"NonzeroSOS";break;
        }
    }
    published=evidenceNumber(report,"ExpectedFields",count)&&evidenceNumber(report,"ReadsAttempted",readCount)&&
        evidenceBool(report,"ReadsComplete",complete)&&evidenceBool(report,"CurrentHardwareAuthority",false)&&
        evidenceNumber(report,"ZeroGuardRegister",zeroGuard)&&evidenceString(report,"StopReason",stop)&&
        report->setObject("Fields",fields)&&prerequisiteEvidence_->setObject(key,report)&&published;
    if(!published)(void)evidenceBool(prerequisiteEvidence_,"PublicationIncomplete",true);
    report->release();fields->release();return complete&&published;
}
__attribute__((noinline)) void HackyNaviEngineController::captureCpGuardEvidence() {
    auto* captured=static_cast<bootstrap::GuardInventory*>(IOMalloc(sizeof(bootstrap::GuardInventory)));
    auto* report=OSDictionary::withCapacity(16);
    auto* fields=OSArray::withCapacity(bootstrap::kGuardInventoryWords);
    if(!captured||!report||!fields) {
        if(captured)IOFree(captured,sizeof(*captured));if(report)report->release();if(fields)fields->release();
        (void)evidenceString(prerequisiteEvidence_,"CpSdmaInventoryStatus","NoMemory");return;
    }
    const auto status=bootstrap::captureGuardInventory(ip_,registerBytes_,{this,preflightAccessValid,read32},*captured);
    bool valid=evidenceString(report,"Status",bootstrap::guardInventoryStatusString(status))&&
        evidenceString(report,"LayoutStatus",bootstrap::statusString(captured->layoutStatus))&&
        evidenceNumber(report,"Reads",captured->reads)&&evidenceBool(report,"ReadsComplete",captured->complete)&&
        evidenceBool(report,"ArchitectureCompared",captured->architectureCompared)&&
        evidenceBool(report,"ArchitectureMatches",captured->architectureMatch)&&evidenceBool(report,"Rs64",captured->rs64)&&
        evidenceNumber(report,"FinalSOS",captured->sosAfter)&&evidenceBool(report,"FinalSOSAttempted",captured->sosAfterAttempted)&&
        evidenceBool(report,"FinalSOSReadSucceeded",captured->sosAfterReadSucceeded)&&
        evidenceBool(report,"FinalSOSValid",captured->sosAfterValid)&&evidenceBool(report,"CurrentHardwareAuthority",false);
    for(size_t i=0;i<bootstrap::kGuardInventoryWords;++i) {
        auto* field=OSDictionary::withCapacity(10);if(!field){valid=false;break;}
        const auto bit=uint32_t(1)<<(i%32);const auto word=i/32;
        valid=evidenceNumber(field,"Index",i)&&evidenceNumber(field,"Register",captured->offsets[i])&&
            evidenceNumber(field,"Value",captured->values[i])&&evidenceNumber(field,"GuardMask",captured->guardMasks[i])&&
            evidenceNumber(field,"GuardExpected",captured->guardExpected[i])&&
            evidenceBool(field,"Attempted",captured->attempted[word]&bit)&&
            evidenceBool(field,"ReadSucceeded",captured->readSucceeded[word]&bit)&&
            evidenceBool(field,"Valid",captured->valid[word]&bit)&&
            evidenceBool(field,"GuardCompared",captured->guardCompared[word]&bit)&&
            evidenceBool(field,"GuardMismatch",captured->guardMismatch[word]&bit)&&fields->setObject(field)&&valid;
        field->release();
    }
    valid=report->setObject("Fields",fields)&&prerequisiteEvidence_->setObject("CpSdmaGuards",report)&&valid;
    if(!valid)(void)evidenceBool(prerequisiteEvidence_,"PublicationIncomplete",true);
    fields->release();report->release();IOFree(captured,sizeof(*captured));
}
__attribute__((noinline)) void HackyNaviEngineController::capturePrerequisiteEvidence(bool postedCold) {
    if(!prerequisiteEvidence_)return;
    (void)evidenceBool(prerequisiteEvidence_,"HistoricalEvidenceOnly",true);
    (void)evidenceBool(prerequisiteEvidence_,"CurrentHardwareAuthority",false);
    (void)evidenceBool(prerequisiteEvidence_,"BeforeControlWrites",!controlWritesStarted_);
    (void)evidenceNumber(prerequisiteEvidence_,"OperationGeneration",startupScope_?startupScope_->generation():0,64);
    psp::Layout training{};
    const auto trainingLayout=psp::buildLayout(ip_,registerBytes_,training);
    (void)evidenceString(prerequisiteEvidence_,"TrainingLayout",psp::statusString(trainingLayout));
    if(trainingLayout==psp::Status::Ok&&preflightAccessValid(this)) {
        const uint32_t offsets[]={training.heartbeat,training.command,training.heartbeat,training.command,training.heartbeat};
        (void)captureRawEvidence("Training",offsets,sizeof(offsets)/sizeof(offsets[0]),training.heartbeat);
    }
    (void)evidenceBool(prerequisiteEvidence_,"PostedColdObserved",postedCold);
    // The LSDMA/CP cold windows keep their existing posted-cold prerequisite.
    // An unknown/prior-driver state grants no right to inspect those windows.
    if(postedCold&&preflightAccessValid(this)) {
        coldaux::Layout auxiliary{};
        const auto layout=coldaux::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,auxiliary);
        (void)evidenceString(prerequisiteEvidence_,"LsdmaLayout",coldaux::statusString(layout));
        if(layout==coldaux::Status::Ok) {
            uint32_t offsets[2*(coldaux::kLsdmaRegisters+1)+1]{};size_t n=0;
            for(unsigned pass=0;pass<2;++pass) {
                offsets[n++]=auxiliary.sos;
                for(const auto offset:auxiliary.lsdma)offsets[n++]=offset;
            }
            offsets[n++]=auxiliary.sos;(void)captureRawEvidence("Lsdma",offsets,n,auxiliary.sos);
        }
        dmcubfw::Layout dmub{};
        const auto dmubLayout=dmcubfw::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,dmub);
        (void)evidenceString(prerequisiteEvidence_,"DmcubLayout",dmcubfw::statusString(dmubLayout));
        if(dmubLayout==dmcubfw::Status::Ok&&preflightAccessValid(this)) {
            size_t n=0;while(n<sizeof(dmub.regs)/sizeof(dmub.regs[0])&&dmub.regs[n])++n;
            (void)captureRawEvidence("InheritedDmcub",dmub.regs,n);
        }
        if(preflightAccessValid(this))captureCpGuardEvidence();
    }
    (void)evidenceBool(prerequisiteEvidence_,"OperationStillCurrent",preflightAccessValid(this));
    // Subsequent stages never mutate this builder. CPU-only diagnostic data
    // cannot substitute for the fresh training/LSDMA/CP/DMUB admission below.
    if(!openOwner_->setProperty("EnginePreflightEvidence",prerequisiteEvidence_))
        IOLog("HackyNavi: prerequisite evidence publication failed\n");
}

__attribute__((noinline)) void HackyNaviEngineController::recordStageEvidence(IOReturn result,bool completed) {
    stageRecorded_=true;
    if(!stageEvidence_||stageEvidence_->getCount()>=80) {
        if(openOwner_)openOwner_->setProperty("EngineStageEvidenceTruncated",kOSBooleanTrue);
        return;
    }
    auto* entry=OSDictionary::withCapacity(20);
    if(!entry){openOwner_->setProperty("EngineStageEvidenceTruncated",kOSBooleanTrue);return;}
    bool ok=evidenceString(entry,"Stage",stageString(stage_))&&evidenceNumber(entry,"Result",uint32_t(result))&&
        evidenceBool(entry,"ReachedNextStage",completed)&&evidenceBool(entry,"ControlWritesStarted",controlWritesStarted_)&&
        evidenceBool(entry,"HistoricalEvidenceOnly",true)&&evidenceBool(entry,"CurrentHardwareAuthority",false)&&
        evidenceNumber(entry,"LastRegister",lastRegister_)&&evidenceNumber(entry,"LastValue",lastValue_);
    const auto number=[&](const char* key,uint64_t value,unsigned bits=32){ok=evidenceNumber(entry,key,value,bits)&&ok;};
    const auto text=[&](const char* key,const char* value){ok=evidenceString(entry,key,value)&&ok;};
    switch(stage_) {
    case Stage::Discovery:text("ProtocolStatus",deviceDiscoveryStatusString(discoveryStatus_));number("BytesCopied",discovery_.bytesCopied,64);break;
    case Stage::Rom:text("ProtocolStatus",runtimerom::statusString(rom_.status));number("BytesRead",rom_.bytesRead,64);break;
    case Stage::Memory:text("ProtocolStatus",memoryLayoutStatusString(memoryStatus_));number("VRAMBytes",memory_.vramBytes,64);break;
    case Stage::Posted:case Stage::StopEngines:
        text("ProtocolStatus",bootstrap::statusString(bootstrap_.status));number("Reads",bootstrap_.readAttempts);
        number("Writes",bootstrap_.writeAttempts);number("SOS",bootstrap_.sosSignOfLife);number("Scratch7",bootstrap_.biosScratch7);
        number("ProtocolLastRegister",bootstrap_.lastOffset);number("ProtocolLastValue",bootstrap_.lastValue);break;
    case Stage::Auxiliary:
        text("ProtocolStatus",coldaux::statusString(lsdma_.status));number("Reads",lsdma_.reads);
        number("Mask",lsdma_.mask);number("Expected",lsdma_.expected);number("ProtocolLastRegister",lsdma_.lastRegister);
        number("ProtocolLastValue",lsdma_.lastValue);break;
    case Stage::StopSiblingAudio:
        number("ProtocolResult",uint32_t(audioStop_.result));number("ProtocolStage",uint32_t(audioStop_.stage));
        number("CommandBefore",audioStop_.commandBefore);number("CommandAfter",audioStop_.commandAfter);
        number("PendingPolls",audioStop_.pendingPolls);number("ElapsedUs",audioStop_.elapsedUs,64);break;
    case Stage::StopInheritedDisplayFirmware:
        text("ProtocolStatus",dmcubfw::statusString(inheritedDisplay_.status));number("Reads",inheritedDisplay_.reads);
        number("Writes",inheritedDisplay_.writes);number("ProtocolLastRegister",inheritedDisplay_.lastRegister);
        number("ProtocolLastValue",inheritedDisplay_.lastValue);break;
    case Stage::ColdDma:
        text("ProtocolStatus",colddma::statusString(coldDma_.status));number("Reads",coldDma_.reads);
        number("Mask",coldDma_.mask);number("Expected",coldDma_.expected);number("Pipe",coldDma_.pipe);
        number("ProtocolLastRegister",coldDma_.lastRegister);number("ProtocolLastValue",coldDma_.lastValue);break;
    case Stage::Reservations: {
        text("ProtocolStatus",vramReservationStatusString(reservationStatus_));number("MissingEvidence",reservations_.missingEvidence);
        number("ReservedBytes",reservations_.reservedBytes,64);
        auto* ranges=OSArray::withCapacity(unsigned(reservations_.count));
        if(ranges) {
            for(size_t i=0;i<reservations_.count;++i) {
                auto* range=OSDictionary::withCapacity(3);if(!range){ok=false;break;}
                ok=evidenceNumber(range,"Offset",reservations_.ranges[i].offset,64)&&
                    evidenceNumber(range,"Bytes",reservations_.ranges[i].bytes,64)&&
                    evidenceNumber(range,"Provenance",reservations_.provenance[i])&&ranges->setObject(range)&&ok;
                range->release();
            }
            ok=entry->setObject("Ranges",ranges)&&ok;ranges->release();
        } else ok=false;
        break;
    }
    default:break;
    }
    const char* readKey=stage_==Stage::PreflightBootDisplay?"EngineBootDisplayPreflightReadEvidence":
        stage_==Stage::BootDisplay?"EngineBootDisplayOwnedReadEvidence":
        stage_==Stage::ColdDma?"EngineColdDmaReadEvidence":nullptr;
    if(readKey) {
        auto* evidence=openOwner_->getProperty(readKey);
        if(evidence)ok=entry->setObject("ReadEvidence",evidence)&&ok;
    }
    if(firmware_) {
        number("FirmwareState",uint32_t(firmware_->state()));
        const auto& r=firmware_->lastRingResult();
        number("PspCommand",r.commandId);number("PspStatus",uint32_t(r.status));
        number("PspResponse",r.responseStatus);number("PspFenceExpected",r.fenceSequence);number("PspFenceObserved",r.observedFence);
    }
    if(ok)ok=stageEvidence_->setObject(entry);entry->release();
    // A copy of the array publishes immutable children; appending to our
    // private builder later cannot alter a prior IORegistry observation.
    auto* copy=OSArray::withArray(stageEvidence_);
    if(copy){ok=openOwner_->setProperty("EngineStageEvidence",copy)&&ok;copy->release();}else ok=false;
    if(!ok)openOwner_->setProperty("EngineStageEvidenceTruncated",kOSBooleanTrue);
}
bool HackyNaviEngineController::makeReservations() {
    const auto& displayCapture=bootScanout_.capture();
    VramRange planes[bootdisplay::kPipeCount+1]={};
    if(displayCapture.planeCount>bootdisplay::kPipeCount||!bootConsole_.stable||
        (bootConsole_.result!=kIOReturnSuccess&&
         bootConsole_.reservationStatus!=eficonsole::Status::NotOnBar))return false;
    for(size_t i=0;i<displayCapture.planeCount;++i)planes[i]=displayCapture.planes[i].reservation;
    size_t planeCount=displayCapture.planeCount;
    // Platform console CPU writes can outlive inherited DCN scanout. Reserve
    // its entire mapped allocation, including pitch/offset padding, until the
    // real IOFramebuffer console handoff; never infer it from visible pixels.
    // A console on another BAR adds no range here, but does not replace the
    // independent complete DCN inventory requirement.
    if(bootConsole_.result==kIOReturnSuccess)planes[planeCount++]=bootConsole_.reservation.pages;
    VramReservationInput input={};input.memory=memory_;
    input.firmwareStatus=firmwareInfoStatus_;input.firmware=firmwareInfo_;
    input.firmwareEvidence=VramReservationEvidence::RomImage;
    input.usageStatus=usageStatus_;input.usage=usage_;
    input.usageEvidence=VramReservationEvidence::RomImage;
    input.preOsFramebuffers=planes;input.preOsFramebufferCount=planeCount;
    input.preOsFramebufferInventoryComplete=displayCapture.complete;
    input.discoveryCaptureStatus=discoveryStatus_;
    input.discoveryBinaryStatus=DiscoveryStatus::Ok;input.discovery=discovery_;
    // Only the actual one-use firmware-first admission permits deferring
    // untouched VCN/JPEG, as in the pinned Linux GMC-before-PSP/media sequence.
    // No media stop result is manufactured. LSDMA, CP/SDMA, DMCUB and the
    // IH/DWB/MCIF/MALL observations remain separate prerequisites. Recovery
    // cannot reuse this contract to retire previously published allocations.
    input.additionalReservationInventoryComplete=state_==State::Starting && stage_==Stage::Reservations &&
        firstAttach_&&startupScope_&&firstAttach_->validFor(*startupScope_,device_,openOwner_)&&
        bootScanout_.phase()==bootdisplaystop::Phase::Armed && bootstrap_.status==bootstrap::Status::Ok &&
        lsdma_.status==coldaux::LsdmaStatus::Ok && coldDma_.status==colddma::Status::Ok &&
        inheritedDisplay_.status==dmcubfw::Status::Ok && inheritedDisplay_.stage==dmcubfw::Stage::Complete;
    input.memoryTrainingInactive=checkTrainingIdle();
    reservationStatus_=buildVramReservations(input,reservations_);
    return reservationStatus_==VramReservationStatus::Ok && reservations_.complete;
}
// Keep the large read-only layout/capture off the startup coordinator's frame.
// The caller still owns the Engine mutex and actual startup access scope.
__attribute__((noinline)) bootdisplay::Status HackyNaviEngineController::captureDisplayPreflight() {
    HackyNaviReadCapture<bootdisplay::ReadCapture> readCapture;
    auto& preflight=observation_.preflightDisplay;preflight.attempted=true;
    preflight.failedPipe=preflight.lastPowerProbe.pipe=0xff;
    bootdisplay::Layout layout={};
    preflight.status=bootdisplay::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,layout);
    if(preflight.status==bootdisplay::Status::Ok) {
        bootdisplay::Capture captured={};
        preflight.status=bootdisplay::captureRecorded(layout,memory_,{this,read32},captured,readCapture.get(),true);
        preflight.reads=captured.readsAttempted;preflight.lastRegister=captured.lastRegister;
        preflight.lastValue=captured.lastValue;preflight.lastReadSucceeded=captured.lastReadSucceeded;
        preflight.failedPipe=captured.failedPipe;preflight.lastPowerProbe=captured.lastPowerProbe;
    }
    publishReadEvidence(openOwner_,"EngineBootDisplayPreflightReadEvidence",readCapture,bootdisplay::statusString(preflight.status));
    return preflight.status;
}
IOReturn HackyNaviEngineController::observeBootBackend(HackyNaviFunctionGroup::Operation& scope) {
    auto& out=observation_.backend;out={};out.attempted=true;
    out.path=out.backend=out.frontend=out.timing=out.opp=out.mpcc=out.dpp=0xff;
    IOReturn result=kIOReturnNoMemory;
    bootBackend_=HackyNaviBootBackendCapture::observe(scope,device_,openOwner_,registerMap_,
        discoveryBytes_,discovery_.binarySize,romBytes_,rom_.bytesRead,rom_.image.offset,1000000,result);
    if(bootBackend_) {
        out.report=bootBackend_->report();const auto& c=bootBackend_->snapshot();out.valid=c.valid;
        if(c.valid) {
            out.path=c.path;out.backend=c.backend;out.frontend=c.frontendIndex;out.timing=c.timingIndex;
            out.opp=c.opp;out.mpcc=c.mpcc;out.dpp=c.dpp;out.bitsPerComponent=c.bitsPerComponent;
            out.scrambled=c.scrambled;out.quarterRate=c.quarterRate;
        }
    } else out.report.result=result;
    IOLog("HackyNavi inherited backend: result=0x%x failure=%u status=%s reads=%u valid=%u route=%u/%u/%u/%u/%u/%u/%u\n",
        unsigned(result),unsigned(out.report.failure),bootbackend::statusString(out.report.classifier.status),
        out.report.mmioReads,unsigned(out.valid),out.path,out.backend,out.frontend,out.timing,out.opp,out.mpcc,out.dpp);
    return result;
}
IOReturn HackyNaviEngineController::start(HackyNaviFunctionGroup::Operation& scope) {
    IOLockLock(lock_);
    if(state_!=State::Prepared)return finish(kIOReturnNotReady);
    if(!scope.matchesGraphics(device_,openOwner_))return finish(kIOReturnOffline);
    startupScope_=&scope;
    if(!providerReady())return finish(kIOReturnOffline);
    state_=State::Starting;holdHardware();mmioActive_=true;
    enterStage(Stage::Discovery);
    discoveryStatus_=acquireDeviceDiscovery({this,read32,write32,delayUs},{2000,1000},discoveryBytes_,
        kDeviceDiscoveryMaxBytes,discovery_);
    if(discoveryStatus_!=DeviceDiscoveryStatus::Ok)return fail(kIOReturnIOError);
    IpLayoutDiagnostic diagnostic={};
    ipStatus_=buildNavi31IpLayout(discoveryBytes_,discovery_.binarySize,ip_,diagnostic);
    if(ipStatus_!=IpLayoutStatus::Ok)
        return fail(kIOReturnUnsupported);
    enterStage(Stage::Rom);runtimerom::Layout romLayout={};
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
    enterStage(Stage::Memory);if(!captureMemory())return fail(kIOReturnUnsupported);
    enterStage(Stage::Posted);
    const auto posted=bootstrap::checkPostedColdState(ip_,registerBytes_,firmwareInfo_.biosScratchRegisterDword,
        {this,read32,nullptr,nullptr},bootstrap_);
    capturePrerequisiteEvidence(posted==bootstrap::Status::Ok);
    if(!scope.matchesGraphics(device_,openOwner_))return fail(kIOReturnOffline);
    if(posted!=bootstrap::Status::Ok)return fail(kIOReturnNotReady);
    enterStage(Stage::Training);if(!checkTrainingIdle())return fail(kIOReturnNotReady);
    HackyNaviFunctionGroup::FirstAttach admission;
    if(scope.consumeFirstAttach(admission)!=kIOReturnSuccess||
        !admission.validFor(scope,device_,openOwner_))return fail(kIOReturnNotReady);
    firstAttach_=&admission;
    enterStage(Stage::Auxiliary);coldaux::Layout auxLayout={};
    auxiliaryLayoutStatus_=coldaux::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,auxLayout);
    if(auxiliaryLayoutStatus_!=coldaux::Status::Ok ||
        coldaux::checkLsdmaStopped(auxLayout,{this,read32},lsdma_)!=coldaux::LsdmaStatus::Ok)
        return fail(kIOReturnNotReady);
    // Negative preflight only: the inherited firmware can still change its
    // state. Reject unsupported inputs before HDA/DMCUB control writes; a
    // passing observation grants no exclusion, reservation or stop authority.
    // The separate owned scanout/console captures below remain mandatory.
    enterStage(Stage::PreflightBootDisplay);
    if(captureDisplayPreflight()!=bootdisplay::Status::Ok)return fail(kIOReturnUnsupported);
    enterStage(Stage::PreflightBootConsole);observation_.preflightConsoleAttempted=true;
    auto& consolePreflight=observation_.preflightConsole;
    if(bootconsole::capture(memory_,consolePreflight)!=kIOReturnSuccess&&
        !(consolePreflight.stable&&consolePreflight.reservationStatus==eficonsole::Status::NotOnBar))
        return fail(consolePreflight.result);
    enterStage(Stage::StopSiblingAudio);
    // Both PCI functions remain owned. Before device-wide firmware controls,
    // stop the actual audio controller and clear only its command BME bit.
    // Mark the attempt first; a failing callback may already have written it.
    controlWritesStarted_=true;
    const IOReturn audioResult=scope.quiesceAudio(1000000,audioStop_);
    if(audioResult!=kIOReturnSuccess)return fail(audioResult);
    enterStage(Stage::StopInheritedDisplayFirmware);dmcubfw::Layout inherited={};
    inheritedDisplay_.status=dmcubfw::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,inherited);
    if(inheritedDisplay_.status!=dmcubfw::Status::Ok)
        return fail(kIOReturnUnsupported);
    controlWritesStarted_=true;
    if(dmcubfw::stopInheritedCold(ip_,inherited,{this,read32,write32,nullptr,delayUs},kDisplayPoll,
        inheritedDisplay_)!=dmcubfw::Status::Ok)return fail(kIOReturnIOError);
    // This observes the downstream route after the actual inherited DMCUB
    // stop. It grants no takeover/clock authority and does not add writes.
    // Unsupported HDMI/DP/FRL profiles do not prevent offscreen engine work;
    // loss of the real Operation/PCI/map/deadline must stop startup itself.
    enterStage(Stage::ObserveBootBackend);
    const IOReturn backendResult=observeBootBackend(scope);
    if(backendResult!=kIOReturnSuccess&&backendResult!=kIOReturnUnsupported)return fail(backendResult);
    enterStage(Stage::BootDisplay);observation_.scanoutAttempted=true;bootdisplay::Layout displayLayout={};
    bootScanoutResult_.pipe=bootScanoutResult_.lastPowerProbe.pipe=0xff;
    bootScanoutResult_.captureStatus=bootdisplay::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,displayLayout);
    if(bootScanoutResult_.captureStatus!=bootdisplay::Status::Ok) {
        bootScanoutResult_.status=bootdisplaystop::Status::CaptureRejected;return fail(kIOReturnUnsupported);
    }
    {
        HackyNaviReadCapture<bootdisplaystop::ReadCapture> readCapture;
        const auto captureStatus=bootdisplaystop::captureRecorded(displayLayout,memory_,{this,read32,nullptr,nullptr},
            bootScanout_,bootScanoutResult_,readCapture.get());
        publishReadEvidence(openOwner_,"EngineBootDisplayOwnedReadEvidence",readCapture,bootdisplaystop::statusString(captureStatus));
        if(captureStatus!=bootdisplaystop::Status::Ok)return fail(kIOReturnUnsupported);
    }
    enterStage(Stage::StopEngines);
    if(bootstrap::quiesceCpSdma(ip_,registerBytes_,{this,read32,write32,delayUs},kBootstrapPoll,
        bootstrap_)!=bootstrap::Status::Ok)return fail(kIOReturnIOError);
    enterStage(Stage::Auxiliary);
    if(coldaux::checkLsdmaStopped(auxLayout,{this,read32},lsdma_)!=coldaux::LsdmaStatus::Ok)
        return fail(kIOReturnNotReady);
    enterStage(Stage::ColdDma);colddma::Layout dmaLayout={};
    coldDma_.status=colddma::buildLayout(discoveryBytes_,discovery_.binarySize,registerBytes_,dmaLayout);
    {
        HackyNaviReadCapture<colddma::ReadCapture> readCapture;
        if(coldDma_.status==colddma::Status::Ok)
            colddma::checkStoppedRecorded(dmaLayout,{this,read32},coldDma_,readCapture.get());
        publishReadEvidence(openOwner_,"EngineColdDmaReadEvidence",readCapture,colddma::statusString(coldDma_.status));
        if(coldDma_.status!=colddma::Status::Ok)return fail(kIOReturnNotReady);
    }
    enterStage(Stage::BootConsole);observation_.consoleAttempted=true;
    if(bootconsole::capture(memory_,bootConsole_)!=kIOReturnSuccess&&
        !(bootConsole_.stable&&bootConsole_.reservationStatus==eficonsole::Status::NotOnBar))
        return fail(bootConsole_.result);
    enterStage(Stage::Reservations);if(!makeReservations())return fail(kIOReturnUnsupported);
    // All existing cold admission/stop checks precede ownership changes. Do
    // not allocate or publish new DMA storage while inherited PCI BME may be
    // set. BME-off does not itself retire posted DMA or GPU-internal clients.
    enterStage(Stage::DisableBusMaster);IOReturn result=programBusMaster(false);
    if(result!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::Nbio);NbioLayout nbioLayout={};
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
    enterStage(Stage::AllocateGart);
    gart_=HackyNaviGARTController::withDevice(scope,device_,openOwner_,registerMap_,discoveryBytes_,
        discovery_.binarySize,config,reservations_.ranges,reservations_.count,result,&factoryTeardownFailure_);
    if(!gart_)return fail(factoryTeardownFailure_!=kIOReturnSuccess?factoryTeardownFailure_:result);
    // The factory's scope covers allocation only. Install this lexical binding
    // for every subsequent descendant MMIO/DMA callback through startup exit.
    HackyNaviGARTController::Access access(*gart_,scope);
    if(!access.normal())return fail(kIOReturnOffline);
    if(startupResult(kIOReturnSuccess)!=kIOReturnSuccess)return fail(kIOReturnOffline);
    enterStage(Stage::UploadGart);if((result=startupResult(gart_->upload()))!=kIOReturnSuccess)return fail(result);
    // GMC/MMHUB programming and its real invalidation/posting observations
    // precede enabling PCI DMA, matching the pinned Linux GMC-before-IH order.
    // The outer owner excludes every PCI/power/reset writer throughout startup.
    enterStage(Stage::Mmhub);if((result=startupResult(gart_->enableMmhub()))!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::BusMaster);
    if((result=programBusMaster(true))!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::Doorbell);
    nbioStatus_=setNbioDoorbellAperture({this,read32,write32},nbio_,true,nbioResult_);
    if(nbioStatus_!=NbioStatus::Ok)
        return fail(kIOReturnIOError);
    enterStage(Stage::FirmwareOwner);firmware_=HackyNaviFirmwareController::withController(gart_,result);
    if(!firmware_)return fail(result);
    if(startupResult(kIOReturnSuccess)!=kIOReturnSuccess)return fail(kIOReturnOffline);
    enterStage(Stage::Sos);if((result=startupResult(firmware_->loadSos(kSosPoll)))!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::PspRing);if((result=startupResult(firmware_->createRing(kPspPoll)))!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::BootConfig);
    if((result=startupResult(firmware_->queryBootConfiguration(kPspPoll,bootConfiguration_)))!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::Tmr);if((result=startupResult(firmware_->initializeTmr(kPspPoll)))!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::GraphicsFirmware);
    if((result=startupResult(firmware_->loadGraphicsFirmware(kPspPoll)))!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::Gfxhub);if((result=startupResult(gart_->enableGfxhub()))!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::GraphicsOwner);graphics_=HackyNaviGraphicsQueue::withFirmware(firmware_,result);
    if(!graphics_)return fail(result);
    if(startupResult(kIOReturnSuccess)!=kIOReturnSuccess)return fail(kIOReturnOffline);
    enterStage(Stage::GraphicsPrepare);if((result=startupResult(graphics_->prepare()))!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::GraphicsStart);if((result=startupResult(graphics_->start(kGraphicsPoll)))!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::GraphicsVerify);observation_.cpAttempted=true;
    if((result=startupResult(graphics_->runWriteTest(kGraphicsPoll,writeTest_)))!=kIOReturnSuccess || !writeTest_.completed)
        return fail(result==kIOReturnSuccess?kIOReturnIOError:result);
    enterStage(Stage::ShaderVerify);observation_.shaderAttempted=true;
    if((result=startupResult(graphics_->runShaderTest(kGraphicsPoll,shaderTest_)))!=kIOReturnSuccess || !shaderTest_.completed)
        return fail(result==kIOReturnSuccess?kIOReturnIOError:result);
    enterStage(Stage::ReusableVerify);observation_.reuseAttempted=true;
    if((result=startupResult(graphics_->runReuseTest(scope,kGraphicsPoll,reuseTest_)))!=kIOReturnSuccess || !reuseTest_.completed)
        return fail(result==kIOReturnSuccess?kIOReturnIOError:result);
    IOLog("HackyNavi reusable compute: jobs=%u words=%u guards=%u stale-rejections=%u controls-reused=%u owners=%u root=%u VMID=%u fences=%u/%u RPTR=%u/%u captures=%llu/%llu\n",
        reuseTest_.jobsCompleted,reuseTest_.wordsCompared,reuseTest_.guardWordsCompared,reuseTest_.staleRejections,
        reuseTest_.controlsReused,unsigned(reuseTest_.dataOwnersPreserved),unsigned(reuseTest_.rootPreserved),
        unsigned(reuseTest_.vmid),reuseTest_.firstFence,reuseTest_.secondFence,reuseTest_.firstRptr,reuseTest_.secondRptr,
        static_cast<unsigned long long>(reuseTest_.firstCapture),static_cast<unsigned long long>(reuseTest_.secondCapture));
    enterStage(Stage::DisplayOwner);
    display_=HackyNaviDisplayController::withFirmware(firmware_,romBytes_,rom_.bytesRead,rom_.image,result);
    if(!display_)return fail(result);
    if(startupResult(kIOReturnSuccess)!=kIOReturnSuccess)return fail(kIOReturnOffline);
    enterStage(Stage::DisplayBoot);if((result=startupResult(display_->boot(scope,kDisplayPoll)))!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::Complete);state_=State::Ready;
    IOLog("HackyNavi engines: GPU CP/shader/reuse verification completed (%u/%u/%u words); DAL DMCUB booted\n",
        writeTest_.wordsCompared,shaderTest_.wordsCompared,reuseTest_.wordsCompared);
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviEngineController::stop(HackyNaviFunctionGroup::RetirementScope& scope) {
    IOLockLock(lock_);
    if(factoryTeardownFailure_!=kIOReturnSuccess)return fail(factoryTeardownFailure_);
    if(state_==State::Retired)return finish(kIOReturnSuccess);
    if((state_==State::Prepared || state_==State::FailedUnpublished)&&
        !hardwareHold_&&!controlWritesStarted_) {
        state_=State::Retired;return finish(kIOReturnSuccess);
    }
    if(!scope.matchesGraphics(device_,openOwner_))return finish(kIOReturnOffline);
    retirementScope_=&scope;
    // Engine records startup failure independently of its descendants. A CPU
    // factory failure after a running queue need not imply ambiguous hardware.
    // Admit only the actual healthy owners; a stage number or a prior fence is
    // not retirement authority. A Prepared queue still owns its real CSIB;
    // Queue separately verifies no start/publication and an empty VM registry.
    // Without a queue, Firmware owns a separate one-shot RLC retirement;
    // it must observe zero CSIB/disabled SRM rather than invent CSIB ownership.
    if(stopAttempted_||resetPreparationAttempted_)return finish(kIOReturnNotPermitted);
    if(state_!=State::Ready&&state_!=State::FaultedRetained)return finish(kIOReturnNotReady);
    if(!gart_||!firmware_||
        gart_->state()!=HackyNaviGARTController::State::BothEnabled||
        firmware_->state()!=HackyNaviFirmwareController::State::FirmwareLoaded||
        (graphics_&&graphics_->state()!=HackyNaviGraphicsQueue::State::Running&&
            graphics_->state()!=HackyNaviGraphicsQueue::State::Prepared)||
        (!graphics_&&display_)||
        (display_&&display_->state()!=HackyNaviDisplayController::State::Prepared&&
            display_->state()!=HackyNaviDisplayController::State::Ready))
        return finish(kIOReturnNotReady);
    HackyNaviGARTController::Access access(*gart_,scope);
    if(!access.valid())return fail(kIOReturnOffline);
    if(!providerReady(true))return fail(kIOReturnOffline);
    stopAttempted_=true;
    mmioActive_=true;IOReturn result=kIOReturnSuccess;
    // Withdraw display consumers while firmwareReady is still true. DMCUB
    // must stay alive until queue/VM/RLC retirement and any coordinated clock
    // release have completed. No new native surface is owned by Engine yet;
    // Display independently rejects an outstanding surface or stream owner.
    enterStage(Stage::StopBootScanout);
    if(bootdisplaystop::stop({this,read32,write32,delayUs},kScanoutPoll,bootScanout_,bootScanoutResult_)!=bootdisplaystop::Status::Ok)
        return fail(kIOReturnIOError);
    // Retire only this firmware owner's actual typed display/session records
    // before its Display consumer and DMCUB dependencies can be torn down.
    // The genuine retirement scope and GART access remain held throughout.
    // No acquired TA is a successful no-op; an ambiguous firmware response
    // fails stop and leaves the normal recovery path responsible for reset.
    {
        enterStage(Stage::RetireDisplayTrustedApplications);
        HackyNaviFirmwareController::DisplayTrustedResult displayTa{};
        result=firmware_->retireDisplayTrustedApplications(scope,1000000,displayTa);
        recordDisplayTaRetirement(displayTa,observation_.displayTaRetirement);
        if(result!=kIOReturnSuccess)return fail(result);
    }
    if(display_) {
        enterStage(Stage::StopDisplayConsumers);
        HackyNaviDisplayController::StopResult displayStop{};
        result=display_->stopConsumers(scope,1000000,kDisplayPoll,displayStop);
        recordDisplayStop(displayStop,observation_.stopConsumers);
        if(result!=kIOReturnSuccess)return fail(result);
    }
    if(graphics_) {
        enterStage(Stage::StopGraphics);if((result=graphics_->stop(kGraphicsPoll))!=kIOReturnSuccess)return fail(result);
        if((result=graphics_->retireCompletedContexts({10000,10}))!=kIOReturnSuccess)return fail(result);
        enterStage(Stage::RetireRlc);
        if((result=graphics_->retireAfterQuiescence(kRlcPoll))!=kIOReturnSuccess)return fail(result);
    } else {
        enterStage(Stage::RetireRlc);
        if((result=firmware_->retireAutoloadRlc(scope,kRlcPoll))!=kIOReturnSuccess)return fail(result);
    }
    if(display_) {
        enterStage(Stage::StopDisplayFirmware);
        HackyNaviDisplayController::StopResult displayStop{};
        result=display_->stopFirmware(scope,1000000,kDisplayPoll,displayStop);
        recordDisplayStop(displayStop,observation_.stopFirmware);
        if(result!=kIOReturnSuccess)return fail(result);
        // A successful DMCUB STOP may still retain reset-only DAL/FAMS policy.
        // The Display method reports Busy in that case; never proceed to PSP
        // or GART release on a merely stopped firmware observation.
    }
    enterStage(Stage::RetireFirmware);
    HackyNaviFirmwareController::ClockResult clocks{};
    if((result=firmware_->retireClockState(scope,1000000,clocks))!=kIOReturnSuccess)return fail(result);
    if((result=firmware_->retireAfterQuiescence(kPspPoll))!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::RetireGart);if((result=gart_->retireAfterQuiescence())!=kIOReturnSuccess)return fail(result);
    enterStage(Stage::RetireNbio);
    nbioStatus_=disableNbioApertures({this,read32,write32},nbio_,nbioResult_);
    if(nbioStatus_!=NbioStatus::Ok)return fail(kIOReturnIOError);
    if(busMasterChanged_) {
        if((result=programBusMaster(false))!=kIOReturnSuccess)return fail(result);
        busMasterChanged_=false;
    }
    state_=State::Retired;const bool drop=hardwareHold_;hardwareHold_=false;
    releaseOwners();finish(kIOReturnSuccess);if(drop)release();return kIOReturnSuccess;
}
IOReturn HackyNaviEngineController::prepareClockTableForReset(HackyNaviFunctionGroup::RetirementScope& scope) {
    IOLockLock(lock_);
    if(resetPreparationAttempted_)return finish(kIOReturnNotPermitted);
    if(!scope.recoveryWindowOpen()||!scope.matchesGraphics(device_,openOwner_))return finish(kIOReturnOffline);
    resetPreparationAttempted_=true;retirementScope_=&scope;
    if(!firmware_)return finish(kIOReturnSuccess);
    if(!gart_)return fail(kIOReturnNotReady);
    HackyNaviGARTController::Access access(*gart_,scope);
    if(!access.valid())return fail(kIOReturnOffline);
    const auto remaining=scope.remainingUs();if(!remaining)return fail(kIOReturnTimeout);
    HackyNaviFirmwareController::ClockResult clocks{};
    const auto result=firmware_->prepareClockTableForReset(scope,remaining<1000000?remaining:1000000,clocks);
    if(result!=kIOReturnSuccess)return fail(result);
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviEngineController::releaseAfterVerifiedReset(const HackyNaviResetController::RetirementProof& proof,
    HackyNaviFunctionGroup::RetirementScope& scope) {
    IOLockLock(lock_);enterStage(Stage::VerifiedReset);IOReturn result=kIOReturnSuccess;
    if(!scope.matchesGraphics(device_,openOwner_))return finish(kIOReturnOffline);
    retirementScope_=&scope;
    // MODE1 proves hardware quiescence, not successful host DMA-domain cleanup.
    // Keep the epoch held if the failed GART factory already reported an error.
    if(factoryTeardownFailure_!=kIOReturnSuccess)return fail(factoryTeardownFailure_);
    // Early startup can fail before GART allocation. Null is deliberately an
    // invalid guard and grants nothing; any present GART requires real scope.
    // Its retain spans releaseOwners(), clearing the borrowed scope only when
    // this stack exits, even if the Engine drops its own last hardware hold.
    HackyNaviGARTController::Access access(gart_,scope);
    if(gart_&&!access.valid())return fail(kIOReturnOffline);
    result=proof.validateFor(device_,openOwner_,registerMap_);
    if(result!=kIOReturnSuccess)return finish(result);
    // The native reset owner checked these exact retained provider resources.
    // Release precisely the subowners that actually exist, including partial
    // startup states, while the outer epoch excludes every new producer.
    if(display_ && (result=display_->releaseAfterVerifiedReset(scope,proof))!=kIOReturnSuccess)return fail(result);
    if(graphics_ && (result=graphics_->releaseAfterDeviceReset())!=kIOReturnSuccess)return fail(result);
    // Reset proof covers every VMID and outstanding memory client. Retire the
    // native registry before tearing down firmware/GART, using the same proof
    // issuer and provider bindings; neither a fence nor detach ACK replaces it.
    if(gart_ && (result=gart_->retireVmContextsAfterVerifiedReset(proof))!=kIOReturnSuccess)return fail(result);
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
    NAME(RetireGart);NAME(RetireNbio);NAME(VerifiedReset);NAME(DisableBusMaster);NAME(BootConsole);NAME(StopSiblingAudio);
    NAME(PreflightBootDisplay);NAME(PreflightBootConsole);NAME(ReusableVerify);
    NAME(StopDisplayConsumers);NAME(StopDisplayFirmware);NAME(ObserveBootBackend);
    NAME(RetireDisplayTrustedApplications);
#undef NAME
    }
    return "Unknown";
}
void HackyNaviEngineController::logFailure() const {
    IOLog("HackyNavi engines: stage=%s result=0x%x retain=%u last BAR5[0x%x]=0x%x\n",
        stageString(stage_),unsigned(result_),unsigned(controlWritesStarted_),lastRegister_,lastValue_);
    switch(stage_) {
    case Stage::PreflightBootDisplay: {
        const auto& p=observation_.preflightDisplay;
        IOLog("HackyNavi display negative preflight: %s pipe=%u register=0x%x value=0x%x read=%u power-valid=0x%x status=0x%x config=0x%x after=0x%x\n",
            bootdisplay::statusString(p.status),p.failedPipe,p.lastRegister,p.lastValue,unsigned(p.lastReadSucceeded),
            p.lastPowerProbe.readsSucceededMask,p.lastPowerProbe.statusBefore,p.lastPowerProbe.config,p.lastPowerProbe.statusAfter);
        break; }
    case Stage::PreflightBootConsole: {
        const auto& p=observation_.preflightConsole;
        IOLog("HackyNavi console negative preflight: result=0x%x queries=0x%x/0x%x stable=%u range=%s\n",
            unsigned(p.result),unsigned(p.firstQuery),unsigned(p.secondQuery),unsigned(p.stable),
            eficonsole::statusString(p.reservationStatus));break; }
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
        IOLog("HackyNavi firmware-first LSDMA: layout=%s status=%s register=0x%x value=0x%x mask=0x%x expected=0x%x\n",
            coldaux::statusString(auxiliaryLayoutStatus_),coldaux::statusString(lsdma_.status),
            lsdma_.lastRegister,lsdma_.lastValue,lsdma_.mask,lsdma_.expected);break;
    case Stage::StopSiblingAudio:
        IOLog("HackyNavi sibling audio: stage=%u result=0x%x elapsed=%llu us complete=%u\n",
            unsigned(audioStop_.stage),unsigned(audioStop_.result),
            static_cast<unsigned long long>(audioStop_.elapsedUs),unsigned(audioStop_.complete));break;
    case Stage::ColdDma:
        IOLog("HackyNavi IH/writeback/MALL: %s register=0x%x value=0x%x mask=0x%x expected=0x%x pass=%u pipe=%u\n",
            colddma::statusString(coldDma_.status),coldDma_.lastRegister,coldDma_.lastValue,
            coldDma_.mask,coldDma_.expected,coldDma_.pass,coldDma_.pipe);break;
    case Stage::StopInheritedDisplayFirmware:
        IOLog("HackyNavi inherited DMCUB: %s step=%u register=0x%x value=0x%x writes=%u\n",
            dmcubfw::statusString(inheritedDisplay_.status),unsigned(inheritedDisplay_.stage),
            inheritedDisplay_.lastRegister,inheritedDisplay_.lastValue,inheritedDisplay_.writes);break;
    case Stage::BootDisplay:case Stage::StopBootScanout:
        IOLog("HackyNavi boot scanout: %s capture=%s pipe=%u register=0x%x value=0x%x read=%u writes=%u power-valid=0x%x status=0x%x config=0x%x after=0x%x\n",
            bootdisplaystop::statusString(bootScanoutResult_.status),bootdisplay::statusString(bootScanoutResult_.captureStatus),
            bootScanoutResult_.pipe,bootScanoutResult_.lastRegister,bootScanoutResult_.lastValue,
            unsigned(bootScanoutResult_.lastReadSucceeded),bootScanoutResult_.writes,
            bootScanoutResult_.lastPowerProbe.readsSucceededMask,bootScanoutResult_.lastPowerProbe.statusBefore,
            bootScanoutResult_.lastPowerProbe.config,bootScanoutResult_.lastPowerProbe.statusAfter);break;
    case Stage::BootConsole:
        IOLog("HackyNavi boot console: result=0x%x queries=0x%x/0x%x stable=%u range=%s\n",
            unsigned(bootConsole_.result),unsigned(bootConsole_.firstQuery),unsigned(bootConsole_.secondQuery),
            unsigned(bootConsole_.stable),eficonsole::statusString(bootConsole_.reservationStatus));break;
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
        stage_==Stage::DisplayBoot || stage_==Stage::StopDisplay ||
        stage_==Stage::StopDisplayConsumers || stage_==Stage::StopDisplayFirmware ||
        stage_==Stage::RetireDisplayTrustedApplications)) {
        const auto& s=firmware_->lastSosResult();const auto& r=firmware_->lastRingResult();
        const auto& d=firmware_->lastDisplayFirmwareResult();
        IOLog("HackyNavi firmware: SOS=%s step=%u register=0x%x value=0x%x; PSP=%s command=0x%x response=0x%x fence=%u/%u\n",
            psp::statusString(s.status),unsigned(s.stage),s.lastRegister,s.lastValue,pspring::statusString(r.status),
            r.commandId,r.responseStatus,r.observedFence,r.fenceSequence);
        IOLog("HackyNavi firmware: graphics=%s SMU=%s DMCUB=%s step=%u register=0x%x value=0x%x\n",
            gfxfw::statusString(firmware_->lastGraphicsResult().status),smu::statusString(firmware_->lastSmuResult().status),
            dmcubfw::statusString(d.status),unsigned(d.stage),d.lastRegister,d.lastValue);
    }
    if(graphics_ && (stage_==Stage::GraphicsPrepare || stage_==Stage::GraphicsStart || stage_==Stage::GraphicsVerify || stage_==Stage::ShaderVerify || stage_==Stage::ReusableVerify ||
        stage_==Stage::StopGraphics || stage_==Stage::RetireRlc)) {
        const auto& i=graphics_->lastInitResult();const auto& q=graphics_->lastQueueResult();
        IOLog("HackyNavi graphics: init=%s step=%u register=0x%x value=0x%x; queue=%s step=%u register=0x%x value=0x%x fence=%u RPTR=%u\n",
            gfxinit::statusString(i.status),unsigned(i.stage),i.lastRegister,i.lastValue,gfxqueue::statusString(q.status),
            unsigned(q.stage),q.lastRegister,q.lastValue,q.fenceValue,q.observedRptr);
        if(stage_==Stage::ShaderVerify)
            IOLog("HackyNavi shader: encode=%s dispatches=%u words=%u mismatch=%u expected=0x%x observed=0x%x\n",
                gfxcompute::statusString(graphics_->lastComputeStatus()),shaderTest_.dispatchesCompleted,
                shaderTest_.wordsCompared,shaderTest_.firstMismatch,shaderTest_.expected,shaderTest_.observed);
        if(stage_==Stage::ReusableVerify)
            IOLog("HackyNavi reusable compute: step=%s result=0x%x cleanup=0x%x retained=%u jobs=%u words=%u guards=%u stale-rejections=%u controls-reused=%u mismatch=%u expected=0x%x observed=0x%x\n",
                HackyNaviGraphicsQueue::reuseStepString(reuseTest_.step),unsigned(reuseTest_.result),unsigned(reuseTest_.cleanupResult),
                unsigned(reuseTest_.retainedOnFailure),reuseTest_.jobsCompleted,reuseTest_.wordsCompared,
                reuseTest_.guardWordsCompared,reuseTest_.staleRejections,reuseTest_.controlsReused,
                reuseTest_.firstMismatch,reuseTest_.expected,reuseTest_.observed);
    }
}
void HackyNaviEngineController::free() {
    if(prerequisiteEvidence_)prerequisiteEvidence_->release();
    if(stageEvidence_)stageEvidence_->release();
    releaseOwners();
    if(bootBackend_)bootBackend_->release();
    if(discoveryBytes_)IOFree(discoveryBytes_,kDeviceDiscoveryMaxBytes);
    if(romBytes_)IOFree(romBytes_,runtimerom::kRecommendedCapacity);
    registers_=nullptr;
    if(registerMap_)registerMap_->release();if(device_)device_->release();if(openOwner_)openOwner_->release();
    if(lock_)IOLockFree(lock_);OSObject::free();
}
