// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_PCI_ACCESS_HPP
#define HACKYNAVI_PCI_ACCESS_HPP
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSArray.h>

// Native PCI access for a caller already holding the actual function-group
// PM/reset/removal/resource epoch. No acquisition or lifetime authority is
// created here. Callers must keep the device/open owner alive and test epoch
// revocation around operations. Never call these helpers during reset blackout.
//
// Source: IOPCIFamily-681.140.3 IOPCIDevice.cpp:1102-1144,1208-1211,1282-1300;
// IOPCIBridge.cpp:3780-3853,4192-4222. getDeviceMemoryWithRegister has a tunnel
// L1 side effect before reading the assigned-resource property. The conventional
// findPCICapability uses configAccess(true) and an unbounded linked-list walk.
// These helpers use the retained actual property and bounded raw config reads;
// they do not enter either convenience path or run the PCI configurator.
namespace hackynavi { namespace nativepci {

// Retains the IOMemoryDescriptor itself before releasing the property snapshot.
// Lookup is by the PCI tag's register number, never array position or guessed
// BAR length. IOPCIBridge can create IOMemoryDescriptor instances directly, so
// an IODeviceMemory-only dynamic cast would reject valid assigned resources.
// Descriptor retain is not a resource-immutability or DMA-retirement proof.
class AssignedMemory {
public:
    AssignedMemory(IOPCIDevice* device,UInt8 barRegister):memory_(nullptr) {
        if(!device || device->isInactive() || barRegister<kIOPCIConfigBaseAddress0 ||
            barRegister>kIOPCIConfigBaseAddress5 || (barRegister&3))return;
        OSObject* property=device->copyProperty(gIODeviceMemoryKey);
        OSArray* ranges=OSDynamicCast(OSArray,property);
        bool valid=ranges && ranges->getCount() && ranges->getCount()<=64;
        const unsigned count=valid?ranges->getCount():0;
        for(unsigned i=0;valid && i<count;++i) {
            IOMemoryDescriptor* candidate=OSDynamicCast(IOMemoryDescriptor,ranges->getObject(i));
            if(!candidate) {valid=false;break;}
            IOPCIAddressSpace tag={};tag.bits=static_cast<UInt32>(candidate->getTag());
            if(tag.s.registerNum!=barRegister)continue;
            if(memory_ || (tag.s.space!=kIOPCI32BitMemorySpace && tag.s.space!=kIOPCI64BitMemorySpace)) {
                valid=false;break;
            }
            IOByteCount segmentBytes=0;
            const IOByteCount bytes=candidate->getLength();
            const addr64_t physical=candidate->getPhysicalSegment(0,&segmentBytes,kIOMemoryMapperNone);
            if(!physical || !bytes || segmentBytes<bytes ||
                uint64_t(bytes)-1>UINT64_MAX-uint64_t(physical)) {valid=false;break;}
            candidate->retain();memory_=candidate;
        }
        if(property)property->release();
        if((!valid || device->isInactive()) && memory_) {memory_->release();memory_=nullptr;}
    }
    ~AssignedMemory() {if(memory_)memory_->release();}
    AssignedMemory(const AssignedMemory&)=delete;
    AssignedMemory& operator=(const AssignedMemory&)=delete;
    explicit operator bool() const {return memory_!=nullptr;}
    IOMemoryDescriptor* get() const {return memory_;}
    IOMemoryDescriptor* operator->() const {return memory_;}
private:
    IOMemoryDescriptor* memory_;
};

// Conventional capabilities only, for a type0 endpoint. At most48 aligned
// entries fit in0x40..0xfc. Detect cycles, invalid/absent headers, duplicate
// requested IDs and malformed tails rather than trusting an early match.
// Failure clears both outputs. No absent-capability return implies D0.
inline bool readCapability(IOPCIDevice* device,UInt8 id,UInt8& offset,UInt16& header) {
    offset=0;header=0;
    if(!device || device->isInactive() || !id || id==0xff)return false;
    const UInt32 identity=device->configRead32(0);
    const UInt8 type=device->configRead8(kIOPCIConfigHeaderType);
    const UInt16 status=device->configRead16(kIOPCIConfigStatus);
    if(!identity || identity==UINT32_MAX || UInt16(identity)==0xffff ||
        type==0xff || (type&0x7f) || status==0xffff || !(status&0x10))return false;
    const UInt8 first=device->configRead8(kIOPCIConfigCapabilitiesPtr);
    UInt8 current=first,found=0;UInt16 foundHeader=0;uint64_t visited=0;
    for(unsigned n=0;current && n<48;++n) {
        if(current<0x40 || (current&3))return false;
        const uint64_t bit=UINT64_C(1)<<(current/4);
        if(visited&bit)return false;
        visited|=bit;
        const UInt16 value=device->configRead16(current);
        if(value==0xffff || !UInt8(value) || UInt8(value)==0xff)return false;
        if(UInt8(value)==id) {
            if(found)return false;
            found=current;foundHeader=value;
        }
        current=UInt8(value>>8);
    }
    if(current || !found || device->configRead16(found)!=foundHeader ||
        device->configRead8(kIOPCIConfigCapabilitiesPtr)!=first ||
        device->configRead8(kIOPCIConfigHeaderType)!=type ||
        device->configRead32(0)!=identity || device->isInactive())return false;
    const UInt16 afterStatus=device->configRead16(kIOPCIConfigStatus);
    if(afterStatus==0xffff || !(afterStatus&0x10))return false;
    offset=found;header=foundHeader;return true;
}

enum class BusMaster { Any,Disabled,Enabled };
// Exact supported Navi31 display function with live PM capability and D0.
// Any means BME may be either state, not that memory decode may be disabled.
// This does not select or change power/BME, validate BARs, or prove that DMA
// stopped. Controller-specific owner/lease/subsystem/revision-cache checks
// remain the caller's responsibility.
inline bool ready(IOPCIDevice* device,IOService* owner,BusMaster busMaster=BusMaster::Any) {
    constexpr UInt32 identity=0x744c1002;
    if(busMaster!=BusMaster::Any && busMaster!=BusMaster::Disabled && busMaster!=BusMaster::Enabled)return false;
    if(!device || !owner || device->isInactive() ||
        !device->isOpen(owner) || device->configRead32(0)!=identity)return false;
    const UInt16 command=device->configRead16(kIOPCIConfigCommand);
    const UInt32 revision=device->configRead32(kIOPCIConfigRevisionID);
    const UInt32 subsystem=device->configRead32(kIOPCIConfigSubSystemVendorID);
    const UInt8 type=device->configRead8(kIOPCIConfigHeaderType);
    if(command==0xffff || !(command&2) || revision==UINT32_MAX ||
        UInt8(revision)!=0xc8 || (revision>>24)!=3 || subsystem==UINT32_MAX ||
        type==0xff || (type&0x7f) ||
        (busMaster==BusMaster::Disabled && (command&4)) ||
        (busMaster==BusMaster::Enabled && !(command&4)))return false;
    UInt8 pm=0;UInt16 capability=0;
    if(!readCapability(device,1,pm,capability) || pm>0xf8)return false;
    const UInt16 power=device->configRead16(UInt8(pm+4));
    return power!=0xffff && !(power&3) && device->configRead16(pm)==capability &&
        device->configRead16(kIOPCIConfigCommand)==command &&
        device->configRead32(kIOPCIConfigRevisionID)==revision &&
        device->configRead32(kIOPCIConfigSubSystemVendorID)==subsystem &&
        device->configRead8(kIOPCIConfigHeaderType)==type &&
        device->configRead32(0)==identity && !device->isInactive() &&
        device->isOpen(owner);
}
} }
#endif
