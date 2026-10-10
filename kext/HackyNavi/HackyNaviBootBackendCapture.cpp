// SPDX-License-Identifier: MIT
#include "HackyNaviBootBackendCapture.hpp"
#include "HackyNaviPciAccess.hpp"
#include "hackynavi/boot_backend_access.hpp"
#include <IOKit/IOLib.h>
#include <kern/clock.h>

OSDefineMetaClassAndStructors(HackyNaviBootBackendCapture,OSObject)
bool HackyNaviBootBackendCapture::init() {
    capture_={};report_={};report_.result=kIOReturnNotReady;
    report_.classifier.status=hackynavi::bootbackend::Status::InvalidArgument;
    return OSObject::init();
}
namespace {
uint64_t ticks(){uint64_t value=0;clock_get_uptime(&value);return value;}
}
struct HackyNaviBootBackendCapture::Access {
    HackyNaviBootBackendCapture& output;
    HackyNaviFunctionGroup::Operation& scope;
    IOPCIDevice* pci;IOService* owner;IOMemoryMap* map;
    uint64_t start,budget;volatile uint32_t* registers;
    hackynavi::bootbackend::ReadTicket ticket;
    Access(HackyNaviBootBackendCapture& out,HackyNaviFunctionGroup::Operation& op,
        IOPCIDevice* device,IOService* openOwner,IOMemoryMap* mapping,uint32_t timeout)
        :output(out),scope(op),pci(device),owner(openOwner),map(mapping),start(ticks()),
         budget(uint64_t(timeout)*1000),registers(nullptr) {
        pci->retain();owner->retain();map->retain();
    }
    ~Access(){map->release();owner->release();pci->release();}
    bool reject(Failure why) {
        if(output.report_.failure==Failure::None)output.report_.failure=why;
        ticket.revoke();return false;
    }
    bool valid() {
        if(output.report_.failure!=Failure::None)return false;
        const uint64_t now=ticks();uint64_t elapsed=0;
        if(now<start)return reject(Failure::Deadline);
        absolutetime_to_nanoseconds(now-start,&elapsed);output.report_.elapsedNanoseconds=elapsed;
        if(elapsed>=budget)return reject(Failure::Deadline);
        return scope.matchesGraphics(pci,owner)?true:reject(Failure::Scope);
    }
    bool physical() {
        if(!valid())return false;
        ++output.report_.pciChecks;
        if(!hackynavi::nativepci::ready(pci,owner))return reject(Failure::Pci);
        const UInt32 bar4=pci->configRead32(kIOPCIConfigBaseAddress4);
        const UInt32 bar5=pci->configRead32(kIOPCIConfigBaseAddress5);
        // BAR4 declaring 64-bit memory would make BAR5 its high half. BAR5 is
        // the actual 32-bit MMIO resource in this exact Navi31 profile.
        if(bar4==UINT32_MAX || (!(bar4&1) && (bar4&6)==4) ||
            bar5==UINT32_MAX || (bar5&7) || !(bar5&~UInt32(15)) ||
            uint64_t(bar5&~UInt32(15))!=output.report_.barPhysical ||
            map->getPhysicalAddress()!=output.report_.barPhysical ||
            map->getLength()!=output.report_.barBytes ||
            reinterpret_cast<volatile uint32_t*>(map->getVirtualAddress())!=registers ||
            (map->getMapOptions()&kIOMapCacheMask)!=kIOMapInhibitCache)
            return reject(Failure::Mapping);
        return valid();
    }
    bool bind() {
        if(!valid())return false;
        const auto address=map->getVirtualAddress();const auto bytes=map->getLength();
        if(!address || (address&3) || bytes<0x80000 || bytes>16U*1024*1024 ||
            (map->getMapOptions()&kIOMapCacheMask)!=kIOMapInhibitCache)
            return reject(Failure::Mapping);
        hackynavi::nativepci::AssignedMemory resource(pci,kIOPCIConfigBaseAddress5);
        if(!resource || resource->getPhysicalAddress()!=map->getPhysicalAddress() ||
            resource->getLength()!=bytes)return reject(Failure::Mapping);
        output.report_.barPhysical=resource->getPhysicalAddress();output.report_.barBytes=bytes;
        registers=reinterpret_cast<volatile uint32_t*>(address);return physical();
    }
    static bool guard(void* p){return static_cast<Access*>(p)->valid();}
    static bool canRead(void* p,hackynavi::bootbackend::Bank bank,uint8_t instance,uint32_t offset) {
        auto& a=*static_cast<Access*>(p);
        if(!a.valid())return false;
        if((offset&3) || offset>a.output.report_.barBytes-4 ||
            !a.ticket.permit(a.output.report_.classifier,bank,instance,offset))return a.reject(Failure::Sequence);
        return true;
    }
    static bool read32(void* p,uint32_t offset,uint32_t& value) {
        auto& a=*static_cast<Access*>(p);value=0;
        a.output.report_.lastMmioRegister=offset;a.output.report_.lastMmioValue=0;
        a.output.report_.lastMmioReadPerformed=false;
        if(!a.valid())return false;
        if(!a.ticket.consume(a.output.report_.classifier,offset))return a.reject(Failure::Sequence);
        if(!a.physical())return false;
        __asm__ __volatile__("" ::: "memory");
        value=a.registers[offset/4];++a.output.report_.mmioReads;
        a.output.report_.lastMmioValue=value;a.output.report_.lastMmioReadPerformed=true;
        OSSynchronizeIO();__asm__ __volatile__("" ::: "memory");
        return a.physical();
    }
};
HackyNaviBootBackendCapture* HackyNaviBootBackendCapture::observe(
    HackyNaviFunctionGroup::Operation& scope,IOPCIDevice* pci,IOService* owner,IOMemoryMap* map,
    const uint8_t* discovery,size_t discoveryBytes,const uint8_t* rom,size_t romBytes,
    size_t imageOffset,uint32_t timeoutUs,IOReturn& result) {
    result=kIOReturnNoMemory;auto* out=new HackyNaviBootBackendCapture;
    if(!out)return nullptr;
    if(!out->init()){out->release();return nullptr;}
    out->capture_={};out->report_={};out->report_.result=kIOReturnBadArgument;
    out->report_.classifier.status=hackynavi::bootbackend::Status::InvalidArgument;
    if(!pci || !owner || !map || !discovery || !discoveryBytes || !rom || !romBytes ||
        imageOffset>=romBytes || !timeoutUs || timeoutUs>1000000) {
        out->report_.failure=Failure::InvalidArgument;result=out->report_.result;return out;
    }
    {
        Access access(*out,scope,pci,owner,map,timeoutUs);
        if(access.bind()) {
            out->report_.classifierAttempted=true;
            const auto status=hackynavi::bootbackend::capture(discovery,discoveryBytes,
                out->report_.barBytes,rom,romBytes,imageOffset,
                {&access,Access::guard,Access::canRead,Access::read32},out->capture_,out->report_.classifier);
            using Status=hackynavi::bootbackend::Status;
            if(status==Status::Ok) {
                if(!access.ticket.complete()) {
                    access.reject(Failure::Sequence);out->report_.result=kIOReturnInternalError;
                } else out->report_.result=access.physical()?kIOReturnSuccess:kIOReturnOffline;
            } else if(status==Status::Changed)out->report_.result=kIOReturnBusy;
            else if(status==Status::ReadFailed)out->report_.result=kIOReturnIOError;
            else if(status==Status::DeviceUnavailable||status==Status::AccessDenied)out->report_.result=kIOReturnOffline;
            else out->report_.result=kIOReturnUnsupported;
        } else out->report_.result=kIOReturnNotReady;
        if(out->report_.failure!=Failure::None) {
            out->capture_.valid=false;
            out->report_.result=out->report_.failure==Failure::Deadline?kIOReturnTimeout:
                out->report_.failure==Failure::Sequence?kIOReturnInternalError:kIOReturnOffline;
        }
        // Capture/classifier status remains raw evidence on transport failures;
        // native result and failure identify post-read D0/mapping/revocation loss.
        if(out->report_.result!=kIOReturnSuccess)out->capture_.valid=false;
    }
    result=out->report_.result;return out;
}
