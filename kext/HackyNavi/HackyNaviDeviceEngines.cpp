// SPDX-License-Identifier: MIT
#include "HackyNaviDevice.hpp"
#include "HackyNaviAcceleratorProtocol.hpp"
#include "HackyNaviLifecycleController.hpp"
#include "HackyNaviClientSession.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <kern/debug.h>

namespace {
bool observationNumber(OSDictionary* out,const char* key,uint64_t value,unsigned bits=32) {
    auto* number=OSNumber::withNumber(value,bits);if(!number)return false;
    const bool result=out->setObject(key,number);number->release();return result;
}

}
IOReturn HackyNaviDevice::createClientSession(HackyNaviClientSession*& out,
    IOService* accelerator,uint64_t acceleratorGeneration) {
    out=nullptr;if(!lock_)return kIOReturnNotReady;
    if(bool(accelerator)!=bool(acceleratorGeneration))return kIOReturnBadArgument;
    IOLockLock(lock_);
    auto* group=engineMode_&&!captureRevoked()&&!stopping_&&powerAvailable_&&opened_?functionGroup_:nullptr;
    auto* lifecycle=group?lifecycle_:nullptr;
    if(group)group->retain();if(lifecycle)lifecycle->retain();
    IOLockUnlock(lock_);
    IOReturn result=kIOReturnNotReady;
    HackyNaviClientSession* candidate=nullptr;
    if(group&&lifecycle) {
        HackyNaviFunctionGroup::Operation scope;result=group->beginOperation(scope);
        if(result==kIOReturnSuccess) {
            IOLockLock(lock_);
            const bool current=group==functionGroup_&&lifecycle==lifecycle_&&!captureRevoked()&&
                !stopping_&&powerAvailable_&&opened_&&!isInactive()&&scope.matchesGraphics(pci_,this)&&
                (!accelerator||(acceleratorReady_&&accelerator_==accelerator&&
                    acceleratorGeneration_==acceleratorGeneration&&scope.generation()==acceleratorGeneration&&
                    accelerator->getProvider()==this&&!accelerator->isInactive()));
            IOLockUnlock(lock_);
            result=current?lifecycle->createClientSession(group,scope,candidate):kIOReturnOffline;
            if(result==kIOReturnSuccess) {
                IOLockLock(lock_);
                const bool stillCurrent=group==functionGroup_&&lifecycle==lifecycle_&&!captureRevoked()&&
                    !stopping_&&powerAvailable_&&opened_&&!isInactive()&&scope.matchesGraphics(pci_,this)&&
                    (!accelerator||(acceleratorReady_&&accelerator_==accelerator&&
                        acceleratorGeneration_==acceleratorGeneration&&scope.generation()==acceleratorGeneration&&
                        accelerator->getProvider()==this&&!accelerator->isInactive()));
                IOLockUnlock(lock_);
                if(stillCurrent){out=candidate;candidate=nullptr;}
                else result=kIOReturnOffline;
            }
        }
    }
    // close/free can reacquire the normal Operation and GART. Never unwind a
    // rejected actual session while its creation Operation remains in scope.
    if(candidate){(void)candidate->close();candidate->release();}
    if(lifecycle)lifecycle->release();if(group)group->release();
    return result;
}

IOReturn HackyNaviDevice::publishAcceleratorReadiness(HackyNaviFunctionGroup::Operation& scope) {
    // Only reached after real lifecycle start, including GPU verification.
    // The registry property drives later matching, never grants an epoch.
    IOLockLock(lock_);
    const bool ready=!acceleratorReady_&&!accelerator_&&engineMode_&&lifecycle_&&
        !captureRevoked()&&!stopping_&&powerAvailable_&&opened_&&!isInactive()&&
        scope.belongsTo(functionGroup_)&&scope.matchesGraphics(pci_,this);
    const bool published=ready&&setProperty(hackynavi::acceleratorbridge::kReady,kOSBooleanTrue);
    if(published)acceleratorReady_=true;
    IOLockUnlock(lock_);
    return published?kIOReturnSuccess:(ready?kIOReturnNoMemory:kIOReturnOffline);
}

IOReturn HackyNaviDevice::publishAccelerator(IOService* child,uint64_t& generation) {
    generation=0;
    IOLockLock(lock_);
    auto* group=acceleratorReady_&&engineMode_&&lifecycle_&&!captureRevoked()&&
        !stopping_&&powerAvailable_&&opened_&&!isInactive()?functionGroup_:nullptr;
    if(group)group->retain();
    IOLockUnlock(lock_);
    if(!group)return kIOReturnNotReady;
    IOReturn result;
    {
        HackyNaviFunctionGroup::Operation scope;
        result=group->beginOperation(scope);
        if(result==kIOReturnSuccess) {
            IOLockLock(lock_);
            const bool install=acceleratorReady_&&!accelerator_&&group==functionGroup_&&
                engineMode_&&lifecycle_&&!captureRevoked()&&!stopping_&&powerAvailable_&&
                opened_&&!isInactive()&&scope.matchesGraphics(pci_,this)&&
                child->getProvider()==this&&!child->isInactive();
            if(install){child->retain();accelerator_=child;acceleratorGeneration_=scope.generation();}
            IOLockUnlock(lock_);
            if(!install)result=kIOReturnNotReady;
            else if(!scope.valid())result=kIOReturnOffline;
            else {
                // Keep the normal Operation through actual publication. No
                // Device mutex spans IOService work. Explicit asynchronous
                // matching cannot reenter this nonrecursive gate on our thread.
                // Publication is the commit point. A concurrent revocation
                // can make the new service immediately unavailable, but the
                // pending retirement must see and terminate this retained
                // child after our gate drains. Do not return startup failure
                // after publishing or race the child's startup unwind.
                generation=scope.generation();
                child->registerService(kIOServiceAsynchronous);
            }
            if(install&&result!=kIOReturnSuccess) {
                IOLockLock(lock_);
                const bool owned=accelerator_==child;
                if(owned){accelerator_=nullptr;acceleratorGeneration_=0;}
                IOLockUnlock(lock_);
                if(owned)child->release();
            }
        }
    }
    group->release();return result;
}

IOReturn HackyNaviDevice::callPlatformFunction(const OSSymbol* name,bool wait,
    void* argument,void* second,void* third,void* fourth) {
    using namespace hackynavi::acceleratorbridge;
    const bool publish=name&&name->isEqualTo(kPublish);
    const bool withdraw=name&&name->isEqualTo(kWithdraw);
    const bool open=name&&name->isEqualTo(kOpen);
    if(!publish&&!withdraw&&!open)
        return IOService::callPlatformFunction(name,wait,argument,second,third,fourth);
    if(!lock_||!argument||second||third||fourth)return kIOReturnBadArgument;
    auto& request=*static_cast<Request*>(argument);
    if(request.bytes!=(open?sizeof(OpenRequest):sizeof(Request))||request.version!=kVersion)
        return kIOReturnBadArgument;
    request.generation=0;
    if(!request.child||
        request.ownerRegistryID!=getRegistryEntryID()||request.child->getProvider()!=this||
        strcmp(request.child->getMetaClass()->getClassName(),"HackyNaviAccelerator")||
        !request.child->metaCast("IOAccelerator"))return kIOReturnBadArgument;
    if(publish)return publishAccelerator(request.child,request.generation);
    if(open) {
        auto& native=*static_cast<OpenRequest*>(argument);
        native.client=nullptr;
        if(native.reserved)return kIOReturnBadArgument;
        // No normal Operation is held here. The actual client start path
        // acquires one and checks this private publication generation there.
        return newAcceleratorUserClient(request.child,native.task,native.securityID,
            native.type,native.properties,&native.client,request.generation);
    }
    // stop() may be invoked by retirement. Waiting for another normal
    // Operation here would deadlock the drain; pointer identity is enough
    // to release a CPU/service reference and cannot access hardware.
    IOLockLock(lock_);
    const bool owned=accelerator_==request.child;
    if(owned){accelerator_=nullptr;acceleratorGeneration_=0;}
    IOLockUnlock(lock_);
    if(owned)request.child->release();
    return owned?kIOReturnSuccess:kIOReturnNotFound;
}

void HackyNaviDevice::retireAccelerator() {
    if(!lock_)return;
    IOLockLock(lock_);
    auto* child=accelerator_;accelerator_=nullptr;
    acceleratorGeneration_=0;
    acceleratorReady_=false;
    setProperty(hackynavi::acceleratorbridge::kReady,kOSBooleanFalse);
    IOLockUnlock(lock_);
    if(!child)return;
    // Asynchronous required termination withdraws Metal discovery and sends
    // actual service-interest notifications. Do not wait here: a notification
    // consumer can itself close a native client while engine retirement owns
    // the function-group drain. Native operations are already revoked by that
    // gate; retaining the child's port never grants access to the old epoch.
    if(!child->isInactive())child->terminate(kIOServiceRequired);
    child->release();
}
namespace {
bool observationString(OSDictionary* out,const char* key,const char* value) {
    auto* string=OSString::withCString(value);if(!string)return false;
    const bool result=out->setObject(key,string);string->release();return result;
}
bool observationBool(OSDictionary* out,const char* key,bool value) {
    return out->setObject(key,value?kOSBooleanTrue:kOSBooleanFalse);
}
bool functionGroupSnapshot(OSDictionary* out,const HackyNaviFunctionGroup::Snapshot& snapshot) {
    bool ok=observationNumber(out,"GroupState",unsigned(snapshot.state))&&
        observationNumber(out,"GroupInstance",snapshot.instance,64)&&
        observationNumber(out,"SnapshotGeneration",snapshot.generation,64);
    for(unsigned i=0;i<2;++i) {
        auto* function=OSDictionary::withCapacity(18);
        if(!function) {ok=false;continue;}
        const auto& f=snapshot.functions[i];const auto& id=f.identity;
        const bool known=id.vendorDevice!=0;
        bool saved=observationBool(function,"LeaseSnapshotAvailable",known);
        if(known)saved=observationNumber(function,"VendorDevice",id.vendorDevice)&&
            observationNumber(function,"RevisionClass",id.revisionClass)&&observationNumber(function,"Subsystem",id.subsystem)&&
            observationNumber(function,"Bus",id.bus)&&observationNumber(function,"Device",id.device)&&
            observationNumber(function,"Function",id.function)&&observationNumber(function,"LastPublishedResult",uint32_t(f.result))&&
            observationBool(function,"Attached",f.attached)&&observationBool(function,"Opened",f.opened)&&
            observationBool(function,"OwnsOpen",f.ownsOpen)&&observationBool(function,"PmInitialized",f.pmInitialized)&&
            observationBool(function,"PmRegistered",f.pmRegistered)&&observationBool(function,"Powered",f.powered)&&
            observationBool(function,"Terminal",f.terminal)&&observationBool(function,"AdmissionRevoked",f.admissionRevoked)&&saved;
        ok=saved&&out->setObject(i?"AudioFunction":"GraphicsFunction",function)&&ok;function->release();
    }
    return ok;
}
const char* creationStageString(HackyNaviFunctionGroup::CreationStage stage) {
    using Stage=HackyNaviFunctionGroup::CreationStage;
    switch(stage) {
#define NAME(x) case Stage::x:return #x
    NAME(Allocation);NAME(Admission);NAME(FindPair);NAME(GraphicsLease);NAME(AudioLease);
    NAME(AudioBegin);NAME(GraphicsBegin);NAME(Topology);NAME(Complete);
#undef NAME
    }return "Unknown";
}
void publishFunctionGroupCreation(IOService* owner,const HackyNaviFunctionGroup::CreationObservation& p) {
    auto* out=OSDictionary::withCapacity(12);
    bool ok=out&&observationBool(out,"HistoricalEvidenceOnly",true)&&
        observationBool(out,"CurrentHardwareAuthority",false)&&observationNumber(out,"FormatVersion",1)&&
        observationString(out,"Stage",creationStageString(p.stage))&&observationNumber(out,"Result",uint32_t(p.result))&&
        observationBool(out,"SnapshotCaptured",p.snapshotCaptured)&&observationBool(out,"CapturedBeforeFactoryCleanup",p.snapshotCaptured);
    if(out&&p.snapshotCaptured)ok=functionGroupSnapshot(out,p.snapshot)&&ok;
    if(ok)ok=owner->setProperty("FunctionGroupCreation",out);
    owner->setProperty("FunctionGroupCreationEvidenceTruncated",ok?kOSBooleanFalse:kOSBooleanTrue);
    if(out)out->release();
}
void publishFunctionGroupAdmission(IOService* owner,HackyNaviFunctionGroup* group,IOReturn result,uint64_t generation) {
    HackyNaviFunctionGroup::Snapshot snapshot={};group->snapshot(snapshot);
    auto* out=OSDictionary::withCapacity(12);
    bool ok=out&&observationBool(out,"HistoricalEvidenceOnly",true)&&observationBool(out,"CurrentHardwareAuthority",false)&&
        observationNumber(out,"FormatVersion",1)&&observationNumber(out,"BeginOperationResult",uint32_t(result))&&
        observationBool(out,"OperationIssued",result==kIOReturnSuccess)&&observationNumber(out,"IssuedGeneration",generation,64)&&
        functionGroupSnapshot(out,snapshot);
    if(ok)ok=owner->setProperty("FunctionGroupAdmission",out);
    owner->setProperty("FunctionGroupAdmissionEvidenceTruncated",ok?kOSBooleanFalse:kOSBooleanTrue);
    if(out)out->release();
}
bool powerObservation(OSDictionary* out,const hackynavi::bootdisplay::PowerProbe& p) {
    return observationNumber(out,"PowerProbePipe",p.pipe)&&
        observationNumber(out,"PowerReadsSucceededMask",p.readsSucceededMask)&&
        observationNumber(out,"PowerStatusBefore",p.statusBefore)&&
        observationNumber(out,"PowerConfig",p.config)&&observationNumber(out,"PowerStatusAfter",p.statusAfter);
}
bool consoleSample(OSDictionary* out,const char* key,const hackynavi::eficonsole::Input& p,
    uint64_t rawBase,uint64_t display) {
    auto* sample=OSDictionary::withCapacity(9);if(!sample)return false;
    const bool valid=observationNumber(sample,"RawBase",rawBase,64)&&
        observationNumber(sample,"InputAddress",p.physicalBase,64)&&
        observationNumber(sample,"Display",display,64)&&observationNumber(sample,"RowBytes",p.rowBytes,64)&&
        observationNumber(sample,"Width",p.width,64)&&observationNumber(sample,"Height",p.height,64)&&
        observationNumber(sample,"Depth",p.depth,64)&&observationNumber(sample,"Offset",p.offset,64)&&
        observationNumber(sample,"Length",p.length,64)&&observationNumber(sample,"Rotation",p.rotation,64)&&
        out->setObject(key,sample);
    sample->release();return valid;
}
bool consoleObservation(OSDictionary* out,const char* key,const hackynavi::bootconsole::Capture& p,bool preflight) {
    auto* report=OSDictionary::withCapacity(12);if(!report)return false;
    bool valid=observationBool(report,"NegativePreflightOnly",preflight)&&
        observationNumber(report,"Result",uint32_t(p.result))&&
        observationNumber(report,"FirstQuery",uint32_t(p.firstQuery))&&
        observationNumber(report,"SecondQuery",uint32_t(p.secondQuery))&&
        observationBool(report,"Stable",p.stable)&&observationNumber(report,"AddressKind",uint32_t(p.addressKind))&&
        observationString(report,"RangeStatus",hackynavi::eficonsole::statusString(p.reservationStatus));
    if(p.firstQuery==kIOReturnSuccess)valid=consoleSample(report,"First",p.first,p.firstRawBase,p.firstDisplay)&&valid;
    if(p.secondQuery==kIOReturnSuccess)valid=consoleSample(report,"Second",p.second,p.secondRawBase,p.secondDisplay)&&valid;
    valid=valid&&out->setObject(key,report);report->release();return valid;
}
bool displayStopObservation(OSDictionary* out,const char* key,
    const HackyNaviEngineController::DisplayStopObservation& p) {
    if(!p.attempted)return true;
    auto* report=OSDictionary::withCapacity(11);if(!report)return false;
    const bool valid=observationNumber(report,"Result",uint32_t(p.result))&&
        observationNumber(report,"ElapsedUs",p.elapsedUs,64)&&
        observationBool(report,"ConsumersStopped",p.consumersStopped)&&
        observationBool(report,"PhysicalIdleChecked",p.physicalIdleChecked)&&
        observationBool(report,"FirmwareStopped",p.firmwareStopped)&&
        observationBool(report,"RuntimeRetained",p.runtimeRetained)&&
        observationBool(report,"PolicyRetained",p.policyRetained)&&
        observationBool(report,"LeaseReleased",p.leaseReleased)&&
        observationBool(report,"ClockRetirementAttempted",p.clockRetirementAttempted)&&
        observationNumber(report,"ClockRetirementResult",uint32_t(p.clockRetirementResult))&&
        out->setObject(key,report);
    report->release();return valid;
}
bool displayTaRetirementObservation(OSDictionary* out,
    const HackyNaviEngineController::DisplayTaRetirementObservation& p) {
    if(!p.attempted)return true;
    auto* report=OSDictionary::withCapacity(15);if(!report)return false;
    const bool valid=observationNumber(report,"Result",uint32_t(p.result))&&
        observationNumber(report,"Generation",p.generation,64)&&
        observationNumber(report,"ElapsedUs",p.elapsedUs,64)&&
        observationNumber(report,"ProtocolStatus",p.protocolStatus)&&
        observationNumber(report,"Command",p.command)&&
        observationNumber(report,"ApplicationStatus",p.applicationStatus)&&
        observationNumber(report,"PspStatus",p.pspStatus)&&
        observationNumber(report,"TaPspStatus",p.taPspStatus)&&
        observationBool(report,"ProtocolAttempted",p.protocolAttempted)&&
        observationBool(report,"ApplicationAttempted",p.applicationAttempted)&&
        observationNumber(report,"AuthenticationsDestroyed",p.authenticationsDestroyed)&&
        observationNumber(report,"TopologiesRemoved",p.topologiesRemoved)&&
        observationNumber(report,"ApplicationsUnloaded",p.applicationsUnloaded)&&
        observationBool(report,"ProtocolRetired",p.protocolRetired)&&
        observationBool(report,"RetentionRequired",p.retentionRequired)&&
        out->setObject("DisplayTrustedApplicationRetirement",report);
    report->release();return valid;
}
bool engineObservation(OSDictionary* out,const HackyNaviEngineController::Observation& p) {
    bool valid=observationBool(out,"ControlWritesStarted",p.controlWritesStarted)&&
        observationBool(out,"DisplayPreflightAttempted",p.preflightDisplay.attempted)&&
        observationBool(out,"ConsolePreflightAttempted",p.preflightConsoleAttempted)&&
        observationBool(out,"BootScanoutAttempted",p.scanoutAttempted)&&
        observationBool(out,"BootConsoleAttempted",p.consoleAttempted)&&
        observationBool(out,"CPVerificationAttempted",p.cpAttempted)&&
        observationBool(out,"ShaderVerificationAttempted",p.shaderAttempted);
    if(p.backend.attempted) {
        const auto& b=p.backend;const auto& d=b.report;
        auto* report=OSDictionary::withCapacity(30);if(!report)return false;
        bool published=observationBool(report,"ObservationOnly",true)&&observationBool(report,"Valid",b.valid)&&
            observationNumber(report,"Result",uint32_t(d.result))&&observationNumber(report,"AccessFailure",uint32_t(d.failure))&&
            observationString(report,"Status",hackynavi::bootbackend::statusString(d.classifier.status))&&
            observationBool(report,"ClassifierAttempted",d.classifierAttempted)&&
            observationNumber(report,"ElapsedNanoseconds",d.elapsedNanoseconds,64)&&
            observationNumber(report,"PciChecks",d.pciChecks)&&observationNumber(report,"MmioReads",d.mmioReads)&&
            observationNumber(report,"LastMmioRegister",d.lastMmioRegister)&&observationNumber(report,"LastMmioValue",d.lastMmioValue)&&
            observationBool(report,"LastMmioReadPerformed",d.lastMmioReadPerformed)&&
            observationNumber(report,"CaptureReads",d.classifier.reads)&&
            observationNumber(report,"CapturePass",d.classifier.pass)&&observationNumber(report,"CaptureBank",uint32_t(d.classifier.bank))&&
            observationNumber(report,"CaptureInstance",d.classifier.instance)&&observationNumber(report,"CaptureRegister",d.classifier.reg);
        if(b.valid)published=observationNumber(report,"AtomPath",b.path)&&observationNumber(report,"Backend",b.backend)&&
            observationNumber(report,"Frontend",b.frontend)&&observationNumber(report,"Timing",b.timing)&&
            observationNumber(report,"Opp",b.opp)&&observationNumber(report,"Mpcc",b.mpcc)&&observationNumber(report,"Dpp",b.dpp)&&
            observationNumber(report,"BitsPerComponent",b.bitsPerComponent)&&observationBool(report,"Scrambled",b.scrambled)&&
            observationBool(report,"QuarterRate",b.quarterRate)&&published;
        valid=published&&out->setObject("InheritedBackend",report)&&valid;report->release();
    }
    if(p.preflightDisplay.attempted) {
        const auto& d=p.preflightDisplay;auto* report=OSDictionary::withCapacity(12);if(!report)return false;
        valid=observationBool(report,"NegativePreflightOnly",true)&&
            observationString(report,"Status",hackynavi::bootdisplay::statusString(d.status))&&
            observationNumber(report,"Reads",d.reads)&&observationNumber(report,"FailedPipe",d.failedPipe)&&
            observationNumber(report,"LastRegister",d.lastRegister)&&observationNumber(report,"LastValue",d.lastValue)&&
            observationBool(report,"LastReadSucceeded",d.lastReadSucceeded)&&powerObservation(report,d.lastPowerProbe)&&
            out->setObject("BootDisplayPreflight",report)&&valid;report->release();
    }
    if(p.preflightConsoleAttempted)valid=consoleObservation(out,"BootConsolePreflight",p.preflightConsole,true)&&valid;
    if(p.consoleAttempted)valid=consoleObservation(out,"BootConsole",p.console,false)&&valid;
    if(p.scanoutAttempted) {
        const auto& d=p.scanout;auto* report=OSDictionary::withCapacity(17);if(!report)return false;
        valid=observationString(report,"Status",hackynavi::bootdisplaystop::statusString(d.status))&&
            observationString(report,"CaptureStatus",hackynavi::bootdisplay::statusString(d.captureStatus))&&
            observationNumber(report,"Reads",d.reads)&&observationNumber(report,"Writes",d.writes)&&
            observationNumber(report,"Polls",d.polls)&&observationNumber(report,"Pipe",d.pipe)&&
            observationNumber(report,"LastRegister",d.lastRegister)&&observationNumber(report,"LastValue",d.lastValue)&&
            observationBool(report,"LastReadSucceeded",d.lastReadSucceeded)&&powerObservation(report,d.lastPowerProbe)&&
            observationBool(report,"WritesMayHaveOccurred",d.writesMayHaveOccurred)&&
            observationBool(report,"RetentionRequired",d.retentionRequired)&&
            out->setObject("BootScanout",report)&&valid;report->release();
    }
    if(p.cpAttempted)valid=observationBool(out,"CPVerificationCompleted",p.cp.completed)&&
        observationNumber(out,"CPWordsCompared",p.cp.wordsCompared)&&
        observationNumber(out,"CPFirstMismatch",p.cp.firstMismatch)&&
        observationNumber(out,"CPExpected",p.cp.expected)&&observationNumber(out,"CPObserved",p.cp.observed)&&valid;
    if(p.shaderAttempted)valid=observationBool(out,"ShaderVerificationCompleted",p.shader.completed)&&
        observationNumber(out,"ShaderDispatchesCompleted",p.shader.dispatchesCompleted)&&
        observationNumber(out,"ShaderWordsCompared",p.shader.wordsCompared)&&
        observationNumber(out,"ShaderFirstMismatch",p.shader.firstMismatch)&&
        observationNumber(out,"ShaderExpected",p.shader.expected)&&observationNumber(out,"ShaderObserved",p.shader.observed)&&valid;
    return displayStopObservation(out,"DisplayConsumerRetirement",p.stopConsumers)&&
        displayStopObservation(out,"DisplayFirmwareRetirement",p.stopFirmware)&&
        displayTaRetirementObservation(out,p.displayTaRetirement)&&valid;
}
}

void HackyNaviDevice::capturePreEngineEvidence(HackyNaviFunctionGroup::Operation& scope) {
    // Device lock and the actual paired-function normal gate are already held.
    // This bounded selector/read acquisition runs before recovery preparation
    // or startup controls. Engine later takes fresh, separately admitted inputs.
    clearSnapshotLocked();
    removeProperty("LastCaptureObservation");removeProperty("PreEngineCapture");
    engineCaptureScope_=&scope;
    const auto generation=powerGeneration_;
    setProperty("SnapshotStatus","CapturingBeforeEngineAttempt");
    snapshotValid_=captureEpochValid()&&captureDiscovery();
    // Console metadata is independent of display address classification. A
    // valid live memory/BAR layout permits observing it even after a display
    // semantic rejection; these tuples never authorize a reservation here.
    auto* console=OSDictionary::withCapacity(2);
    if(console) {
        const bool attempted=captureEpochValid()&&memoryLayout_.vramBytes&&memoryLayout_.bar0Bytes;
        hackynavi::bootconsole::Capture observed{};
        observed.result=observed.firstQuery=observed.secondQuery=kIOReturnNotReady;
        observed.reservationStatus=hackynavi::eficonsole::Status::InvalidMemoryLayout;
        if(attempted)(void)hackynavi::bootconsole::capture(memoryLayout_,observed);
        const bool current=captureEpochValid();
        bool published=observationBool(console,"CaptureAttempted",attempted)&&
            observationBool(console,"OperationStillCurrent",current)&&
            observationBool(console,"CurrentHardwareAuthority",false)&&
            consoleObservation(console,"Observation",observed,true)&&setProperty("BootConsole",console);
        snapshotValid_=published&&snapshotValid_;console->release();
    } else snapshotValid_=false;
    const bool current=captureEpochValid()&&generation==powerGeneration_;
    snapshotValid_=snapshotValid_&&current;
    saveCaptureObservationLocked(generation,!current);
    if(auto* historical=getProperty("LastCaptureObservation"))setProperty("PreEngineCapture",historical);
    // Immutable historical records survive, but their live copies must never
    // remain advertised after Engine changes hardware. Keep only the actual
    // assigned mapping needed by the separately retained lifecycle owner.
    clearSnapshotLocked();engineCaptureScope_=nullptr;
    setProperty("SnapshotStatus","HistoricalPreEngineCaptureOnly");
}

void HackyNaviDevice::functionGroupChanged(IOService* owner,HackyNaviFunctionGroup* group,
    HackyNaviFunctionGroup::Event,uint8_t) {
    auto* self=OSDynamicCast(HackyNaviDevice,owner);if(!self||!self->lock_)return;
    IOLockLock(self->lock_);
    // Acquired can be delivered before withPrimary returns. Only the creator
    // installs its owned reference; this borrowed callback merely wakes it.
    if(!self->functionGroup_||self->functionGroup_==group) {
        if(self->engineNotificationSerial_==UINT64_MAX)
            panic("HackyNavi: engine notification serial exhausted");
        ++self->engineNotificationSerial_;
        self->queueCaptureLocked();
    }
    IOLockUnlock(self->lock_);
}

void HackyNaviDevice::engineWorker() {
    IOLockLock(lock_);
    const uint64_t observed=engineNotificationSerial_;
    const bool ready=!captureRevoked()&&pmInitialized_&&powerAvailable_&&!stopping_&&
        !isInactive()&&opened_&&!engineAttempted_;
    auto* group=ready?functionGroup_:nullptr;if(group)group->retain();
    auto* device=ready&&!group&&!groupCreationAttempted_?pci_:nullptr;
    if(device){device->retain();groupCreationAttempted_=true;}
    IOLockUnlock(lock_);
    if(device) {
        IOReturn result=kIOReturnError;
        HackyNaviFunctionGroup::CreationObservation creation={};
        auto* candidate=HackyNaviFunctionGroup::withPrimary(device,this,functionGroupChanged,result,&creation);
        publishFunctionGroupCreation(this,creation);
        device->release();
        IOLockLock(lock_);
        if(candidate&&!functionGroup_&&!captureRevoked()&&pmInitialized_&&powerAvailable_&&
            !stopping_&&!isInactive()&&opened_) {
            functionGroup_=candidate;candidate=nullptr;
            group=functionGroup_;group->retain();
            // A wake delivered during creation cannot be lost to pending=true.
            if(engineNotificationSerial_==UINT64_MAX)
                panic("HackyNavi: engine notification serial exhausted");
            ++engineNotificationSerial_;
        }
        setProperty("FunctionGroupResult",UInt64(uint32_t(result)),32);
        setProperty("EngineStatus",group?"WaitingForPairedPower":"FunctionGroupUnavailable");
        IOLockUnlock(lock_);
        if(candidate){candidate->shutdown();candidate->release();}
    }
    if(group) {
        const IOReturn result=runEngineAttempt(group);
        // The startup Operation has left scope. A late accelerator can now
        // take its own normal Operation without recursive gate acquisition.
        // Matching can race retirement: its private readiness/epoch checks
        // reject any stale registry hint before child publication.
        if(result==kIOReturnSuccess)registerService(kIOServiceAsynchronous);
        // runEngineAttempt's lexical normal scope is already destroyed. A
        // failed activation must retire now, not wait for a later PM event.
        // The group admits this reason only from its registering worker.
        if(result!=kIOReturnSuccess&&group->retirementRegistered()) {
            const IOReturn retired=group->retirePrimary(this,HackyNaviFunctionGroup::RetirementReason::OwnerFailure);
            if(retired!=kIOReturnSuccess)
                panic("HackyNavi: failed activation could not join retirement result=0x%x",unsigned(retired));
        }
        group->release();
    }
    IOLockLock(lock_);
    capturePending_=false;
    // PM-on may have arrived after beginOperation failed while this call was
    // pending. Requeue only a new notification, never a poll/spin loop.
    if(engineNotificationSerial_!=observed)queueCaptureLocked();
    IOLockUnlock(lock_);
}

IOReturn HackyNaviDevice::runEngineAttempt(HackyNaviFunctionGroup* group) {
    HackyNaviFunctionGroup::Operation scope;
    IOReturn result=group->beginOperation(scope);
    publishFunctionGroupAdmission(this,group,result,scope.generation());
    if(result!=kIOReturnSuccess)return result;
    IOLockLock(lock_);
    if(group!=functionGroup_||captureRevoked()||stopping_||!powerAvailable_||
        !pmInitialized_||isInactive()||!opened_||engineAttempted_||!scope.matchesGraphics(pci_,this)) {
        IOLockUnlock(lock_);return kIOReturnOffline;
    }
    engineAttempted_=true; // One cold attempt; no rearm following any failure.
    setProperty("EngineStatus","CreatingLifecycle");
    setProperty("EngineOperationGeneration",UInt64(scope.generation()),64);
    if(!mapRegisters()||!scope.matchesGraphics(pci_,this)) {
        setProperty("EngineStatus","BAR5MappingFailed");
        IOLockUnlock(lock_);return kIOReturnOffline;
    }
    capturePreEngineEvidence(scope);
    if(!scope.matchesGraphics(pci_,this)||captureRevoked()) {
        setProperty("EngineStatus","PreEngineCaptureRevoked");
        IOLockUnlock(lock_);return kIOReturnOffline;
    }
    auto* device=pci_;device->retain();
    auto* map=registerMap_;map->retain();
    IOLockUnlock(lock_);
    // These factories allocate CPU state and bind actual assigned resources;
    // recovery's selector writes happen only after handler publication below.
    auto* lifecycle=HackyNaviLifecycleController::withDevice(device,this,map,result);
    map->release();device->release();
    if(!lifecycle) {
        IOLockLock(lock_);
        setProperty("EngineStatus","LifecycleCreationFailed");
        setProperty("EngineAttemptResult",UInt64(uint32_t(result)),32);
        IOLockUnlock(lock_);return result;
    }
    IOLockLock(lock_);
    const bool install=group==functionGroup_&&!lifecycle_&&!captureRevoked()&&!stopping_&&
        powerAvailable_&&pmInitialized_&&opened_&&!isInactive()&&scope.matchesGraphics(pci_,this);
    if(install){lifecycle_=lifecycle;lifecycle->retain();}
    IOLockUnlock(lock_);
    if(!install){lifecycle->release();return kIOReturnOffline;}
    // PM retirement can see the fully constructed owner before registration.
    // Primary and paired callbacks revoke/drain a racing registration attempt.
    result=group->registerRetirementHandler(scope,retireEngineOwner,5000000);
    if(result==kIOReturnSuccess)result=lifecycle->prepareRecovery(scope);
    if(result==kIOReturnSuccess)result=lifecycle->start(scope);
    if(result==kIOReturnSuccess&&!scope.valid())result=kIOReturnOffline;
    if(result==kIOReturnSuccess)result=publishAcceleratorReadiness(scope);
    publishEngineObservation(lifecycle,result,false);
    lifecycle->release();
    return result;
}

void HackyNaviDevice::retireEngines(HackyNaviFunctionGroup::RetirementReason reason) {
    if(!lock_)return;
    IOLockLock(lock_);
    auto* group=functionGroup_;if(group)group->retain();
    IOLockUnlock(lock_);
    if(!group)return;
    // Neither the Device mutex nor a controller mutex may be held across this
    // gate drain/worker wait. Returning authorizes the primary callback's ACK.
    const IOReturn result=group->retirePrimary(this,reason);
    group->release();
    if(result!=kIOReturnSuccess)
        panic("HackyNavi: primary retirement rejected result=0x%x",unsigned(result));
}

IOReturn HackyNaviDevice::retireEngineOwner(IOService* owner,HackyNaviFunctionGroup::RetirementScope& scope) {
    auto* self=OSDynamicCast(HackyNaviDevice,owner);if(!self||!self->lock_)return kIOReturnBadArgument;
    self->retireAccelerator();
    IOLockLock(self->lock_);
    auto* lifecycle=self->lifecycle_;if(lifecycle)lifecycle->retain();
    IOLockUnlock(self->lock_);
    if(!lifecycle)return kIOReturnNotReady;
    IOReturn result=lifecycle->retire(scope);
    HackyNaviLifecycleController::Snapshot snapshot={};lifecycle->snapshot(snapshot);
    if(result==kIOReturnSuccess&&(snapshot.engineRetained||snapshot.resetRetained||snapshot.atomRetained))
        result=kIOReturnNotReady;
    self->publishEngineObservation(lifecycle,result,true);
    lifecycle->release();return result;
}

void HackyNaviDevice::publishEngineObservation(HackyNaviLifecycleController* lifecycle,IOReturn result,bool retired) {
    HackyNaviLifecycleController::Snapshot snapshot={};lifecycle->snapshot(snapshot);
    auto* out=OSDictionary::withCapacity(40);
    bool valid=out&&observationString(out,"LifecycleState",HackyNaviLifecycleController::stateString(snapshot.state))&&
        observationString(out,"EngineStage",HackyNaviEngineController::stageString(snapshot.engineStage))&&
        observationNumber(out,"ResetStage",uint32_t(snapshot.resetStage))&&
        observationNumber(out,"AtomState",uint32_t(snapshot.atomState))&&
        observationNumber(out,"OperationResult",uint32_t(result))&&
        observationNumber(out,"EngineResult",uint32_t(snapshot.engineResult))&&
        observationNumber(out,"ResetResult",uint32_t(snapshot.resetResult))&&
        observationNumber(out,"AtomResult",uint32_t(snapshot.atomResult));
    if(out)valid=out->setObject("EngineRetained",snapshot.engineRetained?kOSBooleanTrue:kOSBooleanFalse)&&
        out->setObject("ResetRetained",snapshot.resetRetained?kOSBooleanTrue:kOSBooleanFalse)&&
        out->setObject("AtomRetained",snapshot.atomRetained?kOSBooleanTrue:kOSBooleanFalse)&&
        out->setObject("PermanentPciShadow",snapshot.permanentPciShadow?kOSBooleanTrue:kOSBooleanFalse)&&
        out->setObject("DeviceEpochRetained",snapshot.deviceEpochRetained?kOSBooleanTrue:kOSBooleanFalse)&&
        out->setObject("HistoricalEvidenceOnly",kOSBooleanTrue)&&engineObservation(out,snapshot.engineObservation)&&valid;
    const bool running=!retired&&result==kIOReturnSuccess&&
        snapshot.state==HackyNaviLifecycleController::State::Running;
    IOLockLock(lock_);
    if(valid)setProperty(retired?"LastRetirementObservation":"LastEngineObservation",out);
    if(!retired) {
        setProperty("EngineAttemptResult",UInt64(uint32_t(result)),32);
        setProperty("EngineStartupVerified",running?kOSBooleanTrue:kOSBooleanFalse);
    }
    setProperty("EngineActive",running?kOSBooleanTrue:kOSBooleanFalse);
    setProperty("EngineStatus",retired?(result==kIOReturnSuccess?"Retired":"RetirementFailed"):
        running?"EngineVerificationPassed":"EngineAttemptFailed");
    IOLockUnlock(lock_);
    if(out)out->release();
    IOLog("HackyNaviDevice: %s result=0x%x lifecycle=%s engine=%s\n",retired?"retirement":"activation",
        unsigned(result),HackyNaviLifecycleController::stateString(snapshot.state),
        HackyNaviEngineController::stageString(snapshot.engineStage));
}
