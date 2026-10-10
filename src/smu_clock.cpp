// SPDX-License-Identifier: MIT
// Adapted from Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// smu_v13_0.c soft ranges/table location, smu_cmn.c mailbox/table transport,
// smu13_driver_if_v13_0_0.h exact ABI, smu_v13_0_0_ppt.c metrics/feature map.
// Copyright 2020-2023 Advanced Micro Devices, Inc.
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
#include "hackynavi/smu_clock.hpp"
namespace hackynavi { namespace smuclock { namespace {
bool valid(const Poll& p){return p.timeoutUs&&p.timeoutUs<=2000000&&p.maxResponseReads&&
    p.maxResponseReads<=1000000&&p.delayUs&&p.delayUs<=1000;}
bool valid(const Io& io){return io.valid&&io.read32&&io.write32&&io.delayUs&&io.timeUs&&io.beforeCpuRead&&io.faultOwner;}
Status decode(uint32_t v){switch(v){case 1:return Status::Ok;case 0xff:return Status::CommandFailed;
    case 0xfe:return Status::UnknownCommand;case 0xfd:return Status::BadPrerequisite;
    case 0xfc:return Status::Busy;default:return Status::UnknownResponse;}}
uint32_t le32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
}
Controller::Controller():owner_(nullptr),layout_{},io_{},bound_(false),initialized_(false),faulted_(false),
    terminalOnly_(false),version_(0),allocator_(nullptr),allocation_{},range_{},journal_{},active_(nullptr),poll_{},start_(0),last_(0){}
bool Controller::retentionRequired() const {
    return tableRetentionRequired()||rangeRetentionRequired()||deepSleepRetentionRequired()||dfCstateRetentionRequired();
}
bool Controller::dfCstateRetentionRequired() const {
    return journal_.dfCstate.disallowAttempted&&!journal_.dfCstate.released;
}
bool Controller::deepSleepRetentionRequired() const {
    return !journal_.deepSleep.released&&(journal_.deepSleep.disableAttempted||journal_.deepSleep.inhibited);
}
bool Controller::tableRetentionRequired() const {
    return !journal_.tableSealed&&(journal_.addressHighAttempted||journal_.addressLowAttempted||journal_.transferAttempted);
}
bool Controller::rangeRetentionRequired() const {
    return !journal_.rangesSealed&&(journal_.soc.maximumAttempted||journal_.soc.minimumAttempted||
        journal_.fabric.maximumAttempted||journal_.fabric.minimumAttempted);
}
bool Controller::guard(){
    if(!active_||active_->status!=Status::Ok)return false;
    if(!owner_||!owner_->awake()||!owner_->gfxOffDisabled()||owner_->interfaceVersion()!=kInterfaceVersion||
        owner_->smuVersion()!=version_||!io_.valid(io_.context)){
        active_->status=Status::OwnerUnavailable;return false;
    }
    const uint64_t now=io_.timeUs(io_.context);
    if(now<last_){active_->status=Status::ClockWentBackwards;return false;}
    last_=now;active_->elapsedUs=now-start_;
    if(now-start_>=poll_.timeoutUs){active_->status=Status::Timeout;return false;}
    if(!io_.valid(io_.context)){active_->status=Status::OwnerUnavailable;return false;}
    return true;
}
Status Controller::begin(const Poll& p,Result& r){
    // Do not overwrite a caller's live Result through a reentrant call.
    if(active_)return Status::InvalidState;
    r={};r.stage=Stage::Preflight;
    if(!bound_||faulted_||terminalOnly_){r.status=Status::InvalidState;return r.status;}
    if(!valid(p)){r.status=Status::InvalidArgument;return r.status;}
    active_=&r;poll_=p;start_=last_=io_.timeUs(io_.context);
    return guard()?Status::Ok:r.status;
}
Status Controller::end(Status status){
    if(!active_)return status;
    if(active_->status!=Status::Ok)status=active_->status;
    if(status==Status::Ok&&!guard())status=active_->status;
    active_->status=status;
    if(status==Status::Ok)active_->stage=Stage::Complete;
    else {
        faulted_=true;initialized_=false;
        // Native implementation must seal the common owner without taking
        // descendant locks again. Merely sealing this companion is insufficient.
        if(!journal_.ownerFaultNotified){
            journal_.faultStatus=status;journal_.faultStage=active_->stage;
            journal_.ownerFaultNotified=true;io_.faultOwner(io_.context);
        }
    }
    active_=nullptr;return status;
}
Status Controller::read(uint32_t reg,uint32_t& value){
    if(!guard())return active_->status;
    ++active_->reads;active_->lastRegister=reg;
    const bool ok=io_.read32(io_.context,reg,value);
    active_->lastValue=value;if(!guard())return active_->status;
    if(!ok)return Status::ReadFailed;
    return value==UINT32_MAX?Status::DeviceUnavailable:Status::Ok;
}
Status Controller::write(uint32_t reg,uint32_t value){
    if(!guard())return active_->status;
    ++active_->writes;active_->lastRegister=reg;active_->lastValue=value;active_->mailboxWriteAttempted=true;
    const bool ok=io_.write32(io_.context,reg,value);
    if(reg==layout_.message&&active_->stage==Stage::Message)journal_.transport.messageCallbackSucceeded=ok;
    if(!guard())return active_->status;
    return ok?Status::Ok:Status::WriteFailed;
}
Status Controller::delay(){
    if(!guard())return active_->status;
    if(poll_.delayUs>=poll_.timeoutUs-active_->elapsedUs)return Status::Timeout;
    ++active_->delays;io_.delayUs(io_.context,poll_.delayUs);
    return guard()?Status::Ok:active_->status;
}
Status Controller::response(bool acceptKnownTerminal){
    while(active_->responseReads<poll_.maxResponseReads){
        auto& transport=journal_.transport;
        transport.stage=active_->stage;
        if(active_->stage==Stage::PreviousResponse)transport.previousResponseReadValid=false;
        else transport.responseReadValid=false;
        ++active_->responseReads;auto status=read(layout_.response,active_->response);
        transport.lastResponse=active_->response;
        if(status!=Status::Ok)return status;
        if(active_->stage==Stage::PreviousResponse)transport.previousResponseReadValid=true;
        else transport.responseReadValid=true;
        if(active_->response){
            status=decode(active_->response);
            if(status!=Status::UnknownResponse){
                transport.responseSettled=true;
                transport.settledMessageSerial=transport.lastMessageSerial;
            }
            // Linux's V1 pre-poll accepts known completed/rejected responses;
            // an unknown response is fatal and zero still means not settled.
            return acceptKnownTerminal&&status!=Status::UnknownResponse?Status::Ok:status;
        }
        if(active_->responseReads<poll_.maxResponseReads){status=delay();if(status!=Status::Ok)return status;}
    }
    return Status::Timeout;
}
Status Controller::send(uint32_t message,uint32_t argument,bool output,uint32_t& value,bool acceptKnownPrevious){
    auto& transport=journal_.transport;
    if(transport.requestSerial==UINT64_MAX)return Status::SerialExhausted;
    const uint64_t next=transport.requestSerial+1,last=transport.lastMessageSerial,settled=transport.settledMessageSerial;
    const uint32_t previousMessage=transport.lastMessage,previousArgument=transport.lastArgument;
    transport={};transport.requestSerial=next;transport.lastMessageSerial=last;
    transport.settledMessageSerial=settled;transport.lastMessage=previousMessage;transport.lastArgument=previousArgument;
    transport.requestMessage=message;transport.requestArgument=argument;
    value=0;active_->message=message;active_->argument=argument;
    active_->stage=Stage::PreviousResponse;transport.stage=active_->stage;
    auto status=response(acceptKnownPrevious);if(status!=Status::Ok)return status;
    if(acceptKnownPrevious){journal_.reconciliationPriorResponse=active_->response;journal_.reconciliationPriorSettled=true;}
    if(active_->responseReads>=poll_.maxResponseReads)return Status::Timeout;
    active_->stage=Stage::ClearResponse;transport.stage=active_->stage;transport.responseClearAttempted=true;
    status=write(layout_.response,0);if(status!=Status::Ok)return status;
    transport.responseClearCompleted=true;transport.responseSettled=false;
    active_->stage=Stage::Argument;transport.stage=active_->stage;transport.argumentAttempted=true;
    status=write(layout_.argument,argument);if(status!=Status::Ok)return status;
    transport.argumentCompleted=true;active_->stage=Stage::Message;transport.stage=active_->stage;
    // Caller journals ownership before entry. A failed message write may land.
    transport.messageAttempted=true;transport.lastMessageSerial=next;
    transport.lastMessage=message;transport.lastArgument=argument;
    status=write(layout_.message,message);if(status!=Status::Ok)return status;
    transport.messageCallbackSucceeded=true;active_->stage=Stage::Response;transport.stage=active_->stage;
    status=response();if(status!=Status::Ok)return status;
    if(output){active_->stage=Stage::Output;transport.stage=active_->stage;return read(layout_.argument,value);}
    return Status::Ok;
}
Status Controller::features(uint64_t& value){
    active_->stage=Stage::Features;uint32_t high=0,low=0;
    auto status=send(0x0d,0,true,high);if(status!=Status::Ok)return status;
    status=send(0x0c,0,true,low);if(status!=Status::Ok)return status;
    value=(uint64_t(high)<<32)|low;active_->runningFeatures=value;return Status::Ok;
}
Status Controller::bind(const Navi31IpLayout& ip,uint64_t bar,const smu::State& owner,
    const Io& io,const Poll& p,Result& r){
    if(active_)return Status::InvalidState;
    r={};if(bound_){r.status=Status::InvalidState;return r.status;}
    if(!valid(io)||!valid(p)){r.status=Status::InvalidArgument;return r.status;}
    if(!owner.awake()||!owner.gfxOffDisabled()){r.status=Status::OwnerUnavailable;return r.status;}
    if(owner.interfaceVersion()!=kInterfaceVersion){r.status=Status::UnsupportedInterface;return r.status;}
    if(smu::buildLayout(ip,bar,layout_)!=smu::Status::Ok){r.status=Status::UnsupportedLayout;return r.status;}
    owner_=&owner;io_=io;version_=owner.smuVersion();bound_=true;
    auto status=begin(p,r);if(status!=Status::Ok)return end(status);
    uint32_t firmware=0,interface=0;
    // This is a same-owner recheck, not smu::initialize: pre-poll every message.
    status=send(2,0,true,firmware);
    if(status==Status::Ok)status=send(3,0,true,interface);
    if(status==Status::Ok&&(firmware!=version_||interface!=kInterfaceVersion))status=Status::ChangedFirmware;
    uint64_t first=0,second=0;
    if(status==Status::Ok)status=features(first);
    if(status==Status::Ok)status=features(second);
    if(status==Status::Ok&&first!=second)status=Status::ChangedFeatures;
    if(status==Status::Ok&&(first&0x30u)!=0x30u)status=Status::DisabledFeature;
    if(status==Status::Ok)initialized_=true;
    return end(status);
}
Status Controller::requestSoftRange(Clock clock,uint16_t minimum,uint16_t maximum,const Poll& p,Result& r){
    if(active_)return Status::InvalidState;
    r={};if(!ready()||journal_.rangesSealed||journal_.deepSleep.released||journal_.dfCstate.released){
        r.status=Status::InvalidState;return r.status;}
    // 0xffff is the source's automatic-maximum marker, so the rounded request
    // must neither wrap nor collide with that marker.
    if((clock!=Clock::Soc&&clock!=Clock::Fabric)||!minimum||maximum<minimum||maximum>=UINT16_MAX-1||!valid(p)){
        r.status=Status::InvalidArgument;return r.status;
    }
    auto status=begin(p,r);if(status!=Status::Ok)return end(status);
    uint64_t before=0,after=0;status=features(before);
    if(status==Status::Ok&&journal_.deepSleep.inhibited&&before!=journal_.deepSleep.heldFeatures)
        status=Status::ChangedFeatures;
    const uint64_t feature=clock==Clock::Soc?0x20:0x10;
    if(status==Status::Ok&&!(before&feature))status=Status::DisabledFeature;
    const uint32_t selector=uint32_t(clock)<<16;uint32_t low=0,high=0,dc=0,unused=0;
    if(status==Status::Ok)status=send(0x1d,selector,true,low);
    if(status==Status::Ok)status=send(0x1e,selector,true,high);
    if(status==Status::Ok)status=send(0x27,selector,true,dc);
    if(status==Status::Ok&&(!low||low>high||high>UINT16_MAX||!dc||dc>UINT16_MAX||
        minimum<low||maximum>high||maximum>dc))status=Status::InvalidFrequency;
    auto& journal=clock==Clock::Soc?journal_.soc:journal_.fabric;
    if(status==Status::Ok){
        journal.requestedMinMHz=minimum;journal.requestedMaxMHz=maximum;
        journal.maximumAttempted=true;journal.maximumAcknowledged=false;
        journal.minimumAcknowledged=false;
        active_->stage=Stage::Maximum;status=send(0x1a,selector|(uint32_t(maximum)+1),false,unused);
        if(status==Status::Ok){journal.maximumAcknowledged=true;journal.acknowledgedMaxMHz=maximum;}
    }
    if(status==Status::Ok){
        journal.minimumAttempted=true;active_->stage=Stage::Minimum;
        status=send(0x19,selector|minimum,false,unused);
        if(status==Status::Ok){journal.minimumAcknowledged=true;journal.acknowledgedMinMHz=minimum;}
    }
    if(status==Status::Ok)status=features(after);
    if(status==Status::Ok&&before!=after)status=Status::ChangedFeatures;
    return end(status);
}
Status Controller::inhibitSocFabricDeepSleep(const Poll& p,Result& r){
    if(active_)return Status::InvalidState;
    r={};auto& j=journal_.deepSleep;
    if(!ready()||j.captured||journal_.rangesSealed||rangeRetentionRequired()||journal_.dfCstate.released){
        r.status=Status::InvalidState;return r.status;
    }
    if(!valid(p)){r.status=Status::InvalidArgument;return r.status;}
    auto status=begin(p,r);if(status!=Status::Ok)return end(status);
    uint64_t first=0,second=0;uint32_t unused=0;
    status=features(first);if(status==Status::Ok)status=features(second);
    if(status==Status::Ok&&first!=second)status=Status::ChangedFeatures;
    if(status==Status::Ok&&(first&0x30u)!=0x30u)status=Status::DisabledFeature;
    if(status==Status::Ok){
        // Pinned smu_v13_0_deep_sleep_control selects DS_FCLK/DS_SOCCLK;
        // smu_cmn_feature_set_enabled sends their mapped low-mask bits.
        // No unrelated DS feature, DF_CSTATE bit, or high-mask write is used.
        j.originalFeatures=first;j.changedBits=uint32_t(first)&0x3000u;
        j.heldFeatures=first&~uint64_t(0x3000u);j.captured=true;
        if(j.changedBits){
            j.disableAttempted=true;
            status=send(0x0a,j.changedBits,false,unused);
            if(status==Status::Ok)j.disableAcknowledged=true;
        }
    }
    if(status==Status::Ok)status=features(first);
    if(status==Status::Ok)status=features(second);
    if(status==Status::Ok&&(first!=j.heldFeatures||second!=j.heldFeatures))status=Status::ChangedFeatures;
    if(status==Status::Ok)j.inhibitReadbackComplete=true;
    status=end(status);if(status==Status::Ok)j.inhibited=true;
    return status;
}
Status Controller::restoreSocFabricDeepSleepAfterQuiescence(const Poll& p,Result& r){
    if(active_)return Status::InvalidState;
    r={};auto& j=journal_.deepSleep;
    if(!ready()||!j.inhibited||j.released||rangeRetentionRequired()||dfCstateRetentionRequired()){
        r.status=Status::InvalidState;return r.status;
    }
    if(!valid(p)){r.status=Status::InvalidArgument;return r.status;}
    auto status=begin(p,r);if(status!=Status::Ok)return end(status);
    uint64_t first=0,second=0;uint32_t unused=0;
    status=features(first);if(status==Status::Ok)status=features(second);
    if(status==Status::Ok&&(first!=j.heldFeatures||second!=j.heldFeatures))status=Status::ChangedFeatures;
    if(status==Status::Ok&&j.changedBits){
        j.restoreAttempted=true;
        status=send(0x08,j.changedBits,false,unused);
        if(status==Status::Ok)j.restoreAcknowledged=true;
    }
    if(status==Status::Ok)status=features(first);
    if(status==Status::Ok)status=features(second);
    if(status==Status::Ok&&(first!=j.originalFeatures||second!=j.originalFeatures))status=Status::ChangedFeatures;
    if(status==Status::Ok)j.restoreReadbackComplete=true;
    status=end(status);if(status==Status::Ok)j.released=true;
    return status;
}
Status Controller::disallowDfCstate(const Poll& p,Result& r){return setDfCstate(false,p,r);}
Status Controller::allowDfCstateAfterQuiescence(const Poll& p,Result& r){return setDfCstate(true,p,r);}
Status Controller::setDfCstate(bool allow,const Poll& p,Result& r){
    if(active_)return Status::InvalidState;
    r={};auto& j=journal_.dfCstate;
    if(!ready()||j.released||rangeRetentionRequired()||
        (allow?(!j.disallowed||j.allowAttempted||tableRetentionRequired()):
            (j.disallowAttempted||journal_.rangesSealed||journal_.deepSleep.released))){
        r.status=Status::InvalidState;return r.status;
    }
    if(!valid(p)){r.status=Status::InvalidArgument;return r.status;}
    auto status=begin(p,r);if(status!=Status::Ok)return end(status);
    uint64_t first=0,second=0;uint32_t unused=0;
    status=features(first);if(status==Status::Ok)status=features(second);
    if(status==Status::Ok&&first!=second)status=Status::ChangedFeatures;
    if(status==Status::Ok&&(first&0x30u)!=0x30u)status=Status::DisabledFeature;
    if(status==Status::Ok&&journal_.deepSleep.inhibited&&!journal_.deepSleep.released&&
        first!=journal_.deepSleep.heldFeatures)status=Status::ChangedFeatures;
    const uint64_t before=first;
    if(status==Status::Ok){
        // smu_v13_0_0_set_df_cstate maps DISALLOW=0 / ALLOW=1 to this
        // external-client policy message. It does not toggle feature bit41.
        // Journal before transport: even a failed write may have landed.
        if(allow){j.allowFeatures=before;j.allowAttempted=true;}
        else {j.disallowFeatures=before;j.disallowAttempted=true;}
        status=send(0x3b,allow?1u:0u,false,unused);
        if(status==Status::Ok){if(allow)j.allowAcknowledged=true;else j.disallowAcknowledged=true;}
    }
    if(status==Status::Ok)status=features(first);
    if(status==Status::Ok)status=features(second);
    if(status==Status::Ok&&(first!=before||second!=before))status=Status::ChangedFeatures;
    if(status==Status::Ok){if(allow)j.allowFeaturesVerified=true;else j.disallowFeaturesVerified=true;}
    status=end(status);
    if(status==Status::Ok){if(allow)j.released=true;else j.disallowed=true;}
    return status;
}
bool Controller::allocationValid() const {
    if(!allocator_)return false;
    VramRange range{};uint64_t mc=0,physical=0;
    return allocator_->lookup(allocation_,range)==VramAllocatorStatus::Ok&&range.offset==range_.offset&&
        range.bytes==range_.bytes&&allocator_->addresses(allocation_,mc,physical)==VramAllocatorStatus::Ok&&
        mc==journal_.tableGpuAddress;
}
Status Controller::publishMetrics(const VramAllocator& allocator,VramAllocationHandle handle,const Poll& p,Result& r){
    if(active_)return Status::InvalidState;
    r={};if(!ready()||allocator_){r.status=Status::InvalidState;return r.status;}
    VramRange range{};uint64_t mc=0,physical=0;
    if(!valid(p)){r.status=Status::InvalidArgument;return r.status;}
    if(allocator.lookup(handle,range)!=VramAllocatorStatus::Ok||range.bytes<kMetricsBytes||
        allocator.addresses(handle,mc,physical)!=VramAllocatorStatus::Ok||!mc||(mc&4095)||
        (range.offset&4095)||(range.bytes&4095)){
        r.status=Status::InvalidAllocation;return r.status;
    }
    auto status=begin(p,r);if(status!=Status::Ok)return end(status);
    allocator_=&allocator;allocation_=handle;range_=range;journal_.tableGpuAddress=mc;
    // Persistent driver-table pointer, even when the low half never ACKs.
    journal_.addressHighAttempted=true;++journal_.tableAttemptSerial;
    active_->stage=Stage::AddressHigh;uint32_t unused=0;
    status=send(0x0e,uint32_t(mc>>32),false,unused);
    if(status==Status::Ok){journal_.addressHighAcknowledged=true;journal_.addressLowAttempted=true;++journal_.tableAttemptSerial;
        active_->stage=Stage::AddressLow;status=send(0x0f,uint32_t(mc),false,unused);}
    if(status==Status::Ok)journal_.addressLowAcknowledged=true;
    if(status==Status::Ok&&!allocationValid())status=Status::InvalidAllocation;
    return end(status);
}
bool Controller::transferRead(void* context,uint32_t reg,uint32_t& value){
    auto& self=*static_cast<Controller*>(context);
    if(!self.guard()||!self.allocationValid())return false;
    // MM_DATA all-ones is legitimate data; validate decoded clock fields later.
    if(!self.io_.read32(self.io_.context,reg,value))return false;
    return self.guard()&&self.allocationValid();
}
bool Controller::transferWrite(void* context,uint32_t reg,uint32_t value){
    auto& self=*static_cast<Controller*>(context);
    if(!self.guard()||!self.allocationValid())return false;
    if(!self.io_.write32(self.io_.context,reg,value))return false;
    return self.guard()&&self.allocationValid();
}
Status Controller::captureMetrics(const Poll& p,Metrics& out,Result& r){
    if(active_)return Status::InvalidState;
    out={};r={};if(!ready()||!journal_.addressLowAcknowledged||journal_.tableSealed){r.status=Status::InvalidState;return r.status;}
    if(!valid(p)){r.status=Status::InvalidArgument;return r.status;}
    if(journal_.transferSerial==UINT64_MAX||journal_.tableAttemptSerial==UINT64_MAX){r.status=Status::SerialExhausted;return r.status;}
    auto status=begin(p,r);if(status!=Status::Ok)return end(status);
    if(!allocationValid())return end(Status::InvalidAllocation);
    uint64_t before=0,after=0;status=features(before);
    if(status==Status::Ok&&journal_.deepSleep.inhibited&&!journal_.deepSleep.released&&
        before!=journal_.deepSleep.heldFeatures)status=Status::ChangedFeatures;
    if(status==Status::Ok&&(before&0x30u)!=0x30u)status=Status::DisabledFeature;
    uint32_t unused=0;
    if(status==Status::Ok){
        journal_.transferAttempted=true;++journal_.tableAttemptSerial;journal_.transferAcknowledged=false;
        journal_.cpuVisible=journal_.readComplete=false;active_->stage=Stage::Transfer;
        status=send(0x12,5,false,unused);
        if(status==Status::Ok)journal_.transferAcknowledged=true;
    }
    if(status==Status::Ok){
        active_->stage=Stage::Visibility;
        if(!guard())status=active_->status;
        else if(!io_.beforeCpuRead(io_.context))status=Status::VisibilityFailed;
        else if(!guard())status=active_->status;
        else journal_.cpuVisible=true;
    }
    if(status==Status::Ok){
        active_->stage=Stage::Readback;
        const VramTransferIo transfer={this,layout_.barBytes,transferRead,transferWrite};
        if(readVram(*allocator_,allocation_,0,out.raw,kMetricsBytes,transfer,r.transfer)!=VramTransferStatus::Ok)
            status=Status::TransferFailed;
        else journal_.readComplete=true;
    }
    if(status==Status::Ok){
        for(size_t n=0;n<kClockCount;++n)out.currentClockMHz[n]=le32(out.raw+n*4);
        out.counter=le32(out.raw+80);
        // Required SOC/FCLK fields only: unused/gated clocks may report zero.
        const unsigned required[]={1,3};
        for(unsigned i=0;i<2;++i)if(!out.currentClockMHz[required[i]]||out.currentClockMHz[required[i]]>UINT16_MAX)
            status=Status::InvalidMetrics;
    }
    if(status==Status::Ok)status=features(after);
    if(status==Status::Ok&&before!=after)status=Status::ChangedFeatures;
    if(status==Status::Ok&&(!guard()||!allocationValid()))status=Status::OwnerUnavailable;
    if(status==Status::Ok){
        out.transferSerial=++journal_.transferSerial;
        journal_.completedTableAttemptSerial=journal_.tableAttemptSerial;
        const auto& soc=journal_.soc;const auto& fabric=journal_.fabric;
        out.socRangeObserved=!journal_.rangesSealed&&soc.minimumAcknowledged&&soc.maximumAcknowledged&&
            out.currentClockMHz[1]>=soc.acknowledgedMinMHz&&out.currentClockMHz[1]<=uint32_t(soc.acknowledgedMaxMHz)+1;
        out.fabricRangeObserved=!journal_.rangesSealed&&fabric.minimumAcknowledged&&fabric.maximumAcknowledged&&
            out.currentClockMHz[3]>=fabric.acknowledgedMinMHz&&out.currentClockMHz[3]<=uint32_t(fabric.acknowledgedMaxMHz)+1;
    }
    status=end(status);if(status!=Status::Ok)out={};return status;
}
Status Controller::sealMetricsAfterCompletedTransfer(const Poll& p,Result& r){
    if(active_)return Status::InvalidState;
    r={};
    if(!ready()||journal_.tableSealed||!journal_.addressHighAcknowledged||!journal_.addressLowAcknowledged||
        !journal_.transferSerial||!journal_.transferAcknowledged||!journal_.cpuVisible||!journal_.readComplete||
        journal_.completedTableAttemptSerial!=journal_.tableAttemptSerial){
        r.status=Status::InvalidState;return r.status;
    }
    if(!valid(p)){r.status=Status::InvalidArgument;return r.status;}
    auto status=begin(p,r);if(status!=Status::Ok)return end(status);
    if(!allocationValid())status=Status::InvalidAllocation;
    status=end(status);
    // The final guard completed under the caller's still-held real owner gate.
    // Keep all historical attempts; only the local producer is now terminal.
    // No future table method can write through the retained firmware pointer.
    if(status==Status::Ok)journal_.tableSealed=true;
    return status;
}
Status Controller::reconcileMetricsForRetirement(const Poll& p,Result& r){
    if(active_)return Status::InvalidState;
    r={};r.stage=Stage::Preflight;
    if(!bound_||!faulted_||terminalOnly_||!journal_.ownerFaultNotified||
        !tableRetentionRequired()||!allocator_){r.status=Status::InvalidState;return r.status;}
    if(!valid(p)){r.status=Status::InvalidArgument;return r.status;}
    // Pinned V1 treats unknown responses as firmware hang. A new version or
    // unavailable device is not an uninterrupted old-owner recovery epoch.
    if(journal_.faultStatus==Status::UnknownResponse||journal_.faultStatus==Status::DeviceUnavailable||
        journal_.faultStatus==Status::ChangedFirmware||journal_.faultStatus==Status::ClockWentBackwards){
        r.status=Status::InvalidState;return r.status;
    }
    if(journal_.tableAttemptSerial>UINT64_MAX-3||journal_.transferSerial==UINT64_MAX||
        journal_.transport.requestSerial>UINT64_MAX-5){r.status=Status::SerialExhausted;return r.status;}
    // This private transition never restores readiness. Io.valid is now the
    // native owner's separate actual pre-reset RetirementScope binding.
    terminalOnly_=true;journal_.reconciliationAttempted=true;
    active_=&r;poll_=p;start_=last_=io_.timeUs(io_.context);
    auto status=guard()?Status::Ok:r.status;
    if(status==Status::Ok&&!allocationValid())status=Status::InvalidAllocation;
    uint32_t firmware=0,interface=0,unused=0;
    // The only relaxed pre-poll is here: known replies mean settled, not OK.
    // Every new command, including this query, still requires response 1.
    if(status==Status::Ok)status=send(2,0,true,firmware,true);
    if(status==Status::Ok)status=send(3,0,true,interface);
    if(status==Status::Ok&&(firmware!=version_||interface!=kInterfaceVersion))status=Status::ChangedFirmware;
    if(status==Status::Ok&&!allocationValid())status=Status::InvalidAllocation;
    if(status==Status::Ok){
        journal_.addressHighAttempted=true;journal_.addressHighAcknowledged=false;
        journal_.addressLowAcknowledged=false;++journal_.tableAttemptSerial;
        active_->stage=Stage::AddressHigh;
        status=send(0x0e,uint32_t(journal_.tableGpuAddress>>32),false,unused);
        if(status==Status::Ok)journal_.addressHighAcknowledged=true;
    }
    if(status==Status::Ok){
        journal_.addressLowAttempted=true;++journal_.tableAttemptSerial;active_->stage=Stage::AddressLow;
        status=send(0x0f,uint32_t(journal_.tableGpuAddress),false,unused);
        if(status==Status::Ok)journal_.addressLowAcknowledged=true;
    }
    if(status==Status::Ok&&!allocationValid())status=Status::InvalidAllocation;
    if(status==Status::Ok){
        journal_.transferAttempted=true;++journal_.tableAttemptSerial;journal_.transferAcknowledged=false;
        journal_.cpuVisible=journal_.readComplete=false;active_->stage=Stage::Transfer;
        status=send(0x12,5,false,unused);
        if(status==Status::Ok)journal_.transferAcknowledged=true;
    }
    if(status==Status::Ok){
        active_->stage=Stage::Visibility;
        if(!guard())status=active_->status;
        else if(!io_.beforeCpuRead(io_.context))status=Status::VisibilityFailed;
        else if(!guard())status=active_->status;
        else journal_.cpuVisible=true;
    }
    if(status==Status::Ok){
        active_->stage=Stage::Readback;
        uint8_t raw[kMetricsBytes]={};
        const VramTransferIo io={this,layout_.barBytes,transferRead,transferWrite};
        if(readVram(*allocator_,allocation_,0,raw,sizeof(raw),io,r.transfer)!=VramTransferStatus::Ok)
            status=Status::TransferFailed;
        else journal_.readComplete=true;
        // Raw bytes are not returned as a clock observation. The real transfer
        // completion and ordered full read, not a guessed value, retire DMA.
    }
    if(status==Status::Ok&&!allocationValid())status=Status::InvalidAllocation;
    status=end(status);
    if(status==Status::Ok){
        ++journal_.transferSerial;journal_.completedTableAttemptSerial=journal_.tableAttemptSerial;
        journal_.reconciliationCompleted=true;journal_.tableSealed=true;
    }
    return status;
}
Status Controller::returnRangesToAutomaticAfterQuiescence(const Poll& p,Result& r){
    if(active_)return Status::InvalidState;
    r={};if(!ready()||journal_.rangesSealed){r.status=Status::InvalidState;return r.status;}
    if(!valid(p)){r.status=Status::InvalidArgument;return r.status;}
    RangeJournal* ranges[]={&journal_.soc,&journal_.fabric};
    const uint32_t clocks[]={1,3};uint64_t required=0;
    for(unsigned i=0;i<2;++i){
        const auto& j=*ranges[i];
        if(!j.maximumAttempted&&!j.minimumAttempted)continue;
        if(!j.maximumAcknowledged||!j.minimumAcknowledged){r.status=Status::InvalidState;return r.status;}
        required|=i==0?0x20:0x10;
    }
    auto status=begin(p,r);if(status!=Status::Ok)return end(status);
    uint64_t before=0,after=0;uint32_t unused=0;
    if(required)status=features(before);
    if(status==Status::Ok&&required&&journal_.deepSleep.inhibited&&
        before!=journal_.deepSleep.heldFeatures)status=Status::ChangedFeatures;
    if(status==Status::Ok&&(before&required)!=required)status=Status::DisabledFeature;
    // Linux smu_v13_0_set_soft_freq_limited_range(automatic=true), reached
    // by AMD_DPM_FORCED_LEVEL_AUTO, uses this exact order and sentinel pair.
    for(unsigned i=0;status==Status::Ok&&i<2;++i){
        auto& j=*ranges[i];if(!j.maximumAttempted&&!j.minimumAttempted)continue;
        j.automaticMaximumAttempted=true;active_->stage=Stage::Maximum;
        status=send(0x1a,(clocks[i]<<16)|0xffffu,false,unused);
        if(status!=Status::Ok)break;
        j.automaticMaximumAcknowledged=true;j.automaticMinimumAttempted=true;
        active_->stage=Stage::Minimum;status=send(0x19,clocks[i]<<16,false,unused);
        if(status==Status::Ok)j.automaticMinimumAcknowledged=true;
    }
    if(status==Status::Ok&&required)status=features(after);
    if(status==Status::Ok&&before!=after)status=Status::ChangedFeatures;
    status=end(status);
    if(status==Status::Ok)journal_.rangesSealed=true;
    return status;
}
} }
