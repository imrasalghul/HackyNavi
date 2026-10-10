// SPDX-License-Identifier: MIT
#include "HackyNaviClientSession.hpp"
#include "HackyNaviGARTController.hpp"
#include "HackyNaviGraphicsQueue.hpp"
#include "HackyNaviVMCompletion.hpp"
#include "hackynavi/gfx_aco_fixture.hpp"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <kern/debug.h>
#include <kern/clock.h>

OSDefineMetaClassAndStructors(HackyNaviClientSession,OSObject)
namespace {
uint64_t lastClientEpoch=0;
// VMID reuse must not bypass retained kernel-result accounting. Byte and
// holder caps share one atomic transaction, across all devices/connections.
// The holder cap also bounds tiny results' retained session/VM CPU metadata.
hackynavi::resultbudget::Budget retainedResults;
bool issueEpoch(uint64_t& out) {
    uint64_t previous=__atomic_load_n(&lastClientEpoch,__ATOMIC_RELAXED);
    do {if(previous==UINT64_MAX)return false;}
    while(!__atomic_compare_exchange_n(&lastClientEpoch,&previous,previous+1,false,
        __ATOMIC_RELAXED,__ATOMIC_RELAXED));
    out=previous+1;return true;
}
bool same(HackyNaviClientSession::Handle a,HackyNaviClientSession::Handle b) {
    return a.id==b.id&&a.generation==b.generation;
}
// Heap-backed to keep up to eight variants and 32 resources off the kernel
// stack. These temporary CPU copies are not DMA/control storage; VMProduct
// creates its own private, zero-padded native allocations before admission.
struct ProductInput {
    hackynavi::gfx11product::Request variants[hackynavi::nativeabi::kMaxProductVariants];
    HackyNaviVMContext::ProductResource resources[hackynavi::nativeabi::kMaxProductResources];
    HackyNaviVMContext::ProductComputeRequest request;
};
class OwnedProductInput {
public:
    ProductInput* value;
    OwnedProductInput():value(static_cast<ProductInput*>(IOMalloc(sizeof(ProductInput)))) {
        if(value)bzero(value,sizeof(*value));
    }
    ~OwnedProductInput(){release();}
    void release() {
        if(!value)return;
        for(auto& variant:value->variants)
            if(variant.code)IOFree(const_cast<uint8_t*>(variant.code),variant.codeBytes);
        IOFree(value,sizeof(*value));value=nullptr;
    }
private:
    OwnedProductInput(const OwnedProductInput&)=delete;
    OwnedProductInput& operator=(const OwnedProductInput&)=delete;
};
static_assert(hackynavi::nativeabi::kMaxProductCodeBytes==hackynavi::gfx11product::kMaxCodeBytes,
    "product code boundary agrees with actual ISA validator");
// Declared before Operation/Access in submit methods: OSObject destruction can
// acquire GART, so run only after the Session and all descendant scopes unwind.
struct DeferredCompletion {
    HackyNaviVMCompletion* value=nullptr;
    ~DeferredCompletion(){if(value)value->release();}
};
class OwnedGraphicsInput {
public:
    uint8_t* value;
    OwnedGraphicsInput():value(static_cast<uint8_t*>(IOMalloc(hackynavi::nativeabi::kGraphicsImageBytes))) {}
    ~OwnedGraphicsInput(){if(value)IOFree(value,hackynavi::nativeabi::kGraphicsImageBytes);}
private:
    OwnedGraphicsInput(const OwnedGraphicsInput&)=delete;
    OwnedGraphicsInput& operator=(const OwnedGraphicsInput&)=delete;
};
constexpr hackynavi::gfxqueue::PollOptions kComputePoll={100000,10};
constexpr hackynavi::vmcontext::Poll kRetirePoll={10000,10};
}
bool HackyNaviClientSession::init() {
    lock_=nullptr;group_=nullptr;gart_=nullptr;queue_=nullptr;vm_=nullptr;completion_=nullptr;
    info_={};operationGeneration_=sequence_=0;profileMinor_=0;state_=State::Unconfigured;
    pendingResult_=nullptr;closeResult_=kIOReturnSuccess;
    for(auto& result:results_){result.bytes=nullptr;result.size=0;result.sequence=result.captureGeneration=0;}
    for(auto& buffer:buffers_)buffer={};for(auto& mapping:mappings_)mapping={};
    if(!OSObject::init())return false;
    lock_=IOLockAlloc();return lock_!=nullptr;
}
HackyNaviClientSession* HackyNaviClientSession::withOwners(HackyNaviFunctionGroup* group,
    HackyNaviFunctionGroup::Operation& scope,HackyNaviGARTController* gart,HackyNaviGraphicsQueue* queue,
    uint64_t registry,const hackynavi::MemoryLayout& memory,IOReturn& result) {
    result=kIOReturnNoMemory;auto* session=new HackyNaviClientSession;if(!session)return nullptr;
    if(!session->init()){session->release();return nullptr;}
    result=session->initialize(group,scope,gart,queue,registry,memory);
    if(result!=kIOReturnSuccess){session->release();return nullptr;}
    return session;
}
IOReturn HackyNaviClientSession::initialize(HackyNaviFunctionGroup* group,
    HackyNaviFunctionGroup::Operation& scope,HackyNaviGARTController* gart,HackyNaviGraphicsQueue* queue,
    uint64_t registry,const hackynavi::MemoryLayout& memory) {
    if(!group||!scope.belongsTo(group)||!gart||!gart->normalAccessReady()||!queue||
        queue->state()!=HackyNaviGraphicsQueue::State::Running||!registry||!memory.vramBytes||
        memory.cpuVisibleVramBytes>memory.vramBytes)return kIOReturnNotReady;
    group_=group;group_->retain();gart_=gart;gart_->retain();queue_=queue;queue_->retain();
    operationGeneration_=scope.generation();
    if(!issueEpoch(info_.epoch))return kIOReturnNoResources;
    IOReturn result=kIOReturnBusy;
    for(uint8_t vmid=1;vmid<=7;++vmid) {
        vm_=HackyNaviVMContext::withGART(gart_,vmid,operationGeneration_,result);
        if(vm_||result!=kIOReturnBusy)break;
    }
    if(!vm_)return result;
    info_.registryId=registry;info_.vramBytes=memory.vramBytes;
    info_.cpuVisibleVramBytes=memory.cpuVisibleVramBytes;
    info_.systemBudget=HackyNaviVMContext::kResidentBudget;
    info_.vaBegin=hackynavi::vmpt::kVaBegin;info_.vaEnd=hackynavi::vmpt::kVaEnd;
    info_.maxAllocationBytes=HackyNaviVMContext::kMaxBufferBytes;
    return scope.belongsTo(group_)&&gart_->normalAccessReady()?kIOReturnSuccess:kIOReturnOffline;
}
IOReturn HackyNaviClientSession::finish(IOReturn result){IOLockUnlock(lock_);return result;}
void HackyNaviClientSession::discardResult(RetainedResult& result) {
    if(result.bytes)IOFree(result.bytes,result.size);
    result.bytes=nullptr;result.size=0;result.sequence=result.captureGeneration=0;
    if(result.reservation.bytes()&&!retainedResults.release(result.reservation))
        panic("HackyNavi: retained result budget ownership/underflow");
}
void HackyNaviClientSession::releaseResults() {
    for(auto& result:results_)discardResult(result);
    pendingResult_=nullptr;
}
HackyNaviClientSession::RetainedResult* HackyNaviClientSession::findResult(uint64_t sequence) {
    if(!sequence)return nullptr;
    for(auto& result:results_)if(result.bytes&&result.sequence==sequence)return &result;
    return nullptr;
}
IOReturn HackyNaviClientSession::reserveResult(uint32_t bytes) {
    if(pendingResult_||!bytes)return kIOReturnBadArgument;
    RetainedResult* slot=nullptr;
    const unsigned limit=reusable()?hackynavi::nativeabi::kMaxRetainedResults:1;
    for(unsigned i=0;i<limit;++i)if(!results_[i].bytes&&!results_[i].reservation.bytes()){slot=&results_[i];break;}
    if(!slot||!retainedResults.reserve(bytes,slot->reservation))return kIOReturnNoResources;
    slot->bytes=static_cast<uint8_t*>(IOMalloc(bytes));
    if(!slot->bytes){discardResult(*slot);return kIOReturnNoMemory;}
    slot->size=bytes;bzero(slot->bytes,bytes);pendingResult_=slot;return kIOReturnSuccess;
}
IOReturn HackyNaviClientSession::begin(HackyNaviFunctionGroup::Operation& scope,bool closing) {
    if(state_==State::Closed||!group_||!gart_||!queue_||!vm_)return kIOReturnNotOpen;
    if(state_==State::Lost&&!closing)return kIOReturnOffline;
    const IOReturn result=group_->beginOperation(scope);
    if(result!=kIOReturnSuccess)return result;
    return scope.generation()==operationGeneration_?kIOReturnSuccess:kIOReturnOffline;
}
IOReturn HackyNaviClientSession::configureProfile(uint16_t minor) {
    namespace a=hackynavi::nativeabi;
    IOLockLock(lock_);
    if(state_!=State::Unconfigured)return finish(kIOReturnNotPermitted);
    if(minor!=a::kMinor&&minor!=a::kCompilerMinor&&minor!=a::kProductMinor&&!a::reusableProfile(minor))
        return finish(kIOReturnUnsupported);
    HackyNaviFunctionGroup::Operation scope;auto result=begin(scope);
    if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviGARTController::Access access(*gart_,scope);if(!access.normal())return finish(kIOReturnOffline);
    profileMinor_=minor;state_=State::Prepared;return finish(kIOReturnSuccess);
}
HackyNaviClientSession::Buffer* HackyNaviClientSession::find(Handle handle) {
    if(!handle.id||handle.id>HackyNaviVMContext::kBufferSlots||!handle.generation)return nullptr;
    auto& buffer=buffers_[handle.id-1];return buffer.bytes&&same(buffer.handle,handle)?&buffer:nullptr;
}
bool HackyNaviClientSession::range(Handle handle,uint64_t offset,uint64_t bytes) const {
    if(!handle.id||handle.id>HackyNaviVMContext::kBufferSlots||!handle.generation||!bytes)return false;
    const auto& buffer=buffers_[handle.id-1];
    return same(buffer.handle,handle)&&offset<=buffer.bytes&&bytes<=buffer.bytes-offset;
}
IOReturn HackyNaviClientSession::information(Info& out) {
    out={};IOLockLock(lock_);HackyNaviFunctionGroup::Operation scope;
    auto result=begin(scope);if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviGARTController::Access access(*gart_,scope);
    if(!access.normal())return finish(kIOReturnOffline);
    out=info_;return finish(kIOReturnSuccess);
}
IOReturn HackyNaviClientSession::allocate(uint64_t bytes,uint32_t alignment,Handle& out) {
    out={};IOLockLock(lock_);if(!mutableState())return finish(state_==State::Lost?kIOReturnOffline:kIOReturnNotPermitted);
    HackyNaviFunctionGroup::Operation scope;auto result=begin(scope);
    if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviGARTController::Access access(*gart_,scope);if(!access.normal())return finish(kIOReturnOffline);
    result=vm_->allocateBuffer(bytes,alignment,out);
    if(result==kIOReturnSuccess)buffers_[out.id-1]={out,bytes};
    return finish(result);
}
IOReturn HackyNaviClientSession::releaseBuffer(Handle handle) {
    IOLockLock(lock_);if(!mutableState())return finish(state_==State::Lost?kIOReturnOffline:kIOReturnNotPermitted);
    if(!find(handle))return finish(kIOReturnNotFound);
    HackyNaviFunctionGroup::Operation scope;auto result=begin(scope);
    if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviGARTController::Access access(*gart_,scope);if(!access.normal())return finish(kIOReturnOffline);
    result=vm_->freeBuffer(handle);if(result==kIOReturnSuccess)buffers_[handle.id-1]={};
    return finish(result);
}
IOReturn HackyNaviClientSession::mapBuffer(Handle handle,uint64_t offset,uint64_t va,uint64_t bytes,uint32_t flags) {
    IOLockLock(lock_);if(!mutableState())return finish(state_==State::Lost?kIOReturnOffline:kIOReturnNotPermitted);
    if(!range(handle,offset,bytes))return finish(kIOReturnBadArgument);
    Mapping* free=nullptr;for(auto& mapping:mappings_)if(!mapping.bytes){free=&mapping;break;}
    if(!free)return finish(kIOReturnNoSpace);
    HackyNaviFunctionGroup::Operation scope;auto result=begin(scope);
    if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviGARTController::Access access(*gart_,scope);if(!access.normal())return finish(kIOReturnOffline);
    result=vm_->map(handle,offset,va,bytes,flags);
    if(result==kIOReturnSuccess)*free={handle,offset,va,bytes};
    return finish(result);
}
IOReturn HackyNaviClientSession::unmapBuffer(Handle handle,uint64_t offset,uint64_t va,uint64_t bytes) {
    IOLockLock(lock_);if(!mutableState())return finish(state_==State::Lost?kIOReturnOffline:kIOReturnNotPermitted);
    Mapping* found=nullptr;for(auto& mapping:mappings_)
        if(mapping.bytes&&same(mapping.handle,handle)&&mapping.offset==offset&&mapping.va==va&&mapping.bytes==bytes)
            {found=&mapping;break;}
    if(!found)return finish(kIOReturnNotFound);
    HackyNaviFunctionGroup::Operation scope;auto result=begin(scope);
    if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviGARTController::Access access(*gart_,scope);if(!access.normal())return finish(kIOReturnOffline);
    result=vm_->unmap(handle,va,bytes);if(result==kIOReturnSuccess)*found={};
    return finish(result);
}
IOReturn HackyNaviClientSession::writeBuffer(Handle handle,uint64_t offset,const void* input,size_t bytes) {
    IOLockLock(lock_);if(!mutableState())return finish(state_==State::Lost?kIOReturnOffline:kIOReturnNotPermitted);
    if(!input||bytes>4096||!range(handle,offset,bytes))return finish(kIOReturnBadArgument);
    HackyNaviFunctionGroup::Operation scope;auto result=begin(scope);
    if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviGARTController::Access access(*gart_,scope);if(!access.normal())return finish(kIOReturnOffline);
    return finish(vm_->writeBuffer(handle,offset,input,bytes));
}
IOReturn HackyNaviClientSession::readBuffer(Handle handle,uint64_t offset,void* output,size_t bytes) {
    IOLockLock(lock_);if(!mutableState())return finish(state_==State::Lost?kIOReturnOffline:kIOReturnNotPermitted);
    if(!output||bytes>4096||!range(handle,offset,bytes))return finish(kIOReturnBadArgument);
    HackyNaviFunctionGroup::Operation scope;auto result=begin(scope);
    if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviGARTController::Access access(*gart_,scope);if(!access.normal())return finish(kIOReturnOffline);
    return finish(vm_->readBuffer(handle,offset,output,bytes));
}
IOReturn HackyNaviClientSession::compute(const ComputeRequest& request,uint64_t& sequence) {
    DeferredCompletion deferred;sequence=0;IOLockLock(lock_);if(!mutableState())return finish(state_==State::Lost?kIOReturnOffline:kIOReturnNotPermitted);
    if(!request.bytes||(request.bytes&15)||!range(request.destination,request.destinationOffset,request.bytes)||
        (request.operation!=hackynavi::gfxcompute::Operation::Clear&&
         request.operation!=hackynavi::gfxcompute::Operation::Copy)||
        (request.operation==hackynavi::gfxcompute::Operation::Copy&&!range(request.source,request.sourceOffset,request.bytes)))
        return finish(kIOReturnBadArgument);
    HackyNaviFunctionGroup::Operation scope;auto result=begin(scope);
    if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviGARTController::Access access(*gart_,scope);if(!access.normal())return finish(kIOReturnOffline);
    // Reserve immutable CPU result before any GPU publication. Readback stays
    // under the same group gate, so another producer cannot invalidate its
    // queue-issued completion between dispatch and the final copied byte.
    result=reserveResult(request.bytes);if(result!=kIOReturnSuccess)return finish(result);
    state_=State::Submitting;
    return completeCaptured(scope,vm_->captureCompute(request),sequence,deferred.value);
}
IOReturn HackyNaviClientSession::computeCompiler(const CompilerComputeRequest& request,uint64_t& sequence) {
    DeferredCompletion deferred;sequence=0;IOLockLock(lock_);if(!mutableState())return finish(state_==State::Lost?kIOReturnOffline:kIOReturnNotPermitted);
    if(profileMinor_!=hackynavi::nativeabi::kCompilerMinor)return finish(kIOReturnUnsupported);
    constexpr uint32_t bytes=hackynavi::gfxaco::kDataBytes;
    if((request.sourceOffset&3)||(request.destinationOffset&3)||
        !range(request.source,request.sourceOffset,bytes)||
        !range(request.destination,request.destinationOffset,bytes))return finish(kIOReturnBadArgument);
    HackyNaviFunctionGroup::Operation scope;auto result=begin(scope);
    if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviGARTController::Access access(*gart_,scope);if(!access.normal())return finish(kIOReturnOffline);
    result=reserveResult(bytes);if(result!=kIOReturnSuccess)return finish(result);
    state_=State::Submitting;
    return completeCaptured(scope,vm_->captureCompilerCompute(request),sequence,deferred.value);
}
IOReturn HackyNaviClientSession::computeProduct(const hackynavi::nativeabi::CapturedProductCompute& captured,
    uint64_t& sequence) {
    namespace a=hackynavi::nativeabi;
    DeferredCompletion deferred;sequence=0;IOLockLock(lock_);if(!mutableState())return finish(state_==State::Lost?kIOReturnOffline:kIOReturnNotPermitted);
    if(a::validateProductCompute(captured,info_.epoch,profileMinor_)!=a::Validation::Ok)
        return finish(kIOReturnBadArgument);
    const auto& request=captured.request;
    const Handle output={request.result.id,request.result.generation};
    if(!range(output,request.resultOffset,request.resultBytes))return finish(kIOReturnBadArgument);
    for(uint32_t i=0;i<request.variantCount;++i) {
        const auto& v=captured.variants[i];const Handle code={v.code.id,v.code.generation};
        if(!range(code,v.offset,v.codeBytes))return finish(kIOReturnBadArgument);
        // Code upload BOs are CPU staging only. Reject every mapping of the
        // actual handle; only the private RX copy may become GPU executable.
        for(const auto& mapping:mappings_)
            if(mapping.bytes&&same(mapping.handle,code))return finish(kIOReturnNotPermitted);
    }
    for(uint32_t i=0;i<request.resourceCount;++i) {
        const auto& r=captured.resources[i];
        if(!range({r.buffer.id,r.buffer.generation},r.offset,r.bytes))return finish(kIOReturnBadArgument);
    }
    HackyNaviFunctionGroup::Operation scope;auto result=begin(scope);
    if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviGARTController::Access access(*gart_,scope);if(!access.normal())return finish(kIOReturnOffline);
    OwnedProductInput incoming;if(!incoming.value)return finish(kIOReturnNoMemory);
    auto& native=incoming.value->request;
    native.variants=incoming.value->variants;native.variantCount=request.variantCount;
    native.resources=incoming.value->resources;native.resourceCount=request.resourceCount;
    native.result=output;native.resultOffset=request.resultOffset;native.resultBytes=request.resultBytes;
    for(unsigned d=0;d<3;++d){native.grid[d]=request.grid[d];native.nominalLocal[d]=request.nominalLocal[d];}
    for(uint32_t i=0;i<request.variantCount;++i) {
        const auto& wire=captured.variants[i];auto& v=incoming.value->variants[i];
        auto* code=static_cast<uint8_t*>(IOMalloc(wire.codeBytes));
        if(!code)return finish(kIOReturnNoMemory);
        v.code=code;v.codeBytes=wire.codeBytes;v.executableBytes=wire.executableBytes;
        for(unsigned d=0;d<3;++d)v.local[d]=wire.local[d];
        v.waveSize=wire.waveSize;v.numSgprs=wire.numSgprs;v.numVgprs=wire.numVgprs;
        // readBuffer synchronizes the actual native DMA owner before copying.
        // Session serialization excludes writes, frees and mapping changes
        // throughout all copies, capture, GPU completion and retirement.
        result=vm_->readBuffer({wire.code.id,wire.code.generation},wire.offset,code,wire.codeBytes);
        if(result!=kIOReturnSuccess)return finish(result);
    }
    for(uint32_t i=0;i<request.resourceCount;++i) {
        const auto& r=captured.resources[i];
        incoming.value->resources[i]={r.binding,{r.buffer.id,r.buffer.generation},r.offset,r.bytes};
    }
    result=reserveResult(request.resultBytes);if(result!=kIOReturnSuccess)return finish(result);
    state_=State::Submitting;
    result=vm_->captureProductCompute(native);
    incoming.release(); // private immutable native owners now hold all controls
    return completeCaptured(scope,result,sequence,deferred.value);
}
IOReturn HackyNaviClientSession::graphicsInformation(HNColorTopology& output) {
    output={};IOLockLock(lock_);
    if(!mutableState())return finish(state_==State::Lost?kIOReturnOffline:kIOReturnNotPermitted);
    if(!hackynavi::nativeabi::graphicsProfile(profileMinor_))return finish(kIOReturnUnsupported);
    HackyNaviFunctionGroup::Operation scope;auto result=begin(scope);
    if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviGARTController::Access access(*gart_,scope);if(!access.normal())return finish(kIOReturnOffline);
    // Reuse only the actual same-generation record. A missing record permits
    // its first capture; the native owner's attempt latch forbids replay after
    // failure. No VM/queue mutex is held while selectors are inspected.
    result=gart_->copyColorTopology(scope,output);
    if(result==kIOReturnNotReady) {
        uint64_t ticks=0,ns=0;clock_get_uptime(&ticks);absolutetime_to_nanoseconds(ticks,&ns);
        result=gart_->captureColorTopology(scope,ns/1000+1000000,output);
    }
    if(result!=kIOReturnSuccess) {
        output={};state_=State::Lost;containWorkloadFailure(scope,result);
    }
    return finish(result);
}
IOReturn HackyNaviClientSession::graphicsProduct(const hackynavi::nativeabi::ProductGraphicsRequest& request,
    uint64_t& sequence) {
    return graphicsProduct(request,nullptr,nullptr,nullptr,sequence);
}
IOReturn HackyNaviClientSession::graphicsProduct(const hackynavi::nativeabi::ProductBufferedGraphicsRequest& request,
    uint64_t& sequence) {
    return graphicsProduct(request.draw,&request,nullptr,nullptr,sequence);
}
IOReturn HackyNaviClientSession::graphicsProduct(const hackynavi::nativeabi::ProductVertexBufferedGraphicsRequest& request,
    uint64_t& sequence) {
    return graphicsProduct(request.draw,nullptr,&request,nullptr,sequence);
}
IOReturn HackyNaviClientSession::graphicsProduct(const hackynavi::nativeabi::ProductVertexFragmentBufferedGraphicsRequest& request,
    uint64_t& sequence) {
    return graphicsProduct(request.vertexDraw.draw,nullptr,nullptr,&request,sequence);
}
IOReturn HackyNaviClientSession::graphicsProduct(const hackynavi::nativeabi::ProductGraphicsRequest& request,
    const hackynavi::nativeabi::ProductBufferedGraphicsRequest* buffered,
    const hackynavi::nativeabi::ProductVertexBufferedGraphicsRequest* vertex,
    const hackynavi::nativeabi::ProductVertexFragmentBufferedGraphicsRequest* combined,uint64_t& sequence) {
    namespace a=hackynavi::nativeabi;
    DeferredCompletion deferred;sequence=0;IOLockLock(lock_);
    if(!mutableState())return finish(state_==State::Lost?kIOReturnOffline:kIOReturnNotPermitted);
    if(unsigned(bool(buffered))+unsigned(bool(vertex))+unsigned(bool(combined))>1)return finish(kIOReturnBadArgument);
    if((combined?a::validateProductVertexFragmentBufferedGraphics(*combined,info_.epoch,profileMinor_):
        vertex?a::validateProductVertexBufferedGraphics(*vertex,info_.epoch,profileMinor_):
        buffered?a::validateProductBufferedGraphics(*buffered,info_.epoch,profileMinor_):
        a::validateProductGraphics(request,info_.epoch,profileMinor_))!=a::Validation::Ok)
        return finish(kIOReturnBadArgument);
    const auto* vertexInput=combined?&combined->vertexDraw:vertex;
    const a::Handle fragmentHandle=combined?combined->fragment:buffered?buffered->fragment:a::Handle{};
    const uint64_t fragmentOffset=combined?combined->fragmentOffset:buffered?buffered->fragmentOffset:0;
    const Handle code={request.code.id,request.code.generation},target={request.target.id,request.target.generation};
    if(!range(code,request.codeOffset,a::kGraphicsImageBytes)||
        !range(target,request.targetOffset,request.targetLayout.total_bytes))return finish(kIOReturnBadArgument);
    if(fragmentHandle.id&&!range({fragmentHandle.id,fragmentHandle.generation},fragmentOffset,16))
        return finish(kIOReturnBadArgument);
    if(vertexInput&&!range({vertexInput->vertex.id,vertexInput->vertex.generation},vertexInput->vertexOffset,48))
        return finish(kIOReturnBadArgument);
    for(const auto& mapping:mappings_)
        if(mapping.bytes&&same(mapping.handle,code))return finish(kIOReturnNotPermitted);
    HackyNaviFunctionGroup::Operation scope;auto result=begin(scope);
    if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviGARTController::Access access(*gart_,scope);if(!access.normal())return finish(kIOReturnOffline);
    OwnedGraphicsInput input;if(!input.value)return finish(kIOReturnNoMemory);
    result=vm_->readBuffer(code,request.codeOffset,input.value,a::kGraphicsImageBytes);
    if(result!=kIOReturnSuccess)return finish(result);
    result=reserveResult(uint32_t(request.targetLayout.total_bytes));if(result!=kIOReturnSuccess)return finish(result);
    HackyNaviVMContext::ProductGraphicsRequest native={input.value,a::kGraphicsImageBytes,
        request.vertexBytes,request.fragmentBytes,target,request.targetOffset,request.targetLayout};
    state_=State::Submitting;
    if(combined) {
        const HackyNaviVMContext::ProductVertexFragmentBufferedGraphicsRequest withBuffers={native,
            {vertexInput->vertex.id,vertexInput->vertex.generation},
            {fragmentHandle.id,fragmentHandle.generation},vertexInput->vertexOffset,fragmentOffset};
        result=vm_->captureProductGraphics(scope,withBuffers);
    } else if(vertexInput) {
        const HackyNaviVMContext::ProductVertexBufferedGraphicsRequest withVertex={native,
            {vertexInput->vertex.id,vertexInput->vertex.generation},vertexInput->vertexOffset};
        result=vm_->captureProductGraphics(scope,withVertex);
    } else if(buffered) {
        const HackyNaviVMContext::ProductBufferedGraphicsRequest withBuffer={native,
            {buffered->fragment.id,buffered->fragment.generation},buffered->fragmentOffset};
        result=vm_->captureProductGraphics(scope,withBuffer);
    } else result=vm_->captureProductGraphics(scope,native);
    // Topology capture can fault retained device ownership before this VM has
    // been published. Schedule the same real owner-failure retirement route.
    if(result!=kIOReturnSuccess)containWorkloadFailure(scope,result);
    return completeCaptured(scope,result,sequence,deferred.value);
}
void HackyNaviClientSession::containWorkloadFailure(HackyNaviFunctionGroup::Operation& scope,IOReturn result) {
    if(result==kIOReturnSuccess||!vm_||!queue_||!group_)return;
    HackyNaviVMContext::Snapshot snapshot={};vm_->snapshot(snapshot);
    // These are actual native owner observations: publication self-hold, a
    // queue fault, or a queue-issued completed job whose readback/retirement
    // failed. A captured/unpublished VM and allocation failures do not qualify.
    if(!snapshot.requiresHardwareRetirement&&
        queue_->state()!=HackyNaviGraphicsQueue::State::FaultedRetained&&
        gart_->state()!=HackyNaviGARTController::State::FaultedRetained&&!completion_)return;
    const IOReturn requested=group_->requestOwnerFailure(scope);
    if(requested!=kIOReturnSuccess)
        panic("HackyNavi: exposed client failure lost retirement authority result=0x%x request=0x%x",
            unsigned(result),unsigned(requested));
}
IOReturn HackyNaviClientSession::completeCaptured(HackyNaviFunctionGroup::Operation& scope,IOReturn result,
    uint64_t& sequence,HackyNaviVMCompletion*& deferredRelease) {
    // Same Session mutex and real Operation/GART scope from capture through
    // readback and consumption. No caller-provided fence can grant reuse.
    auto* record=pendingResult_;
    if(result!=kIOReturnSuccess) {
        // A rejected capture can leave private controls; never reopen it.
        state_=reusable()?State::Lost:State::Sealed;
        discardResult(*record);pendingResult_=nullptr;return finish(result);
    }
    result=queue_->submitIsolated(vm_,kComputePoll,completion_);
    if(result==kIOReturnSuccess)result=completion_->readDestination(0,record->bytes,record->size);
    const uint64_t observed=result==kIOReturnSuccess?completion_->job():0;
    const uint64_t capture=result==kIOReturnSuccess?completion_->captureGeneration():0;
    if(result==kIOReturnSuccess&&(!observed||observed<=sequence_||!capture))result=kIOReturnNotReady;
    if(result==kIOReturnSuccess)result=reusable()?completion_->consumeJob(kRetirePoll):completion_->retireContext(kRetirePoll);
    if(result==kIOReturnSuccess&&!gart_->normalAccessReady())result=kIOReturnOffline;
    if(result==kIOReturnSuccess) {
        record->sequence=observed;record->captureGeneration=capture;sequence_=observed;sequence=observed;
        state_=reusable()?State::ReusableIdle:State::Sealed;
        if(reusable()){deferredRelease=completion_;completion_=nullptr;}
    } else {
        state_=reusable()?State::Lost:State::Sealed;
        containWorkloadFailure(scope,result);discardResult(*record);
    }
    pendingResult_=nullptr;return finish(result);
}
IOReturn HackyNaviClientSession::wait(uint64_t sequence,bool& complete) {
    complete=false;IOLockLock(lock_);
    if(state_==State::Closed)return finish(kIOReturnNotOpen);
    if(!findResult(sequence))return finish(kIOReturnNotFound);
    // Historical GPU completion and copied result, not a fresh hardware lease.
    complete=true;return finish(kIOReturnSuccess);
}
IOReturn HackyNaviClientSession::readResult(uint64_t sequence,uint64_t offset,void* output,size_t bytes) {
    IOLockLock(lock_);if(state_==State::Closed)return finish(kIOReturnNotOpen);
    auto* record=findResult(sequence);if(!record)return finish(kIOReturnNotFound);
    if(!output||!bytes||bytes>4096||offset>record->size||bytes>record->size-offset)return finish(kIOReturnBadArgument);
    memcpy(output,record->bytes+offset,bytes);return finish(kIOReturnSuccess);
}
IOReturn HackyNaviClientSession::releaseResult(uint64_t sequence) {
    IOLockLock(lock_);if(state_==State::Closed)return finish(kIOReturnNotOpen);
    if(!reusable())return finish(kIOReturnUnsupported);
    auto* record=findResult(sequence);if(!record)return finish(kIOReturnNotFound);
    // CPU-only even in Lost: never touch native completion, BOs or VM admission.
    discardResult(*record);return finish(kIOReturnSuccess);
}
IOReturn HackyNaviClientSession::close() {
    if(!lock_)return kIOReturnSuccess;
    IOLockLock(lock_);if(state_==State::Closed)return finish(closeResult_);
    // begin() checks admission before closing it. This mutex excludes every
    // client method; Device retirement never waits for this client mutex.
    HackyNaviFunctionGroup::Operation scope;IOReturn result=begin(scope,true);state_=State::Closed;
    const bool operationAcquired=result==kIOReturnSuccess;
    if(vm_) {
        HackyNaviVMContext::Snapshot snapshot={};vm_->snapshot(snapshot);
        if(snapshot.state==HackyNaviVMContext::State::Retired)result=kIOReturnSuccess;
        else if(!snapshot.requiresHardwareRetirement)result=vm_->releaseBeforeExposure();
        else if(result==kIOReturnSuccess&&completion_) {
            HackyNaviGARTController::Access access(*gart_,scope);
            result=access.normal()?completion_->retireContext(kRetirePoll):kIOReturnOffline;
        } else if(result==kIOReturnSuccess)result=kIOReturnNotReady;
    } else result=kIOReturnSuccess;
    // If admission was already revoked, the original exposed failure or real
    // PM callback owns retirement. Never mint a scope or wait behind it here.
    if(operationAcquired)containWorkloadFailure(scope,result);
    closeResult_=result;
    releaseResults();
    // Do not release native owners under this lock. free() drops references;
    // registry/publication holds retain any failed exposed VM independently.
    return finish(result);
}
void HackyNaviClientSession::free() {
    // A failed factory runs inside its caller's Operation: it must not acquire
    // another scope. Only unpublished CPU cleanup is attempted here. Exposed
    // contexts retain themselves/registry ownership until actual retirement.
    if(vm_) {vm_->releaseBeforeExposure();vm_->release();vm_=nullptr;}
    if(completion_){completion_->release();completion_=nullptr;}
    if(queue_){queue_->release();queue_=nullptr;}
    if(gart_){gart_->release();gart_=nullptr;}
    if(group_){group_->release();group_=nullptr;}
    releaseResults();
    if(lock_){IOLockFree(lock_);lock_=nullptr;}
    OSObject::free();
}
