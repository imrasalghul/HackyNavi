// SPDX-License-Identifier: MIT
#include "HackyNaviGARTController.hpp"
#include "HackyNaviPciAccess.hpp"
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
using namespace hackynavi;
namespace {
uint64_t nowUs(){uint64_t ticks=0,ns=0;clock_get_uptime(&ticks);absolutetime_to_nanoseconds(ticks,&ns);return ns/1000;}
bool same(const colortopology::Snapshot& a,const colortopology::Snapshot& b) {
    // All fields are explicitly initialized, with no padding in this ABI.
    static_assert(sizeof(colortopology::Snapshot)==32,"unexpected capture ABI");
    return !bcmp(&a,&b,sizeof(a));
}
}
bool HackyNaviGARTController::graphicsTopologyIoReady() const {
    return graphicsTopologyScope_ && graphicsTopologyAttempted_ && mmioActive_ &&
        colorTopologyValid_ && colorTopologyGeneration_==graphicsTopologyGeneration_ &&
        graphicsTopologyGeneration_==ownerGeneration_ && nowUs()<graphicsTopologyDeadline_ &&
        colorTopologyReady(*graphicsTopologyScope_) && nowUs()<graphicsTopologyDeadline_;
}
bool HackyNaviGARTController::graphicsTopologyIdentity(void* context,colortopology::Identity& out) {
    auto* self=static_cast<HackyNaviGARTController*>(context);out={};
    if(!self->graphicsTopologyIoReady() || (self->graphicsTopologyPhase_!=1 && self->graphicsTopologyPhase_!=3))return false;
    auto* device=self->device_;
    out.vendorDevice=device->configRead32(0);if(!self->graphicsTopologyIoReady())return false;
    out.revisionClass=device->configRead32(kIOPCIConfigRevisionID);if(!self->graphicsTopologyIoReady())return false;
    out.subsystem=device->configRead32(kIOPCIConfigSubSystemVendorID);if(!self->graphicsTopologyIoReady())return false;
    out.bar4=device->configRead32(kIOPCIConfigBaseAddress4);if(!self->graphicsTopologyIoReady())return false;
    out.bar5=device->configRead32(kIOPCIConfigBaseAddress5);if(!self->graphicsTopologyIoReady())return false;
    out.command=device->configRead16(kIOPCIConfigCommand);if(!self->graphicsTopologyIoReady())return false;
    out.headerType=device->configRead8(kIOPCIConfigHeaderType);
    return self->graphicsTopologyIoReady() && self->registerMap_->getPhysicalAddress()==(out.bar5&~UInt32(15)) &&
        self->registerMap_->getLength()==self->registerBytes_;
}
bool HackyNaviGARTController::graphicsTopologyRead(void* context,uint32_t address,uint32_t& value) {
    auto* self=static_cast<HackyNaviGARTController*>(context);bool permitted=false;
    if(self->graphicsTopologyPhase_==1 || self->graphicsTopologyPhase_==3) {
        const auto& l=self->colorTopologyLayout_;
        permitted=address==l.gbAddrConfig || address==l.revisionStrap || address==l.index || address==l.data || address==l.indexHi;
    } else if(self->graphicsTopologyPhase_==2) {
        const auto& l=self->graphicsTopologyLayout_;
        permitted=address==l.selector || address==l.saHardware || address==l.saUser ||
            address==l.rbHardware || address==l.rbUser || address==l.wgpHardware || address==l.wgpUser;
    }
    return permitted && self->graphicsTopologyIoReady() && read32(self,address,value) && self->graphicsTopologyIoReady();
}
bool HackyNaviGARTController::graphicsTopologyWrite(void* context,uint32_t address,uint32_t value) {
    auto* self=static_cast<HackyNaviGARTController*>(context);bool permitted=false;
    if(self->graphicsTopologyPhase_==1 || self->graphicsTopologyPhase_==3) {
        const auto& l=self->colorTopologyLayout_;
        const auto& t=self->graphicsTopologyColorTrace_[self->graphicsTopologyPhase_==1?0:1];
        permitted=address==l.index && value==l.revisionStrap && t.selectorWritesAttempted==t.selectorWritesCompleted+1;
    } else if(self->graphicsTopologyPhase_==2) {
        const auto& t=self->graphicsTopologyTrace_;
        const bool select=(value&0xff0000ffu)==0x40000000u && graphicstopology::validSelector(value,self->graphicsTopologyMetadata_);
        const bool restore=t.originalReadComplete && value==t.originalSelector && t.restoreWritesAttempted==t.restoreWritesCompleted+1;
        permitted=address==self->graphicsTopologyLayout_.selector && t.originalReadComplete &&
            t.selectorWritesAttempted==t.selectorWritesCompleted+1 && (select || restore);
    }
    return permitted && self->graphicsTopologyIoReady() && write32(self,address,value) && self->graphicsTopologyIoReady();
}
IOReturn HackyNaviGARTController::captureGraphicsTopology(HackyNaviFunctionGroup::Operation& scope,
    uint64_t deadline,graphicstopology::Facts& diagnostic) {
    diagnostic={};IOLockLock(lock_);
    if(!colorTopologyReady(scope) || !colorTopologyValid_ || colorTopologyGeneration_!=scope.generation())return endOperation(kIOReturnNotReady);
    if(graphicsTopologyAttempted_)return endOperation(kIOReturnNotPermitted);
    const uint64_t now=nowUs();if(deadline<=now || deadline-now>1000000)return endOperation(kIOReturnBadArgument);
    graphicsTopologyStatus_=graphicstopology::build(static_cast<const uint8_t*>(discovery_->getBytesNoCopy()),
        discovery_->getLength(),registerBytes_,graphicsTopologyMetadata_,graphicsTopologyLayout_);
    if(graphicsTopologyStatus_!=graphicstopology::Status::Ok)return endOperation(kIOReturnUnsupported);
    if(!colorTopologyReady(scope) || nowUs()>=deadline)return endOperation(kIOReturnOffline);
    graphicsTopologyAttempted_=true;graphicsTopologyScope_=&scope;graphicsTopologyDeadline_=deadline;
    graphicsTopologyGeneration_=scope.generation();mmioActive_=true;
    const colortopology::Io colorIo={this,graphicsTopologyIdentity,graphicsTopologyRead,graphicsTopologyWrite};
    graphicsTopologyPhase_=1;
    bool complete=colortopology::capture(colorTopologyLayout_,colorIo,graphicsTopologyColor_[0],graphicsTopologyColorTrace_[0])==colortopology::Status::Ok &&
        same(graphicsTopologyColor_[0],colorTopologySnapshot_);
    if(complete) {
        graphicsTopologyPhase_=2;
        graphicsTopologyStatus_=graphicstopology::capture(graphicsTopologyMetadata_,graphicsTopologyLayout_,
            {this,graphicsTopologyRead,graphicsTopologyWrite},graphicsTopologySnapshot_,graphicsTopologyTrace_);
        complete=graphicsTopologyStatus_==graphicstopology::Status::Ok;
    }
    if(complete) {
        graphicsTopologyPhase_=3;
        complete=colortopology::capture(colorTopologyLayout_,colorIo,graphicsTopologyColor_[1],graphicsTopologyColorTrace_[1])==colortopology::Status::Ok &&
            same(graphicsTopologyColor_[1],colorTopologySnapshot_) && graphicsTopologyIoReady();
    }
    graphicsTopologyScope_=nullptr;graphicsTopologyPhase_=0;
    if(!complete) {
        if(graphicsTopologyStatus_==graphicstopology::Status::Ok)graphicsTopologyStatus_=graphicstopology::Status::TransportError;
        graphicsTopologyValid_=false;state_=State::FaultedRetained;firmwareReady_=false;
        return endOperation(nowUs()>=deadline?kIOReturnTimeout:kIOReturnIOError);
    }
    graphicsTopologyValid_=true;diagnostic=graphicsTopologySnapshot_.facts;return endOperation(kIOReturnSuccess);
}
IOReturn HackyNaviGARTController::copyGraphicsTopology(HackyNaviFunctionGroup::Operation& scope,
    graphicstopology::Facts& diagnostic) {
    diagnostic={};IOLockLock(lock_);
    if(!colorTopologyReady(scope) || !graphicsTopologyValid_ || !graphicsTopologyAttempted_ ||
        graphicsTopologyGeneration_!=scope.generation() || colorTopologyGeneration_!=scope.generation())return endOperation(kIOReturnNotReady);
    diagnostic=graphicsTopologySnapshot_.facts;return endOperation(kIOReturnSuccess);
}
