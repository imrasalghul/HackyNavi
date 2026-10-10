// SPDX-License-Identifier: MIT
// Ownership/PM contract checked against XNU 11417.140.69 IOService.cpp and
// IOServicePM.cpp, and IOPCIFamily 681.140.3 IOPCIDevice.cpp:handleOpen/Close.
// IOService attach supports multiple providers; separate clients are used here
// to retain independent, concrete power-state observations for both functions.
#include "HackyNaviFunctionGroup.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSArray.h>
#include <kern/clock.h>
#include <kern/debug.h>

OSDefineMetaClassAndStructors(HackyNaviFunctionGroup,OSObject)
OSDefineMetaClassAndStructors(HackyNaviFunctionLease,IOService)

namespace {
constexpr uint32_t kGraphicsIdentity=0x744c1002;
constexpr uint32_t kPowerRevoked=1,kTerminalRevoked=2;
constexpr uint32_t kRecoveryEntered=0x80000000U,kRetirementReasons=31;
constexpr uint32_t kNoRecoveryReasons=14;
constexpr uint32_t kRecoveryReasons=17; // PowerDown | OwnerFailure.
bool recoveryReasonSet(uint32_t flags) {
    const uint32_t reasons=flags&kRetirementReasons;
    return reasons&&(reasons&~kRecoveryReasons)==0;
}
enum RetirementPhase : uint32_t { RetirementIdle,RetirementQueued,RetirementDraining,
    RetirementCalling,RetirementComplete,RetirementFailed };
uint64_t uptimeTicks() {uint64_t value=0;clock_get_uptime(&value);return value;}
uint64_t lastInstance=0;
bool allocateInstance(uint64_t& serial) {
    uint64_t previous=__atomic_load_n(&lastInstance,__ATOMIC_RELAXED);
    do {
        if(previous==UINT64_MAX)return false;
    } while(!__atomic_compare_exchange_n(&lastInstance,&previous,previous+1,false,
        __ATOMIC_RELAXED,__ATOMIC_RELAXED));
    serial=previous+1;return true;
}
IOPMPowerState kStates[2]={
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,kIOPMPowerOn|kIOPMDeviceUsable,kIOPMPowerOn,kIOPMPowerOn,0,0,0,0,0,0,0,0}
};
bool identify(IOPCIDevice* pci,HackyNaviFunctionGroup::Identity& out) {
    if(!pci||pci->isInactive())return false;
    out={};out.bus=pci->getBusNumber();out.device=pci->getDeviceNumber();out.function=pci->getFunctionNumber();
    out.vendorDevice=pci->configRead32(0);out.revisionClass=pci->configRead32(8);
    out.subsystem=pci->configRead32(kIOPCIConfigSubSystemVendorID);
    const UInt8 header=pci->configRead8(kIOPCIConfigHeaderType);
    return out.vendorDevice!=UINT32_MAX && UInt16(out.vendorDevice)==0x1002 &&
        UInt16(out.vendorDevice>>16)!=0 && out.revisionClass!=UINT32_MAX &&
        UInt8(out.revisionClass)!=0xff && out.subsystem!=UINT32_MAX &&
        header!=0xff && (header&0x7f)==0 && pci->configRead32(0)==out.vendorDevice;
}
bool sameIdentity(const HackyNaviFunctionGroup::Identity& a,const HackyNaviFunctionGroup::Identity& b) {
    return a.vendorDevice==b.vendorDevice && a.revisionClass==b.revisionClass &&
        a.subsystem==b.subsystem && a.bus==b.bus && a.device==b.device && a.function==b.function;
}
}

bool HackyNaviFunctionGroup::init() {
    lock_=nullptr;teardownLock_=nullptr;operationLock_=nullptr;owner_=nullptr;bridge_=nullptr;leases_[0]=leases_[1]=nullptr;
    notify_=nullptr;instance_=0;generation_=1;acquired_=closing_=closed_=false;
    shutdownRequested_=0;
    primaryRevoked_=0;
    operationThread_=nullptr;
    activeOperation_=nullptr;operationGeneration_=0;
    retirementWaitLock_=nullptr;retirementCall_=retirementWatch_=nullptr;
    retirementHandler_=nullptr;retirementThread_=nullptr;registrationThread_=nullptr;
    retirementStart_=retirementBudgetTicks_=retirementGeneration_=0;
    retirementArmed_=retirementFlags_=retirementPhase_=retirementStage_=0;
    retirementWaiters_=retirementFunctions_=0;
    runtimeFailureOrigin_=0;
    firstAttachConsumed_=false;
    if(!OSObject::init())return false;
    lock_=IOLockAlloc();teardownLock_=IOLockAlloc();operationLock_=IOLockAlloc();
    retirementWaitLock_=IOLockAlloc();
    retirementCall_=thread_call_allocate_with_priority(retirementWorker,this,THREAD_CALL_PRIORITY_KERNEL);
    retirementWatch_=thread_call_allocate_with_priority(retirementWatchdog,this,THREAD_CALL_PRIORITY_HIGH);
    return lock_&&teardownLock_&&operationLock_&&retirementWaitLock_&&retirementCall_&&retirementWatch_&&
        allocateInstance(instance_);
}
HackyNaviFunctionGroup* HackyNaviFunctionGroup::withPrimary(IOPCIDevice* primary,
    IOService* owner,Notify notify,IOReturn& result,CreationObservation* observation) {
    if(observation) {*observation={};observation->stage=CreationStage::Allocation;
        observation->result=kIOReturnNoMemory;}
    result=kIOReturnNoMemory;auto* self=new HackyNaviFunctionGroup;
    if(!self)return nullptr;
    if(!self->init()){self->release();return nullptr;}
    CreationStage stage=CreationStage::Admission;
    result=self->initialize(primary,owner,notify,stage);
    // Retain actual pre-cleanup state even when the factory cannot return its
    // owner. snapshot is CPU-only and is safe only after init built the locks.
    // No provider/factory pointers escape in this observation.
    if(observation) {observation->stage=stage;observation->result=result;
        self->snapshot(observation->snapshot);observation->snapshotCaptured=true;}
    if(result!=kIOReturnSuccess){self->shutdown();self->release();return nullptr;}
    return self;
}
IOReturn HackyNaviFunctionGroup::findPair(IOPCIDevice* primary,IOPCIDevice*& audio) {
    audio=nullptr;Identity identity={};
    if(!identify(primary,identity)||identity.vendorDevice!=kGraphicsIdentity ||
        UInt8(identity.revisionClass)!=0xc8 || (identity.revisionClass>>24)!=3 || identity.function!=0)
        return kIOReturnUnsupported;
    bridge_=primary->getParentEntry(gIOServicePlane);
    if(!bridge_)return kIOReturnNotAttached;
    bridge_->retain();OSIterator* children=bridge_->getChildIterator(gIOServicePlane);
    if(!children)return kIOReturnNoMemory;
    IOReturn result=kIOReturnSuccess;unsigned found=0,visited=0;
    while(auto* object=children->getNextObject()) {
        if(++visited>1024){result=kIOReturnNoResources;break;}
        auto* pci=OSDynamicCast(IOPCIDevice,object);
        if(!pci || pci->getBusNumber()!=identity.bus || pci->getDeviceNumber()!=identity.device)continue;
        const UInt8 function=pci->getFunctionNumber();
        if(function==0 && pci==primary && !(found&1)) {found|=1;continue;}
        Identity other={};
        if(function!=1 || (found&2) || !identify(pci,other) ||
            (other.revisionClass>>8)!=0x040300) {result=kIOReturnUnsupported;break;}
        audio=pci;audio->retain();found|=2;
    }
    if(!children->isValid())result=kIOReturnNotReady;
    children->release();
    if(result==kIOReturnSuccess && found!=3)result=kIOReturnNotFound;
    if(result!=kIOReturnSuccess && audio){audio->release();audio=nullptr;}
    return result;
}
IOReturn HackyNaviFunctionGroup::initialize(IOPCIDevice* primary,IOService* owner,Notify notify,CreationStage& stage) {
    stage=CreationStage::Admission;
    if(!primary||!owner||!notify||owner->isInactive()||!primary->isOpen(owner))return kIOReturnBadArgument;
    owner_=owner;owner->retain();notify_=notify;
    stage=CreationStage::FindPair;
    IOPCIDevice* audio=nullptr;IOReturn result=findPair(primary,audio);
    if(result!=kIOReturnSuccess)return result;
    stage=CreationStage::GraphicsLease;leases_[0]=HackyNaviFunctionLease::create(this,primary,owner,result);
    if(leases_[0]) {stage=CreationStage::AudioLease;leases_[1]=HackyNaviFunctionLease::create(this,audio,nullptr,result);}
    audio->release();if(!leases_[0]||!leases_[1])return result;
    // Install the audio open before requesting either child's PM state. No
    // published group epoch exists during partial attachment or failed cleanup.
    stage=CreationStage::AudioBegin;result=leases_[1]->begin();if(result!=kIOReturnSuccess)return result;
    stage=CreationStage::GraphicsBegin;result=leases_[0]->begin();if(result!=kIOReturnSuccess)return result;
    stage=CreationStage::Topology;
    if(!topologyMatches())return kIOReturnNotReady;
    IOLockLock(lock_);acquired_=true;IOLockUnlock(lock_);
    changed(Event::Acquired,0xff);stage=CreationStage::Complete;return kIOReturnSuccess;
}
bool HackyNaviFunctionGroup::topologyMatches() const {
    if(!bridge_||!leases_[0]||!leases_[1])return false;
    auto* primary=leases_[0]->pci_;auto* audio=leases_[1]->pci_;
    if(!primary||!audio||primary->getParentEntry(gIOServicePlane)!=bridge_||
        audio->getParentEntry(gIOServicePlane)!=bridge_)return false;
    OSIterator* children=bridge_->getChildIterator(gIOServicePlane);if(!children)return false;
    unsigned found=0,visited=0;bool valid=true;
    while(auto* object=children->getNextObject()) {
        if(++visited>1024){valid=false;break;}
        auto* pci=OSDynamicCast(IOPCIDevice,object);
        if(!pci||pci->getBusNumber()!=leases_[0]->identity_.bus||
            pci->getDeviceNumber()!=leases_[0]->identity_.device)continue;
        const unsigned bit=pci==primary?1U:pci==audio?2U:0U;
        if(!bit || (found&bit)){valid=false;break;}found|=bit;
    }
    valid=valid && children->isValid() && found==3;children->release();return valid;
}
IOReturn HackyNaviFunctionGroup::checkPoweredLocked() {
    if(__atomic_load_n(&primaryRevoked_,__ATOMIC_ACQUIRE))return kIOReturnNotReady;
    if(__atomic_load_n(&retirementStart_,__ATOMIC_ACQUIRE))return kIOReturnNotReady;
    if(__atomic_load_n(&shutdownRequested_,__ATOMIC_ACQUIRE))return kIOReturnNotReady;
    if(!acquired_||closing_||closed_||!owner_||owner_->isInactive()||!leases_[0]||!leases_[1])return kIOReturnNotReady;
    if(__atomic_load_n(&generation_,__ATOMIC_ACQUIRE)==UINT64_MAX)return kIOReturnNotReady;
    return leases_[0]->validatePowered() && leases_[1]->validatePowered() && topologyMatches() &&
        !leases_[0]->accessRevoked() && !leases_[1]->accessRevoked()?
        kIOReturnSuccess:kIOReturnNotReady;
}
IOReturn HackyNaviFunctionGroup::captureEpoch(Epoch& out) {
    out=Epoch{};IOLockLock(lock_);
    const uint64_t before=__atomic_load_n(&generation_,__ATOMIC_ACQUIRE);
    IOReturn result=checkPoweredLocked();
    if(result==kIOReturnSuccess && before==__atomic_load_n(&generation_,__ATOMIC_ACQUIRE)) {
        out.issuer_=this;out.instance_=instance_;out.generation_=before;
    } else result=kIOReturnNotReady;
    IOLockUnlock(lock_);return result;
}
IOReturn HackyNaviFunctionGroup::validateEpoch(const Epoch& epoch) {
    IOLockLock(lock_);IOReturn result=kIOReturnNotReady;
    if(epoch.issuer_==this && epoch.instance_==instance_ &&
        epoch.generation_==__atomic_load_n(&generation_,__ATOMIC_ACQUIRE)) {
        result=checkPoweredLocked();
        if(epoch.generation_!=__atomic_load_n(&generation_,__ATOMIC_ACQUIRE))result=kIOReturnNotReady;
    }
    IOLockUnlock(lock_);return result;
}
HackyNaviFunctionGroup::Operation::Operation():issuer_(nullptr),thread_(nullptr),instance_(0),generation_(0),
    graphics_(nullptr),openOwner_(nullptr){}
HackyNaviFunctionGroup::Operation::~Operation(){if(issuer_)issuer_->endOperation(*this);}
bool HackyNaviFunctionGroup::Operation::valid() const{return issuer_&&issuer_->operationValid(*this);}
IOReturn HackyNaviFunctionGroup::Operation::quiesceAudio(uint32_t timeout,AudioStopReport& report) {
    return issuer_?issuer_->quiesceAudio(*this,timeout,report):kIOReturnNotReady;
}
bool HackyNaviFunctionGroup::Operation::matchesGraphics(IOPCIDevice* pci,IOService* owner) const {
    return valid()&&pci&&owner&&issuer_->leases_[0]->pci_==pci&&issuer_->owner_==owner&&
        issuer_->leases_[0]->borrowedOwner_==owner&&pci->isOpen(owner)&&valid();
}
IOReturn HackyNaviFunctionGroup::Operation::consumeFirstAttach(FirstAttach& out) {
    return issuer_?issuer_->consumeFirstAttach(*this,out):kIOReturnNotReady;
}
bool HackyNaviFunctionGroup::FirstAttach::validFor(const Operation& scope,IOPCIDevice* pci,IOService* owner) const {
    return operation_==&scope&&issuer_&&scope.issuer_==issuer_&&
        instance_==scope.instance_&&generation_==scope.generation_&&
        issuer_->firstAttachConsumed_&&scope.matchesGraphics(pci,owner);
}
IOReturn HackyNaviFunctionGroup::consumeFirstAttach(Operation& scope,FirstAttach& out) {
    if(!operationValid(scope)||out.issuer_||firstAttachConsumed_||!retirementRegistered())
        return kIOReturnNotReady;
    firstAttachConsumed_=true;
    out.operation_=&scope;out.issuer_=this;out.instance_=instance_;out.generation_=scope.generation_;
    return operationValid(scope)?kIOReturnSuccess:kIOReturnOffline;
}
IOReturn HackyNaviFunctionGroup::beginOperation(Operation& out) {
    if(out.issuer_||__atomic_load_n(&operationThread_,__ATOMIC_ACQUIRE)==current_thread()||
        __atomic_load_n(&retirementThread_,__ATOMIC_ACQUIRE)==current_thread())return kIOReturnBusy;
    if(__atomic_load_n(&shutdownRequested_,__ATOMIC_ACQUIRE))return kIOReturnNotReady;
    // Operation gate is always outermost. PM callback publication is atomic,
    // so it remains visible while either callback waits behind this gate.
    IOLockLock(operationLock_);
    __atomic_store_n(&operationThread_,current_thread(),__ATOMIC_RELEASE);
    IOLockLock(lock_);
    const uint64_t generation=__atomic_load_n(&generation_,__ATOMIC_ACQUIRE);
    IOReturn result=checkPoweredLocked();
    if(result==kIOReturnSuccess&&generation==__atomic_load_n(&generation_,__ATOMIC_ACQUIRE)) {
        retain();out.issuer_=this;out.thread_=current_thread();out.instance_=instance_;out.generation_=generation;
        out.graphics_=leases_[0]->pci_;out.openOwner_=owner_;
        activeOperation_=&out;operationGeneration_=generation;
        if(!operationValid(out)) {
            // Keep the retained reference until after both locks are released.
            out.issuer_=nullptr;out.thread_=nullptr;out.instance_=out.generation_=0;
            out.graphics_=nullptr;out.openOwner_=nullptr;activeOperation_=nullptr;operationGeneration_=0;
            IOLockUnlock(lock_);__atomic_store_n(&operationThread_,nullptr,__ATOMIC_RELEASE);
            IOLockUnlock(operationLock_);release();return kIOReturnNotReady;
        }
        IOLockUnlock(lock_);return kIOReturnSuccess;
    }
    IOLockUnlock(lock_);__atomic_store_n(&operationThread_,nullptr,__ATOMIC_RELEASE);
    IOLockUnlock(operationLock_);return kIOReturnNotReady;
}
bool HackyNaviFunctionGroup::operationValid(const Operation& scope) const {
    // No lock acquisition here: a normal operation already owns operationLock_,
    // which keeps these pointers alive and excludes callback state mutation.
    // Revocation and generation publication deliberately occur outside it.
    return scope.issuer_==this&&scope.thread_==current_thread()&&scope.instance_==instance_&&
        activeOperation_==&scope&&operationGeneration_==scope.generation_&&
        __atomic_load_n(&operationThread_,__ATOMIC_ACQUIRE)==current_thread()&&
        !__atomic_load_n(&primaryRevoked_,__ATOMIC_ACQUIRE)&&
        !__atomic_load_n(&retirementStart_,__ATOMIC_ACQUIRE)&&
        scope.generation_!=UINT64_MAX&&!__atomic_load_n(&shutdownRequested_,__ATOMIC_ACQUIRE)&&
        scope.generation_==__atomic_load_n(&generation_,__ATOMIC_ACQUIRE)&&
        acquired_&&!closing_&&!closed_&&owner_&&!owner_->isInactive()&&leases_[0]&&leases_[1]&&
        !leases_[0]->accessRevoked()&&!leases_[1]->accessRevoked()&&
        !leases_[0]->isInactive()&&!leases_[1]->isInactive()&&
        scope.graphics_==leases_[0]->pci_&&scope.openOwner_==owner_&&
        leases_[0]->pci_&&!leases_[0]->pci_->isInactive()&&
        leases_[1]->pci_&&!leases_[1]->pci_->isInactive();
}
// Stack-scoped CPU mapping only: no DMA memory or published address is owned
// here. Operation keeps the real paired leases alive and serializes callbacks.
struct HackyNaviFunctionGroup::AudioAccess {
    HackyNaviFunctionGroup& group;Operation* operation;RetirementScope* retirement;AudioStopReport& report;
    HackyNaviFunctionLease* lease;IOPCIDevice* pci;IOMemoryDescriptor* resource;
    IOMemoryMap* map;volatile uint8_t* registers;
    uint64_t start,last,barAddress,bytes;uint32_t budget,barLow,barHigh;
    uint16_t pcieHeader,pcieFlags;
    AudioAccess(HackyNaviFunctionGroup& g,Operation* op,RetirementScope* retire,AudioStopReport& r,uint32_t us):
        group(g),operation(op),retirement(retire),report(r),lease(nullptr),pci(nullptr),resource(nullptr),
        map(nullptr),registers(nullptr),start(0),last(0),barAddress(0),bytes(0),
        budget(us),barLow(0),barHigh(0),pcieHeader(0),pcieFlags(0){}
    ~AudioAccess(){if(map)map->release();if(resource)resource->release();}
    bool scopeReady() const {
        return operation?group.operationValid(*operation):retirement&&group.retirementValid(*retirement);
    }
    static uint64_t timeUs() {
        uint64_t ticks=0,ns=0;clock_get_uptime(&ticks);absolutetime_to_nanoseconds(ticks,&ns);
        return ns/1000;
    }
    bool fail(IOReturn error){if(report.result==kIOReturnSuccess)report.result=error;return false;}
    bool clockReady() {
        const uint64_t now=timeUs();
        if(now<last)return fail(kIOReturnError);
        last=now;report.elapsedUs=now-start;
        return report.elapsedUs<budget||fail(kIOReturnTimeout);
    }
    // These helpers deliberately do not call ready()/owned(): identity, PM,
    // capability and BAR validation themselves use configuration accesses.
    // Bracket EACH access so revocation during a scan cannot cause its next
    // access, and a long scan cannot silently exceed the operation deadline.
    bool configReady() {
        if(!scopeReady()||!lease||!pci)return fail(kIOReturnOffline);
        return clockReady();
    }
    bool config8(uint8_t offset,uint8_t& value) {
        if(!configReady())return false;
        value=pci->configRead8(offset);
        if(!configReady())return false;
        return value!=0xff||fail(kIOReturnOffline);
    }
    bool config16(uint8_t offset,uint16_t& value) {
        if(!configReady())return false;
        value=pci->configRead16(offset);
        if(!configReady())return false;
        return value!=0xffff||fail(kIOReturnOffline);
    }
    bool config32(uint8_t offset,uint32_t& value) {
        if(!configReady())return false;
        value=pci->configRead32(offset);
        if(!configReady())return false;
        return value!=UINT32_MAX||fail(kIOReturnOffline);
    }
    bool disableBusMaster(uint16_t command) {
        if(!configReady())return false;
        report.busMasterWriteAttempted=true;
        pci->configWrite16(kIOPCIConfigCommand,command&~UInt16(4));
        OSSynchronizeIO();
        return configReady(); // Failure does not assert that the write missed.
    }
    bool identity() {
        if(!configReady())return false;
        Identity value={};
        value.bus=pci->getBusNumber();value.device=pci->getDeviceNumber();value.function=pci->getFunctionNumber();
        uint8_t header=0;uint32_t repeated=0;
        if(!config32(0,value.vendorDevice)||!config32(8,value.revisionClass)||
           !config32(kIOPCIConfigSubSystemVendorID,value.subsystem)||
           !config8(kIOPCIConfigHeaderType,header)||!config32(0,repeated))return false;
        return (sameIdentity(lease->identity_,value)&&(header&0x7f)==0&&repeated==value.vendorDevice)||
            fail(kIOReturnOffline);
    }
    bool owned() {
        if(!configReady()||!pci->isOpen(lease)||!identity())return fail(kIOReturnOffline);
        uint16_t status=0;uint8_t offset=0;
        if(!config16(kIOPCIConfigStatus,status)||!config8(0x34,offset))return false;
        if(!(status&0x10))return fail(kIOReturnOffline);
        uint64_t visited=0;
        for(unsigned n=0;n<48;++n) {
            if(offset<0x40||(offset&3))return fail(kIOReturnOffline);
            const uint64_t bit=uint64_t(1)<<(offset/4);
            if(visited&bit)return fail(kIOReturnOffline);visited|=bit;
            uint16_t capability=0;
            if(!config16(offset,capability))return false;
            if(UInt8(capability)==0||UInt8(capability)==0xff)return fail(kIOReturnOffline);
            if(UInt8(capability)==1) {
                if(offset>0xf8)return fail(kIOReturnOffline);
                uint16_t control=0,repeated=0;
                if(!config16(offset+4,control)||!config16(offset,repeated))return false;
                if((control&3)||repeated!=capability)return fail(kIOReturnOffline);
                return identity();
            }
            offset=UInt8(capability>>8);
        }
        return fail(kIOReturnOffline);
    }
    bool barMatches() {
        uint16_t command=0;uint32_t low=0,high=0;
        if(!config16(kIOPCIConfigCommand,command)||!config32(kIOPCIConfigBaseAddress0,low)||
           ((barLow&6)==4&&!config32(kIOPCIConfigBaseAddress1,high)))return false;
        if(!(command&2)||low!=barLow||high!=barHigh)return fail(kIOReturnOffline);
        return true;
    }
    bool ready() {
        return clockReady()&&owned()&&map&&registers&&barMatches()&&
            scopeReady()&&clockReady();
    }
    bool address(uint32_t offset,unsigned width) {
        return offset<=bytes&&width<=bytes-offset&&!(offset&(width-1))&&ready();
    }
    template<class T> static bool read(void* p,uint32_t offset,T& value) {
        auto& a=*static_cast<AudioAccess*>(p);
        if(!a.address(offset,sizeof(T)))return false;
        __asm__ __volatile__("" ::: "memory");
        value=*reinterpret_cast<volatile T*>(a.registers+offset);
        OSSynchronizeIO();__asm__ __volatile__("" ::: "memory");
        return a.ready();
    }
    template<class T> static bool write(void* p,uint32_t offset,T value) {
        auto& a=*static_cast<AudioAccess*>(p);
        if(!a.address(offset,sizeof(T)))return false;
        __asm__ __volatile__("" ::: "memory");
        *reinterpret_cast<volatile T*>(a.registers+offset)=value;
        OSSynchronizeIO();__asm__ __volatile__("" ::: "memory");
        // False after the write is intentionally ambiguous; portable Result
        // already marked touched before invoking this callback.
        return a.ready();
    }
    static bool valid(void* p){return static_cast<AudioAccess*>(p)->ready();}
    static bool now(void* p,uint64_t& value) {
        auto& a=*static_cast<AudioAccess*>(p);
        if(!a.clockReady()||!a.scopeReady())return false;
        value=a.last;return true;
    }
    static bool delay(void* p,uint32_t us) {
        auto& a=*static_cast<AudioAccess*>(p);
        if(us>1000||!a.ready())return false;
        IODelay(us);return a.ready();
    }
    bool findPcie() {
        uint16_t status=0;uint8_t offset=0;
        if(!config16(kIOPCIConfigStatus,status)||!config8(0x34,offset))return false;
        if(!(status&0x10))return fail(kIOReturnUnsupported);
        uint64_t visited=0;
        for(unsigned n=0;n<48;++n) {
            if(!owned()||!clockReady())return false;
            if(offset<0x40||(offset&3))return fail(kIOReturnUnsupported);
            const uint64_t bit=uint64_t(1)<<(offset/4);
            if(visited&bit)return fail(kIOReturnUnsupported);visited|=bit;
            uint16_t header=0;if(!config16(offset,header))return false;
            if(UInt8(header)==0||UInt8(header)==0xff)return fail(kIOReturnOffline);
            if(UInt8(header)==0x10) {
                if(offset>0xf4)return fail(kIOReturnUnsupported);
                uint16_t flags=0,repeated=0;
                if(!config16(offset+2,flags))return false;
                if(!(flags&15)||(flags&15)>2||(flags&0xf0))
                    return fail(kIOReturnUnsupported); // PCIe endpoint, v1/v2.
                report.pcieCapability=offset;pcieHeader=header;pcieFlags=flags;
                if(!config16(offset,repeated))return false;
                if(repeated!=header)return fail(kIOReturnOffline);
                return owned()&&clockReady();
            }
            offset=UInt8(header>>8);
        }
        return fail(kIOReturnUnsupported);
    }
    bool prepare() {
        start=last=timeUs();
        if(!scopeReady())return fail(kIOReturnOffline);
        lease=group.leases_[1];pci=lease->pci_;
        if(!owned()||!lease->noOtherAudioClients()||!findPcie())return false;
        if(!config16(kIOPCIConfigCommand,report.commandBefore))return false;
        if(!(report.commandBefore&2))return fail(kIOReturnNotReady);
        if(!config32(kIOPCIConfigBaseAddress0,barLow))return false;
        if((barLow&1)||((barLow&6)!=0&&(barLow&6)!=4))
            return fail(kIOReturnUnsupported);
        if((barLow&6)==4) {
            if(!config32(kIOPCIConfigBaseAddress1,barHigh))return false;
        }
        barAddress=(uint64_t(barHigh)<<32)|(barLow&~UInt32(15));
        if(!barAddress)return fail(kIOReturnNotReady);
        // IOPCIDevice's convenience getter may call setTunnelL1Enable and enter
        // the config workloop. Inspect its already assigned property directly
        // instead, while the common operation gate excludes provider teardown.
        // IOPCIBridge::constructRange actually creates IOMemoryDescriptor
        // instances; the convenience getter's legacy IODeviceMemory return
        // type is not a guarantee of that subclass or an exported metaclass.
        OSObject* property=pci->copyProperty(gIODeviceMemoryKey);
        auto* ranges=OSDynamicCast(OSArray,property);
        bool duplicate=false;
        if(ranges&&ranges->getCount()<=6)for(unsigned i=0;i<ranges->getCount();++i) {
            auto* candidate=OSDynamicCast(IOMemoryDescriptor,ranges->getObject(i));
            if(candidate&&(candidate->getTag()&0xff)==kIOPCIConfigBaseAddress0) {
                if(resource){duplicate=true;break;}
                resource=candidate;resource->retain();
            }
        }
        if(property)property->release();
        if(duplicate||!resource||resource->getPhysicalAddress()!=barAddress)
            return fail(kIOReturnUnsupported);
        bytes=resource->getLength();report.barBytes=bytes;
        if(bytes<0x80||bytes>0x10000||barAddress>UINT64_MAX-bytes||!owned()||
           !barMatches()||!clockReady())return fail(kIOReturnNotReady);
        map=resource->map(kIOMapInhibitCache);
        if(!map)return fail(kIOReturnNoMemory);
        if(!map->getVirtualAddress()||(map->getVirtualAddress()&3)||
           map->getPhysicalAddress()!=barAddress||map->getLength()!=bytes||
           (map->getMapOptions()&kIOMapCacheMask)!=kIOMapInhibitCache)
            return fail(kIOReturnUnsupported);
        registers=reinterpret_cast<volatile uint8_t*>(map->getVirtualAddress());
        return ready();
    }
    bool stop() {
        if(!prepare())return false;
        report.stage=AudioStage::Controller;
        const hackynavi::audioquiesce::Io io={this,valid,read<uint8_t>,read<uint16_t>,read<uint32_t>,
            write<uint8_t>,write<uint16_t>,write<uint32_t>,now,delay};
        if(!clockReady())return false;
        const auto status=hackynavi::audioquiesce::stop(io,bytes,
            uint32_t(budget-report.elapsedUs),report.controller);
        if(status!=hackynavi::audioquiesce::Status::Ok) {
            if(report.result==kIOReturnSuccess)report.result=
                status==hackynavi::audioquiesce::Status::Timeout?kIOReturnTimeout:kIOReturnNotReady;
            return false;
        }
        report.stage=AudioStage::BusMasterDisable;
        if(!ready())return false;
        uint16_t command=0;
        if(!config16(kIOPCIConfigCommand,command))return false;
        if(!(command&2))return fail(kIOReturnOffline);
        if(command&4) {
            if(!ready()||!disableBusMaster(command))return false;
        }
        if(!ready())return false;
        if(!config16(kIOPCIConfigCommand,report.commandAfter))return false;
        if(report.commandAfter!=(command&~UInt16(4)))return fail(kIOReturnNotReady);
        report.busMasterDisabled=true;report.stage=AudioStage::TransactionsPending;
        // PCIe Device Status only describes outstanding nonposted requests.
        // No claim of posted-write draining or whole-GPU retirement is made.
        for(unsigned n=0;n<=budget/10;++n) {
            if(!ready())return false;
            const UInt8 cap=report.pcieCapability;
            uint16_t header=0,flags=0;
            if(!config16(cap,header)||!config16(cap+2,flags))return false;
            if(header!=pcieHeader||flags!=pcieFlags)
                return fail(kIOReturnOffline);
            ++report.pendingPolls;
            if(!config16(cap+0xa,report.deviceStatus))return false;
            if(!ready())return false;
            if(!(report.deviceStatus&0x20)) {
                uint32_t control=0;uint16_t finalCommand=0;
                if(!read(this,8,control)||control==UINT32_MAX||(control&0x103)||
                   !config16(kIOPCIConfigCommand,finalCommand)||finalCommand!=report.commandAfter||
                   !lease->noOtherAudioClients()||!ready())return fail(kIOReturnNotReady);
                report.nonpostedPendingClear=true;return true;
            }
            if(!delay(this,10))return false;
        }
        return fail(kIOReturnTimeout);
    }
};
IOReturn HackyNaviFunctionGroup::quiesceAudio(Operation& scope,uint32_t timeout,AudioStopReport& out) {
    out={};out.stage=AudioStage::Preflight;out.result=kIOReturnBadArgument;
    out.controller.status=hackynavi::audioquiesce::Status::InvalidArgument;
    out.controller.stream=0xff;
    if(!timeout||timeout>1000000||!operationValid(scope))return out.result;
    out.generation=scope.generation();out.result=kIOReturnSuccess;
    AudioAccess access(*this,&scope,nullptr,out,timeout);
    if(access.stop()) {out.complete=true;out.stage=AudioStage::Complete;}
    else if(out.result==kIOReturnSuccess)out.result=kIOReturnNotReady;
    return out.result;
}
IOReturn HackyNaviFunctionGroup::quiesceAudio(RetirementScope& scope,uint32_t timeout,AudioStopReport& out) {
    out={};out.stage=AudioStage::Preflight;out.result=kIOReturnBadArgument;
    out.controller.status=hackynavi::audioquiesce::Status::InvalidArgument;out.controller.stream=0xff;
    const uint32_t remaining=scope.remainingUs();
    if(!timeout||timeout>1000000||!remaining||!retirementValid(scope))return out.result;
    if(timeout>remaining)timeout=remaining;
    out.generation=scope.generation();out.result=kIOReturnSuccess;
    AudioAccess access(*this,nullptr,&scope,out,timeout);
    if(access.stop()) {out.complete=true;out.stage=AudioStage::Complete;}
    else if(out.result==kIOReturnSuccess)out.result=kIOReturnNotReady;
    return out.result;
}
HackyNaviFunctionGroup::RetirementScope::RetirementScope(HackyNaviFunctionGroup* group,
    uint64_t generation,uint64_t deadline):issuer_(group),thread_(current_thread()),
    instance_(group->instance_),generation_(generation),deadline_(deadline){}
bool HackyNaviFunctionGroup::RetirementScope::valid() const {
    return issuer_&&issuer_->retirementValid(*this);
}
bool HackyNaviFunctionGroup::RetirementScope::matchesGraphics(IOPCIDevice* pci,IOService* owner) const {
    return valid()&&issuer_->owner_==owner&&issuer_->leases_[0]->pci_==pci&&
        issuer_->leases_[0]->borrowedOwner_==owner;
}
IOReturn HackyNaviFunctionGroup::RetirementScope::quiesceAudio(uint32_t timeout,AudioStopReport& report) {
    return issuer_?issuer_->quiesceAudio(*this,timeout,report):kIOReturnNotReady;
}
uint32_t HackyNaviFunctionGroup::RetirementScope::remainingUs() const {
    if(!issuer_||thread_!=current_thread())return 0;
    const uint64_t now=uptimeTicks();if(now>=deadline_)return 0;
    uint64_t ns=0;absolutetime_to_nanoseconds(deadline_-now,&ns);
    const uint64_t us=ns/1000;return us>UINT32_MAX?UINT32_MAX:uint32_t(us);
}
uint32_t HackyNaviFunctionGroup::RetirementScope::reasons() const {
    return issuer_?__atomic_load_n(&issuer_->retirementFlags_,__ATOMIC_ACQUIRE)&kRetirementReasons:0;
}
void HackyNaviFunctionGroup::RetirementScope::progress(uint32_t stage) {
    if(!issuer_||thread_!=current_thread())panic("HackyNavi: retirement scope changed thread");
    __atomic_store_n(&issuer_->retirementStage_,stage,__ATOMIC_RELEASE);
}
IOReturn HackyNaviFunctionGroup::RetirementScope::enterRecovery() {
    if(!valid())return kIOReturnOffline;
    uint32_t flags=__atomic_load_n(&issuer_->retirementFlags_,__ATOMIC_ACQUIRE);
    do {
        if(!recoveryReasonSet(flags)||
            (flags&kRecoveryEntered))return kIOReturnNotPermitted;
        // One atomic word linearizes termination against the first possible
        // native reset/restore dependency. A later terminal callback sees this
        // bit before waiting for a worker that may need its inherited workloop.
    } while(!__atomic_compare_exchange_n(&issuer_->retirementFlags_,&flags,flags|kRecoveryEntered,
        false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE));
    return valid()?kIOReturnSuccess:kIOReturnOffline;
}
bool HackyNaviFunctionGroup::RetirementScope::permitsPciRecovery() const {
    if(!valid())return false;
    const uint32_t flags=__atomic_load_n(&issuer_->retirementFlags_,__ATOMIC_ACQUIRE);
    return (flags&kRecoveryEntered)&&recoveryReasonSet(flags)&&valid();
}
IOReturn HackyNaviFunctionGroup::registerRetirementHandler(Operation& operation,
    RetirementHandler handler,uint32_t timeoutUs) {
    if(!handler||timeoutUs<1000||timeoutUs>5000000||!operationValid(operation))return kIOReturnBadArgument;
    // The caller owns operationLock_; no retirement handler can be replaced
    // after publication. This is a measured total budget, not per-poll time.
    if(retirementRegistered()||__atomic_load_n(&retirementStart_,__ATOMIC_ACQUIRE))return kIOReturnBusy;
    uint64_t ticks=0;clock_interval_to_absolutetime_interval(timeoutUs,kMicrosecondScale,&ticks);
    if(!ticks)return kIOReturnBadArgument;
    retirementHandler_=handler;retirementBudgetTicks_=ticks;
    registrationThread_=current_thread();
    __atomic_store_n(&retirementArmed_,1,__ATOMIC_RELEASE);
    // A racing callback can invalidate registration's operation. Keep the
    // installed handler for teardown, but never authorize activation afterward.
    return operationValid(operation)?kIOReturnSuccess:kIOReturnNotReady;
}
bool HackyNaviFunctionGroup::retirementValid(const RetirementScope& scope) const {
    if(!retirementWindowValid(scope))return false;
    // Logical software termination is permitted for cleanup. Actual provider
    // open/D0/config availability is checked below before hardware access.
    // This worker owns operationLock_. Callbacks cannot ACK or mutate the
    // paired powered/open state until its real owner completes retirement.
    for(unsigned i=0;i<2;++i) {
        auto* lease=leases_[i];auto* pci=lease->pci_;
        if(lease->group_!=this||!lease->attached_||!lease->started_||!lease->opened_||!lease->pmRegistered_||
            !lease->powered_||!pci||
            !pci->isOpen(lease->borrowedOwner_?lease->borrowedOwner_:lease)||
            !lease->identityMatches(true))return false;
    }
    return topologyMatches()&&uptimeTicks()<scope.deadline_;
}
bool HackyNaviFunctionGroup::retirementWindowValid(const RetirementScope& scope) const {
    if(scope.issuer_!=this||scope.thread_!=current_thread()||scope.instance_!=instance_||
        scope.generation_!=__atomic_load_n(&retirementGeneration_,__ATOMIC_ACQUIRE)||
        __atomic_load_n(&retirementThread_,__ATOMIC_ACQUIRE)!=current_thread()||
        __atomic_load_n(&retirementPhase_,__ATOMIC_ACQUIRE)!=RetirementCalling||
        (!__atomic_load_n(&retirementWaiters_,__ATOMIC_ACQUIRE)&&
         !__atomic_load_n(&runtimeFailureOrigin_,__ATOMIC_ACQUIRE))||uptimeTicks()>=scope.deadline_||
        !acquired_||closing_||closed_||!owner_||!leases_[0]||!leases_[1])return false;
    return true;
}
bool HackyNaviFunctionGroup::RetirementScope::recoveryWindowOpen() const {
    if(!issuer_||!issuer_->retirementWindowValid(*this))return false;
    const uint32_t flags=__atomic_load_n(&issuer_->retirementFlags_,__ATOMIC_ACQUIRE);
    return (flags&kRecoveryEntered)&&recoveryReasonSet(flags)&&
        issuer_->retirementWindowValid(*this);
}
IOReturn HackyNaviFunctionGroup::retirePrimary(IOService* owner,RetirementReason reason) {
    if(!owner||owner!=owner_)return kIOReturnBadArgument;
    if(reason!=RetirementReason::PowerDown&&reason!=RetirementReason::ProviderTerminating&&
        reason!=RetirementReason::SystemShutdown&&reason!=RetirementReason::Close&&
        reason!=RetirementReason::OwnerFailure)return kIOReturnBadArgument;
    // The registering sleepable worker may retire its failed activation only
    // after dropping Operation. No PM/termination caller may relabel itself as
    // this recoverable reason. The Device calls this in the same invocation.
    if(reason==RetirementReason::OwnerFailure&&(!retirementRegistered()||
        registrationThread_!=current_thread()||
        __atomic_load_n(&operationThread_,__ATOMIC_ACQUIRE)==current_thread()))return kIOReturnNotPermitted;
    // Publish before testing registration. A racing registration can no longer
    // authorize activation after this primary callback starts teardown.
    __atomic_store_n(&primaryRevoked_,1,__ATOMIC_RELEASE);advanceGeneration();
    if(retirementRegistered()){requestRetirement(reason,0);return kIOReturnSuccess;}
    if(__atomic_load_n(&operationThread_,__ATOMIC_ACQUIRE)==current_thread()||
        __atomic_load_n(&retirementThread_,__ATOMIC_ACQUIRE)==current_thread())
        panic("HackyNavi: primary retirement inside hardware scope");
    // Even unarmed construction must drain before the primary unmaps/closes.
    IOLockLock(operationLock_);IOLockUnlock(operationLock_);
    if(retirementRegistered())requestRetirement(reason,0);
    return kIOReturnSuccess;
}
IOReturn HackyNaviFunctionGroup::requestOwnerFailure(Operation& scope) {
    // A failed read/provider removal or concurrent PM callback can already
    // invalidate normal I/O. Do not require operationValid(), D0 or fresh PCI
    // reads merely to contain that failure. The actual lexical operation gate
    // still excludes lease destruction and the retirement worker. No caller
    // supplied epoch/readiness bit can stand in for this private binding.
    if(scope.issuer_!=this||scope.thread_!=current_thread()||scope.instance_!=instance_||
        activeOperation_!=&scope||operationGeneration_!=scope.generation_||!scope.generation_||
        scope.generation_==UINT64_MAX||__atomic_load_n(&operationThread_,__ATOMIC_ACQUIRE)!=current_thread()||
        !acquired_||closing_||closed_||!owner_||scope.openOwner_!=owner_||!scope.graphics_||
        !leases_[0]||!leases_[1]||leases_[0]->group_!=this||leases_[1]->group_!=this||
        leases_[0]->pci_!=scope.graphics_||leases_[0]->borrowedOwner_!=scope.openOwner_||
        !retirementRegistered()||!firstAttachConsumed_)return kIOReturnNotPermitted;
    // This is an actual asynchronous origin, not a fake callback waiter. The
    // worker/watchdog own group retains; the origin remains until completion.
    __atomic_store_n(&runtimeFailureOrigin_,1,__ATOMIC_RELEASE);
    __atomic_store_n(&primaryRevoked_,1,__ATOMIC_RELEASE);advanceGeneration();
    enqueueRetirement(RetirementReason::OwnerFailure,0);
    return kIOReturnSuccess;
}
void HackyNaviFunctionGroup::retirementFailed(IOReturn result,const char* why) {
    const uint32_t phase=__atomic_exchange_n(&retirementPhase_,RetirementFailed,__ATOMIC_ACQ_REL);
    const uint64_t now=uptimeTicks(),start=__atomic_load_n(&retirementStart_,__ATOMIC_ACQUIRE);
    uint64_t ns=0;if(now>=start)absolutetime_to_nanoseconds(now-start,&ns);
    // Deliberate fail-stop: neither returning an IOReturn nor withholding a
    // delayed ACK is a power-loss veto. Do not close, unmap or release DMA.
    panic("HackyNavi: retirement fail-stop %s result=0x%x instance=%llu generation=%llu phase=%u stage=%u reasons=0x%x functions=0x%x elapsed-us=%llu",
        why,unsigned(result),static_cast<unsigned long long>(instance_),
        static_cast<unsigned long long>(__atomic_load_n(&retirementGeneration_,__ATOMIC_ACQUIRE)),
        phase,__atomic_load_n(&retirementStage_,__ATOMIC_ACQUIRE),
        __atomic_load_n(&retirementFlags_,__ATOMIC_ACQUIRE),
        __atomic_load_n(&retirementFunctions_,__ATOMIC_ACQUIRE),static_cast<unsigned long long>(ns/1000));
}
void HackyNaviFunctionGroup::requestRetirement(RetirementReason reason,uint8_t function) {
    if(!retirementRegistered())return; // Observation-only diagnostic client.
    if(__atomic_load_n(&operationThread_,__ATOMIC_ACQUIRE)==current_thread()||
        __atomic_load_n(&retirementThread_,__ATOMIC_ACQUIRE)==current_thread())
        retirementFailed(kIOReturnCannotLock,"lifecycle callback reentered its hardware scope");
    if(__atomic_fetch_add(&retirementWaiters_,1,__ATOMIC_ACQ_REL)==UINT32_MAX)
        retirementFailed(kIOReturnNoResources,"callback count overflow");
    enqueueRetirement(reason,function);
    const uint64_t deadline=__atomic_load_n(&retirementStart_,__ATOMIC_ACQUIRE)+retirementBudgetTicks_;
    IOLockLock(retirementWaitLock_);
    while(__atomic_load_n(&retirementPhase_,__ATOMIC_ACQUIRE)!=RetirementComplete) {
        if(uptimeTicks()>=deadline)retirementFailed(kIOReturnTimeout,"callback retirement deadline");
        IOLockSleepDeadline(retirementWaitLock_,&retirementPhase_,deadline,THREAD_UNINT);
    }
    IOLockUnlock(retirementWaitLock_);
    __atomic_fetch_sub(&retirementWaiters_,1,__ATOMIC_RELEASE);
}
void HackyNaviFunctionGroup::enqueueRetirement(RetirementReason reason,uint8_t function) {
    // Both callback waiters and an exact Operation's runtime-failure origin
    // use this one atomic reason/start transition. No wait/owner lock here.
    const uint32_t bit=uint32_t(reason);
    const uint32_t previous=__atomic_fetch_or(&retirementFlags_,bit,__ATOMIC_ACQ_REL);
    if((bit&kNoRecoveryReasons)&&(previous&kRecoveryEntered)&&
        __atomic_load_n(&retirementPhase_,__ATOMIC_ACQUIRE)!=RetirementComplete)
        retirementFailed(kIOReturnCannotLock,"terminal callback intersects PCI recovery");
    __atomic_fetch_or(&retirementFunctions_,function<2?uint32_t(1)<<function:4U,__ATOMIC_RELEASE);
    const uint64_t now=uptimeTicks();uint64_t expected=0;
    if(__atomic_compare_exchange_n(&retirementStart_,&expected,now?now:1,false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        if(UINT64_MAX-now<retirementBudgetTicks_)retirementFailed(kIOReturnOverrun,"deadline overflow");
        advanceGeneration();
        __atomic_store_n(&retirementGeneration_,__atomic_load_n(&generation_,__ATOMIC_ACQUIRE),__ATOMIC_RELEASE);
        __atomic_store_n(&retirementPhase_,RetirementQueued,__ATOMIC_RELEASE);
        // Both callouts were allocated before registration. Each owns a retain;
        // arm the independent watcher before any possibly blocking gate wait.
        retain();retain();
        if(thread_call_enter_delayed(retirementWatch_,(now?now:1)+retirementBudgetTicks_)||
            thread_call_enter(retirementCall_))retirementFailed(kIOReturnInternalError,"duplicate retirement callout");
    }
}
void HackyNaviFunctionGroup::retirementWorker(thread_call_param_t context,thread_call_param_t) {
    auto* group=static_cast<HackyNaviFunctionGroup*>(context);
    __atomic_store_n(&group->retirementPhase_,RetirementDraining,__ATOMIC_RELEASE);
    IOLockLock(group->operationLock_);
    __atomic_store_n(&group->retirementThread_,current_thread(),__ATOMIC_RELEASE);
    const uint64_t deadline=__atomic_load_n(&group->retirementStart_,__ATOMIC_ACQUIRE)+group->retirementBudgetTicks_;
    if(uptimeTicks()>=deadline)group->retirementFailed(kIOReturnTimeout,"operation drain deadline");
    __atomic_store_n(&group->retirementPhase_,RetirementCalling,__ATOMIC_RELEASE);
    RetirementScope scope(group,__atomic_load_n(&group->retirementGeneration_,__ATOMIC_ACQUIRE),deadline);
    // Even on surprise removal the real owner must inspect its retained CPU
    // state. scope.valid() then prevents hardware access; an unpublished owner
    // can still complete, but retained hardware cannot be claimed retired.
    const IOReturn result=group->retirementHandler_(group->owner_,scope);
    if(result!=kIOReturnSuccess)group->retirementFailed(result,"owner retirement failed");
    if(uptimeTicks()>=deadline)group->retirementFailed(kIOReturnTimeout,"owner retirement deadline");
    scope.issuer_=nullptr;
    __atomic_store_n(&group->retirementThread_,nullptr,__ATOMIC_RELEASE);
    IOLockUnlock(group->operationLock_);
    IOLockLock(group->retirementWaitLock_);
    __atomic_store_n(&group->retirementPhase_,RetirementComplete,__ATOMIC_RELEASE);
    __atomic_store_n(&group->runtimeFailureOrigin_,0,__ATOMIC_RELEASE);
    IOLockWakeup(group->retirementWaitLock_,&group->retirementPhase_,false);
    IOLockUnlock(group->retirementWaitLock_);
    if(thread_call_cancel(group->retirementWatch_))group->release(); // Cancelled watch retain.
    group->release(); // Worker retain; no accesses afterward.
}
void HackyNaviFunctionGroup::retirementWatchdog(thread_call_param_t context,thread_call_param_t) {
    auto* group=static_cast<HackyNaviFunctionGroup*>(context);
    // No lifecycle/operation/lease lock or provider access can obstruct this
    // check. The callback's timed wait is a second independent deadline check.
    if(__atomic_load_n(&group->retirementPhase_,__ATOMIC_ACQUIRE)!=RetirementComplete)
        group->retirementFailed(kIOReturnTimeout,"independent retirement watchdog");
    group->release(); // Watch retain; no accesses afterward.
}
void HackyNaviFunctionGroup::endOperation(Operation& scope) {
    // Unlocking a kernel mutex from another thread corrupts ownership. This is
    // a programming-contract violation, not GPU failure containment.
    if(scope.issuer_!=this||scope.thread_!=current_thread()||activeOperation_!=&scope||
        operationGeneration_!=scope.generation_)panic("HackyNavi: operation scope changed binding/thread");
    scope.issuer_=nullptr;scope.thread_=nullptr;scope.instance_=scope.generation_=0;
    scope.graphics_=nullptr;scope.openOwner_=nullptr;activeOperation_=nullptr;operationGeneration_=0;
    __atomic_store_n(&operationThread_,nullptr,__ATOMIC_RELEASE);
    IOLockUnlock(operationLock_);release();
}
void HackyNaviFunctionGroup::snapshot(Snapshot& out) {
    IOLockLock(lock_);out={};out.instance=instance_;out.generation=__atomic_load_n(&generation_,__ATOMIC_ACQUIRE);
    out.state=closed_?State::Closed:closing_?State::Closing:State::Acquiring;
    for(unsigned i=0;i<2;++i) {
        out.functions[i].result=kIOReturnNotReady;
        if(leases_[i])leases_[i]->copySnapshot(out.functions[i]);
    }
    if(!closing_&&!closed_) {
        if(__atomic_load_n(&shutdownRequested_,__ATOMIC_ACQUIRE)||
            __atomic_load_n(&retirementStart_,__ATOMIC_ACQUIRE)||
            __atomic_load_n(&primaryRevoked_,__ATOMIC_ACQUIRE)||out.generation==UINT64_MAX||
            out.functions[0].terminal||out.functions[1].terminal)out.state=State::Revoked;
        else if(acquired_&&out.functions[0].powered&&out.functions[1].powered&&
            !out.functions[0].admissionRevoked&&!out.functions[1].admissionRevoked)out.state=State::Powered;
    }
    IOLockUnlock(lock_);
}
void HackyNaviFunctionGroup::advanceGeneration() {
    uint64_t previous=__atomic_load_n(&generation_,__ATOMIC_RELAXED);
    do {
        if(previous==UINT64_MAX)return;
    } while(!__atomic_compare_exchange_n(&generation_,&previous,previous+1,false,
        __ATOMIC_RELEASE,__ATOMIC_RELAXED));
}
void HackyNaviFunctionGroup::changed(Event event,uint8_t function) {
    // The concrete transition already published revocation/generation before
    // waiting on the operation gate. Delivery must not invalidate an operation
    // newly admitted after the completed transition.
    retain();IOLockLock(lock_);
    Notify notify=(acquired_&&!closing_&&!closed_)?notify_:nullptr;
    IOService* owner=owner_;if(owner)owner->retain();IOLockUnlock(lock_);
    if(notify&&owner)notify(owner,this,event,function);
    if(owner)owner->release();release();
}
void HackyNaviFunctionGroup::shutdown() {
    __atomic_store_n(&shutdownRequested_,1,__ATOMIC_RELEASE);
    advanceGeneration();
    if(!lock_||!teardownLock_||!operationLock_)return;
    if(__atomic_load_n(&operationThread_,__ATOMIC_ACQUIRE)==current_thread())
        panic("HackyNavi: shutdown inside normal operation");
    requestRetirement(RetirementReason::Close,0xff);
    IOLockLock(teardownLock_);
    IOLockLock(operationLock_);
    IOLockLock(lock_);if(closed_){IOLockUnlock(lock_);IOLockUnlock(operationLock_);IOLockUnlock(teardownLock_);return;}
    closing_=true;HackyNaviFunctionLease* leases[2]={leases_[0],leases_[1]};
    leases_[0]=leases_[1]=nullptr;IOLockUnlock(lock_);IOLockUnlock(operationLock_);
    for(unsigned i=0;i<2;++i)if(leases[i]){leases[i]->shutdown();leases[i]->release();}
    IOLockLock(lock_);closed_=true;closing_=false;IOLockUnlock(lock_);IOLockUnlock(teardownLock_);
}
void HackyNaviFunctionGroup::free() {
    // Every queued/running callout owns its own group retain. There can be no
    // pending call when the last reference reaches here; freeing a call from
    // its own final callback is supported. Never cancel_wait on that callback.
    if(retirementCall_&&!thread_call_free(retirementCall_))panic("HackyNavi: pending retirement worker at free");
    if(retirementWatch_&&!thread_call_free(retirementWatch_))panic("HackyNavi: pending retirement watchdog at free");
    if(retirementWaitLock_)IOLockFree(retirementWaitLock_);
    if(bridge_)bridge_->release();if(owner_)owner_->release();
    if(operationLock_)IOLockFree(operationLock_);
    if(teardownLock_)IOLockFree(teardownLock_);if(lock_)IOLockFree(lock_);OSObject::free();
}

bool HackyNaviFunctionLease::init(OSDictionary* properties) {
    lock_=nullptr;lifecycleLock_=nullptr;group_=nullptr;pci_=nullptr;borrowedOwner_=nullptr;identity_={};
    admissionRevoked_=kPowerRevoked;
    attached_=started_=opened_=pmInitialized_=pmRegistered_=powered_=terminal_=false;result_=kIOReturnNotReady;
    if(!IOService::init(properties))return false;
    lock_=IOLockAlloc();lifecycleLock_=IOLockAlloc();return lock_&&lifecycleLock_;
}
HackyNaviFunctionLease* HackyNaviFunctionLease::create(HackyNaviFunctionGroup* group,
    IOPCIDevice* pci,IOService* borrowedOwner,IOReturn& result) {
    result=kIOReturnNoMemory;auto* self=new HackyNaviFunctionLease;
    if(!self)return nullptr;
    if(!self->init()){self->release();return nullptr;}
    if(!identify(pci,self->identity_)){result=kIOReturnOffline;self->release();return nullptr;}
    self->group_=group;group->retain();self->pci_=pci;pci->retain();
    self->borrowedOwner_=borrowedOwner;if(borrowedOwner)borrowedOwner->retain();
    result=kIOReturnSuccess;return self;
}
bool HackyNaviFunctionLease::identityMatches(bool requireD0) const {
    HackyNaviFunctionGroup::Identity now={};
    if(!identify(pci_,now)||!sameIdentity(identity_,now))return false;
    if(!requireD0)return true;
    // IOPCIDevice::findPCICapability returns zero when configAccess(true) is
    // denied, which cannot be interpreted as "device has no PM capability".
    // Both functions in this exact profile must expose a real conventional PM
    // capability. Walk at most 48 aligned entries with cycle detection using
    // read-only config access; absence/unreadability/malformed lists fail.
    const UInt16 status=pci_->configRead16(kIOPCIConfigStatus);
    if(status==0xffff||!(status&0x10))return false;
    UInt8 offset=pci_->configRead8(0x34);uint64_t visited=0;
    for(unsigned n=0;n<48;++n) {
        if(offset<0x40||(offset&3))return false;
        const uint64_t bit=uint64_t(1)<<(offset/4);
        if(visited&bit)return false;visited|=bit;
        const UInt16 capability=pci_->configRead16(offset);
        if(capability==0xffff||UInt8(capability)==0||UInt8(capability)==0xff)return false;
        if(UInt8(capability)==1) {
            if(offset>0xf8)return false;
            const UInt16 control=pci_->configRead16(offset+4);
            HackyNaviFunctionGroup::Identity after={};
            return control!=0xffff&&!(control&3)&&pci_->configRead16(offset)==capability&&
                identify(pci_,after)&&sameIdentity(identity_,after);
        }
        offset=UInt8(capability>>8);
    }
    return false;
}
bool HackyNaviFunctionLease::noOtherAudioClients() const {
    if(borrowedOwner_)return true;
    OSIterator* clients=pci_->getClientIterator();if(!clients)return false;
    bool valid=true;unsigned visited=0;
    while(auto* object=clients->getNextObject()) {
        if(++visited>64){valid=false;break;}
        auto* service=OSDynamicCast(IOService,object);
        if(service&&service!=this&&!service->isInactive()){valid=false;break;}
    }
    valid=valid&&clients->isValid();clients->release();return valid;
}
IOReturn HackyNaviFunctionLease::begin() {
    IOLockLock(lifecycleLock_);const IOReturn result=beginLocked();IOLockUnlock(lifecycleLock_);return result;
}
IOReturn HackyNaviFunctionLease::beginLocked() {
    IOLockLock(lock_);const bool unavailable=terminal_||attached_||started_;IOLockUnlock(lock_);
    if(unavailable)return kIOReturnNotReady;
    if(!pci_||!group_||!identityMatches(false))return kIOReturnOffline;
    if(!attach(pci_))return kIOReturnNotAttached;
    IOLockLock(lock_);attached_=true;IOLockUnlock(lock_);
    if(!IOService::start(pci_))return kIOReturnError;
    IOLockLock(lock_);started_=true;IOLockUnlock(lock_);
    if(!noOtherAudioClients())return kIOReturnExclusiveAccess;
    const bool opened=borrowedOwner_?pci_->isOpen(borrowedOwner_):pci_->open(this);
    IOLockLock(lock_);opened_=opened;
    const bool valid=opened&&!terminal_&&!isInactive()&&identityMatches(false);
    // A provider can finish didTerminate while open() is still in flight.
    // That callback sets terminal_ first; if the open subsequently succeeds,
    // account for and close it here without relying on a later stop callback.
    const bool closeLateOpen=opened&&!valid&&!borrowedOwner_;
    if(!valid)opened_=false;
    IOLockUnlock(lock_);
    if(closeLateOpen)pci_->close(this);
    if(!valid)return opened?kIOReturnOffline:kIOReturnExclusiveAccess;
    PMinit();IOLockLock(lock_);pmInitialized_=true;IOLockUnlock(lock_);
    pci_->joinPMtree(this);
    IOReturn result=registerPowerDriver(this,kStates,2);
    IOLockLock(lock_);pmRegistered_=result==kIOReturnSuccess;IOLockUnlock(lock_);
    if(result==kIOReturnSuccess)result=changePowerStateTo(0);
    if(result==kIOReturnSuccess)result=changePowerStateToPriv(1);
    IOLockLock(lock_);
    if(result==kIOReturnSuccess && (terminal_||isInactive()||!opened_||
        !pci_->isOpen(borrowedOwner_?borrowedOwner_:this)||!identityMatches(false)))result=kIOReturnOffline;
    result_=result;IOLockUnlock(lock_);return result;
}
void HackyNaviFunctionLease::notify(HackyNaviFunctionGroup::Event event) {
    IOLockLock(lock_);auto* group=group_;if(group)group->retain();
    const uint8_t function=identity_.function;IOLockUnlock(lock_);
    if(group){group->changed(event,function);group->release();}
}
IOReturn HackyNaviFunctionLease::setPowerState(unsigned long ordinal,IOService*) {
    // Publish before the lease mutex, so a validator that already sampled
    // another function still observes an incoming transition. The callback
    // then joins the common gate before changing state or acknowledging PM.
    __atomic_fetch_or(&admissionRevoked_,kPowerRevoked,__ATOMIC_RELEASE);
    // XNU 11417.140.69 invokes this on a thread call, outside its PM workloop.
    // Keep this callback in flight: negative returns and expired delayed ACKs
    // both let PM continue. The immutable group reference survives this lease.
    if(ordinal!=1&&group_)group_->requestRetirement(
        HackyNaviFunctionGroup::RetirementReason::PowerDown,identity_.function);
    auto* transition=beginTransition();
    IOLockLock(lock_);
    const bool valid=ordinal==1&&pmInitialized_&&!terminal_&&!isInactive()&&opened_&&
        pci_&&pci_->isOpen(borrowedOwner_?borrowedOwner_:this)&&identityMatches(true);
    powered_=valid;result_=valid?kIOReturnSuccess:kIOReturnNotReady;
    if(valid)__atomic_fetch_and(&admissionRevoked_,~kPowerRevoked,__ATOMIC_RELEASE);
    IOLockUnlock(lock_);endTransition(transition);notify(HackyNaviFunctionGroup::Event::PowerChanged);
    // Acknowledgement permits the parent transition. No return value from
    // this observation service purports to keep the PCI domain powered.
    return IOPMAckImplied;
}
void HackyNaviFunctionLease::revoke(HackyNaviFunctionGroup::Event event) {
    __atomic_fetch_or(&admissionRevoked_,kTerminalRevoked,__ATOMIC_RELEASE);
    if(group_)group_->requestRetirement(event==HackyNaviFunctionGroup::Event::SystemShutdown?
        HackyNaviFunctionGroup::RetirementReason::SystemShutdown:
        event==HackyNaviFunctionGroup::Event::Closed?HackyNaviFunctionGroup::RetirementReason::Close:
        HackyNaviFunctionGroup::RetirementReason::ProviderTerminating,identity_.function);
    auto* transition=beginTransition();
    IOLockLock(lock_);terminal_=true;powered_=false;result_=kIOReturnOffline;
    IOLockUnlock(lock_);endTransition(transition);notify(event);
}
HackyNaviFunctionGroup* HackyNaviFunctionLease::beginTransition() {
    IOLockLock(lock_);auto* group=group_;if(group)group->retain();IOLockUnlock(lock_);
    if(group){
        group->advanceGeneration();
        if(__atomic_load_n(&group->operationThread_,__ATOMIC_ACQUIRE)==current_thread())
            panic("HackyNavi: synchronous lifecycle callback inside normal operation");
        IOLockLock(group->operationLock_);
    }
    return group;
}
void HackyNaviFunctionLease::endTransition(HackyNaviFunctionGroup* group) {
    if(group){IOLockUnlock(group->operationLock_);group->release();}
}
bool HackyNaviFunctionLease::willTerminate(IOService* provider,IOOptionBits options) {
    revoke(HackyNaviFunctionGroup::Event::ProviderTerminating);
    return IOService::willTerminate(provider,options);
}
bool HackyNaviFunctionLease::didTerminate(IOService* provider,IOOptionBits options,bool* defer) {
    // Closing is essential: XNU defers stop while an open client remains.
    // With a registered owner, revoke waits for the shared real retirement
    // before this close. Borrowed graphics is still closed only by its owner.
    revoke(HackyNaviFunctionGroup::Event::ProviderTerminating);
    IOLockLock(lock_);const bool close=opened_&&!borrowedOwner_;opened_=false;
    IOLockUnlock(lock_);if(close&&pci_)pci_->close(this);
    return IOService::didTerminate(provider,options,defer);
}
void HackyNaviFunctionLease::systemWillShutdown(IOOptionBits specifier) {
    revoke(HackyNaviFunctionGroup::Event::SystemShutdown);
    IOService::systemWillShutdown(specifier);
}
bool HackyNaviFunctionLease::validatePowered() {
    IOLockLock(lock_);
    const bool valid=!accessRevoked()&&attached_&&started_&&opened_&&pmInitialized_&&pmRegistered_&&powered_&&!terminal_&&
        !isInactive()&&pci_&&pci_->isOpen(borrowedOwner_?borrowedOwner_:this)&&
        (!borrowedOwner_||!borrowedOwner_->isInactive())&&identityMatches(true)&&!accessRevoked();
    IOLockUnlock(lock_);return valid;
}
bool HackyNaviFunctionLease::accessRevoked() const {
    return __atomic_load_n(&admissionRevoked_,__ATOMIC_ACQUIRE)!=0;
}
void HackyNaviFunctionLease::copySnapshot(HackyNaviFunctionGroup::FunctionSnapshot& out) {
    IOLockLock(lock_);out={identity_,attached_,opened_,opened_&&!borrowedOwner_,
        pmInitialized_,pmRegistered_,powered_,terminal_,accessRevoked(),result_};
    IOLockUnlock(lock_);
}
void HackyNaviFunctionLease::shutdown() {
    if(!lock_||!lifecycleLock_)return;
    revoke(HackyNaviFunctionGroup::Event::Closed);
    IOLockLock(lifecycleLock_);
    IOLockLock(lock_);terminal_=true;powered_=false;const bool pm=pmInitialized_;
    pmInitialized_=pmRegistered_=false;
    IOLockUnlock(lock_);
    // PMstop is outside the mutex needed by callbacks. It revokes future
    // normal PM work; the terminal guard independently rejects a late callback.
    if(pm)PMstop();
    IOLockLock(lock_);const bool close=opened_&&!borrowedOwner_;opened_=false;
    const bool started=started_,attached=attached_;started_=attached_=false;
    // group_ is immutable until free, allowing lifecycle callbacks to arm the
    // watchdog without first waiting on this mutex. Group shutdown already
    // dropped its lease references; lease destruction breaks the remaining arc.
    IOLockUnlock(lock_);
    if(close&&pci_)pci_->close(this);
    if(started&&pci_)IOService::stop(pci_);
    if(attached&&pci_)detach(pci_);
    IOLockUnlock(lifecycleLock_);
}
void HackyNaviFunctionLease::stop(IOService*) {shutdown();}
void HackyNaviFunctionLease::free() {
    // Attached/open/PM instances have been explicitly shut down before the
    // retained owner graph is released; init/create failure owns only CPU data.
    if(group_)group_->release();if(pci_)pci_->release();if(borrowedOwner_)borrowedOwner_->release();
    if(lifecycleLock_)IOLockFree(lifecycleLock_);if(lock_)IOLockFree(lock_);IOService::free();
}
