// SPDX-License-Identifier: MIT
#pragma once
#include "HackyNaviVMContext.hpp"
#include "HackyNaviFunctionGroup.hpp"
#include "hackynavi/memory_layout.hpp"
#include "hackynavi/retained_result_budget.hpp"
#include "hackynavi/native_abi.hpp"
class HackyNaviGraphicsQueue;
class HackyNaviVMCompletion;
class HackyNaviEngineController;

// One kernel-issued client epoch and VMID. Legacy profiles seal one capture;
// v1.4 preserves data allocations after actual completed-job consumption and
// bounds immutable historical output to eight explicitly released records.
// All addresses crossing the client boundary are isolated GPU VAs;
// CPU data is copied through bounded kernel buffers, never mapped into a GPU
// command/control allocation. Generic shaders/PM4 and CPU mappings are absent.
// Native methods are independently serialized and reacquire the actual group
// Operation/GART access on every request. No user-provided readiness flags.
class HackyNaviClientSession : public OSObject {
    OSDeclareDefaultStructors(HackyNaviClientSession)
public:
    using Handle=HackyNaviVMContext::Handle;
    using ComputeRequest=HackyNaviVMContext::ComputeRequest;
    using CompilerComputeRequest=HackyNaviVMContext::CompilerComputeRequest;
    struct Info {
        uint64_t epoch,registryId,vramBytes,cpuVisibleVramBytes,systemBudget;
        uint64_t vaBegin,vaEnd,maxAllocationBytes;
    };
    IOReturn information(Info&);
    // One immutable negotiated profile, before any allocation or dispatch.
    IOReturn configureProfile(uint16_t minor);
    IOReturn allocate(uint64_t bytes,uint32_t alignment,Handle&);
    IOReturn releaseBuffer(Handle);
    IOReturn mapBuffer(Handle,uint64_t offset,uint64_t va,uint64_t bytes,uint32_t access);
    IOReturn unmapBuffer(Handle,uint64_t offset,uint64_t va,uint64_t bytes);
    IOReturn writeBuffer(Handle,uint64_t offset,const void* kernelInput,size_t bytes);
    IOReturn readBuffer(Handle,uint64_t offset,void* kernelOutput,size_t bytes);
    IOReturn compute(const ComputeRequest&,uint64_t& sequence);
    IOReturn computeCompiler(const CompilerComputeRequest&,uint64_t& sequence);
    // The wire message is already captured in kernel memory. Resolve current
    // staging BO generations and copy their bytes under this session's lock;
    // no user pointer or writable GPU code survives into native capture.
    IOReturn computeProduct(const hackynavi::nativeabi::CapturedProductCompute&,uint64_t& sequence);
    IOReturn graphicsInformation(HNColorTopology&);
    IOReturn graphicsProduct(const hackynavi::nativeabi::ProductGraphicsRequest&,uint64_t& sequence);
    IOReturn graphicsProduct(const hackynavi::nativeabi::ProductBufferedGraphicsRequest&,uint64_t& sequence);
    IOReturn graphicsProduct(const hackynavi::nativeabi::ProductVertexBufferedGraphicsRequest&,uint64_t& sequence);
    IOReturn graphicsProduct(const hackynavi::nativeabi::ProductVertexFragmentBufferedGraphicsRequest&,uint64_t& sequence);
    IOReturn wait(uint64_t sequence,bool& completed);
    IOReturn readResult(uint64_t sequence,uint64_t relativeOffset,void* kernelOutput,size_t bytes);
    IOReturn releaseResult(uint64_t sequence);
    // Closes CPU admission first; only real retirement may free exposed DMA.
    // On failure registry ownership survives dropping this client's reference.
    IOReturn close();
protected:
    bool init() override;
    void free() override;
private:
    friend class HackyNaviEngineController;
    static HackyNaviClientSession* withOwners(HackyNaviFunctionGroup*,
        HackyNaviFunctionGroup::Operation&,HackyNaviGARTController*,HackyNaviGraphicsQueue*,
        uint64_t registryId,const hackynavi::MemoryLayout&,IOReturn&);
    IOReturn initialize(HackyNaviFunctionGroup*,HackyNaviFunctionGroup::Operation&,
        HackyNaviGARTController*,HackyNaviGraphicsQueue*,uint64_t,const hackynavi::MemoryLayout&);
    IOReturn finish(IOReturn);
    IOReturn graphicsProduct(const hackynavi::nativeabi::ProductGraphicsRequest&,
        const hackynavi::nativeabi::ProductBufferedGraphicsRequest*,
        const hackynavi::nativeabi::ProductVertexBufferedGraphicsRequest*,
        const hackynavi::nativeabi::ProductVertexFragmentBufferedGraphicsRequest*,uint64_t& sequence);
    IOReturn begin(HackyNaviFunctionGroup::Operation&,bool closing=false);
    // Requires the session mutex, an actual Operation/GART guard and a reserved
    // result. Completes the captured native job and releases the session mutex.
    IOReturn completeCaptured(HackyNaviFunctionGroup::Operation&,IOReturn captureResult,uint64_t& sequence,
        HackyNaviVMCompletion*& deferredRelease);
    // Failure-only containment after genuine queue/VM exposure, never input
    // rejection or allocation failure. Called with the same actual Operation;
    // schedules without joining while the session/descendant scopes unwind.
    void containWorkloadFailure(HackyNaviFunctionGroup::Operation&,IOReturn);
    // Session lock held, or exclusive unpublished/final destruction. Frees
    // actual result memory before returning its exact global reservation.
    struct RetainedResult {
        uint8_t* bytes;
        size_t size;
        uint64_t sequence,captureGeneration;
        hackynavi::resultbudget::Reservation reservation;
    };
    RetainedResult* findResult(uint64_t sequence);
    IOReturn reserveResult(uint32_t bytes);
    void discardResult(RetainedResult&);
    void releaseResults();
    enum class State { Unconfigured,Prepared,Submitting,ReusableIdle,Sealed,Lost,Closed };
    bool mutableState() const {return state_==State::Prepared||state_==State::ReusableIdle;}
    bool reusable() const {return hackynavi::nativeabi::reusableProfile(profileMinor_);}
    struct Buffer {Handle handle;uint64_t bytes;};
    struct Mapping {Handle handle;uint64_t offset,va,bytes;};
    Buffer* find(Handle);
    bool range(Handle,uint64_t offset,uint64_t bytes) const;
    IOLock* lock_;
    HackyNaviFunctionGroup* group_;
    HackyNaviGARTController* gart_;
    HackyNaviGraphicsQueue* queue_;
    HackyNaviVMContext* vm_;
    HackyNaviVMCompletion* completion_;
    Info info_;
    Buffer buffers_[HackyNaviVMContext::kBufferSlots];
    Mapping mappings_[HackyNaviVMContext::kMappingSlots];
    uint64_t operationGeneration_,sequence_;
    RetainedResult results_[hackynavi::nativeabi::kMaxRetainedResults];
    RetainedResult* pendingResult_;
    uint16_t profileMinor_;
    State state_;
    IOReturn closeResult_;

};
