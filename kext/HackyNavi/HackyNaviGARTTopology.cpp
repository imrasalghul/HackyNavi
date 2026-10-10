// SPDX-License-Identifier: MIT
#include "HackyNaviGARTController.hpp"
#include "HackyNaviPciAccess.hpp"
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>

using namespace hackynavi;
namespace {
uint64_t nowUs() {
    uint64_t ticks=0,ns=0;clock_get_uptime(&ticks);absolutetime_to_nanoseconds(ticks,&ns);return ns/1000;
}
}
bool HackyNaviGARTController::colorTopologyReady(const HackyNaviFunctionGroup::Operation& scope) const {
    // The lexical guard identity matters in addition to its numeric epoch.
    // normalAccessReady checks the owner thread before access_ is dereferenced.
    return normalAccessReady() && access_->operation_==&scope && ownerGeneration_ &&
        scope.generation()==ownerGeneration_ && scope.matchesGraphics(device_,openOwner_) &&
        state_==State::BothEnabled && hardwareHold_ && firmwareOwner_ && firmwareReady_ &&
        !vmAdmissionClosed_ && !vmRetirementActive_ && vmRetirementFailure_==kIOReturnSuccess &&
        discovery_ && ipLayout_.valid && registerMap_ && registers_ &&
        nativepci::ready(device_,openOwner_,nativepci::BusMaster::Enabled) &&
        normalAccessReady() && scope.matchesGraphics(device_,openOwner_);
}
bool HackyNaviGARTController::colorTopologyIoReady() const {
    return colorTopologyScope_ && colorTopologyAttempted_ && mmioActive_ &&
        nowUs()<colorTopologyDeadline_ && colorTopologyReady(*colorTopologyScope_) &&
        nowUs()<colorTopologyDeadline_;
}
bool HackyNaviGARTController::colorTopologyIdentity(void* context,colortopology::Identity& out) {
    auto* self=static_cast<HackyNaviGARTController*>(context);out={};
    if(!self->colorTopologyIoReady()) return false;
    auto* device=self->device_;
    out.vendorDevice=device->configRead32(0);
    if(!self->colorTopologyIoReady()) return false;
    out.revisionClass=device->configRead32(kIOPCIConfigRevisionID);
    if(!self->colorTopologyIoReady()) return false;
    out.subsystem=device->configRead32(kIOPCIConfigSubSystemVendorID);
    if(!self->colorTopologyIoReady()) return false;
    out.bar4=device->configRead32(kIOPCIConfigBaseAddress4);
    if(!self->colorTopologyIoReady()) return false;
    out.bar5=device->configRead32(kIOPCIConfigBaseAddress5);
    if(!self->colorTopologyIoReady()) return false;
    out.command=device->configRead16(kIOPCIConfigCommand);
    if(!self->colorTopologyIoReady()) return false;
    out.headerType=device->configRead8(kIOPCIConfigHeaderType);
    return self->colorTopologyIoReady() &&
        self->registerMap_->getPhysicalAddress()==(out.bar5&~UInt32(15)) &&
        self->registerMap_->getLength()==self->registerBytes_;
}
bool HackyNaviGARTController::colorTopologyRead(void* context,uint32_t address,uint32_t& value) {
    auto* self=static_cast<HackyNaviGARTController*>(context);
    const auto& l=self->colorTopologyLayout_;
    if(address!=l.gbAddrConfig && address!=l.revisionStrap && address!=l.index &&
        address!=l.data && address!=l.indexHi) return false;
    return self->colorTopologyIoReady() && read32(self,address,value) && self->colorTopologyIoReady();
}
bool HackyNaviGARTController::colorTopologyWrite(void* context,uint32_t address,uint32_t value) {
    auto* self=static_cast<HackyNaviGARTController*>(context);
    // There is no arbitrary register-write endpoint: only this owned strap's
    // RSMU selector can be touched. The portable trace precedes this callback.
    return address==self->colorTopologyLayout_.index && value==self->colorTopologyLayout_.revisionStrap &&
        self->colorTopologyIoReady() && write32(self,address,value) && self->colorTopologyIoReady();
}
IOReturn HackyNaviGARTController::captureColorTopology(HackyNaviFunctionGroup::Operation& scope,
    uint64_t deadline,HNColorTopology& diagnostic) {
    diagnostic={};IOLockLock(lock_);
    if(!colorTopologyReady(scope)) return endOperation(kIOReturnNotReady);
    if(colorTopologyAttempted_) return endOperation(kIOReturnNotPermitted);
    const uint64_t started=nowUs();
    if(deadline<=started || deadline-started>1000000) return endOperation(kIOReturnBadArgument);
    colorTopologyStatus_=colortopology::buildLayout(
        static_cast<const uint8_t*>(discovery_->getBytesNoCopy()),discovery_->getLength(),
        registerBytes_,colorTopologyLayout_);
    if(colorTopologyStatus_!=colortopology::Status::Ok) return endOperation(kIOReturnUnsupported);
    if(!colorTopologyReady(scope) || nowUs()>=deadline) return endOperation(kIOReturnOffline);
    colorTopologyAttempted_=true;colorTopologyScope_=&scope;colorTopologyDeadline_=deadline;
    colorTopologyGeneration_=scope.generation();mmioActive_=true;
    const colortopology::Io io={this,colorTopologyIdentity,colorTopologyRead,colorTopologyWrite};
    colorTopologyStatus_=colortopology::capture(colorTopologyLayout_,io,colorTopologySnapshot_,colorTopologyTrace_);
    const bool complete=colorTopologyStatus_==colortopology::Status::Ok && colorTopologyIoReady();
    colorTopologyScope_=nullptr;
    if(!complete) {
        if(colorTopologyStatus_==colortopology::Status::Ok)
            colorTopologyStatus_=colortopology::Status::TransportError;
        colorTopologyValid_=false;
        // Neither an ambiguous selector write nor any partial/read-only capture
        // authorizes another attempt. Existing DMA/publication holds survive.
        state_=State::FaultedRetained;firmwareReady_=false;
        return endOperation(nowUs()>=deadline?kIOReturnTimeout:kIOReturnIOError);
    }
    colorTopologyValid_=true;diagnostic=colorTopologySnapshot_.topology;
    return endOperation(kIOReturnSuccess);
}
IOReturn HackyNaviGARTController::copyColorTopology(HackyNaviFunctionGroup::Operation& scope,
    HNColorTopology& diagnostic) {
    diagnostic={};IOLockLock(lock_);
    if(!colorTopologyReady(scope) || !colorTopologyValid_ || !colorTopologyAttempted_ ||
        colorTopologyGeneration_!=scope.generation()) return endOperation(kIOReturnNotReady);
    diagnostic=colorTopologySnapshot_.topology;return endOperation(kIOReturnSuccess);
}
