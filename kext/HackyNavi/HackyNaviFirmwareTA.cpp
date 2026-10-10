// SPDX-License-Identifier: MIT
#include "HackyNaviFirmwareController.hpp"
#include "HackyNaviDisplayController.hpp"
#include <IOKit/IOLib.h>

using namespace hackynavi;
namespace {
constexpr pspring::PollOptions kTaPoll={100000,10};
constexpr pspta::Kind kTaKinds[]={pspta::Kind::Hdcp,pspta::Kind::Dtm};
bool validKind(pspta::Kind kind) { return kind==pspta::Kind::Hdcp||kind==pspta::Kind::Dtm; }
bool overlaps(const void* a,size_t an,const void* b,size_t bn) {
    const uintptr_t x=reinterpret_cast<uintptr_t>(a),y=reinterpret_cast<uintptr_t>(b);
    return x<=y ? y-x<an : x-y<bn;
}
IOReturn displayError(pspdisplayta::Status s) {
    using S=pspdisplayta::Status;
    if(s==S::Ok)return kIOReturnSuccess;
    if(s==S::InvalidArgument||s==S::AliasedStorage)return kIOReturnBadArgument;
    if(s==S::Busy)return kIOReturnBusy;
    if(s==S::Exhausted)return kIOReturnNoSpace;
    if(s==S::StaleToken||s==S::InvalidState)return kIOReturnNotPermitted;
    return kIOReturnIOError;
}
}

bool HackyNaviFirmwareController::taValid(void* context) {
    auto* self=static_cast<HackyNaviFirmwareController*>(context);
    if(self->taFailure_!=kIOReturnSuccess)return false;
    const bool normal=self->taScope_&&!self->taRetirementScope_&&self->gart_->normalAccessReady()&&
        self->taScope_->matchesGraphics(self->gart_->device_,self->gart_->openOwner_)&&
        self->taScope_->generation()==self->taGeneration_&&self->state_==State::FirmwareLoaded;
    // Revoked retirement has its own new generation. It can close only the
    // privately retained old records; it never publishes a new session.
    const bool retiring=self->taRetirementScope_&&!self->taScope_&&
        self->gart_->retirementAccessReady(*self->taRetirementScope_)&&
        self->taRetirementScope_->matchesGraphics(self->gart_->device_,self->gart_->openOwner_)&&
        (self->state_==State::FirmwareLoaded||self->state_==State::Quiesced);
    if((!normal&&!retiring)||!self->providerReady()) {
        self->taFailure_=kIOReturnOffline;return false;
    }
    const auto now=clockTime(self);
    if(now<self->taLast_){self->taFailure_=kIOReturnError;return false;}
    self->taLast_=now;
    if(now-self->taStart_>=self->taBudget_){self->taFailure_=kIOReturnTimeout;return false;}
    return true;
}
bool HackyNaviFirmwareController::taRead(void* c,uint32_t reg,uint32_t& value) {
    return taValid(c)&&read32(c,reg,value)&&taValid(c);
}
bool HackyNaviFirmwareController::taWrite(void* c,uint32_t reg,uint32_t value) {
    return taValid(c)&&write32(c,reg,value)&&taValid(c);
}
bool HackyNaviFirmwareController::taFlush(void* c) {
    auto* self=static_cast<HackyNaviFirmwareController*>(c);
    // Use the TA callbacks inside the flush too: its publication write and
    // posting read belong to the same deadline as the command/VRAM accesses.
    return taValid(c)&&hackynavi::flushHdp({c,taRead,taWrite},self->gart_->hdpState_,
        self->gart_->hdpResult_)==HdpStatus::Ok&&taValid(c);
}
bool HackyNaviFirmwareController::taVisibility(void* c) {
    return taValid(c)&&beforeReadback(c)&&taValid(c);
}
void HackyNaviFirmwareController::taDelayUs(void* c,uint32_t us) {
    auto* self=static_cast<HackyNaviFirmwareController*>(c);
    if(!taValid(c))return;
    if(us>=self->taBudget_-(self->taLast_-self->taStart_)) {
        self->taFailure_=kIOReturnTimeout;return;
    }
    if(us>=1000)IOSleep((us+999)/1000);else IODelay(us);
    (void)taValid(c);
}
void HackyNaviFirmwareController::taDelayMs(void* c,uint32_t ms) {
    auto* self=static_cast<HackyNaviFirmwareController*>(c);
    if(ms>UINT32_MAX/1000){self->taFailure_=kIOReturnTimeout;return;}
    taDelayUs(c,ms*1000);
}
pspring::Io HackyNaviFirmwareController::taIo() {
    return {this,taRead,taWrite,taFlush,taVisibility,taDelayUs,taDelayMs};
}
IOReturn HackyNaviFirmwareController::beginTrustedApplication(HackyNaviFunctionGroup::Operation& scope,
    pspta::Kind kind,uint32_t timeoutUs,TrustedApplicationResult& out,uint64_t started) {
    // The outer scope excludes changes to retained Display pointers checked
    // by taOutputAliases. Invalid/foreign callers cannot inspect that ledger
    // or clear a potentially aliased output before acquiring real authority.
    if(!gart_||!gart_->normalAccessReady()||!scope.matchesGraphics(gart_->device_,gart_->openOwner_))
        return kIOReturnOffline;
    if(taOutputAliases(&out,sizeof(out),&scope,sizeof(scope)))return kIOReturnBadArgument;
    out={};out.result=kIOReturnBadArgument;
    if(!validKind(kind)||!timeoutUs||timeoutUs>2000000)return out.result;
    if(!started)started=clockTime(this);
    const auto result=begin(State::FirmwareLoaded);if(result!=kIOReturnSuccess)return out.result=result;
    if(!hardwareHold_||!gart_->firmwareReady_||gart_->state_!=HackyNaviGARTController::State::BothEnabled||
        ring_.phase()!=pspring::Phase::Running||ring_.pending()||!ring_.tmrReady()||
        !ring_.needsRingRetention()||!ring_.needsTmrRetention())return out.result=finish(kIOReturnNotReady);
    if(taGeneration_&&taGeneration_!=scope.generation())return out.result=finish(kIOReturnOffline);
    taScope_=&scope;taRetirementScope_=nullptr;taGeneration_=scope.generation();taStart_=taLast_=started;
    taBudget_=timeoutUs;taFailure_=kIOReturnSuccess;
    if(!taValid(this))return finishTrustedApplication(kind,out,taFailure_);
    return kIOReturnSuccess;
}
IOReturn HackyNaviFirmwareController::finishTrustedApplication(pspta::Kind kind,
    TrustedApplicationResult& out,IOReturn result,bool faulted) {
    if(result==kIOReturnSuccess&&!taValid(this))faulted=true;
    if(taFailure_!=kIOReturnSuccess)result=taFailure_;
    if(faulted) {state_=State::FaultedRetained;gart_->firmwareReady_=false;}
    out.result=result;out.generation=taGeneration_;out.elapsedUs=taLast_-taStart_;
    if(out.ring.commandId)ringResult_=out.ring;
    (void)ring_.taInformation(kind,out.application);
    if(result!=kIOReturnSuccess)
        IOLog("HackyNavi: PSP TA kind=%u result=0x%x parser-attempted=%u parser=%s command=%u ring=%s phase=%u "
            "load-ack=%u unload-ack=%u retained=%u elapsed-us=%llu\n",
            unsigned(kind),result,unsigned(out.parserAttempted),pspta::statusString(out.parser),
            out.ring.commandId,pspring::statusString(out.ring.status),
            unsigned(out.application.phase),unsigned(out.application.loadAcknowledged),
            unsigned(out.application.unloadAcknowledged),unsigned(out.application.retained),
            static_cast<unsigned long long>(out.elapsedUs));
    taScope_=nullptr;taRetirementScope_=nullptr;taBudget_=0;
    return finish(result);
}
IOReturn HackyNaviFirmwareController::loadTrustedApplication(HackyNaviFunctionGroup::Operation& scope,
    pspta::Kind kind,uint32_t timeoutUs,TrustedApplicationResult& out) {
    const auto ready=beginTrustedApplication(scope,kind,timeoutUs,out);if(ready!=kIOReturnSuccess)return ready;
    return finishTrustedApplication(kind,out,loadTrustedApplicationLocked(kind,out));
}
IOReturn HackyNaviFirmwareController::loadTrustedApplicationLocked(pspta::Kind kind,TrustedApplicationResult& out) {
    out={};out.result=kIOReturnBadArgument;
    if(!ring_.taInformation(kind,out.application)||out.application.phase!=pspring::TaPhase::Fresh)
        return out.result=taError(kIOReturnNotPermitted);
    FirmwareResource resource{};pspta::Image image{};
    if(!getFirmwareResource(FirmwareResourceId::PspTa,resource))
        return out.result=taError(kIOReturnNotFound);
    out.parserAttempted=true;out.parser=pspta::parse(resource.data,resource.bytes,image);
    if(out.parser!=pspta::Status::Ok)return out.result=taError(kIOReturnUnsupported);
    const auto& application=kind==pspta::Kind::Hdcp?image.hdcp:image.dtm;
    const auto& input=application.image;
    if(!input.size||(input.size&3)||input.size>psp::kStagingBytes||
        input.offset>resource.bytes||input.size>resource.bytes-input.offset)
        return out.result=taError(kIOReturnUnsupported);
    const auto stageSlot=kind==pspta::Kind::Hdcp?HdcpStage:DtmStage;
    const auto sharedSlot=kind==pspta::Kind::Hdcp?HdcpShared:DtmShared;
    // Fresh TA also requires fresh native slots. Never replace a retained BO.
    if(slots_[stageSlot].handle.owner||slots_[sharedSlot].handle.owner)
        return out.result=taError(kIOReturnNotPermitted);
    const uint64_t stageBytes=(uint64_t(input.size)+4095)&~UINT64_C(4095);
    if(gart_->allocator_.allocate(stageBytes,4096,slots_[stageSlot])!=VramAllocatorStatus::Ok)
        return out.result=taError(kIOReturnNoSpace);
    if(gart_->allocator_.allocate(pspta::kSharedBytes,4096,slots_[sharedSlot])!=VramAllocatorStatus::Ok) {
        if(gart_->allocator_.release(slots_[stageSlot].handle)!=VramAllocatorStatus::Ok)
            return out.result=taError(kIOReturnInternalError,true);
        slots_[stageSlot]={};return out.result=taError(kIOReturnNoSpace);
    }
    if(pspring::validateInputAllocation(ring_,slots_[stageSlot].handle,0,input.size)!=pspring::Status::Ok||
        pspring::validateInputAllocation(ring_,slots_[sharedSlot].handle,0,pspta::kSharedBytes)!=pspring::Status::Ok)
        return out.result=taError(kIOReturnInternalError,true);
    // Every actual MM_INDEX/MM_DATA access is guarded by this same live scope
    // and measured deadline. uploadVram verifies each DWORD by uncached readback.
    const VramTransferIo transfer={this,gart_->registerBytes_,taRead,taWrite};
    static_assert(psp::kStagingBytes>=pspta::kSharedBytes,"TA shared initialization workspace");
    for(size_t i=0;i<pspta::kSharedBytes;++i)workspace_[i]=0;
    if(uploadVram(gart_->allocator_,slots_[sharedSlot].handle,0,workspace_,pspta::kSharedBytes,
        transfer,out.sharedUpload)!=VramTransferStatus::Ok||
        uploadVram(gart_->allocator_,slots_[stageSlot].handle,0,resource.data+input.offset,input.size,
        transfer,out.firmwareUpload)!=VramTransferStatus::Ok||!taFlush(this))
        return out.result=taError(kIOReturnIOError,true);
    if(pspring::submitLoadTa(taIo(),ring_,kind,slots_[stageSlot].handle,0,input.size,
        slots_[sharedSlot].handle,out.ring)!=pspring::Status::Ok||
        pspring::wait(taIo(),ring_,kTaPoll,out.ring)!=pspring::Status::Ok)
        return out.result=taError(kIOReturnIOError,true);
    if(!ring_.taInformation(kind,out.application)||out.application.phase!=pspring::TaPhase::Loaded||
        !out.application.sessionValid||!out.application.loadAcknowledged||!out.application.retained)
        return out.result=taError(kIOReturnInternalError,true);
    // Keep the private firmware slot until TA unload/owner retirement too. The
    // portable ring needs it only until LOAD completion; retaining it longer
    // avoids reuse ambiguity without inventing an additional hardware user.
    return out.result=taError(kIOReturnSuccess);
}

void HackyNaviFirmwareController::taFault(void* context) {
    auto* self=static_cast<HackyNaviFirmwareController*>(context);
    // Callback holds Firmware -> GART. fault() would recursively unlock them.
    self->state_=State::FaultedRetained;self->gart_->firmwareReady_=false;
}
IOReturn HackyNaviFirmwareController::taError(IOReturn result,bool faulted) {
    if(faulted)taFault(this);
    return result;
}
bool HackyNaviFirmwareController::taOutputAliases(const void* out,size_t bytes,
    const void* scope,size_t scopeBytes,const HackyNaviDisplayController* display) const {
    // Reject before clearing outputs; a reference can otherwise overwrite its
    // actual owner, lexical authority, allocator or persistent PSP workspace.
    for(const auto& record:displayTaRecords_)
        if(record.owner&&overlaps(out,bytes,record.owner,sizeof(*record.owner)))return true;
    return overlaps(out,bytes,this,sizeof(*this))||overlaps(out,bytes,scope,scopeBytes)||
        (gart_&&overlaps(out,bytes,gart_,sizeof(*gart_)))||
        (workspace_&&overlaps(out,bytes,workspace_,psp::kStagingBytes))||
        (display&&overlaps(out,bytes,display,sizeof(*display)));
}
bool HackyNaviFirmwareController::displayTopologyLocked(HackyNaviDisplayController& display,
    uint8_t path,pspdisplayta::Topology& topology) const {
    if(display.firmware_!=this||display.gart_!=gart_||!display.lease_||!display.hardwareHold_||
        gart_->displayOwner_!=&display||display.state_!=HackyNaviDisplayController::State::Ready||
        !display.providerReady()||!display.validPath(path,0x0c)||
        display.linkKinds_[path]!=HackyNaviDisplayController::LinkKind::Hdmi||
        display.links_[path].phase()!=dio::Phase::Enabled||
        display.streams_[path].phase()!=hdmistream::Phase::Running||
        !display.streams_[path].requiresHardwareRetirement()||display.streamPipe_[path]>=4||
        display.aux_.ownsPins()||display.ddc_.ownsController()||!display.inbox_.isOpen())return false;
    const auto& saved=display.routes_[path];const auto& owned=display.links_[path].route();
    if(saved.connectorObject!=owned.connectorObject||saved.encoderObject!=owned.encoderObject||
        saved.phy!=owned.phy||saved.ddc!=owned.ddc||saved.hpd!=owned.hpd||
        saved.hpdActiveHigh!=owned.hpdActiveHigh||saved.phy>=5||saved.ddc>=5||
        display.links_[path].frontendIndex()>=5)return false;
    // Pinned Linux link_is_hdcp14 assumes HDMI capability for this DTM
    // informational field. It does not probe the sink or prove authentication.
    topology={display.streamPipe_[path],saved.ddc,display.links_[path].frontendIndex(),saved.phy,true};
    return true;
}
void HackyNaviFirmwareController::snapshotDisplayTrusted(DisplayTrustedResult& out) {
    out.ownedDisplays=out.ownedAuthentications=0;
    for(const auto& record:displayTaRecords_) {
        if(record.owner)++out.ownedDisplays;
        if(record.authenticationActive)++out.ownedAuthentications;
    }
    out.bindingRetired=displayTa_.phase()==pspdisplayta::Phase::Retired;
    out.retentionRequired=displayTa_.retentionRequired()||ring_.needsTaRetention()||out.ownedDisplays;
}
IOReturn HackyNaviFirmwareController::finishDisplayTrusted(DisplayTrustedResult& out,IOReturn result) {
    snapshotDisplayTrusted(out);
    if(result==kIOReturnSuccess&&!taValid(this)&&out.retentionRequired)taFault(this);
    if(taFailure_!=kIOReturnSuccess)result=taFailure_;
    out.result=result;out.generation=taGeneration_;out.elapsedUs=taLast_-taStart_;
    if(out.protocol.psp.commandId)ringResult_=out.protocol.psp;
    else if(out.application.ring.commandId)ringResult_=out.application.ring;
    if(out.applicationAttempted) {
        out.application.generation=taGeneration_;out.application.elapsedUs=out.elapsedUs;
    }
    if(result!=kIOReturnSuccess)
        IOLog("HackyNavi: display TA result=0x%x typed-attempt=%u typed=%s PSP-ack=%u app-ack=%u "
            "app-status=%u displays=%u auth=%u retained=%u elapsed-us=%llu\n",result,
            unsigned(out.protocolAttempted),pspdisplayta::statusString(out.protocol.status),
            unsigned(out.protocol.pspAcknowledged),unsigned(out.protocol.applicationAcknowledged),
            out.protocol.applicationStatus,out.ownedDisplays,out.ownedAuthentications,
            unsigned(out.retentionRequired),static_cast<unsigned long long>(out.elapsedUs));
    taScope_=nullptr;taRetirementScope_=nullptr;taBudget_=0;
    return finish(result);
}
IOReturn HackyNaviFirmwareController::displayTrustedOperation(HackyNaviFunctionGroup::Operation& scope,
    HackyNaviDisplayController& display,uint8_t path,DisplayTaAction action,
    pspdisplayta::Protocol protocol,pspdisplayta::Content content,uint32_t timeoutUs,DisplayTrustedResult& out) {
    if(!gart_||!gart_->normalAccessReady()||!scope.matchesGraphics(gart_->device_,gart_->openOwner_))
        return kIOReturnOffline;
    if(taOutputAliases(&out,sizeof(out),&scope,sizeof(scope),&display))return kIOReturnBadArgument;
    out={};out.result=kIOReturnBadArgument;
    if(!timeoutUs||timeoutUs>2000000||path>=kMaxDisplayPaths)return out.result;
    if(action==DisplayTaAction::Create&&
        ((protocol!=pspdisplayta::Protocol::Hdcp1&&protocol!=pspdisplayta::Protocol::Hdcp2)||
         (content!=pspdisplayta::Content::Type0&&content!=pspdisplayta::Content::Type1&&content!=pspdisplayta::Content::Maximum)||
         (protocol==pspdisplayta::Protocol::Hdcp1&&content!=pspdisplayta::Content::Type0)))return out.result;
    if(!gart_||display.firmware_!=this||display.gart_!=gart_||!display.lock_||
        !gart_->normalAccessReady()||!scope.matchesGraphics(gart_->device_,gart_->openOwner_))
        return out.result=kIOReturnOffline;
    const uint64_t started=clockTime(this);
    IOLockLock(display.lock_);
    const auto ready=beginTrustedApplication(scope,pspta::Kind::Hdcp,timeoutUs,out.application,started);
    if(ready!=kIOReturnSuccess) {IOLockUnlock(display.lock_);return out.result=ready;}
    HackyNaviDisplayController* drop=nullptr;
    // This single exit clears lexical callbacks and drops retained Display
    // objects only after all three owner locks have been released.
    const auto done=[&](IOReturn result) {
        const auto value=finishDisplayTrusted(out,result);
        IOLockUnlock(display.lock_);if(drop)drop->release();return value;
    };
    size_t found=pspdisplayta::kDisplays,freeSlot=pspdisplayta::kDisplays;
    for(size_t i=0;i<pspdisplayta::kDisplays;++i) {
        if(displayTaRecords_[i].owner==&display&&displayTaRecords_[i].path==path)found=i;
        if(!displayTaRecords_[i].owner&&freeSlot==pspdisplayta::kDisplays)freeSlot=i;
    }
    pspdisplayta::Topology topology{};
    const bool needsLive=action==DisplayTaAction::Add||action==DisplayTaAction::Create||action==DisplayTaAction::Query;
    if(needsLive&&!displayTopologyLocked(display,path,topology))return done(kIOReturnNotReady);
    if(action==DisplayTaAction::Add) {
        if(found!=pspdisplayta::kDisplays)return done(kIOReturnBusy);
        if(freeSlot==pspdisplayta::kDisplays)return done(kIOReturnNoSpace);
        if(displayTa_.phase()!=pspdisplayta::Phase::Fresh&&displayTa_.phase()!=pspdisplayta::Phase::Bound)
            return done(kIOReturnNotPermitted);
        // Validate conflicts before loading or binding either application.
        for(const auto& record:displayTaRecords_) if(record.owner&&
            (record.topology.otg==topology.otg||record.topology.ddc==topology.ddc||
             record.topology.streamEncoder==topology.streamEncoder||record.topology.transmitter==topology.transmitter))
            return done(kIOReturnBusy);
        if(displayTa_.phase()==pspdisplayta::Phase::Fresh) {
            for(const auto kind:kTaKinds) {
                pspring::TaInformation info{};
                if(!ring_.taInformation(kind,info))return done(kIOReturnNotReady);
                if(info.phase==pspring::TaPhase::Fresh) {
                    out.applicationAttempted=true;
                    const auto loaded=loadTrustedApplicationLocked(kind,out.application);
                    if(loaded!=kIOReturnSuccess)return done(loaded);
                } else if(info.phase!=pspring::TaPhase::Loaded||!info.sessionValid||!info.retained||info.invocations)
                    return done(kIOReturnNotPermitted);
            }
            const pspdisplayta::Io io={taIo(),taValid,taFault};out.protocolAttempted=true;
            const auto bound=pspdisplayta::bind(ring_,io,displayTa_,out.protocol);
            if(bound!=pspdisplayta::Status::Ok)return done(displayError(bound));
        }
        auto& record=displayTaRecords_[freeSlot];record.owner=&display;display.retain();
        record.path=path;record.topology=topology;out.protocolAttempted=true;
        const auto status=pspdisplayta::addDisplay(displayTa_,topology,kTaPoll,record.display,out.protocol);
        if(status==pspdisplayta::Status::Ok)record.topologyActive=true;
        else if(!out.protocol.sharedWriteAttempted&&displayTa_.phase()!=pspdisplayta::Phase::FaultedRetained) {
            drop=record.owner;record=DisplayTaRecord{};
        }
        return done(displayError(status));
    }
    if(found==pspdisplayta::kDisplays||!displayTaRecords_[found].topologyActive||
        displayTa_.phase()!=pspdisplayta::Phase::Bound)return done(kIOReturnNotPermitted);
    auto& record=displayTaRecords_[found];
    if(needsLive&&(record.topology.otg!=topology.otg||record.topology.ddc!=topology.ddc||
        record.topology.streamEncoder!=topology.streamEncoder||record.topology.transmitter!=topology.transmitter))
        return done(kIOReturnNotPermitted);
    if(action==DisplayTaAction::Create&&record.authenticationActive)return done(kIOReturnBusy);
    if(action==DisplayTaAction::Remove&&record.authenticationActive)return done(kIOReturnBusy);
    if((action==DisplayTaAction::Query||action==DisplayTaAction::Destroy)&&!record.authenticationActive)
        return done(kIOReturnNotPermitted);
    pspdisplayta::Status status=pspdisplayta::Status::InvalidArgument;out.protocolAttempted=true;
    switch(action) {
    case DisplayTaAction::Create:
        status=pspdisplayta::createAuthentication(displayTa_,record.display,protocol,content,kTaPoll,record.authentication,out.protocol);
        if(status==pspdisplayta::Status::Ok)record.authenticationActive=true;
        break;
    case DisplayTaAction::Query:
        status=pspdisplayta::queryAuthentication(displayTa_,record.authentication,kTaPoll,out.protocol);break;
    case DisplayTaAction::Destroy:
        status=pspdisplayta::destroyAuthentication(displayTa_,record.authentication,kTaPoll,out.protocol);
        if(status==pspdisplayta::Status::Ok){record.authenticationActive=false;++out.destroyedAuthentications;}
        break;
    case DisplayTaAction::Remove:
        status=pspdisplayta::removeDisplay(displayTa_,record.display,kTaPoll,out.protocol);
        if(status==pspdisplayta::Status::Ok){drop=record.owner;record=DisplayTaRecord{};++out.removedDisplays;}
        break;
    case DisplayTaAction::Add:break;
    }
    return done(displayError(status));
}
IOReturn HackyNaviFirmwareController::registerDisplayTrustedTopology(HackyNaviFunctionGroup::Operation& s,
    HackyNaviDisplayController& d,uint8_t p,uint32_t us,DisplayTrustedResult& r) {
    return displayTrustedOperation(s,d,p,DisplayTaAction::Add,pspdisplayta::Protocol::Hdcp1,pspdisplayta::Content::Type0,us,r);
}
IOReturn HackyNaviFirmwareController::createDisplayAuthentication(HackyNaviFunctionGroup::Operation& s,
    HackyNaviDisplayController& d,uint8_t p,pspdisplayta::Protocol v,pspdisplayta::Content c,uint32_t us,DisplayTrustedResult& r) {
    return displayTrustedOperation(s,d,p,DisplayTaAction::Create,v,c,us,r);
}
IOReturn HackyNaviFirmwareController::queryDisplayAuthentication(HackyNaviFunctionGroup::Operation& s,
    HackyNaviDisplayController& d,uint8_t p,uint32_t us,DisplayTrustedResult& r) {
    return displayTrustedOperation(s,d,p,DisplayTaAction::Query,pspdisplayta::Protocol::Hdcp1,pspdisplayta::Content::Type0,us,r);
}
IOReturn HackyNaviFirmwareController::destroyDisplayAuthentication(HackyNaviFunctionGroup::Operation& s,
    HackyNaviDisplayController& d,uint8_t p,uint32_t us,DisplayTrustedResult& r) {
    return displayTrustedOperation(s,d,p,DisplayTaAction::Destroy,pspdisplayta::Protocol::Hdcp1,pspdisplayta::Content::Type0,us,r);
}
IOReturn HackyNaviFirmwareController::removeDisplayTrustedTopology(HackyNaviFunctionGroup::Operation& s,
    HackyNaviDisplayController& d,uint8_t p,uint32_t us,DisplayTrustedResult& r) {
    return displayTrustedOperation(s,d,p,DisplayTaAction::Remove,pspdisplayta::Protocol::Hdcp1,pspdisplayta::Content::Type0,us,r);
}
IOReturn HackyNaviFirmwareController::retireDisplayTrustedApplications(HackyNaviFunctionGroup::RetirementScope& scope,
    uint32_t timeoutUs,DisplayTrustedResult& out) {
    if(!gart_||!gart_->retirementAccessReady(scope)||
        !scope.matchesGraphics(gart_->device_,gart_->openOwner_))return kIOReturnOffline;
    if(taOutputAliases(&out,sizeof(out),&scope,sizeof(scope)))return kIOReturnBadArgument;
    out={};out.result=kIOReturnBadArgument;
    if(!timeoutUs||timeoutUs>2000000)return out.result;
    const uint64_t started=clockTime(this);
    IOLockLock(lock_);IOLockLock(gart_->lock_);
    if(!gart_->retirementAccessReady(scope))return out.result=finish(kIOReturnOffline);
    snapshotDisplayTrusted(out);
    if(!out.retentionRequired)return out.result=finish(kIOReturnSuccess);
    if(!providerReady()||!hardwareHold_||
        (state_!=State::FirmwareLoaded&&state_!=State::Quiesced)||
        displayTa_.phase()==pspdisplayta::Phase::FaultedRetained||
        ring_.phase()!=pspring::Phase::Running||ring_.pending()||!ring_.tmrReady())
        return out.result=finish(kIOReturnNotReady);
    taScope_=nullptr;taRetirementScope_=&scope;taStart_=taLast_=started;
    taBudget_=timeoutUs;taFailure_=kIOReturnSuccess;gart_->mmioActive_=true;
    HackyNaviDisplayController* drops[pspdisplayta::kDisplays]{};
    const auto done=[&](IOReturn result) {
        const auto value=finishDisplayTrusted(out,result);
        for(auto* display:drops)if(display)display->release();return value;
    };
    if(!taValid(this))return done(taFailure_);
    if(displayTa_.phase()==pspdisplayta::Phase::Bound) {
        for(size_t i=0;i<pspdisplayta::kDisplays;++i) {
            auto& record=displayTaRecords_[i];if(!record.owner)continue;
            if(!record.topologyActive)return done(taError(kIOReturnInternalError,true));
            if(record.authenticationActive) {
                out.protocolAttempted=true;
                const auto status=pspdisplayta::destroyAuthentication(displayTa_,record.authentication,kTaPoll,out.protocol);
                if(status!=pspdisplayta::Status::Ok)return done(displayError(status));
                record.authenticationActive=false;++out.destroyedAuthentications;
            }
            out.protocolAttempted=true;
            const auto status=pspdisplayta::removeDisplay(displayTa_,record.display,kTaPoll,out.protocol);
            if(status!=pspdisplayta::Status::Ok)return done(displayError(status));
            record.topologyActive=false;++out.removedDisplays;
            drops[i]=record.owner;record=DisplayTaRecord{};
        }
        out.protocolAttempted=true;
        const auto status=pspdisplayta::retire(displayTa_,out.protocol);
        if(status!=pspdisplayta::Status::Ok)return done(displayError(status));
    }
    for(const auto kind:kTaKinds) {
        pspring::TaInformation info{};
        if(!ring_.taInformation(kind,info))return done(taError(kIOReturnInternalError,true));
        if(!info.retained)continue;
        out.applicationAttempted=true;
        const auto result=unloadTrustedApplicationLocked(kind,out.application);
        if(result!=kIOReturnSuccess)return done(result);
        ++out.unloadedApplications;
    }
    return done(kIOReturnSuccess);
}
IOReturn HackyNaviFirmwareController::unloadTrustedApplication(HackyNaviFunctionGroup::Operation& scope,
    pspta::Kind kind,uint32_t timeoutUs,TrustedApplicationResult& out) {
    const auto ready=beginTrustedApplication(scope,kind,timeoutUs,out);if(ready!=kIOReturnSuccess)return ready;
    return finishTrustedApplication(kind,out,unloadTrustedApplicationLocked(kind,out));
}
IOReturn HackyNaviFirmwareController::unloadTrustedApplicationLocked(pspta::Kind kind,TrustedApplicationResult& out) {
    out={};out.result=kIOReturnBadArgument;
    if(displayTa_.retentionRequired())return out.result=kIOReturnBusy;
    if(!ring_.taInformation(kind,out.application)||out.application.phase!=pspring::TaPhase::Loaded||
        !out.application.sessionValid||!out.application.retained)
        return out.result=taError(kIOReturnNotPermitted);
    if(pspring::submitUnloadTa(taIo(),ring_,kind,out.ring)!=pspring::Status::Ok||
        pspring::wait(taIo(),ring_,kTaPoll,out.ring)!=pspring::Status::Ok)
        return out.result=taError(kIOReturnIOError,true);
    if(!ring_.taInformation(kind,out.application)||out.application.phase!=pspring::TaPhase::Retired||
        out.application.retained||!out.application.unloadAcknowledged||!taValid(this))
        return out.result=taError(kIOReturnInternalError,true);
    const Slot first=kind==pspta::Kind::Hdcp?HdcpStage:DtmStage;
    for(unsigned i=unsigned(first);i<unsigned(first)+2;++i) {
        if(gart_->allocator_.release(slots_[i].handle)!=VramAllocatorStatus::Ok)
            return out.result=taError(kIOReturnInternalError,true);
        slots_[i]={};
    }
    return out.result=taError(kIOReturnSuccess);
}
