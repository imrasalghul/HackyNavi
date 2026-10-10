// SPDX-License-Identifier: MIT
#include "HackyNaviScanoutSurface.hpp"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>

// Aperture sequence: pinned Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0
// dcn30_hubp.c:hubp3_set_vm_system_aperture_settings (dcn32 function table);
// amdgpu_dm.c:mmhub_read_system_context derives low/high from fb_start/fb_end
// with disabled AGP; dcn20_hwseq.c:enable_plane orders HUBP clock/init first.
// This native adaptation uses the existing independently implemented portable
// surface/pixel protocols and live GART allocator; it copies no Linux code.
#if !defined(__x86_64__)
#error HackyNaviScanoutSurface requires ordered x86_64 uncached BAR5 accesses
#endif
OSDefineMetaClassAndStructors(HackyNaviScanoutSurface,OSObject)
using namespace hackynavi;
namespace {
const pixelpipeline::Poll kPixelPoll={1000,10};
const hubpscanout::PollOptions kHubpPoll={1000,10};
}
bool HackyNaviScanoutSurface::init() {
    display_=nullptr;gart_=nullptr;scope_=nullptr;retirement_=nullptr;report_=nullptr;
    config_={};allocation_={};surface_={};hubpLayout_={};pixelLayout_={};
    state_=State::Empty;lease_=hardwareHold_=false;
    powerStatus_=powerConfig_=0;powerValid_=false;
    initializedBytes_=start_=last_=0;budget_=0;
    return OSObject::init();
}
HackyNaviScanoutSurface* HackyNaviScanoutSurface::withDisplay(
    HackyNaviDisplayController* display,HackyNaviFunctionGroup::Operation& scope,
    const Config& config,IOReturn& result) {
    result=kIOReturnNoMemory;auto* self=new HackyNaviScanoutSurface;
    if(!self)return nullptr;
    if(!self->init()){self->release();return nullptr;}
    result=self->initialize(display,scope,config);
    if(result!=kIOReturnSuccess){self->release();return nullptr;}
    return self;
}
IOReturn HackyNaviScanoutSurface::initialize(HackyNaviDisplayController* display,
    HackyNaviFunctionGroup::Operation& scope,const Config& c) {
    pixelpipeline::Plan pixelPlan={};
    if(!display||!display->gart_||c.pipe>=hubpscanout::kPipeCount||!c.pitchPixels||
       c.pitchPixels>16384||(c.pitchPixels&63)||c.width>c.pitchPixels||
       pixelpipeline::plan({c.width,c.height,c.format,c.outputBpc},pixelPlan)!=pixelpipeline::Status::Ok)
        return kIOReturnBadArgument;
    if(!display->gart_->normalAccessReady()||
       !scope.matchesGraphics(display->gart_->device_,display->gart_->openOwner_))return kIOReturnOffline;
    display_=display;display_->retain();gart_=display_->gart_;config_=c;
    display_->lockOwners();
    IOReturn result=kIOReturnNotReady;
    if(gart_->normalAccessReady()&&scope.matchesGraphics(gart_->device_,gart_->openOwner_)&&
       display_->state_==HackyNaviDisplayController::State::Ready&&display_->lease_&&
       gart_->displayOwner_==display_&&!display_->scanoutOwner_&&gart_->firmwareReady_&&
       gart_->state_==HackyNaviGARTController::State::BothEnabled&&gart_->discovery_) {
        const auto* discovery=static_cast<const uint8_t*>(gart_->discovery_->getBytesNoCopy());
        const size_t bytes=gart_->discovery_->getLength();
        const uint64_t footprint=uint64_t(c.pitchPixels)*4*c.height;
        const uint64_t allocated=(footprint+4095)&~UINT64_C(4095);
        if(hubpscanout::buildLayout(discovery,bytes,gart_->registerBytes_,hubpLayout_)!=hubpscanout::Status::Ok||
           pixelpipeline::buildLayout(discovery,bytes,gart_->registerBytes_,pixelLayout_)!=pixelpipeline::Status::Ok)
            result=kIOReturnUnsupported;
        else if(gart_->allocator_.allocate(allocated,4096,allocation_)!=VramAllocatorStatus::Ok)
            result=kIOReturnNoSpace;
        else {
            surface_={allocation_.handle,0,c.pitchPixels,0,0,c.width,c.height,c.format};
            hubpscanout::Plan plan={};
            if(hubpscanout::plan(gart_->config_.memory,gart_->allocator_,surface_,plan)!=hubpscanout::Status::Ok)
                result=kIOReturnUnsupported;
            else if(!gart_->normalAccessReady()||!scope.matchesGraphics(gart_->device_,gart_->openOwner_))result=kIOReturnOffline;
            else {display_->scanoutOwner_=this;lease_=true;state_=State::Allocated;result=kIOReturnSuccess;}
        }
    }
    return display_->finish(result);
}
uint64_t HackyNaviScanoutSurface::timeUs() {
    uint64_t ticks=0,ns=0;clock_get_uptime(&ticks);absolutetime_to_nanoseconds(ticks,&ns);return ns/1000;
}
bool HackyNaviScanoutSurface::ready() {
    if(!scopeValid()||
       !lease_||display_->scanoutOwner_!=this||gart_->displayOwner_!=display_||
       display_->state_!=HackyNaviDisplayController::State::Ready||!gart_->firmwareReady_||
       gart_->state_!=HackyNaviGARTController::State::BothEnabled) {
        if(report_&&report_->result==kIOReturnSuccess)report_->result=kIOReturnOffline;
        return false;
    }
    const uint64_t now=timeUs();
    if(now<last_||now-start_>=budget_) {
        if(report_&&report_->result==kIOReturnSuccess)report_->result=now<last_?kIOReturnError:kIOReturnTimeout;
        return false;
    }
    last_=now;report_->elapsedUs=now-start_;return true;
}
bool HackyNaviScanoutSurface::scopeValid() const {
    if(bool(scope_)==bool(retirement_))return false;
    return scope_?(gart_->normalAccessReady()&&scope_->matchesGraphics(gart_->device_,gart_->openOwner_)):
        (gart_->accessReady()&&retirement_->matchesGraphics(gart_->device_,gart_->openOwner_));
}
bool HackyNaviScanoutSurface::begin(HackyNaviFunctionGroup::Operation* scope,
    HackyNaviFunctionGroup::RetirementScope* retirement,uint32_t us,Report& r) {
    // Called only after locking Display -> Firmware -> GART. Scope is outer.
    r={};r.result=kIOReturnSuccess;r.allocationBytes=allocation_.range.bytes;
    r.initializedBytes=initializedBytes_;scope_=scope;retirement_=retirement;report_=&r;
    start_=last_=timeUs();budget_=us;
    if(!us||us>2000000){r.result=kIOReturnBadArgument;return false;}
    if(retirement_) {
        const uint32_t remaining=retirement_->remainingUs();
        if(!remaining){r.result=kIOReturnTimeout;return false;}
        if(budget_>remaining)budget_=remaining;
    }
    if(!ready())return false;
    gart_->mmioActive_=true;return true;
}
IOReturn HackyNaviScanoutSurface::finish(IOReturn result,Report& r) {
    if(r.result==kIOReturnSuccess)r.result=result;
    r.retentionRequired=hardwareHold_;r.initializedBytes=initializedBytes_;
    scope_=nullptr;retirement_=nullptr;report_=nullptr;
    return display_->finish(r.result);
}
bool HackyNaviScanoutSurface::rawRead(void* p,uint32_t offset,uint32_t& value) {
    auto* self=static_cast<HackyNaviScanoutSurface*>(p);
    if(!self->ready()||!self->gart_->ioReady(offset))return false;
    auto& r=*self->report_;++r.reads;r.lastRegister=offset;
    __asm__ __volatile__("" ::: "memory");value=self->gart_->registers_[offset/4];
    OSSynchronizeIO();__asm__ __volatile__("" ::: "memory");r.lastValue=value;
    // MM_DATA may legitimately be all ones; register protocols reject it.
    return self->ready();
}
bool HackyNaviScanoutSurface::rawWrite(void* p,uint32_t offset,uint32_t value) {
    auto* self=static_cast<HackyNaviScanoutSurface*>(p);
    if(!self->ready()||!self->gart_->ioReady(offset))return false;
    auto& r=*self->report_;++r.writes;r.lastRegister=offset;r.lastValue=value;r.writeAttempted=true;
    __asm__ __volatile__("" ::: "memory");self->gart_->registers_[offset/4]=value;
    OSSynchronizeIO();__asm__ __volatile__("" ::: "memory");return self->ready();
}
bool HackyNaviScanoutSurface::power() {
    uint32_t status=0,config=0,after=0;const auto* regs=hubpLayout_.registers[config_.pipe];
    const auto powerReg=regs[size_t(hubpscanout::Register::Power)];
    const auto configReg=regs[size_t(hubpscanout::Register::PowerConfig)];
    if(!rawRead(this,powerReg,status)||(status!=0&&status!=0x10000000)||
        !rawRead(this,configReg,config)||config>1||
        (powerValid_&&(status!=powerStatus_||config!=powerConfig_))||
        !rawRead(this,powerReg,after)||after!=status||
        !rawRead(this,configReg,after)||after!=config||
        !rawRead(this,powerReg,after)||after!=status)return false;
    powerStatus_=status;powerConfig_=config;powerValid_=true;return true;
}
bool HackyNaviScanoutSurface::pipeRead(void* p,uint32_t offset,uint32_t& value) {
    auto* self=static_cast<HackyNaviScanoutSurface*>(p);
    return self->power()&&rawRead(p,offset,value)&&value!=UINT32_MAX&&self->power();
}
bool HackyNaviScanoutSurface::pipeWrite(void* p,uint32_t offset,uint32_t value) {
    auto* self=static_cast<HackyNaviScanoutSurface*>(p);
    return self->power()&&rawWrite(p,offset,value)&&self->power();
}
void HackyNaviScanoutSurface::delay(void* p,uint32_t us) {
    auto* self=static_cast<HackyNaviScanoutSurface*>(p);
    if(us<=1000&&self->ready()){IODelay(us);(void)self->ready();}
}
void HackyNaviScanoutSurface::holdHardware() {if(!hardwareHold_){retain();hardwareHold_=true;}}
IOReturn HackyNaviScanoutSurface::upload(HackyNaviFunctionGroup::Operation& scope,uint64_t offset,
    const uint8_t* data,size_t bytes,uint32_t timeout,Report& out) {
    display_->lockOwners();
    if(!begin(&scope,nullptr,timeout,out))return finish(out.result,out);
    out.stage=Stage::Upload;
    if(state_!=State::Allocated||offset!=initializedBytes_||!data||!bytes||(bytes&3)||
       bytes>kVramTransferMaxBytes||bytes-1>UINTPTR_MAX-reinterpret_cast<uintptr_t>(data)||
       offset>=allocation_.range.bytes||bytes>allocation_.range.bytes-offset)
        return finish(kIOReturnBadArgument,out);
    // CPU posted writes can remain ambiguous after transport/revocation. Pin
    // before dispatch; readback plus HDP drains a successful chunk. A failed
    // chunk cannot release/reuse its VRAM until independent verified reset.
    holdHardware();
    const VramTransferIo io={this,gart_->registerBytes_,rawRead,rawWrite};
    if(uploadVram(gart_->allocator_,allocation_.handle,offset,data,bytes,io,out.upload)!=VramTransferStatus::Ok) {
        state_=State::FaultedRetained;return finish(kIOReturnIOError,out);
    }
    out.stage=Stage::Visibility;
    if(flushHdp({this,rawRead,rawWrite},gart_->hdpState_,out.hdp)!=HdpStatus::Ok||!ready()) {
        state_=State::FaultedRetained;return finish(kIOReturnIOError,out);
    }
    initializedBytes_+=bytes;
    if(initializedBytes_==allocation_.range.bytes)state_=State::Initialized;
    hardwareHold_=false;out.stage=Stage::Complete;
    const IOReturn result=finish(kIOReturnSuccess,out);release();return result;
}
IOReturn HackyNaviScanoutSurface::prepareBlanked(HackyNaviFunctionGroup::Operation& scope,uint32_t timeout,Report& out) {
    const uint64_t started=timeUs();
    display_->lockOwners();
    if(!begin(&scope,nullptr,timeout,out))return finish(out.result,out);
    start_=last_=started;
    if(state_!=State::Initialized||!display_->pipePowers_[config_.pipe].powered()||
       !display_->pipeClocks_[config_.pipe].requiresHardwareRetirement()||
       !display_->dppReferenceKHz_||!display_->dispReferenceKHz_||!display_->clockSmu_.ready())
        return finish(kIOReturnNotReady,out);
    out.stage=Stage::VerifyClocks;
    for(unsigned i=0;i<2;++i) {
        const auto result=display_->verifyGlobalClockReferenceLocked(scope,
            i?displaysmu::Clock::Dpp:displaysmu::Clock::Disp,start_,budget_,out.globalClocks[i]);
        if(result!=kIOReturnSuccess||!ready())return finish(result,out);
    }
    holdHardware();out.stage=Stage::Pixel;
    const pixelpipeline::Config pixelConfig={config_.width,config_.height,config_.format,config_.outputBpc};
    if(pixel_.configure(pixelLayout_,{this,pipeRead,pipeWrite,delay},config_.pipe,pixelConfig,kPixelPoll,out.pixel)
       !=pixelpipeline::Status::Ok) {state_=State::FaultedRetained;return finish(kIOReturnIOError,out);}
    out.stage=Stage::Aperture;
    if(hubpaperture::program(static_cast<const uint8_t*>(gart_->discovery_->getBytesNoCopy()),
       gart_->discovery_->getLength(),gart_->registerBytes_,{this,rawRead,rawWrite,delay},
       config_.pipe,gart_->config_.memory,out.aperture)!=hubpaperture::Status::Ok) {
        state_=State::FaultedRetained;return finish(kIOReturnIOError,out);
    }
    out.stage=Stage::Surface;
    if(hubp_.configure(hubpLayout_,{this,pipeRead,pipeWrite,delay},config_.pipe,gart_->config_.memory,
       gart_->allocator_,surface_,out.hubp)!=hubpscanout::Status::Ok||!ready()) {
        state_=State::FaultedRetained;return finish(kIOReturnIOError,out);
    }
    state_=State::Prepared;out.stage=Stage::Complete;return finish(kIOReturnSuccess,out);
}
IOReturn HackyNaviScanoutSurface::retireBlanked(HackyNaviFunctionGroup::Operation& scope,uint32_t timeout,Report& out) {
    return retireBlankedImpl(&scope,nullptr,timeout,out);
}
IOReturn HackyNaviScanoutSurface::retireBlanked(HackyNaviFunctionGroup::RetirementScope& scope,uint32_t timeout,Report& out) {
    return retireBlankedImpl(nullptr,&scope,timeout,out);
}
IOReturn HackyNaviScanoutSurface::retireBlankedImpl(HackyNaviFunctionGroup::Operation* scope,
    HackyNaviFunctionGroup::RetirementScope* retirement,uint32_t timeout,Report& out) {
    display_->lockOwners();
    if(!begin(scope,retirement,timeout,out))return finish(out.result,out);
    if(state_!=State::Prepared)return finish(kIOReturnNotReady,out);
    out.stage=Stage::StopSurface;
    if(hubp_.stop(kHubpPoll,out.hubp)!=hubpscanout::Status::Ok) {
        state_=State::FaultedRetained;return finish(kIOReturnIOError,out);
    }
    out.stage=Stage::StopPixel;
    if(pixel_.stop(kPixelPoll,out.pixel)!=pixelpipeline::Status::Ok||!ready()) {
        state_=State::FaultedRetained;return finish(kIOReturnIOError,out);
    }
    out.stage=Stage::Release;const IOReturn result=releaseLocked();
    if(result!=kIOReturnSuccess){state_=State::FaultedRetained;return finish(result,out);}
    state_=State::Retired;hardwareHold_=false;out.stage=Stage::Complete;
    finish(kIOReturnSuccess,out);release();return kIOReturnSuccess;
}
IOReturn HackyNaviScanoutSurface::releaseLocked() {
    if(lease_&&(display_->scanoutOwner_!=this||!display_->lease_||gart_->displayOwner_!=display_))
        return kIOReturnInternalError;
    if(allocation_.handle.owner) {
        if(gart_->allocator_.release(allocation_.handle)!=VramAllocatorStatus::Ok)return kIOReturnInternalError;
        allocation_={};surface_={};initializedBytes_=0;
    }
    if(lease_) {
        display_->scanoutOwner_=nullptr;lease_=false;
    }
    return kIOReturnSuccess;
}
IOReturn HackyNaviScanoutSurface::releaseUnpublished(HackyNaviFunctionGroup::Operation& scope) {
    return releaseUnpublishedImpl(&scope,nullptr);
}
IOReturn HackyNaviScanoutSurface::releaseUnpublished(HackyNaviFunctionGroup::RetirementScope& scope) {
    return releaseUnpublishedImpl(nullptr,&scope);
}
IOReturn HackyNaviScanoutSurface::releaseUnpublishedImpl(HackyNaviFunctionGroup::Operation* normal,
    HackyNaviFunctionGroup::RetirementScope* retirement) {
    display_->lockOwners();
    if(bool(normal)==bool(retirement)||!(normal?(gart_->normalAccessReady()&&normal->matchesGraphics(gart_->device_,gart_->openOwner_)):
        (gart_->accessReady()&&retirement->matchesGraphics(gart_->device_,gart_->openOwner_))))return display_->finish(kIOReturnOffline);
    if(hardwareHold_||hubp_.requiresHardwareRetirement()||pixel_.requiresHardwareRetirement())
        return display_->finish(kIOReturnBusy);
    const IOReturn result=releaseLocked();if(result==kIOReturnSuccess)state_=State::Retired;
    return display_->finish(result);
}
IOReturn HackyNaviScanoutSurface::releaseAfterVerifiedReset(HackyNaviFunctionGroup::RetirementScope& scope,
    const HackyNaviResetController::RetirementProof& proof) {
    if(!gart_->accessReady()||!scope.matchesGraphics(gart_->device_,gart_->openOwner_))return kIOReturnOffline;
    // Reset owns its own mutex. Validate BEFORE Display -> Firmware -> GART;
    // the actual outer retirement scope stays held throughout proof transfer.
    IOReturn result=proof.validateFor(gart_->device_,gart_->openOwner_,gart_->registerMap_);
    if(result!=kIOReturnSuccess)return result;
    display_->lockOwners();
    if(!gart_->accessReady()||!scope.matchesGraphics(gart_->device_,gart_->openOwner_))return display_->finish(kIOReturnOffline);
    result=releaseLocked();if(result!=kIOReturnSuccess)return display_->finish(result);
    hubp_.clearAfterVerifiedReset();pixel_.clearAfterVerifiedReset();state_=State::Retired;
    powerStatus_=powerConfig_=0;powerValid_=false;
    const bool drop=hardwareHold_;hardwareHold_=false;
    display_->finish(kIOReturnSuccess);if(drop)release();return kIOReturnSuccess;
}
void HackyNaviScanoutSurface::free() {
    // Hardware publication self-retains. This path only releases unpublished
    // allocations or already-retired bookkeeping; it issues no device access.
    if(display_) {
        display_->lockOwners();(void)releaseLocked();display_->finish(kIOReturnSuccess);
        display_->release();display_=nullptr;gart_=nullptr;
    }
    OSObject::free();
}
