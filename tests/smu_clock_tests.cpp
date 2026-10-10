// SPDX-License-Identifier: MIT
// Offline fault model only; no model callbacks are linked into the kext.
#include "hackynavi/smu_clock.hpp"
#include "smu_fixture.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace hackynavi;
namespace sc=hackynavi::smuclock;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::printf("line %u: %s\n",unsigned(__LINE__),#x);}}while(0)
const sc::Poll poll={100000,2000,2};
struct Command {uint32_t message,argument;};
struct Model {
    smu_test::Simulation boot;
    VramAllocator allocator;VramAllocation allocation{};MemoryLayout memoryLayout{};
    sc::Controller controller;sc::Result result{};sc::Metrics metrics{};
    uint8_t memory[4096]{},payload[sc::kMetricsBytes]{};
    std::vector<Command> commands;
    uint64_t mc=0,physical=0,table=0,now=100,runningFeatures=0x3038;
    uint32_t response=1,argument=0,index=0,hi=0,reads=0,writes=0,valids=0,clocks=0,visibility=0,faults=0;
    uint32_t failRead=0,failWrite=0,onesRead=0,revokeValid=0,revokeAfterRead=0,revokeAfterWrite=0;
    uint32_t badCommand=0,badResponse=0,busyReads=0,busyEach=0,featureReads=0,changeFeatureAt=0;
    uint32_t readLatency=1,writeLatency=1,backwardsAt=0,deadlineAt=0;
    uint32_t minimum=500,maximum=2500,dcMaximum=2000;
    bool live=true,valid=true,forbidden=false,visible=false,failVisibility=false,allOnesMetrics=false;
    bool reenterAutomatic=false,reenterRecovery=false,reenterDeepSleep=false,ignoreDeepSleep=false;
    bool reenterDfCstate=false;uint32_t reentries=0;
    explicit Model(const gfxfw::State& firmware){
        boot.interfaceVersion=sc::kInterfaceVersion;
        valid=boot.awake(firmware)==smu::Status::Ok;
        valid=valid&&deriveMemoryLayout({0x8000,0x85ff,0x4000,UINT64_C(24)<<30,UINT64_C(8)<<30,
            UINT64_C(256)<<20},memoryLayout)==MemoryLayoutStatus::Ok;
        valid=valid&&allocator.configure(memoryLayout,{0x800000,4096},nullptr,0)==VramAllocatorStatus::Ok;
        valid=valid&&allocator.allocate(4096,4096,allocation)==VramAllocatorStatus::Ok;
        valid=valid&&allocator.addresses(allocation.handle,mc,physical)==VramAllocatorStatus::Ok;
        for(size_t n=0;n<sizeof(payload);++n)payload[n]=uint8_t(n*37+11);
        for(unsigned n=0;n<sc::kClockCount;++n)pspring_test::put(payload+n*4,0);
        pspring_test::put(payload+4,1800);pspring_test::put(payload+12,1800);
        pspring_test::put(payload+80,0); // Zero counter is valid; no made-up sequence semantics.
        std::memset(memory,0xa5,sizeof(memory));
    }
    sc::Io io(){return {this,check,read,write,delay,time,visibleBeforeRead,fault};}
    static bool check(void* context){auto& s=*static_cast<Model*>(context);++s.valids;
        if(s.revokeValid==s.valids)s.live=false;return s.live;}
    static uint64_t time(void* context){auto& s=*static_cast<Model*>(context);++s.clocks;
        if(s.backwardsAt==s.clocks)return 0;
        if(s.deadlineAt==s.clocks)s.now+=poll.timeoutUs;
        return s.now;}
    static void delay(void* context,uint32_t us){static_cast<Model*>(context)->now+=us;}
    static void fault(void* context){auto& s=*static_cast<Model*>(context);++s.faults;s.live=false;}
    static bool visibleBeforeRead(void* context){auto& s=*static_cast<Model*>(context);++s.visibility;
        ++s.now;s.visible=!s.failVisibility;return s.visible;}
    size_t selected(){const uint64_t offset=(uint64_t(hi)<<31)|(index&0x7fffffff);
        if(!(index&0x80000000)||offset<allocation.range.offset||offset-allocation.range.offset>sizeof(memory)-4||
            (offset&3)){forbidden=true;return 0;}return size_t(offset-allocation.range.offset);}
    static bool read(void* context,uint32_t reg,uint32_t& value){
        auto& s=*static_cast<Model*>(context);++s.reads;s.now+=s.readLatency;
        if(!s.live)s.forbidden=true;
        if(reg==s.boot.layout.response)value=s.busyReads?(--s.busyReads,0):s.response;
        else if(reg==s.boot.layout.argument)value=s.argument;
        else if(reg==4){if(!s.visible)s.forbidden=true;value=pspring_test::get(s.memory+s.selected());}
        else {s.forbidden=true;value=0;}
        if(s.onesRead==s.reads)value=UINT32_MAX;
        if(s.revokeAfterRead==s.reads)s.live=false;
        return s.failRead!=s.reads;
    }
    static bool write(void* context,uint32_t reg,uint32_t value){
        auto& s=*static_cast<Model*>(context);++s.writes;s.now+=s.writeLatency;
        if(!s.live)s.forbidden=true;
        const auto& transport=s.controller.journal().transport;
        if(reg==s.boot.layout.response){
            if(value||!transport.responseClearAttempted||transport.stage!=sc::Stage::ClearResponse)s.forbidden=true;
            s.response=value;
        }else if(reg==s.boot.layout.argument){
            if(!transport.argumentAttempted||transport.stage!=sc::Stage::Argument||
                transport.requestArgument!=value)s.forbidden=true;
            s.argument=value;
        }
        else if(reg==s.boot.layout.message){
            if(s.response)s.forbidden=true;
            if(!transport.messageAttempted||transport.stage!=sc::Stage::Message||
                transport.lastMessage!=value||transport.lastArgument!=s.argument||
                !transport.requestSerial||transport.lastMessageSerial!=transport.requestSerial)s.forbidden=true;
            s.commands.push_back({value,s.argument});
            if(value==2)s.argument=s.boot.smuVersion;
            else if(value==3)s.argument=s.boot.interfaceVersion;
            else if(value==0x0c||value==0x0d){++s.featureReads;
                if(s.changeFeatureAt==s.featureReads)s.runningFeatures^=uint64_t(1)<<12;
                s.argument=value==0x0c?uint32_t(s.runningFeatures):uint32_t(s.runningFeatures>>32);}
            else if(value==0x1d)s.argument=s.minimum;
            else if(value==0x1e)s.argument=s.maximum;
            else if(value==0x27)s.argument=s.dcMaximum;
            else if(value==0x0a||value==0x08){
                const auto& j=s.controller.journal().deepSleep;
                if(!j.captured||!s.argument||(s.argument&~0x3000u)||s.argument!=j.changedBits||
                    (value==0x0a?!j.disableAttempted:!j.restoreAttempted))s.forbidden=true;
                if(!s.ignoreDeepSleep){if(value==0x0a)s.runningFeatures&=~uint64_t(s.argument);
                    else s.runningFeatures|=s.argument;}
                if(s.reenterDeepSleep){++s.reentries;
                    if(s.controller.inhibitSocFabricDeepSleep(poll,s.result)!=sc::Status::InvalidState||
                        s.controller.restoreSocFabricDeepSleepAfterQuiescence(poll,s.result)!=sc::Status::InvalidState)
                        s.forbidden=true;}
            }
            else if(value==0x3b){
                const auto& j=s.controller.journal().dfCstate;
                if(s.argument>1||(s.argument==0?!j.disallowAttempted:!j.allowAttempted))s.forbidden=true;
                if(s.reenterDfCstate){++s.reentries;
                    if(s.controller.disallowDfCstate(poll,s.result)!=sc::Status::InvalidState||
                        s.controller.allowDfCstateAfterQuiescence(poll,s.result)!=sc::Status::InvalidState)
                        s.forbidden=true;}
            }
            else if(value==0x1a||value==0x19){
                if((s.argument>>16)!=1&&(s.argument>>16)!=3)s.forbidden=true;
            }else if(value==0x0e)s.table=(s.table&UINT32_MAX)|(uint64_t(s.argument)<<32);
            else if(value==0x0f)s.table=(s.table&~uint64_t(UINT32_MAX))|s.argument;
            else if(value==0x12){
                if(s.argument!=5||s.table!=s.mc)s.forbidden=true;
                s.visible=false;
                if(s.allOnesMetrics)std::memset(s.memory,0xff,sc::kMetricsBytes);
                else std::memcpy(s.memory,s.payload,sc::kMetricsBytes);
            }else s.forbidden=true;
            if(s.reenterAutomatic&&(value==0x1a||value==0x19)&&
                ((s.argument&65535)==65535||(s.argument&65535)==0)){
                ++s.reentries;
                if(s.controller.returnRangesToAutomaticAfterQuiescence(poll,s.result)!=sc::Status::InvalidState||
                    s.controller.requestSoftRange(sc::Clock::Soc,1800,1800,poll,s.result)!=sc::Status::InvalidState||
                    s.controller.sealMetricsAfterCompletedTransfer(poll,s.result)!=sc::Status::InvalidState)s.forbidden=true;
            }
            if(s.reenterRecovery){
                ++s.reentries;
                if(s.controller.reconcileMetricsForRetirement(poll,s.result)!=sc::Status::InvalidState||
                    s.controller.captureMetrics(poll,s.metrics,s.result)!=sc::Status::InvalidState||
                    s.controller.returnRangesToAutomaticAfterQuiescence(poll,s.result)!=sc::Status::InvalidState)
                    s.forbidden=true;
            }
            s.response=s.commands.size()==s.badCommand?s.badResponse:1;s.busyReads=s.busyEach;
        }else if(reg==0x18)s.hi=value;
        else if(reg==0)s.index=value;
        else s.forbidden=true;
        if(s.revokeAfterWrite==s.writes)s.live=false;
        return s.failWrite!=s.writes; // Deliberately models writes landing before failure.
    }
    sc::Status bind(){return controller.bind(smu_test::ips(),0x80000,boot.state,io(),poll,result);}
    sc::Status publish(){return controller.publishMetrics(allocator,allocation.handle,poll,result);}
    sc::Status capture(){return controller.captureMetrics(poll,metrics,result);}
    sc::Status automatic(){return controller.returnRangesToAutomaticAfterQuiescence(poll,result);}
    sc::Status inhibit(){return controller.inhibitSocFabricDeepSleep(poll,result);}
    sc::Status restore(){return controller.restoreSocFabricDeepSleepAfterQuiescence(poll,result);}
    sc::Status disallowDf(){return controller.disallowDfCstate(poll,result);}
    sc::Status allowDf(){return controller.allowDfCstateAfterQuiescence(poll,result);}
    sc::Status reconcile(){return controller.reconcileMetricsForRetirement(poll,result);}
    void failedTransfer(){
        CHECK(bind()==sc::Status::Ok);CHECK(publish()==sc::Status::Ok);
        failWrite=writes+9; // Two feature commands, then a delivered transfer message.
        CHECK(capture()==sc::Status::WriteFailed);CHECK(controller.faulted());
        CHECK(controller.tableRetentionRequired());CHECK(controller.journal().transport.messageAttempted);
        CHECK(controller.journal().transport.lastMessage==0x12);
        CHECK(faults==1);CHECK(!live);failWrite=0;
        // Offline model of native admission to the same retained, pre-reset
        // owner under a fresh actual RetirementScope; controller fault stays.
        live=true;
    }
    void terminalFailure(){
        CHECK(controller.faulted());CHECK(!controller.ready());CHECK(faults==1);
        CHECK(controller.tableRetentionRequired());CHECK(!controller.journal().reconciliationCompleted);
        CHECK(!forbidden);const auto r=reads,w=writes;
        CHECK(reconcile()==sc::Status::InvalidState);CHECK(capture()==sc::Status::InvalidState);
        CHECK(automatic()==sc::Status::InvalidState);CHECK(reads==r&&writes==w);
    }
    sc::Status full(){auto status=bind();if(status!=sc::Status::Ok)return status;
        status=controller.requestSoftRange(sc::Clock::Soc,1800,1800,poll,result);if(status!=sc::Status::Ok)return status;
        status=controller.requestSoftRange(sc::Clock::Fabric,1800,1800,poll,result);if(status!=sc::Status::Ok)return status;
        status=publish();return status==sc::Status::Ok?capture():status;}
    void sealedFailure(){CHECK(controller.faulted());CHECK(faults==1);CHECK(!controller.ready());CHECK(!forbidden);
        // Inspect failure output before an InvalidState call clears it again.
        if(!controller.journal().transferSerial){
            bool zero=!metrics.counter&&!metrics.transferSerial&&!metrics.socRangeObserved&&!metrics.fabricRangeObserved;
            for(auto value:metrics.raw)zero=zero&&!value;
            for(auto value:metrics.currentClockMHz)zero=zero&&!value;
            CHECK(zero);
        }
        const auto beforeReads=reads,beforeWrites=writes;
        CHECK(capture()==sc::Status::InvalidState);CHECK(reads==beforeReads&&writes==beforeWrites);
        CHECK(controller.sealMetricsAfterCompletedTransfer(poll,result)==sc::Status::InvalidState);
        CHECK(faults==1);CHECK(!metrics.transferSerial);
    }
};
void success(const gfxfw::State& firmware){
    Model s(firmware);CHECK(s.valid);CHECK(s.full()==sc::Status::Ok);CHECK(s.controller.ready());CHECK(!s.forbidden);
    CHECK(s.faults==0);CHECK(s.metrics.transferSerial==1);CHECK(s.metrics.counter==0);
    CHECK(s.metrics.currentClockMHz[1]==1800&&s.metrics.currentClockMHz[3]==1800);
    CHECK(s.metrics.socRangeObserved&&s.metrics.fabricRangeObserved);
    CHECK(std::memcmp(s.metrics.raw,s.payload,sc::kMetricsBytes)==0);
    CHECK(s.result.transfer.bytesRead==sc::kMetricsBytes);CHECK(s.controller.tableRetentionRequired());
    CHECK(s.controller.journal().tableAttemptSerial==3&&s.controller.journal().completedTableAttemptSerial==3);
    CHECK(s.controller.rangeRetentionRequired());CHECK(s.controller.retentionRequired());
    unsigned ranges=0;for(size_t i=0;i<s.commands.size();++i)if(s.commands[i].message==0x1a){
        CHECK(i+1<s.commands.size());CHECK(s.commands[i+1].message==0x19);
        CHECK((s.commands[i].argument&65535)==1801);CHECK((s.commands[i+1].argument&65535)==1800);++ranges;}
    CHECK(ranges==2);CHECK(s.table==s.mc);
    // A new capture always issues a real transfer, accepts wrapped/raw counters,
    // and preserves unrelated all-ones data without interpreting it as removal.
    pspring_test::put(s.payload+80,UINT32_MAX);pspring_test::put(s.payload+100,UINT32_MAX);
    pspring_test::put(s.payload+4,1700);CHECK(s.capture()==sc::Status::Ok);
    CHECK(s.metrics.transferSerial==2&&s.metrics.counter==UINT32_MAX);
    CHECK(s.controller.journal().tableAttemptSerial==4&&s.controller.journal().completedTableAttemptSerial==4);
    CHECK(!s.metrics.socRangeObserved&&s.metrics.fabricRangeObserved);
    CHECK(pspring_test::get(s.metrics.raw+100)==UINT32_MAX);
    const auto beforeReads=s.reads,beforeWrites=s.writes;
    CHECK(s.controller.sealMetricsAfterCompletedTransfer(poll,s.result)==sc::Status::Ok);
    CHECK(s.reads==beforeReads&&s.writes==beforeWrites);CHECK(s.controller.journal().tableSealed);
    CHECK(!s.controller.tableRetentionRequired());CHECK(s.controller.rangeRetentionRequired());
    CHECK(s.controller.retentionRequired());CHECK(s.capture()==sc::Status::InvalidState);
    CHECK(s.publish()==sc::Status::InvalidState);CHECK(s.reads==beforeReads&&s.writes==beforeWrites);
    CHECK(s.allocator.release(s.allocation.handle)==VramAllocatorStatus::Ok);
    CHECK(s.controller.sealMetricsAfterCompletedTransfer(poll,s.result)==sc::Status::InvalidState);
    Model t(firmware);CHECK(t.bind()==sc::Status::Ok);CHECK(t.publish()==sc::Status::Ok);
    CHECK(t.controller.sealMetricsAfterCompletedTransfer(poll,t.result)==sc::Status::InvalidState);
    CHECK(t.controller.tableRetentionRequired());CHECK(t.capture()==sc::Status::Ok);
    CHECK(t.controller.sealMetricsAfterCompletedTransfer(poll,t.result)==sc::Status::Ok);
    CHECK(!t.controller.retentionRequired());CHECK(!t.forbidden);
}
void preflight(const gfxfw::State& firmware){
    Model s(firmware);sc::Io invalid=s.io();invalid.faultOwner=nullptr;
    CHECK(s.controller.bind(smu_test::ips(),0x80000,s.boot.state,invalid,poll,s.result)==sc::Status::InvalidArgument);
    CHECK(!s.reads&&!s.writes);CHECK(s.bind()==sc::Status::Ok);
    const auto reads=s.reads,writes=s.writes;
    CHECK(s.controller.requestSoftRange(sc::Clock::Soc,0,1800,poll,s.result)==sc::Status::InvalidArgument);
    CHECK(s.controller.requestSoftRange(sc::Clock::Soc,1800,1799,poll,s.result)==sc::Status::InvalidArgument);
    CHECK(s.controller.requestSoftRange(sc::Clock::Soc,1800,65535,poll,s.result)==sc::Status::InvalidArgument);
    CHECK(s.controller.requestSoftRange(sc::Clock::Soc,1800,65534,poll,s.result)==sc::Status::InvalidArgument);
    CHECK(s.controller.requestSoftRange(static_cast<sc::Clock>(2),1800,1800,poll,s.result)==sc::Status::InvalidArgument);
    CHECK(s.controller.publishMetrics(s.allocator,{},poll,s.result)==sc::Status::InvalidAllocation);
    CHECK(s.controller.captureMetrics({0,10,1},s.metrics,s.result)==sc::Status::InvalidState);
    CHECK(s.reads==reads&&s.writes==writes);CHECK(!s.faults);CHECK(s.controller.ready());
    Model wrong(firmware);smu_test::Simulation newer;newer.interfaceVersion=0x40;
    CHECK(newer.awake(firmware)==smu::Status::Ok);
    CHECK(wrong.controller.bind(smu_test::ips(),0x80000,newer.state,wrong.io(),poll,wrong.result)==sc::Status::UnsupportedInterface);
    CHECK(!wrong.reads&&!wrong.writes);
}
void semanticFailures(const gfxfw::State& firmware){
    {Model s(firmware);s.runningFeatures=0x10;CHECK(s.bind()==sc::Status::DisabledFeature);s.sealedFailure();}
    {Model s(firmware);s.changeFeatureAt=3;CHECK(s.bind()==sc::Status::ChangedFeatures);s.sealedFailure();}
    {Model s(firmware);s.onesRead=1;CHECK(s.bind()==sc::Status::DeviceUnavailable);CHECK(!s.writes);s.sealedFailure();}
    {Model s(firmware);s.response=0;CHECK(s.bind()==sc::Status::Timeout);CHECK(!s.writes);s.sealedFailure();}
    {Model s(firmware);CHECK(s.bind()==sc::Status::Ok);s.runningFeatures&=~uint64_t(0x20);
        CHECK(s.controller.requestSoftRange(sc::Clock::Soc,1800,1800,poll,s.result)==sc::Status::DisabledFeature);
        CHECK(!s.controller.rangeRetentionRequired());s.sealedFailure();}
    {Model s(firmware);CHECK(s.bind()==sc::Status::Ok);s.dcMaximum=1700;
        CHECK(s.controller.requestSoftRange(sc::Clock::Soc,1800,1800,poll,s.result)==sc::Status::InvalidFrequency);
        CHECK(!s.controller.rangeRetentionRequired());s.sealedFailure();}
    {Model s(firmware);CHECK(s.bind()==sc::Status::Ok);CHECK(s.publish()==sc::Status::Ok);s.failVisibility=true;
        CHECK(s.capture()==sc::Status::VisibilityFailed);CHECK(s.controller.journal().transferAcknowledged);
        CHECK(!s.controller.journal().cpuVisible);CHECK(s.controller.tableRetentionRequired());s.sealedFailure();}
    {Model s(firmware);CHECK(s.bind()==sc::Status::Ok);CHECK(s.publish()==sc::Status::Ok);s.allOnesMetrics=true;
        CHECK(s.capture()==sc::Status::InvalidMetrics);CHECK(s.controller.tableRetentionRequired());s.sealedFailure();}
    {Model s(firmware);CHECK(s.bind()==sc::Status::Ok);CHECK(s.publish()==sc::Status::Ok);
        CHECK(s.allocator.release(s.allocation.handle)==VramAllocatorStatus::Ok);VramAllocation replacement{};
        CHECK(s.allocator.allocate(4096,4096,replacement)==VramAllocatorStatus::Ok);
        CHECK(s.capture()==sc::Status::InvalidAllocation);CHECK(s.controller.tableRetentionRequired());s.sealedFailure();}
    {Model s(firmware);CHECK(s.full()==sc::Status::Ok);s.failRead=s.reads+1;
        CHECK(s.capture()!=sc::Status::Ok);CHECK(s.controller.tableRetentionRequired());s.sealedFailure();}
    {Model s(firmware);CHECK(s.full()==sc::Status::Ok);s.live=false;
        CHECK(s.controller.sealMetricsAfterCompletedTransfer(poll,s.result)==sc::Status::OwnerUnavailable);
        CHECK(s.controller.tableRetentionRequired());s.sealedFailure();}
    {Model s(firmware);s.busyEach=2;CHECK(s.full()==sc::Status::Ok);CHECK(!s.forbidden);}
    {Model s(firmware);CHECK(s.bind()==sc::Status::Ok);CHECK(s.publish()==sc::Status::Ok);s.readLatency=5000;
        CHECK(s.capture()==sc::Status::Timeout);CHECK(s.result.elapsedUs>=poll.timeoutUs);
        CHECK(s.controller.tableRetentionRequired());s.sealedFailure();}
}
void faultMatrix(const gfxfw::State& firmware){
    Model good(firmware);CHECK(good.full()==sc::Status::Ok);
    for(uint32_t n=1;n<=good.reads;++n){Model s(firmware);s.failRead=n;
        CHECK(s.full()!=sc::Status::Ok);CHECK(s.reads==n);s.sealedFailure();}
    for(uint32_t n=1;n<=good.writes;++n){Model s(firmware);s.failWrite=n;
        CHECK(s.full()!=sc::Status::Ok);CHECK(s.writes==n);s.sealedFailure();}
    // Revocation both before and immediately after every actual callback.
    for(uint32_t n=1;n<=good.valids;++n){Model s(firmware);s.revokeValid=n;
        CHECK(s.full()!=sc::Status::Ok);s.sealedFailure();}
    for(uint32_t n=1;n<=good.reads;++n){Model s(firmware);s.revokeAfterRead=n;
        CHECK(s.full()==sc::Status::OwnerUnavailable);CHECK(s.reads==n);s.sealedFailure();}
    for(uint32_t n=1;n<=good.writes;++n){Model s(firmware);s.revokeAfterWrite=n;
        CHECK(s.full()==sc::Status::OwnerUnavailable);CHECK(s.writes==n);s.sealedFailure();}
    for(uint32_t n=2;n<=good.clocks;++n){Model s(firmware);s.backwardsAt=n;
        const auto status=s.full();
        // A new operation's first timestamp is not comparable with the last
        // completed operation; a monotonic native clock is an owner contract.
        if(status!=sc::Status::Ok){CHECK(status==sc::Status::ClockWentBackwards);s.sealedFailure();}
    }
    const uint32_t codes[]={0xff,0xfe,0xfd,0xfc,7,UINT32_MAX};
    for(uint32_t n=1;n<=good.commands.size();++n)for(auto code:codes){Model s(firmware);s.badCommand=n;s.badResponse=code;
        CHECK(s.full()!=sc::Status::Ok);CHECK(s.commands.size()==n);s.sealedFailure();}
    for(uint32_t n=2;n<=good.clocks;++n){Model s(firmware);s.deadlineAt=n;
        const auto status=s.full();if(status!=sc::Status::Ok){CHECK(status==sc::Status::Timeout);s.sealedFailure();}
    }
    std::printf("Fault baselines: %u reads, %u writes, %u validity checks, %u clocks, %zu commands\n",
        good.reads,good.writes,good.valids,good.clocks,good.commands.size());
}
void automaticPolicy(const gfxfw::State& firmware){
    {Model s(firmware);CHECK(s.full()==sc::Status::Ok);const auto commands=s.commands.size();
        s.reenterAutomatic=true;CHECK(s.automatic()==sc::Status::Ok);CHECK(s.reentries==4);CHECK(!s.forbidden);
        CHECK(s.controller.journal().rangesSealed);CHECK(!s.controller.rangeRetentionRequired());
        CHECK(s.controller.tableRetentionRequired());CHECK(s.controller.retentionRequired());
        const auto& soc=s.controller.journal().soc;const auto& fabric=s.controller.journal().fabric;
        CHECK(soc.automaticMaximumAttempted&&soc.automaticMaximumAcknowledged&&soc.automaticMinimumAttempted&&soc.automaticMinimumAcknowledged);
        CHECK(fabric.automaticMaximumAttempted&&fabric.automaticMaximumAcknowledged&&fabric.automaticMinimumAttempted&&fabric.automaticMinimumAcknowledged);
        CHECK(s.commands.size()==commands+8);
        CHECK(s.commands[commands+2].message==0x1a&&s.commands[commands+2].argument==0x1ffff);
        CHECK(s.commands[commands+3].message==0x19&&s.commands[commands+3].argument==0x10000);
        CHECK(s.commands[commands+4].message==0x1a&&s.commands[commands+4].argument==0x3ffff);
        CHECK(s.commands[commands+5].message==0x19&&s.commands[commands+5].argument==0x30000);
        const auto writes=s.writes,reads=s.reads;
        CHECK(s.automatic()==sc::Status::InvalidState);
        CHECK(s.controller.requestSoftRange(sc::Clock::Soc,1800,1800,poll,s.result)==sc::Status::InvalidState);
        CHECK(s.reads==reads&&s.writes==writes);
        CHECK(s.capture()==sc::Status::Ok);CHECK(!s.metrics.socRangeObserved&&!s.metrics.fabricRangeObserved);
        CHECK(s.controller.sealMetricsAfterCompletedTransfer(poll,s.result)==sc::Status::Ok);
        CHECK(!s.controller.retentionRequired());
    }
    // Only touched clocks receive a policy command, and scratch is independent.
    {Model s(firmware);CHECK(s.bind()==sc::Status::Ok);
        CHECK(s.controller.requestSoftRange(sc::Clock::Fabric,1800,1800,poll,s.result)==sc::Status::Ok);
        CHECK(s.automatic()==sc::Status::Ok);CHECK(!s.controller.retentionRequired());
        CHECK(!s.controller.journal().soc.automaticMaximumAttempted);
        CHECK(s.controller.journal().fabric.automaticMinimumAcknowledged);
    }
    {Model s(firmware);CHECK(s.bind()==sc::Status::Ok);const auto reads=s.reads,writes=s.writes;
        CHECK(s.automatic()==sc::Status::Ok);CHECK(s.controller.journal().rangesSealed);
        CHECK(s.reads==reads&&s.writes==writes);CHECK(!s.controller.retentionRequired());}
    {Model s(firmware);CHECK(s.full()==sc::Status::Ok);const auto reads=s.reads,writes=s.writes;
        CHECK(s.controller.returnRangesToAutomaticAfterQuiescence({0,1,1},s.result)==sc::Status::InvalidArgument);
        CHECK(s.reads==reads&&s.writes==writes);CHECK(!s.faults);CHECK(s.controller.rangeRetentionRequired());}
    {Model s(firmware);CHECK(s.full()==sc::Status::Ok);s.runningFeatures&=~uint64_t(0x20);
        CHECK(s.automatic()==sc::Status::DisabledFeature);CHECK(s.controller.rangeRetentionRequired());
        CHECK(!s.controller.journal().soc.automaticMaximumAttempted);s.sealedFailure();}
    {Model s(firmware);CHECK(s.full()==sc::Status::Ok);s.changeFeatureAt=s.featureReads+3;
        CHECK(s.automatic()==sc::Status::ChangedFeatures);CHECK(s.controller.rangeRetentionRequired());
        CHECK(s.controller.journal().fabric.automaticMinimumAcknowledged);s.sealedFailure();}
    {Model s(firmware);CHECK(s.full()==sc::Status::Ok);
        CHECK(s.controller.sealMetricsAfterCompletedTransfer(poll,s.result)==sc::Status::Ok);
        CHECK(s.allocator.release(s.allocation.handle)==VramAllocatorStatus::Ok);
        CHECK(s.automatic()==sc::Status::Ok);CHECK(!s.controller.retentionRequired());}
    Model good(firmware);CHECK(good.full()==sc::Status::Ok);
    const auto initialReads=good.reads,initialWrites=good.writes,initialValids=good.valids,initialClocks=good.clocks;
    const auto initialCommands=good.commands.size();CHECK(good.automatic()==sc::Status::Ok);
    for(uint32_t n=initialReads+1;n<=good.reads;++n){Model s(firmware);CHECK(s.full()==sc::Status::Ok);s.failRead=n;
        CHECK(s.automatic()!=sc::Status::Ok);CHECK(s.reads==n);CHECK(s.controller.rangeRetentionRequired());s.sealedFailure();}
    for(uint32_t n=initialWrites+1;n<=good.writes;++n){Model s(firmware);CHECK(s.full()==sc::Status::Ok);s.failWrite=n;
        CHECK(s.automatic()!=sc::Status::Ok);CHECK(s.writes==n);CHECK(s.controller.rangeRetentionRequired());s.sealedFailure();}
    for(uint32_t n=initialValids+1;n<=good.valids;++n){Model s(firmware);CHECK(s.full()==sc::Status::Ok);s.revokeValid=n;
        CHECK(s.automatic()==sc::Status::OwnerUnavailable);CHECK(s.controller.rangeRetentionRequired());s.sealedFailure();}
    for(uint32_t n=initialReads+1;n<=good.reads;++n){Model s(firmware);CHECK(s.full()==sc::Status::Ok);s.revokeAfterRead=n;
        CHECK(s.automatic()==sc::Status::OwnerUnavailable);CHECK(s.reads==n);CHECK(s.controller.rangeRetentionRequired());s.sealedFailure();}
    for(uint32_t n=initialWrites+1;n<=good.writes;++n){Model s(firmware);CHECK(s.full()==sc::Status::Ok);s.revokeAfterWrite=n;
        CHECK(s.automatic()==sc::Status::OwnerUnavailable);CHECK(s.writes==n);CHECK(s.controller.rangeRetentionRequired());s.sealedFailure();}
    const uint32_t codes[]={0xff,0xfe,0xfd,0xfc,7,UINT32_MAX};
    for(uint32_t n=uint32_t(initialCommands)+1;n<=good.commands.size();++n)for(auto code:codes){
        Model s(firmware);CHECK(s.full()==sc::Status::Ok);s.badCommand=n;s.badResponse=code;
        CHECK(s.automatic()!=sc::Status::Ok);CHECK(s.commands.size()==n);CHECK(s.controller.rangeRetentionRequired());s.sealedFailure();}
    for(uint32_t n=initialClocks+2;n<=good.clocks;++n){Model s(firmware);CHECK(s.full()==sc::Status::Ok);s.deadlineAt=n;
        CHECK(s.automatic()==sc::Status::Timeout);CHECK(s.controller.rangeRetentionRequired());s.sealedFailure();}
    std::printf("Automatic terminal fault baselines: %u reads, %u writes, %u validity checks, %u clocks, %zu commands\n",
        good.reads-initialReads,good.writes-initialWrites,good.valids-initialValids,good.clocks-initialClocks,good.commands.size()-initialCommands);
}
void reconciliation(const gfxfw::State& firmware){
    {Model s(firmware);s.failedTransfer();const auto commands=s.commands.size();
        const auto attempt=s.controller.journal().tableAttemptSerial;
        s.reenterRecovery=true;CHECK(s.reconcile()==sc::Status::Ok);CHECK(s.reentries==5);
        CHECK(!s.forbidden);CHECK(!s.controller.ready());CHECK(s.controller.faulted());CHECK(s.faults==1);
        const auto& j=s.controller.journal();CHECK(j.reconciliationAttempted&&j.reconciliationPriorSettled);
        CHECK(j.reconciliationPriorResponse==1&&j.reconciliationCompleted&&j.tableSealed);
        CHECK(j.tableAttemptSerial==attempt+3&&j.completedTableAttemptSerial==j.tableAttemptSerial);
        CHECK(j.transport.lastMessage==0x12&&j.transport.settledMessageSerial==j.transport.lastMessageSerial);
        CHECK(s.result.transfer.bytesRead==sc::kMetricsBytes);CHECK(!s.controller.retentionRequired());
        CHECK(s.commands.size()==commands+5);
        const uint32_t sequence[]={2,3,0x0e,0x0f,0x12};
        for(unsigned n=0;n<5;++n)CHECK(s.commands[commands+n].message==sequence[n]);
        CHECK(s.commands[commands+2].argument==uint32_t(s.mc>>32));
        CHECK(s.commands[commands+3].argument==uint32_t(s.mc));
        CHECK(s.commands[commands+4].argument==5);
        const auto reads=s.reads,writes=s.writes;
        CHECK(s.reconcile()==sc::Status::InvalidState);CHECK(s.capture()==sc::Status::InvalidState);
        CHECK(s.automatic()==sc::Status::InvalidState);CHECK(s.publish()==sc::Status::InvalidState);
        CHECK(s.reads==reads&&s.writes==writes);CHECK(s.allocator.release(s.allocation.handle)==VramAllocatorStatus::Ok);
    }
    for(unsigned failedHalf=0;failedHalf<2;++failedHalf){
        Model s(firmware);CHECK(s.bind()==sc::Status::Ok);
        s.failWrite=s.writes+3*(failedHalf+1);CHECK(s.publish()==sc::Status::WriteFailed);
        CHECK(s.controller.journal().transport.lastMessage==(failedHalf?0x0fu:0x0eu));
        CHECK(s.controller.journal().addressHighAcknowledged==bool(failedHalf));
        CHECK(!s.controller.journal().addressLowAcknowledged);CHECK(!s.controller.journal().transferAttempted);
        CHECK(s.controller.tableRetentionRequired());s.failWrite=0;s.live=true;
        CHECK(s.reconcile()==sc::Status::Ok);CHECK(s.table==s.mc);CHECK(!s.controller.retentionRequired());
        CHECK(!s.forbidden);
    }
    // Failed response-clear/argument writes can leave zero with NO message
    // sent. Do not invent a skip-prepoll exception or synthesize an ACK.
    for(unsigned failedWrite=1;failedWrite<=2;++failedWrite){
        Model s(firmware);CHECK(s.bind()==sc::Status::Ok);
        const auto lastMessage=s.controller.journal().transport.lastMessageSerial;
        s.failWrite=s.writes+failedWrite;CHECK(s.publish()==sc::Status::WriteFailed);
        CHECK(s.response==0);CHECK(!s.controller.journal().transport.messageAttempted);
        CHECK(s.controller.journal().transport.lastMessageSerial==lastMessage);
        s.failWrite=0;s.live=true;const auto writes=s.writes;
        CHECK(s.reconcile()==sc::Status::Timeout);CHECK(s.writes==writes);s.terminalFailure();
    }
    {Model s(firmware);s.failedTransfer();const auto reads=s.reads,writes=s.writes;
        CHECK(s.controller.reconcileMetricsForRetirement({0,1,1},s.result)==sc::Status::InvalidArgument);
        CHECK(!s.controller.journal().reconciliationAttempted);CHECK(s.reads==reads&&s.writes==writes);
        CHECK(s.reconcile()==sc::Status::Ok);}
    {Model s(firmware);s.failedTransfer();s.allOnesMetrics=true;
        // This terminal raw read is DMA completion evidence, not a frequency
        // observation. No decoded Metrics result is manufactured or returned.
        CHECK(s.reconcile()==sc::Status::Ok);CHECK(!s.metrics.transferSerial);
        CHECK(s.result.transfer.bytesRead==sc::kMetricsBytes);CHECK(!s.controller.ready());}
    // Known prior rejections settle the transport, never stand in for the new
    // five commands' success responses or manufacture a completed transfer.
    const uint32_t prior[]={0xff,0xfe,0xfd,0xfc};
    for(auto code:prior){Model s(firmware);s.failedTransfer();s.response=code;
        CHECK(s.reconcile()==sc::Status::Ok);CHECK(s.controller.journal().reconciliationPriorResponse==code);
        CHECK(s.controller.journal().transferAcknowledged&&s.controller.journal().readComplete);CHECK(!s.forbidden);}
    {Model s(firmware);s.failedTransfer();s.response=0;s.busyReads=2;
        // Once initial busy observations finish, a real late ACK appears.
        s.response=1;CHECK(s.reconcile()==sc::Status::Ok);CHECK(s.result.delays==2);}
    {Model s(firmware);s.failedTransfer();s.response=0;const auto w=s.writes;
        CHECK(s.reconcile()==sc::Status::Timeout);CHECK(s.writes==w);s.terminalFailure();}
    for(auto code:{7u,UINT32_MAX}){Model s(firmware);s.failedTransfer();s.response=code;const auto w=s.writes;
        CHECK(s.reconcile()!=sc::Status::Ok);CHECK(s.writes==w);s.terminalFailure();}
    {Model s(firmware);CHECK(s.bind()==sc::Status::Ok);CHECK(s.publish()==sc::Status::Ok);
        const auto r=s.reads,w=s.writes;CHECK(s.reconcile()==sc::Status::InvalidState);
        CHECK(s.reads==r&&s.writes==w);CHECK(s.controller.ready());}
    {Model s(firmware);s.failedTransfer();s.boot.smuVersion^=1;
        CHECK(s.reconcile()==sc::Status::ChangedFirmware);s.terminalFailure();}
    {Model s(firmware);s.failedTransfer();s.boot.interfaceVersion^=1;
        CHECK(s.reconcile()==sc::Status::ChangedFirmware);s.terminalFailure();}
    {Model s(firmware);s.failedTransfer();CHECK(s.allocator.release(s.allocation.handle)==VramAllocatorStatus::Ok);
        VramAllocation replacement{};CHECK(s.allocator.allocate(4096,4096,replacement)==VramAllocatorStatus::Ok);
        const auto w=s.writes;CHECK(s.reconcile()==sc::Status::InvalidAllocation);CHECK(s.writes==w);s.terminalFailure();}
    {Model s(firmware);s.failedTransfer();s.live=false;CHECK(s.reconcile()==sc::Status::OwnerUnavailable);s.terminalFailure();}
    {Model s(firmware);s.failedTransfer();s.failVisibility=true;
        CHECK(s.reconcile()==sc::Status::VisibilityFailed);s.terminalFailure();}
    // A prior successful range stays owned, even after table-only recovery.
    {Model s(firmware);CHECK(s.full()==sc::Status::Ok);s.failWrite=s.writes+9;
        CHECK(s.capture()==sc::Status::WriteFailed);s.failWrite=0;s.live=true;
        CHECK(s.reconcile()==sc::Status::Ok);CHECK(!s.controller.tableRetentionRequired());
        CHECK(s.controller.rangeRetentionRequired());CHECK(s.automatic()==sc::Status::InvalidState);}
    // Fatal original replies are not turned into recoverable epochs merely
    // because a later register happens to contain a success value.
    for(auto code:{7u,UINT32_MAX}){Model s(firmware);CHECK(s.bind()==sc::Status::Ok);CHECK(s.publish()==sc::Status::Ok);
        s.badCommand=uint32_t(s.commands.size())+3;s.badResponse=code;CHECK(s.capture()!=sc::Status::Ok);
        s.response=1;s.live=true;const auto r=s.reads,w=s.writes;CHECK(s.reconcile()==sc::Status::InvalidState);
        CHECK(s.reads==r&&s.writes==w);CHECK(s.controller.tableRetentionRequired());}
    Model good(firmware);good.failedTransfer();
    const auto reads=good.reads,writes=good.writes,valids=good.valids,clocks=good.clocks;
    const auto commands=good.commands.size();CHECK(good.reconcile()==sc::Status::Ok);
    for(uint32_t n=reads+1;n<=good.reads;++n){Model s(firmware);s.failedTransfer();s.failRead=n;
        CHECK(s.reconcile()!=sc::Status::Ok);CHECK(s.reads==n);s.terminalFailure();}
    for(uint32_t n=writes+1;n<=good.writes;++n){Model s(firmware);s.failedTransfer();s.failWrite=n;
        CHECK(s.reconcile()!=sc::Status::Ok);CHECK(s.writes==n);s.terminalFailure();}
    for(uint32_t n=valids+1;n<=good.valids;++n){Model s(firmware);s.failedTransfer();s.revokeValid=n;
        CHECK(s.reconcile()==sc::Status::OwnerUnavailable);s.terminalFailure();}
    for(uint32_t n=reads+1;n<=good.reads;++n){Model s(firmware);s.failedTransfer();s.revokeAfterRead=n;
        CHECK(s.reconcile()==sc::Status::OwnerUnavailable);CHECK(s.reads==n);s.terminalFailure();}
    for(uint32_t n=writes+1;n<=good.writes;++n){Model s(firmware);s.failedTransfer();s.revokeAfterWrite=n;
        CHECK(s.reconcile()==sc::Status::OwnerUnavailable);CHECK(s.writes==n);s.terminalFailure();}
    const uint32_t bad[]={0,0xff,0xfe,0xfd,0xfc,7,UINT32_MAX};
    for(uint32_t n=uint32_t(commands)+1;n<=good.commands.size();++n)for(auto code:bad){
        Model s(firmware);s.failedTransfer();s.badCommand=n;s.badResponse=code;
        CHECK(s.reconcile()!=sc::Status::Ok);CHECK(s.commands.size()==n);s.terminalFailure();}
    for(uint32_t n=clocks+2;n<=good.clocks;++n){Model s(firmware);s.failedTransfer();s.deadlineAt=n;
        CHECK(s.reconcile()==sc::Status::Timeout);s.terminalFailure();}
    for(uint32_t n=clocks+2;n<=good.clocks;++n){Model s(firmware);s.failedTransfer();s.backwardsAt=n;
        CHECK(s.reconcile()==sc::Status::ClockWentBackwards);s.terminalFailure();}
    std::printf("Reconciliation fault baselines: %u reads, %u writes, %u validity checks, %u clocks, %zu commands\n",
        good.reads-reads,good.writes-writes,good.valids-valids,good.clocks-clocks,good.commands.size()-commands);
}
void deepSleepPolicy(const gfxfw::State& firmware){
    for(uint32_t bits:{0u,0x1000u,0x2000u,0x3000u}){
        Model s(firmware);s.runningFeatures=UINT64_C(0x20000000000)|0x38u|bits;
        const auto original=s.runningFeatures;CHECK(s.bind()==sc::Status::Ok);s.reenterDeepSleep=true;
        const auto first=s.commands.size();CHECK(s.inhibit()==sc::Status::Ok);
        const auto& j=s.controller.journal().deepSleep;
        CHECK(j.captured&&j.inhibitReadbackComplete&&j.inhibited&&!j.released);
        CHECK(j.originalFeatures==original&&j.heldFeatures==(original&~UINT64_C(0x3000)));
        CHECK(j.changedBits==bits&&j.disableAttempted==bool(bits)&&j.disableAcknowledged==bool(bits));
        CHECK(s.controller.deepSleepRetentionRequired()&&s.controller.retentionRequired());
        CHECK(s.commands.size()-first==(bits?9u:8u));
        const auto writes=s.writes;CHECK(s.inhibit()==sc::Status::InvalidState);CHECK(s.writes==writes);
        CHECK(s.controller.requestSoftRange(sc::Clock::Soc,1800,1800,poll,s.result)==sc::Status::Ok);
        const auto heldWrites=s.writes;CHECK(s.restore()==sc::Status::InvalidState);CHECK(s.writes==heldWrites);
        CHECK(s.controller.ready());CHECK(s.automatic()==sc::Status::Ok);
        CHECK(s.controller.deepSleepRetentionRequired());CHECK(s.restore()==sc::Status::Ok);
        CHECK(s.runningFeatures==original&&j.restoreReadbackComplete&&j.released);
        CHECK(j.restoreAttempted==bool(bits)&&j.restoreAcknowledged==bool(bits));
        CHECK(!s.controller.retentionRequired()&&!s.forbidden);CHECK(s.reentries==(bits?2u:0u));
        const auto r=s.reads,w=s.writes;
        CHECK(s.restore()==sc::Status::InvalidState);CHECK(s.inhibit()==sc::Status::InvalidState);
        CHECK(s.controller.requestSoftRange(sc::Clock::Fabric,1800,1800,poll,s.result)==sc::Status::InvalidState);
        CHECK(s.reads==r&&s.writes==w);
    }
    {Model s(firmware);const auto w=s.writes;CHECK(s.inhibit()==sc::Status::InvalidState);
        CHECK(s.restore()==sc::Status::InvalidState);CHECK(s.writes==w);CHECK(s.bind()==sc::Status::Ok);
        const auto r=s.reads;CHECK(s.controller.inhibitSocFabricDeepSleep({0,1,1},s.result)==sc::Status::InvalidArgument);
        CHECK(s.reads==r);CHECK(s.inhibit()==sc::Status::Ok);
        CHECK(s.controller.restoreSocFabricDeepSleepAfterQuiescence({0,1,1},s.result)==sc::Status::InvalidArgument);
        CHECK(s.restore()==sc::Status::Ok);}
    {Model s(firmware);CHECK(s.bind()==sc::Status::Ok);
        CHECK(s.controller.requestSoftRange(sc::Clock::Soc,1800,1800,poll,s.result)==sc::Status::Ok);
        const auto w=s.writes;CHECK(s.inhibit()==sc::Status::InvalidState);CHECK(s.writes==w);}
    for(bool restoring:{false,true}){
        {Model s(firmware);CHECK(s.bind()==sc::Status::Ok);if(restoring)CHECK(s.inhibit()==sc::Status::Ok);
            s.ignoreDeepSleep=true;CHECK((restoring?s.restore():s.inhibit())==sc::Status::ChangedFeatures);
            CHECK(s.controller.deepSleepRetentionRequired());CHECK(s.controller.faulted()&&s.faults==1);}
        // Every individual enabled-mask response can expose a changing policy.
        for(uint32_t changed=1;changed<=8;++changed){Model s(firmware);CHECK(s.bind()==sc::Status::Ok);
            if(restoring)CHECK(s.inhibit()==sc::Status::Ok);
            s.changeFeatureAt=s.featureReads+changed;
            const auto status=restoring?s.restore():s.inhibit();
            // A change before the first complete acquisition sample is the
            // actual new baseline when the second complete sample agrees.
            if(!restoring&&changed<=2){CHECK(status==sc::Status::Ok);CHECK(s.restore()==sc::Status::Ok);}
            else {CHECK(status==sc::Status::ChangedFeatures);CHECK(s.controller.faulted()&&s.faults==1);}
            CHECK(!s.forbidden);
        }
        Model good(firmware);CHECK(good.bind()==sc::Status::Ok);if(restoring)CHECK(good.inhibit()==sc::Status::Ok);
        const auto reads=good.reads,writes=good.writes,valids=good.valids,clocks=good.clocks;
        const auto commands=uint32_t(good.commands.size());CHECK((restoring?good.restore():good.inhibit())==sc::Status::Ok);
        // Fault callbacks model a protocol transport only; they never stand
        // in for actual GPU/firmware evidence. Every ambiguous attempted
        // disable/restore must retain ownership and forbid later mailbox work.
        const auto failCheck=[&](Model& s){
            CHECK(s.controller.faulted()&&!s.controller.ready()&&s.faults==1&&!s.forbidden);
            const auto& j=s.controller.journal().deepSleep;
            CHECK(!j.released);CHECK(s.controller.deepSleepRetentionRequired()==(j.disableAttempted||j.inhibited));
            if(restoring)CHECK(s.controller.deepSleepRetentionRequired());
            const auto r=s.reads,w=s.writes;CHECK(s.restore()==sc::Status::InvalidState);
            CHECK(s.inhibit()==sc::Status::InvalidState);CHECK(s.reads==r&&s.writes==w);
        };
        const auto prepare=[&](Model& s){CHECK(s.bind()==sc::Status::Ok);if(restoring)CHECK(s.inhibit()==sc::Status::Ok);};
        for(uint32_t n=reads+1;n<=good.reads;++n){Model s(firmware);prepare(s);s.failRead=n;
            CHECK((restoring?s.restore():s.inhibit())!=sc::Status::Ok);CHECK(s.reads==n);failCheck(s);}
        for(uint32_t n=writes+1;n<=good.writes;++n){Model s(firmware);prepare(s);s.failWrite=n;
            CHECK((restoring?s.restore():s.inhibit())!=sc::Status::Ok);CHECK(s.writes==n);failCheck(s);}
        for(uint32_t n=valids+1;n<=good.valids;++n){Model s(firmware);prepare(s);s.revokeValid=n;
            CHECK((restoring?s.restore():s.inhibit())==sc::Status::OwnerUnavailable);failCheck(s);}
        for(uint32_t n=reads+1;n<=good.reads;++n){Model s(firmware);prepare(s);s.revokeAfterRead=n;
            CHECK((restoring?s.restore():s.inhibit())==sc::Status::OwnerUnavailable);CHECK(s.reads==n);failCheck(s);}
        for(uint32_t n=writes+1;n<=good.writes;++n){Model s(firmware);prepare(s);s.revokeAfterWrite=n;
            CHECK((restoring?s.restore():s.inhibit())==sc::Status::OwnerUnavailable);CHECK(s.writes==n);failCheck(s);}
        for(uint32_t n=clocks+2;n<=good.clocks;++n){Model s(firmware);prepare(s);s.deadlineAt=n;
            CHECK((restoring?s.restore():s.inhibit())==sc::Status::Timeout);failCheck(s);}
        for(uint32_t n=clocks+2;n<=good.clocks;++n){Model s(firmware);prepare(s);s.backwardsAt=n;
            CHECK((restoring?s.restore():s.inhibit())==sc::Status::ClockWentBackwards);failCheck(s);}
        const uint32_t bad[]={0,0xff,0xfe,0xfd,0xfc,7,UINT32_MAX};
        for(uint32_t n=commands+1;n<=good.commands.size();++n)for(auto code:bad){Model s(firmware);prepare(s);
            s.badCommand=n;s.badResponse=code;CHECK((restoring?s.restore():s.inhibit())!=sc::Status::Ok);failCheck(s);}
    }
    for(unsigned operation=0;operation<3;++operation){Model s(firmware);CHECK(s.bind()==sc::Status::Ok);
        CHECK(s.inhibit()==sc::Status::Ok);
        if(operation==1)CHECK(s.publish()==sc::Status::Ok);
        if(operation==2)CHECK(s.controller.requestSoftRange(sc::Clock::Soc,1800,1800,poll,s.result)==sc::Status::Ok);
        s.runningFeatures^=UINT64_C(1)<<41;
        const auto status=operation==0?s.controller.requestSoftRange(sc::Clock::Soc,1800,1800,poll,s.result):
            operation==1?s.capture():s.automatic();
        CHECK(status==sc::Status::ChangedFeatures);CHECK(s.controller.deepSleepRetentionRequired());
        CHECK(s.controller.faulted()&&!s.forbidden);
    }
}
void dfCstatePolicy(const gfxfw::State& firmware){
    // Feature bit41 is not a readback of the external-client allowance. Both
    // masks must produce the same explicit 0x3b policy commands, no bit toggle.
    for(bool enabled:{false,true})for(bool sleepFirst:{false,true}){
        Model s(firmware);if(enabled)s.runningFeatures|=UINT64_C(1)<<41;
        const auto original=s.runningFeatures;CHECK(s.bind()==sc::Status::Ok);
        if(sleepFirst)CHECK(s.inhibit()==sc::Status::Ok);
        s.reenterDfCstate=true;const auto first=s.commands.size();
        CHECK(s.disallowDf()==sc::Status::Ok);const auto& j=s.controller.journal().dfCstate;
        CHECK(j.disallowAttempted&&j.disallowAcknowledged&&j.disallowFeaturesVerified&&j.disallowed);
        CHECK(!j.allowAttempted&&!j.released);CHECK(s.controller.dfCstateRetentionRequired());
        CHECK(s.controller.retentionRequired());CHECK(s.commands.size()==first+9);
        CHECK(s.commands[first+4].message==0x3b&&s.commands[first+4].argument==0);
        CHECK(j.disallowFeatures==s.runningFeatures);
        const auto r=s.reads,w=s.writes;CHECK(s.disallowDf()==sc::Status::InvalidState);
        CHECK(s.reads==r&&s.writes==w);
        if(!sleepFirst)CHECK(s.inhibit()==sc::Status::Ok);
        CHECK(s.restore()==sc::Status::InvalidState);
        CHECK(s.controller.requestSoftRange(sc::Clock::Soc,1800,1800,poll,s.result)==sc::Status::Ok);
        CHECK(s.allowDf()==sc::Status::InvalidState);CHECK(s.automatic()==sc::Status::Ok);
        CHECK(s.publish()==sc::Status::Ok);CHECK(s.allowDf()==sc::Status::InvalidState);
        CHECK(s.capture()==sc::Status::Ok);
        CHECK(s.controller.sealMetricsAfterCompletedTransfer(poll,s.result)==sc::Status::Ok);
        const auto release=s.commands.size();CHECK(s.allowDf()==sc::Status::Ok);
        CHECK(s.commands.size()==release+9&&s.commands[release+4].message==0x3b&&s.commands[release+4].argument==1);
        CHECK(j.allowAttempted&&j.allowAcknowledged&&j.allowFeaturesVerified&&j.released);
        CHECK(j.allowFeatures==s.runningFeatures);CHECK(!s.controller.dfCstateRetentionRequired());
        CHECK(s.controller.deepSleepRetentionRequired());CHECK(s.restore()==sc::Status::Ok);
        CHECK(s.runningFeatures==original&&!s.controller.retentionRequired());
        CHECK(s.reentries==2&&!s.forbidden);
        const auto finalReads=s.reads,finalWrites=s.writes;
        CHECK(s.disallowDf()==sc::Status::InvalidState);CHECK(s.allowDf()==sc::Status::InvalidState);
        CHECK(s.controller.requestSoftRange(sc::Clock::Fabric,1800,1800,poll,s.result)==sc::Status::InvalidState);
        CHECK(s.inhibit()==sc::Status::InvalidState);CHECK(s.reads==finalReads&&s.writes==finalWrites);
    }
    {Model s(firmware);CHECK(s.disallowDf()==sc::Status::InvalidState);CHECK(s.allowDf()==sc::Status::InvalidState);
        CHECK(s.bind()==sc::Status::Ok);CHECK(s.allowDf()==sc::Status::InvalidState);
        const auto r=s.reads,w=s.writes;
        CHECK(s.controller.disallowDfCstate({0,1,1},s.result)==sc::Status::InvalidArgument);
        CHECK(s.reads==r&&s.writes==w);CHECK(s.disallowDf()==sc::Status::Ok);
        CHECK(s.controller.allowDfCstateAfterQuiescence({0,1,1},s.result)==sc::Status::InvalidArgument);
        CHECK(s.allowDf()==sc::Status::Ok);CHECK(!s.controller.retentionRequired());
        CHECK(s.inhibit()==sc::Status::InvalidState);CHECK(!s.forbidden);}
    {Model s(firmware);CHECK(s.bind()==sc::Status::Ok);
        CHECK(s.controller.requestSoftRange(sc::Clock::Soc,1800,1800,poll,s.result)==sc::Status::Ok);
        CHECK(s.disallowDf()==sc::Status::InvalidState);CHECK(s.automatic()==sc::Status::Ok);
        CHECK(s.disallowDf()==sc::Status::InvalidState);CHECK(!s.forbidden);}
    for(bool releasing:{false,true}){
        const auto prepare=[&](Model& s){CHECK(s.bind()==sc::Status::Ok);
            CHECK(s.inhibit()==sc::Status::Ok);if(releasing)CHECK(s.disallowDf()==sc::Status::Ok);};
        const auto operation=[&](Model& s){return releasing?s.allowDf():s.disallowDf();};
        Model good(firmware);prepare(good);
        const auto reads=good.reads,writes=good.writes,valids=good.valids,clocks=good.clocks;
        const auto commands=uint32_t(good.commands.size());CHECK(operation(good)==sc::Status::Ok);
        const auto failure=[&](Model& s){
            const auto& j=s.controller.journal().dfCstate;
            CHECK(s.controller.faulted()&&!s.controller.ready()&&s.faults==1&&!s.forbidden);
            CHECK(!j.released);CHECK(s.controller.dfCstateRetentionRequired()==j.disallowAttempted);
            CHECK(s.controller.deepSleepRetentionRequired());if(releasing)CHECK(s.controller.dfCstateRetentionRequired());
            const auto r=s.reads,w=s.writes;CHECK(s.disallowDf()==sc::Status::InvalidState);
            CHECK(s.allowDf()==sc::Status::InvalidState);CHECK(s.restore()==sc::Status::InvalidState);
            CHECK(s.reads==r&&s.writes==w);
        };
        for(uint32_t n=reads+1;n<=good.reads;++n){Model s(firmware);prepare(s);s.failRead=n;
            CHECK(operation(s)!=sc::Status::Ok);CHECK(s.reads==n);failure(s);}
        for(uint32_t n=writes+1;n<=good.writes;++n){Model s(firmware);prepare(s);s.failWrite=n;
            CHECK(operation(s)!=sc::Status::Ok);CHECK(s.writes==n);failure(s);}
        for(uint32_t n=valids+1;n<=good.valids;++n){Model s(firmware);prepare(s);s.revokeValid=n;
            CHECK(operation(s)==sc::Status::OwnerUnavailable);failure(s);}
        for(uint32_t n=reads+1;n<=good.reads;++n){Model s(firmware);prepare(s);s.revokeAfterRead=n;
            CHECK(operation(s)==sc::Status::OwnerUnavailable);CHECK(s.reads==n);failure(s);}
        for(uint32_t n=writes+1;n<=good.writes;++n){Model s(firmware);prepare(s);s.revokeAfterWrite=n;
            CHECK(operation(s)==sc::Status::OwnerUnavailable);CHECK(s.writes==n);failure(s);}
        for(uint32_t n=clocks+2;n<=good.clocks;++n){Model s(firmware);prepare(s);s.deadlineAt=n;
            CHECK(operation(s)==sc::Status::Timeout);failure(s);}
        for(uint32_t n=clocks+2;n<=good.clocks;++n){Model s(firmware);prepare(s);s.backwardsAt=n;
            CHECK(operation(s)==sc::Status::ClockWentBackwards);failure(s);}
        const uint32_t bad[]={0,0xff,0xfe,0xfd,0xfc,7,UINT32_MAX};
        for(uint32_t n=commands+1;n<=good.commands.size();++n)for(auto code:bad){Model s(firmware);prepare(s);
            s.badCommand=n;s.badResponse=code;CHECK(operation(s)!=sc::Status::Ok);failure(s);}
        for(uint32_t n=1;n<=8;++n){Model s(firmware);prepare(s);s.changeFeatureAt=s.featureReads+n;
            CHECK(operation(s)==sc::Status::ChangedFeatures);failure(s);}
    }
}
}
int main(){smu_test::FirmwareFixture fixture;gfxfw::State firmware;
    CHECK(fixture.ready(firmware));if(failures)return 1;
    success(firmware);preflight(firmware);semanticFailures(firmware);faultMatrix(firmware);automaticPolicy(firmware);
    reconciliation(firmware);deepSleepPolicy(firmware);dfCstatePolicy(firmware);
    std::printf("%u checks, %u failures\n",checks,failures);return failures?1:0;}
