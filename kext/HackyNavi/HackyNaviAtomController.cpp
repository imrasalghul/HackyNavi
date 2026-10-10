// SPDX-License-Identifier: MIT
// Native cail_reg_read/write equivalence: pinned amdgpu_atombios.c uses absolute
// DWORD RREG32/WREG32 offsets, NOT discovery-relative offsets. Native MC/PLL
// callbacks there are empty; this adapter rejects those spaces instead.
#include "HackyNaviAtomController.hpp"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include <libkern/libkern.h>
#if !defined(__x86_64__)
#error HackyNaviAtomController requires native x86_64 ordered PCI MMIO
#endif
OSDefineMetaClassAndStructors(HackyNaviAtomController,OSObject)
using namespace hackynavi;
namespace {
constexpr uint32_t kIdentity=0x744c1002,kScratchComplete=0x200;
constexpr uint32_t kWaitIndex=0x0c,kWaitData=0x0d,kWaitSelection=0x58184;
constexpr uint32_t kWaitComplete=0x80000000;
constexpr uint16_t kBusMaster=4,kMemoryDecode=2;
constexpr size_t kDefaultScratchBytes=20*1024;
uint16_t u16(const uint8_t* b,size_t p) { return uint16_t(uint16_t(b[p])|uint16_t(uint16_t(b[p+1])<<8)); }
uint32_t crc32(const uint8_t* p,size_t n) {
    uint32_t crc=0xffffffffu;
    for(size_t i=0;i<n;++i) {
        crc^=p[i];for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320u:0u);
    }
    return ~crc;
}
}
bool HackyNaviAtomController::init() {
    lock_=nullptr;reset_=nullptr;device_=nullptr;openOwner_=nullptr;map_=nullptr;
    registers_=nullptr;registerBytes_=deadline_=0;rom_=nullptr;workspace_=nullptr;scratch_=nullptr;scratchBytes_=0;
    bzero(parameters_,sizeof(parameters_));program_={};execution_={};observations_={};
    subsystem_=scratch7Dword_=selector_=0;active_=hardwareHold_=selectorKnown_=false;
    state_=State::Empty;result_=kIOReturnSuccess;
    if(!OSObject::init())return false;
    lock_=IOLockAlloc();return lock_!=nullptr;
}
HackyNaviAtomController* HackyNaviAtomController::withReset(HackyNaviResetController* reset,
    IOPCIDevice* device,IOService* owner,IOMemoryMap* map,IOReturn& result) {
    result=kIOReturnNoMemory;auto* self=new HackyNaviAtomController;
    if(!self)return nullptr;
    if(!self->init()) {self->release();return nullptr;}
    result=self->initialize(reset,device,owner,map);
    if(result!=kIOReturnSuccess) {self->release();return nullptr;}
    return self;
}
IOReturn HackyNaviAtomController::initialize(HackyNaviResetController* reset,IOPCIDevice* device,
    IOService* owner,IOMemoryMap* map) {
    if(!reset || !device || !owner || !map || !reset->matchesDevice(device,owner,map) ||
       !reset->romBytes() || !reset->romByteCount() || reset->romByteCount()>atomvm::kAddressSpace ||
       !map->getVirtualAddress() || (map->getVirtualAddress()&3) || map->getLength()<0x80000 ||
       map->getLength()>16U*1024*1024 || (map->getMapOptions()&kIOMapCacheMask)!=kIOMapInhibitCache)
        return kIOReturnBadArgument;
    if(reset->state()!=HackyNaviResetController::State::Prepared &&
       reset->state()!=HackyNaviResetController::State::ProtocolCompleteRetained)return kIOReturnNotReady;
    reset_=reset;reset_->retain();device_=device;device_->retain();openOwner_=owner;openOwner_->retain();
    map_=map;map_->retain();registerBytes_=map->getLength();
    registers_=reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress());
    subsystem_=device->configRead32(kIOPCIConfigSubSystemVendorID);
    if(!providerReady(false))return kIOReturnOffline;
    rom_=OSData::withBytes(reset_->romBytes(),unsigned(reset_->romByteCount()));
    if(!rom_)return kIOReturnNoMemory;
    const uint8_t* b=static_cast<const uint8_t*>(rom_->getBytesNoCopy());
    const size_t n=rom_->getLength();ImageInfo image={};AtomInfo atom={};FirmwareInfo firmware={};
    if(parseImage(b,n,0,image)!=Status::Ok || image.vendorId!=0x1002 || image.deviceId!=0x744c ||
       image.codeType!=0 || !image.checksumValid || image.size!=n ||
       parseAtom(b,n,image,atom)!=Status::Ok || UInt16(subsystem_)!=atom.subsystemVendorId ||
       UInt16(subsystem_>>16)!=atom.subsystemDeviceId || parseFirmwareInfo(b,n,image,firmware)!=Status::Ok ||
       !atom.tables[9].offset)return kIOReturnUnsupported;
    if(atomvm::open(b,n,program_)!=atomvm::Status::Ok ||
       atomvm::prepareAsicInit(program_,true,parameters_)!=atomvm::Status::Ok)return kIOReturnUnsupported;
    const uint32_t table=u16(b,size_t(program_.commands)+4);
    // This is a firmware execution-profile guard, not a trust/authenticity
    // check. The CRC identifies the supplied board's complete ASIC_Init table
    // (including header). It binds the wait-site observation below to reviewed
    // bytecode, so a different ROM requires a new semantic audit. No VBIOS
    // bytecode is embedded in the kext; execution uses the actual captured ROM.
    if(table>=n || n-table<132 || u16(b,table)!=132 || crc32(b+table,132)!=0xc5fb8999u ||
       firmware.biosScratchRegisterDword!=0x4c)return kIOReturnUnsupported;
    scratch7Dword_=firmware.biosScratchRegisterDword+7;
    workspace_=static_cast<atomvm::Workspace*>(IOMalloc(sizeof(atomvm::Workspace)));
    // The reviewed ASIC_Init never uses FB operands. Keep the Linux default
    // CPU scratch size for the interpreter ABI; this is not a VRAM allocation.
    scratchBytes_=kDefaultScratchBytes;scratch_=static_cast<uint8_t*>(IOMalloc(scratchBytes_));
    if(!workspace_ || !scratch_)return kIOReturnNoMemory;
    bzero(scratch_,scratchBytes_);
    if(atomvm::analyze(program_,0,*workspace_,execution_)!=atomvm::Status::Ok ||
       execution_.analyzedTables!=1 || execution_.analyzedInstructions!=29)return kIOReturnUnsupported;
    state_=State::Prepared;return kIOReturnSuccess;
}
bool HackyNaviAtomController::providerReady(bool busMasterOff) const {
    if(!device_ || !openOwner_ || !map_ || !reset_ || !reset_->matchesDevice(device_,openOwner_,map_) ||
       device_->isInactive() || !device_->isOpen(openOwner_) || device_->configRead32(0)!=kIdentity ||
       device_->configRead32(kIOPCIConfigSubSystemVendorID)!=subsystem_ || subsystem_==UINT32_MAX)return false;
    const UInt16 command=device_->configRead16(kIOPCIConfigCommand);
    const UInt32 revision=device_->configRead32(kIOPCIConfigRevisionID);
    if(command==0xffff || !(command&kMemoryDecode) || (busMasterOff && (command&kBusMaster)) ||
       (revision&0xff)!=0xc8 || (revision>>24)!=3)return false;
    UInt8 pm=0;
    if(device_->findPCICapability(1,&pm)) {
        if(pm<0x40 || pm>0xf8 || (pm&3))return false;
        const UInt16 power=device_->configRead16(pm+4);if(power==0xffff || (power&3))return false;
    }
    const UInt32 bar4=device_->configRead32(kIOPCIConfigBaseAddress4),bar5=device_->configRead32(kIOPCIConfigBaseAddress5);
    IODeviceMemory* resource=device_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    return resource && bar4!=UINT32_MAX && !(!(bar4&1) && (bar4&6)==4) &&
        bar5!=UINT32_MAX && !(bar5&7) && (bar5&~UInt32(15)) &&
        resource->getPhysicalAddress()==(bar5&~UInt32(15)) &&
        map_->getPhysicalAddress()==resource->getPhysicalAddress() && map_->getLength()==resource->getLength();
}
bool HackyNaviAtomController::deadlineReady() {
    uint64_t now=0;clock_get_uptime(&now);
    if(now>=deadline_) {observations_.deadlineExpired=true;return false;}
    return true;
}
bool HackyNaviAtomController::accessReady(uint32_t index) {
    observations_.lastRegisterDword=index;
    return active_ && state_==State::Executing && registers_ && uint64_t(index)*4+4<=registerBytes_ &&
        deadlineReady() && providerReady(true);
}
bool HackyNaviAtomController::read(void* context,atomvm::Space space,uint32_t index,uint32_t& value) {
    auto* self=static_cast<HackyNaviAtomController*>(context);
    if(space!=atomvm::Space::Mmio || !self->accessReady(index))return false;
    __asm__ __volatile__("" ::: "memory");value=self->registers_[index];
    OSSynchronizeIO();__asm__ __volatile__("" ::: "memory");self->observations_.lastValue=value;
    if(index==kWaitData && self->selectorKnown_ && self->selector_==kWaitSelection) {
        ++self->observations_.waitReads;self->observations_.waitValue=value;
        if(value==kWaitComplete)self->observations_.firmwareWaitSatisfied=true;
    }
    return true;
}
bool HackyNaviAtomController::write(void* context,atomvm::Space space,uint32_t index,uint32_t value) {
    auto* self=static_cast<HackyNaviAtomController*>(context);
    if(space!=atomvm::Space::Mmio || !self->accessReady(index))return false;
    // Retain provider, map and reset owner before the FIRST potential write.
    self->holdHardware();self->observations_.lastValue=value;
    if(index==kWaitIndex) {self->selector_=value;self->selectorKnown_=true;}
    __asm__ __volatile__("" ::: "memory");self->registers_[index]=value;
    OSSynchronizeIO();__asm__ __volatile__("" ::: "memory");return true;
}
bool HackyNaviAtomController::delay(void* context,uint32_t us) {
    auto* self=static_cast<HackyNaviAtomController*>(context);
    if(!self->active_ || self->state_!=State::Executing || !self->deadlineReady())return false;
    if(us>=1000) {IOSleep(us/1000);if(us%1000)IODelay(us%1000);} else if(us)IODelay(us);
    return self->deadlineReady();
}
void HackyNaviAtomController::event(void*,atomvm::Event event,uint8_t value) {
    IOLog("HackyNavi ATOM event: type=%u value=0x%x\n",unsigned(event),unsigned(value));
}
void HackyNaviAtomController::holdHardware() { if(!hardwareHold_) {retain();hardwareHold_=true;} }
IOReturn HackyNaviAtomController::finish(IOReturn result) {
    result_=result;active_=false;IOLockUnlock(lock_);return result;
}
IOReturn HackyNaviAtomController::fail(IOReturn result) {
    state_=hardwareHold_?State::FaultedRetained:State::FailedUnpublished;
    IOLog("HackyNavi ATOM: result=0x%x VM=%s table=%u offset=0x%x opcode=%u instructions=%u wait=%u/0x%x scratch7=0x%x deadline=%u retained=%u\n",
        unsigned(result),atomvm::statusName(execution_.status),unsigned(execution_.tableIndex),unsigned(execution_.instructionOffset),
        unsigned(execution_.opcode),execution_.instructions,unsigned(observations_.firmwareWaitSatisfied),observations_.waitValue,
        observations_.scratch7After,unsigned(observations_.deadlineExpired),unsigned(hardwareHold_));
    return finish(result);
}
IOReturn HackyNaviAtomController::initializeAfterMode1() {
    IOLockLock(lock_);
    if(state_!=State::Prepared || !reset_ || reset_->state()!=HackyNaviResetController::State::ProtocolCompleteRetained ||
       !reset_->observations().resetProtocolCompleted || !reset_->observations().restoredPciVerified ||
       !reset_->observations().transactionsPendingClear)return finish(kIOReturnNotReady);
    if(!providerReady(true))return finish(kIOReturnOffline);
    observations_={};selectorKnown_=false;selector_=0;
    // Matches Linux's bounded command-loop policy, with a second cumulative
    // bytecode delay bound. The supplied script's hardware wait is <=4 seconds.
    clock_interval_to_deadline(20,kSecondScale,&deadline_);
    state_=State::Executing;active_=true;
    if(!read(this,atomvm::Space::Mmio,scratch7Dword_,observations_.scratch7Before) ||
       observations_.scratch7Before==UINT32_MAX)return fail(kIOReturnIOError);
    const atomvm::Io io={this,read,write,delay,event};
    const atomvm::Limits limits={2000000,5000000,32};
    const atomvm::Status s=atomvm::execute(program_,0,parameters_,sizeof(parameters_),scratch_,scratchBytes_,
        io,limits,*workspace_,execution_);
    if(s!=atomvm::Status::Ok)return fail(observations_.deadlineExpired || s==atomvm::Status::DelayLimit ||
        s==atomvm::Status::InstructionLimit?kIOReturnTimeout:kIOReturnIOError);
    observations_.tableExecuted=true;
    if(!read(this,atomvm::Space::Mmio,scratch7Dword_,observations_.scratch7After) ||
       observations_.scratch7After==UINT32_MAX || !providerReady(true))return fail(kIOReturnIOError);
    observations_.scratchMarkerSet=(observations_.scratch7After&kScratchComplete)!=0;
    observations_.pciCommand=device_->configRead16(kIOPCIConfigCommand);
    if(!observations_.firmwareWaitSatisfied)return fail(kIOReturnTimeout);
    if(!observations_.scratchMarkerSet || observations_.pciCommand==0xffff || (observations_.pciCommand&kBusMaster))
        return fail(kIOReturnIOError);
    state_=State::InitializedRetained;
    IOLog("HackyNavi ATOM: ASIC_Init executed; firmware wait observed, scratch7=0x%x, BME off; engine recovery and memory retirement remain\n",
        observations_.scratch7After);
    return finish(kIOReturnSuccess);
}
IOReturn HackyNaviAtomController::releaseAfterVerifiedReset(const HackyNaviResetController::RetirementProof& proof) {
    IOLockLock(lock_);
    if(active_ || state_==State::Executing || state_==State::Retired)return finish(kIOReturnNotReady);
    if(!proof.belongsTo(reset_))return finish(kIOReturnBadArgument);
    const IOReturn verified=proof.validateFor(device_,openOwner_,map_);
    if(verified!=kIOReturnSuccess)return finish(verified);
    const bool held=hardwareHold_;hardwareHold_=false;state_=State::Retired;
    const IOReturn result=finish(kIOReturnSuccess);if(held)release();return result;
}
void HackyNaviAtomController::free() {
    if(workspace_)IOFree(workspace_,sizeof(atomvm::Workspace));
    if(scratch_)IOFree(scratch_,scratchBytes_);if(rom_)rom_->release();
    registers_=nullptr;if(map_)map_->release();if(device_)device_->release();
    if(openOwner_)openOwner_->release();if(reset_)reset_->release();
    if(lock_)IOLockFree(lock_);OSObject::free();
}
