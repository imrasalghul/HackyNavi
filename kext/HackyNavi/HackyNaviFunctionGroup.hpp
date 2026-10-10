// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_FUNCTION_GROUP_HPP
#define HACKYNAVI_FUNCTION_GROUP_HPP
#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <stdint.h>
#include "hackynavi/audio_quiesce.hpp"

class HackyNaviFunctionLease;

// Actual PCI-client ownership of a Navi31 graphics/audio function pair. No
// device-wide reset, DMA allocations or driver termination are performed.
// Graphics must already be opened by primaryOwner. Audio is exclusively opened
// by a dedicated IOService child; an existing owner causes acquisition to fail.
// Each function has its own PM child, so its on/off callbacks are independent.
//
// Normal operations hold a common gate against both functions' PM callbacks
// and group shutdown. Registered hardware owners must complete real retirement
// on the shared callback worker before ACK/PCI close. Failure takes the explicit
// fail-stop path; an error return is not a power-loss veto. Unregistered clients
// remain observation-only and cannot consume first-attach authority. Firmware/SMM and direct
// external reset writers are not excluded by IOService's open arbitration.
class HackyNaviFunctionGroup : public OSObject {
    OSDeclareDefaultStructors(HackyNaviFunctionGroup)
public:
    enum class State { Acquiring, Powered, Revoked, Closing, Closed };
    enum class Event { Acquired, PowerChanged, ProviderTerminating, SystemShutdown, Closed };
    struct Identity {
        uint32_t vendorDevice, revisionClass, subsystem;
        uint8_t bus, device, function;
    };
    struct FunctionSnapshot {
        Identity identity;
        bool attached, opened, ownsOpen, pmInitialized, pmRegistered, powered, terminal, admissionRevoked;
        IOReturn result;
    };
    struct Snapshot {
        State state;
        uint64_t instance,generation;
        FunctionSnapshot functions[2];
    };
    enum class CreationStage { Allocation, Admission, FindPair, GraphicsLease,
        AudioLease, AudioBegin, GraphicsBegin, Topology, Complete };
    struct CreationObservation {
        CreationStage stage;
        IOReturn result;
        bool snapshotCaptured;
        Snapshot snapshot;
    };
    class Epoch {
        friend class HackyNaviFunctionGroup;
    public:
        Epoch():issuer_(nullptr),instance_(0),generation_(0){}
        uint64_t generation() const{return generation_;}
    private:
        const HackyNaviFunctionGroup* issuer_;
        uint64_t instance_;
        uint64_t generation_;
    };
    // Sleepable, nonrecursive and bound to its creating thread. One scope
    // spans the full operation, including every subordinate queue/controller
    // lock. Never wait for a PM callback, call shutdown(), or enter a second
    // operation while holding it. Recheck valid() immediately before each I/O.
    // Do not enter an inherited PCI/config workloop synchronously from the
    // scope (including restoreDeviceState). Termination callbacks can hold
    // that workloop while waiting to drain this gate. Reset requires the
    // worker's RetirementScope::enterRecovery(), never a normal Operation.
    // Destruction releases the gate before dropping the retained group.
    class FirstAttach;
    struct AudioStopReport;
    class Operation {
        friend class HackyNaviFunctionGroup;
        friend class FirstAttach;
    public:
        Operation();
        ~Operation();
        bool valid() const;
        bool belongsTo(const HackyNaviFunctionGroup* group) const {return issuer_==group&&valid();}
        bool matchesGraphics(IOPCIDevice*,IOService* openOwner) const;
        IOReturn consumeFirstAttach(FirstAttach&);
        uint64_t generation() const {return generation_;}
        IOReturn quiesceAudio(uint32_t timeoutUs,AudioStopReport&);
    private:
        Operation(const Operation&)=delete;
        Operation& operator=(const Operation&)=delete;
        HackyNaviFunctionGroup* issuer_;
        thread_t thread_;
        uint64_t instance_,generation_;
        // Exact providers retained by issuer_ for this lexical gate hold.
        IOPCIDevice* graphics_;
        IOService* openOwner_;
    };
    enum class RetirementReason : uint32_t {
        PowerDown=1, ProviderTerminating=2, SystemShutdown=4, Close=8, OwnerFailure=16
    };
    // Issued only on the one retirement worker for actual callback waiters or
    // a privately latched Operation failure. Normal admission is revoked first.
    // This scope authorizes cleanup,
    // never allocation/submission/startup, and cannot escape the handler.
    class RetirementScope {
        friend class HackyNaviFunctionGroup;
    public:
        bool valid() const;
        bool matchesGraphics(IOPCIDevice*,IOService*) const;
        IOReturn quiesceAudio(uint32_t timeoutUs,AudioStopReport&);
        uint64_t generation() const {return generation_;}
        uint64_t deadline() const {return deadline_;}
        uint32_t remainingUs() const;
        uint32_t reasons() const;
        void progress(uint32_t stage);
        // At most once, for ordinary PM down, failed activation, or an actual
        // runtime Operation's exposed-workload failure after its gate drains. Termination/close cannot
        // wait for PCI restore on a workloop it may already hold. A concurrent
        // terminal callback after this atomic boundary takes the fail-stop path.
        IOReturn enterRecovery();
        bool permitsPciRecovery() const;
        // Atomic/lifetime/deadline check only: no PCI/MMIO access. The reset
        // owner uses this solely for its source-defined blackout handshake.
        // Ordinary accesses still require matchesGraphics()/valid().
        bool recoveryWindowOpen() const;
    private:
        RetirementScope(HackyNaviFunctionGroup*,uint64_t,uint64_t);
        RetirementScope(const RetirementScope&)=delete;
        RetirementScope& operator=(const RetirementScope&)=delete;
        HackyNaviFunctionGroup* issuer_;
        thread_t thread_;
        uint64_t instance_,generation_,deadline_;
    };
    // Bind before any hardware activation. Success is accepted only from the
    // actual owner's retirement procedure after all its retained DMA/hardware
    // owners are clear. Any failure or measured deadline expiry panics before
    // ACK/PCI close, retaining owners; an error return is not a PM veto.
    // The handler must not close/detach/request PM or wait for these callbacks.
    using RetirementHandler=IOReturn (*)(IOService* owner,RetirementScope&);
    IOReturn registerRetirementHandler(Operation&,RetirementHandler,uint32_t timeoutUs);
    bool retirementRegistered() const {
        return __atomic_load_n(&retirementArmed_,__ATOMIC_ACQUIRE)!=0;
    }
    // One use per acquired pair. This is software admission for firmware-first
    // startup, not a stopped-engine/reset proof. The recipient must still
    // observe posted firmware, zero sOS and the explicit boot producers. It
    // expires with its exact Operation and cannot authorize a recovery path.
    class FirstAttach {
        friend class HackyNaviFunctionGroup;
    public:
        FirstAttach():operation_(nullptr),issuer_(nullptr),instance_(0),generation_(0){}
        bool validFor(const Operation&,IOPCIDevice*,IOService*) const;
    private:
        FirstAttach(const FirstAttach&)=delete;
        FirstAttach& operator=(const FirstAttach&)=delete;
        const Operation* operation_;
        const HackyNaviFunctionGroup* issuer_;
        uint64_t instance_,generation_;
    };
    enum class AudioStage { Preflight, Controller, BusMasterDisable, TransactionsPending, Complete };
    struct AudioStopReport {
        AudioStage stage;
        IOReturn result;
        hackynavi::audioquiesce::Result controller;
        uint64_t generation,barBytes,elapsedUs;
        uint32_t pendingPolls;
        uint16_t commandBefore,commandAfter,deviceStatus;
        uint8_t pcieCapability;
        bool busMasterWriteAttempted,busMasterDisabled,nonpostedPendingClear,complete;
    };
    // Notification is an invalidation/wakeup, never authority to reset. It can
    // race creation's return/shutdown and is delivered with retained group and
    // owner. Construction suppresses notifications until both clients exist;
    // Acquired uses function 0xff and does not claim asynchronous power-on.
    // Return promptly; do not call shutdown() from a notification/PM callback.
    // Obtain a new snapshot/epoch on the caller's serialized sleepable worker.
    using Notify = void (*)(IOService* owner,HackyNaviFunctionGroup*,Event,uint8_t function);
    static HackyNaviFunctionGroup* withPrimary(IOPCIDevice* graphics,
        IOService* primaryOwner,Notify,IOReturn&,CreationObservation* observation=nullptr);
    // Creation requests both PM children on, but returns without assuming the
    // asynchronous PM callbacks occurred. Capture succeeds only after both
    // actual callbacks and fresh PCI identity/D0/open/topology validation.
    // Both methods sample state; they do not hold exclusion across a later
    // MMIO/DMA transaction. beginOperation supplies the common CPU gate;
    // active DMA requires the registered callback-scoped retirement handler.
    IOReturn captureEpoch(Epoch&);
    IOReturn validateEpoch(const Epoch&);
    // Retains this group and locks the operational gate until out is destroyed.
    // On failure out is unchanged. The scope alone does not retire outstanding
    // DMA; registered hardware owners must implement the retirement handler.
    IOReturn beginOperation(Operation& out);
    // Requires the actual retirement handler to be installed. Consumption is
    // irreversible even if subsequent startup fails before its first write.
    IOReturn consumeFirstAttach(Operation&,FirstAttach&);
    // The actual primary Device must join retirement before its own PM ACK,
    // unmap or PCI close. Revokes permanently even before handler registration,
    // drains any registration race and runs the same paired transition. Call
    // with no Device/controller mutex held; it may wait for the normal gate.
    IOReturn retirePrimary(IOService* primaryOwner,RetirementReason);
    // Report real exposed workload/retirement ambiguity from the EXACT still-
    // held Operation. Its hardware validity may already be revoked; this only
    // grants failure reporting, never further PCI/MMIO/DMA authority. Private
    // issuer/thread/instance/issued-generation/provider bindings are checked.
    // Permanently revokes admission and schedules the existing retained worker
    // and watchdog, WITHOUT joining or taking owner/retirement wait locks.
    // Safe under a Session lock; the caller must unwind descendants/Operation
    // promptly. Success means the shared transition is scheduled, not recovered.
    IOReturn requestOwnerFailure(Operation&);
    // Explicit hardware operation; never invoked by construction or capture.
    // With this group's Operation held: stop the real function-1 HDA controller,
    // leave CRST asserted, clear PCI BME, and observe Transactions Pending clear.
    // Requires an already enabled, assigned audio BAR0 and D0; never wakes the
    // function or enables DMA. Total budget includes mapping/config accesses.
    // The report is an observation within this scope, NOT a GPU reset or DMA
    // retirement proof (PCI pending excludes posted writes). No DMA owner may
    // be released based on it, on error, or when the Operation is destroyed.
    IOReturn quiesceAudio(Operation&,uint32_t totalTimeoutUs,AudioStopReport&);
    IOReturn quiesceAudio(RetirementScope&,uint32_t totalTimeoutUs,AudioStopReport&);
    void snapshot(Snapshot&);
    // Caller retains this object for all methods. Explicit shutdown is required
    // to break the PM/client ownership graph. With a handler registered, this
    // joins the same terminal retirement before releasing either lease. Close
    // cannot initiate MODE1; perform recovery in its permitted callback scope.
    void shutdown();
protected:
    bool init() override;
    void free() override;
private:
    friend class HackyNaviFunctionLease;
    struct AudioAccess;
    IOReturn initialize(IOPCIDevice*,IOService*,Notify,CreationStage&);
    IOReturn findPair(IOPCIDevice*,IOPCIDevice*&);
    bool topologyMatches() const;
    IOReturn checkPoweredLocked();
    void changed(Event,uint8_t);
    void advanceGeneration();
    bool operationValid(const Operation&) const;
    void endOperation(Operation&);
    bool retirementValid(const RetirementScope&) const;
    bool retirementWindowValid(const RetirementScope&) const;
    void enqueueRetirement(RetirementReason,uint8_t function);
    void requestRetirement(RetirementReason,uint8_t function);
    static void retirementWorker(thread_call_param_t,thread_call_param_t);
    static void retirementWatchdog(thread_call_param_t,thread_call_param_t);
    void retirementFailed(IOReturn,const char*);
    IOLock* lock_;
    IOLock* teardownLock_;
    IOLock* operationLock_;
    thread_t operationThread_;
    Operation* activeOperation_;
    uint64_t operationGeneration_;
    IOService* owner_;
    IORegistryEntry* bridge_;
    HackyNaviFunctionLease* leases_[2];
    Notify notify_;
    uint64_t instance_,generation_;
    uint32_t shutdownRequested_;
    uint32_t primaryRevoked_;
    bool acquired_,closing_,closed_;
    IOLock* retirementWaitLock_;
    thread_call_t retirementCall_,retirementWatch_;
    RetirementHandler retirementHandler_;
    thread_t retirementThread_;
    thread_t registrationThread_;
    uint64_t retirementStart_,retirementBudgetTicks_,retirementGeneration_;
    uint32_t retirementArmed_,retirementFlags_,retirementPhase_,retirementStage_;
    uint32_t retirementWaiters_,retirementFunctions_;
    uint32_t runtimeFailureOrigin_;
    bool firstAttachConsumed_;
};

// Manually attached native IOService. It is not published for matching and has
// no user client. A distinct instance participates in each function's PM tree.
class HackyNaviFunctionLease : public IOService {
    OSDeclareDefaultStructors(HackyNaviFunctionLease)
public:
    bool init(OSDictionary* =nullptr) override;
    IOReturn setPowerState(unsigned long,IOService*) override;
    bool willTerminate(IOService*,IOOptionBits) override;
    bool didTerminate(IOService*,IOOptionBits,bool*) override;
    void stop(IOService*) override;
    void systemWillShutdown(IOOptionBits) override;
protected:
    void free() override;
private:
    friend class HackyNaviFunctionGroup;
    static HackyNaviFunctionLease* create(HackyNaviFunctionGroup*,IOPCIDevice*,
        IOService* borrowedOpenOwner,IOReturn&);
    IOReturn begin();
    IOReturn beginLocked();
    void shutdown();
    void revoke(HackyNaviFunctionGroup::Event);
    // Returns a retained group with its operational gate held. Never wait
    // for that gate while holding this lease's state mutex.
    HackyNaviFunctionGroup* beginTransition();
    void endTransition(HackyNaviFunctionGroup*);
    void notify(HackyNaviFunctionGroup::Event);
    void copySnapshot(HackyNaviFunctionGroup::FunctionSnapshot&);
    bool validatePowered();
    bool identityMatches(bool requireD0) const;
    bool noOtherAudioClients() const;
    bool accessRevoked() const;
    IOLock* lock_;
    IOLock* lifecycleLock_;
    HackyNaviFunctionGroup* group_;
    IOPCIDevice* pci_;
    IOService* borrowedOwner_;
    HackyNaviFunctionGroup::Identity identity_;
    uint32_t admissionRevoked_;
    bool attached_,started_,opened_,pmInitialized_,pmRegistered_,powered_,terminal_;
    IOReturn result_;
};
#endif
