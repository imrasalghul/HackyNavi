// SPDX-License-Identifier: MIT
#include "HackyNaviUserClient.hpp"
#include "HackyNaviDevice.hpp"
#include "HackyNaviClientSession.hpp"
#include "hackynavi/gfx_aco_fixture.hpp"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSBoolean.h>
#include <kern/task.h>

OSDefineMetaClassAndStructors(HackyNaviUserClient,IOUserClient)
namespace a=hackynavi::nativeabi;
static_assert(a::kCompilerResultBytes==hackynavi::gfxaco::kDataBytes&&
    uint32_t(a::CompilerShaderClass::AppleAddOneGfx1100V1)==
    uint32_t(hackynavi::gfxaco::ShaderClass::AppleAddOneGfx1100V1),"compiler ABI identity");
namespace {
IOReturn validation(a::Validation value) {
    if(value==a::Validation::Ok)return kIOReturnSuccess;
    if(value==a::Validation::Unsupported||value==a::Validation::Version)return kIOReturnUnsupported;
    return kIOReturnBadArgument;
}
template<class T> T copied(const void* bytes) {T value={};memcpy(&value,bytes,sizeof(value));return value;}
template<class T> void output(void* bytes,const T& value) {memcpy(bytes,&value,sizeof(value));}
HackyNaviClientSession::Handle handle(a::Handle value) {return {value.id,value.generation};}
struct Message {
    void* data;size_t bytes;
    explicit Message(size_t size):data(size?IOMalloc(size):nullptr),bytes(size) {if(data)bzero(data,bytes);}
    ~Message(){if(data)IOFree(data,bytes);}
};
}

IOReturn HackyNaviDevice::newUserClient(task_t task,void* securityID,UInt32 type,
    OSDictionary* properties,IOUserClient** handler) {
    return createNativeUserClient(task,securityID,type,properties,nullptr,0,handler);
}
IOReturn HackyNaviDevice::newAcceleratorUserClient(IOService* accelerator,task_t task,
    void* securityID,UInt32 type,OSDictionary* properties,IOUserClient** handler,uint64_t& generation) {
    generation=0;
    if(!handler)return kIOReturnBadArgument;
    *handler=nullptr;
    if(type!=a::kUserClientType)return kIOReturnUnsupported;
    if(!lock_||!accelerator||!task||task!=current_task())return kIOReturnNotPermitted;
    IOLockLock(lock_);
    const bool current=accelerator_==accelerator&&acceleratorReady_&&acceleratorGeneration_&&
        engineMode_&&lifecycle_&&!captureRevoked()&&!stopping_&&powerAvailable_&&opened_&&
        !isInactive()&&!accelerator->isInactive()&&accelerator->getProvider()==this;
    const uint64_t expected=current?acceleratorGeneration_:0;
    if(current)accelerator->retain();
    IOLockUnlock(lock_);
    if(!current)return kIOReturnNotReady;
    // This snapshot is checked again under the one real normal Operation in
    // createClientSession. It cannot itself authorize a VM or engine access.
    const IOReturn result=createNativeUserClient(task,securityID,type,properties,accelerator,expected,handler);
    accelerator->release();
    if(result==kIOReturnSuccess)generation=expected;
    return result;
}
IOReturn HackyNaviDevice::createNativeUserClient(task_t task,void* securityID,UInt32 type,
    OSDictionary* properties,IOService* accelerator,uint64_t generation,IOUserClient** handler) {
    if(!handler)return kIOReturnBadArgument;
    *handler=nullptr;
    if(type!=a::kUserClientType)return kIOReturnUnsupported;
    if(!task||task!=current_task()||isInactive())return kIOReturnNotPermitted;
    auto* client=new HackyNaviUserClient;if(!client)return kIOReturnNoMemory;
    if(!client->initWithTask(task,securityID,type,properties)){client->release();return kIOReturnNoMemory;}
    if(accelerator&&!client->bindAccelerator(this,accelerator,generation)) {
        client->release();return kIOReturnNotReady;
    }
    auto* provider=accelerator?accelerator:this;
    if(!client->attach(provider)){client->release();return kIOReturnNotAttached;}
    if(!client->start(provider)) {
        const IOReturn result=client->startResult();client->detach(provider);client->release();return result;
    }
    *handler=client;return kIOReturnSuccess;
}
bool HackyNaviUserClient::initWithTask(task_t task,void* token,UInt32 type,OSDictionary* properties) {
    stateLock_=nullptr;task_=nullptr;owner_=nullptr;accelerator_=nullptr;acceleratorGeneration_=0;
    session_=nullptr;hello_={};
    startResult_=kIOReturnNotReady;closeResult_=kIOReturnSuccess;
    negotiated_=closed_=closing_=false;
    if(!task||type!=a::kUserClientType||!IOUserClient::initWithTask(task,token,type,properties))return false;
    stateLock_=IOLockAlloc();if(!stateLock_)return false;
    task_=task;task_reference(task_);
    // XNU's default external-method lock is shared. Explicitly request serial
    // IPC methods, while still protecting stop/death with our small state lock.
    return setProperty("IOUserClientDefaultLocking",kOSBooleanTrue)&&
        setProperty("IOUserClientDefaultLockingSetProperties",kOSBooleanTrue)&&
        setProperty("IOUserClientDefaultLockingSingleThreadExternalMethod",kOSBooleanTrue);
}
bool HackyNaviUserClient::bindAccelerator(HackyNaviDevice* owner,IOService* accelerator,uint64_t generation) {
    // Only the core's private factory can configure this unpublished client.
    if(!owner||!accelerator||!generation||owner_||accelerator_||session_||getProvider())return false;
    owner_=owner;owner_->retain();accelerator_=accelerator;accelerator_->retain();
    acceleratorGeneration_=generation;return true;
}
bool HackyNaviUserClient::start(IOService* provider) {
    auto* owner=accelerator_?owner_:OSDynamicCast(HackyNaviDevice,provider);
    if(!owner){startResult_=kIOReturnBadArgument;return false;}
    if(accelerator_&&(provider!=accelerator_||getProvider()!=accelerator_||
        accelerator_->getProvider()!=owner||accelerator_->isInactive())) {
        startResult_=kIOReturnOffline;return false;
    }
    if(!IOUserClient::start(provider)){startResult_=kIOReturnError;return false;}
    if(!owner_){owner_=owner;owner_->retain();}
    HackyNaviClientSession* candidate=nullptr;
    startResult_=owner_->createClientSession(candidate,accelerator_,acceleratorGeneration_);
    IOLockLock(stateLock_);
    if(startResult_==kIOReturnSuccess) {
        // A provider termination may close admission while session creation
        // blocks on the normal engine gate. Never install behind that close.
        if(!closed_&&!isInactive()&&!session_){session_=candidate;candidate=nullptr;}
        else startResult_=kIOReturnOffline;
    }
    IOLockUnlock(stateLock_);
    if(candidate){(void)candidate->close();candidate->release();}
    if(startResult_!=kIOReturnSuccess){IOUserClient::stop(provider);return false;}
    return true;
}
IOReturn HackyNaviUserClient::closeSession() {
    if(!stateLock_)return kIOReturnSuccess;
    IOLockLock(stateLock_);
    // stop/death/explicit close may overlap. Admission is closed before waiting
    // for the one close invocation; no GPU operation runs under this mutex.
    closed_=true;
    while(closing_)IOLockSleep(stateLock_,&closing_,THREAD_UNINT);
    auto* session=session_;
    if(!session){const IOReturn result=closeResult_;IOLockUnlock(stateLock_);return result;}
    closing_=true;session_=nullptr;IOLockUnlock(stateLock_);
    const IOReturn result=session->close();session->release();
    IOLockLock(stateLock_);closeResult_=result;closing_=false;
    IOLockWakeup(stateLock_,&closing_,false);IOLockUnlock(stateLock_);return result;
}
IOReturn HackyNaviUserClient::clientClose() {
    const IOReturn result=closeSession();
    if(!isInactive())terminate(); // Never hold stateLock_ across IOService callbacks.
    return result;
}
void HackyNaviUserClient::stop(IOService* provider) {
    (void)closeSession();IOUserClient::stop(provider);
}
void HackyNaviUserClient::free() {
    (void)closeSession();
    if(owner_){owner_->release();owner_=nullptr;}
    if(accelerator_){accelerator_->release();accelerator_=nullptr;}
    if(task_){task_deallocate(task_);task_=nullptr;}
    if(stateLock_){IOLockFree(stateLock_);stateLock_=nullptr;}
    IOUserClient::free();
}
IOReturn HackyNaviUserClient::externalMethod(uint32_t selector,IOExternalMethodArguments* args,
    IOExternalMethodDispatch*,OSObject*,void*) {
    if(!args||!stateLock_||!task_||current_task()!=task_)return kIOReturnNotPermitted;
    if(args->version!=kIOExternalMethodArgumentsCurrentVersion||args->selector!=selector||
        args->scalarInputCount||args->scalarOutputCount||args->asyncWakePort||args->asyncReference||args->asyncReferenceCount||
        args->structureInputDescriptor||args->structureOutputDescriptor||args->structureOutputDescriptorSize||
        args->structureVariableOutputData||!args->structureInput||!args->structureOutput||
        args->structureInputSize<sizeof(a::Header)||args->structureInputSize>a::kMaxMessage||
        args->structureOutputSize<sizeof(a::Header)||args->structureOutputSize>a::kMaxMessage)
        return kIOReturnBadArgument;
    const size_t inBytes=args->structureInputSize,outBytes=args->structureOutputSize;
    args->structureOutputSize=0;
    Message input(inBytes),reply(outBytes);
    if(!input.data||!reply.data)return kIOReturnNoMemory;
    memcpy(input.data,args->structureInput,inBytes); // The one input capture.
    const IOReturn result=dispatchCopied(selector,input.data,inBytes,reply.data,outBytes);
    if(result==kIOReturnSuccess){memcpy(args->structureOutput,reply.data,outBytes);args->structureOutputSize=uint32_t(outBytes);}
    return result;
}
IOReturn HackyNaviUserClient::dispatchCopied(uint32_t selector,const void* input,size_t inBytes,
    void* reply,size_t outBytes) {
    if(selector>=uint32_t(a::Selector::Count))return kIOReturnUnsupported;
    IOLockLock(stateLock_);
    auto* session=(!closed_&&!isInactive())?session_:nullptr;if(session)session->retain();
    const auto hello=hello_;const bool negotiated=negotiated_;
    IOLockUnlock(stateLock_);
    if(!session)return kIOReturnOffline;
    IOReturn result=kIOReturnBadArgument;
    const auto header=copied<a::Header>(input);
    if(selector==uint32_t(a::Selector::Hello)) {
        if(inBytes==sizeof(a::HelloRequest)&&outBytes==sizeof(a::HelloReply)&&!negotiated) {
            const auto request=copied<a::HelloRequest>(input);
            result=validation(a::validateHelloRequest(request,a::kCompilerCapabilities|a::kProductCapabilities|a::kVertexBufferedGraphicsCapabilities));
            if(result==kIOReturnSuccess&&request.header.minor!=a::kMinor&&
                request.header.minor!=a::kCompilerMinor&&request.header.minor!=a::kProductMinor&&
                !a::reusableProfile(request.header.minor))result=kIOReturnUnsupported;
            HackyNaviClientSession::Info info={};
            if(result==kIOReturnSuccess)result=session->information(info);
            if(result==kIOReturnSuccess) {
                a::HelloReply value={};value.header=a::makeHeader(sizeof(value),info.epoch,request.header.minor);
                const uint64_t profile=a::graphicsProfile(request.header.minor)?a::graphicsCapabilities(request.header.minor):
                    request.header.minor==a::kReusableMinor?a::kReusableCapabilities:
                    request.header.minor==a::kProductMinor?a::kProductCapabilities:
                    (request.header.minor==a::kCompilerMinor?a::kCompilerCapabilities:a::kBuiltInCapabilities);
                value.capabilities=profile;value.registryId=info.registryId;
                value.vramBytes=info.vramBytes;value.cpuVisibleVramBytes=info.cpuVisibleVramBytes;
                value.systemBudget=info.systemBudget;value.vaBegin=info.vaBegin;value.vaEnd=info.vaEnd;
                value.maxAllocationBytes=info.maxAllocationBytes;value.vendorId=0x1002;value.deviceId=0x744c;
                value.gcVersion=0x0b0000;value.pageBytes=4096;value.maxResources=HackyNaviVMContext::kBufferSlots;
                result=validation(a::validateHello(value,info.registryId,request.requiredCapabilities));
                if(result==kIOReturnSuccess)result=session->configureProfile(request.header.minor);
                IOLockLock(stateLock_);
                if(closed_||session_!=session||negotiated_)result=kIOReturnOffline;
                if(result==kIOReturnSuccess){hello_=value;negotiated_=true;output(reply,value);}
                IOLockUnlock(stateLock_);
            }
        }
        session->release();return result;
    }
    if(!negotiated){session->release();return kIOReturnNotReady;}
    result=validation(a::validateHeader(header,inBytes,hello.header.epoch,hello.header.minor));
    if(result!=kIOReturnSuccess){session->release();return result;}
    const auto basic=a::makeHeader(outBytes,hello.header.epoch,hello.header.minor);
    result=kIOReturnBadArgument;
    switch(a::Selector(selector)) {
    case a::Selector::Allocate:
        if(inBytes==sizeof(a::AllocateRequest)&&outBytes==sizeof(a::AllocateReply)) {
            const auto request=copied<a::AllocateRequest>(input);
            result=validation(a::validateAllocate(request,hello));
            if(request.alignment>UINT32_MAX)result=kIOReturnBadArgument;
            HackyNaviClientSession::Handle owned={};
            if(result==kIOReturnSuccess)result=session->allocate(request.bytes,uint32_t(request.alignment),owned);
            if(result==kIOReturnSuccess){a::AllocateReply value={};value.header=basic;
                value.handle={owned.id,owned.generation};value.bytes=request.bytes;value.domain=uint32_t(a::Domain::System);output(reply,value);}
        }break;
    case a::Selector::Free:
        if(inBytes==sizeof(a::FreeRequest)&&outBytes==sizeof(a::Header)) {
            const auto request=copied<a::FreeRequest>(input);
            if(!request.reserved&&a::validHandle(request.handle))result=session->releaseBuffer(handle(request.handle));
        }break;
    case a::Selector::MapVa:case a::Selector::UnmapVa:
        if(inBytes==sizeof(a::VaRequest)&&outBytes==sizeof(a::Header)) {
            const auto request=copied<a::VaRequest>(input);const bool unmap=a::Selector(selector)==a::Selector::UnmapVa;
            // The session checks actual allocation/mapping bounds. This validates
            // envelope/address syntax without guessing a user's allocation size.
            result=validation(a::validateVa(request,hello,UINT64_MAX,unmap));
            if(result==kIOReturnSuccess)result=unmap?
                session->unmapBuffer(handle(request.handle),request.offset,request.va,request.bytes):
                session->mapBuffer(handle(request.handle),request.offset,request.va,request.bytes,request.access);
        }break;
    case a::Selector::WriteBuffer:case a::Selector::ReadBuffer:
        if(inBytes>=sizeof(a::BufferIoRequest)) {
            const auto request=copied<a::BufferIoRequest>(input);const bool write=a::Selector(selector)==a::Selector::WriteBuffer;
            result=validation(a::validateBufferIo(request,inBytes,hello.header.epoch,write,hello.header.minor));
            if(outBytes!=(write?sizeof(a::Header):sizeof(a::DataReply)+request.bytes))result=kIOReturnBadArgument;
            if(result==kIOReturnSuccess)result=write?
                session->writeBuffer(handle(request.handle),request.offset,static_cast<const uint8_t*>(input)+sizeof(request),request.bytes):
                session->readBuffer(handle(request.handle),request.offset,static_cast<uint8_t*>(reply)+sizeof(a::DataReply),request.bytes);
            if(result==kIOReturnSuccess&&!write){a::DataReply value={basic,request.bytes,0};output(reply,value);}
        }break;
    case a::Selector::Compute:
        if(inBytes==sizeof(a::ComputeRequest)&&outBytes==sizeof(a::SubmitReply)) {
            const auto request=copied<a::ComputeRequest>(input);result=validation(a::validateCompute(request,hello.header.epoch,hello.header.minor));
            HackyNaviClientSession::ComputeRequest native={};
            native.operation=request.operation==uint32_t(a::ComputeOperation::Clear)?
                hackynavi::gfxcompute::Operation::Clear:hackynavi::gfxcompute::Operation::Copy;
            native.source=handle(request.source);native.destination=handle(request.destination);
            native.sourceOffset=request.sourceOffset;native.destinationOffset=request.destinationOffset;native.bytes=request.bytes;
            for(unsigned i=0;i<4;++i)native.pattern[i]=request.pattern[i];
            uint64_t sequence=0;if(result==kIOReturnSuccess)result=session->compute(native,sequence);
            if(result==kIOReturnSuccess){a::SubmitReply value={basic,sequence,0};output(reply,value);}
        }break;
    case a::Selector::Wait:
        if(inBytes==sizeof(a::WaitRequest)&&outBytes==sizeof(a::WaitReply)) {
            const auto request=copied<a::WaitRequest>(input);
            if(request.sequence&&request.timeoutNs<=a::kMaxWaitNs) {
                bool complete=false;result=session->wait(request.sequence,complete);
                if(result==kIOReturnSuccess){a::WaitReply value={basic,request.sequence,complete?request.sequence:0,
                    uint32_t(complete?a::FenceState::Complete:a::FenceState::Pending),0};output(reply,value);}
            }
        }break;
    case a::Selector::ReadResult:
        if(inBytes==sizeof(a::ReadResultRequest)) {
            const auto request=copied<a::ReadResultRequest>(input);result=validation(a::validateReadResult(request,hello.header.epoch,hello.header.minor));
            if(outBytes!=sizeof(a::DataReply)+request.bytes)result=kIOReturnBadArgument;
            if(result==kIOReturnSuccess)result=session->readResult(request.sequence,request.offset,
                static_cast<uint8_t*>(reply)+sizeof(a::DataReply),request.bytes);
            if(result==kIOReturnSuccess){a::DataReply value={basic,request.bytes,0};output(reply,value);}
        }break;
    case a::Selector::ReleaseResult:
        if(inBytes==sizeof(a::ReleaseResultRequest)&&outBytes==sizeof(a::Header)) {
            const auto request=copied<a::ReleaseResultRequest>(input);
            result=validation(a::validateReleaseResult(request,hello));
            if(result==kIOReturnSuccess)result=session->releaseResult(request.sequence);
        }break;
    case a::Selector::Close:
        if(inBytes==sizeof(a::Header)&&outBytes==sizeof(a::Header))result=closeSession();
        break;
    case a::Selector::CompilerCompute:
        if(inBytes==sizeof(a::CompilerComputeRequest)&&outBytes==sizeof(a::SubmitReply)) {
            const auto request=copied<a::CompilerComputeRequest>(input);
            result=validation(a::validateCompilerCompute(request,hello));
            HackyNaviClientSession::CompilerComputeRequest native={handle(request.source),handle(request.destination),
                request.sourceOffset,request.destinationOffset};
            uint64_t sequence=0;if(result==kIOReturnSuccess)result=session->computeCompiler(native,sequence);
            if(result==kIOReturnSuccess){a::SubmitReply value={basic,sequence,0};output(reply,value);}
        }break;
    case a::Selector::ProductCompute:
        if(outBytes==sizeof(a::SubmitReply)) {
            Message decoded(sizeof(a::CapturedProductCompute));
            if(!decoded.data){result=kIOReturnNoMemory;break;}
            auto& request=*static_cast<a::CapturedProductCompute*>(decoded.data);
            result=validation(a::decodeProductCompute(input,inBytes,hello,request));
            uint64_t sequence=0;
            if(result==kIOReturnSuccess)result=session->computeProduct(request,sequence);
            if(result==kIOReturnSuccess){a::SubmitReply value={basic,sequence,0};output(reply,value);}
        }break;
    case a::Selector::GraphicsInformation:
        if(inBytes==sizeof(a::Header)&&outBytes==sizeof(a::GraphicsInformationReply)&&
            a::graphicsProfile(hello.header.minor)) {
            a::GraphicsInformationReply value={};value.header=basic;
            result=session->graphicsInformation(value.topology);
            if(result==kIOReturnSuccess)output(reply,value);
        }break;
    case a::Selector::ProductBufferedGraphics:
        if(outBytes==sizeof(a::SubmitReply)) {
            a::ProductBufferedGraphicsRequest request={};
            result=validation(a::decodeProductBufferedGraphics(input,inBytes,hello,request));
            uint64_t sequence=0;
            if(result==kIOReturnSuccess)result=session->graphicsProduct(request,sequence);
            if(result==kIOReturnSuccess){a::SubmitReply value={basic,sequence,0};output(reply,value);}
        }break;
    case a::Selector::ProductVertexBufferedGraphics:
        if(outBytes==sizeof(a::SubmitReply)) {
            a::ProductVertexBufferedGraphicsRequest request={};
            result=validation(a::decodeProductVertexBufferedGraphics(input,inBytes,hello,request));
            uint64_t sequence=0;
            if(result==kIOReturnSuccess)result=session->graphicsProduct(request,sequence);
            if(result==kIOReturnSuccess){a::SubmitReply value={basic,sequence,0};output(reply,value);}
        }break;
    case a::Selector::ProductVertexFragmentBufferedGraphics:
        if(outBytes==sizeof(a::SubmitReply)) {
            a::ProductVertexFragmentBufferedGraphicsRequest request={};
            result=validation(a::decodeProductVertexFragmentBufferedGraphics(input,inBytes,hello,request));
            uint64_t sequence=0;
            if(result==kIOReturnSuccess)result=session->graphicsProduct(request,sequence);
            if(result==kIOReturnSuccess){a::SubmitReply value={basic,sequence,0};output(reply,value);}
        }break;
    case a::Selector::ProductGraphics:
        if(outBytes==sizeof(a::SubmitReply)) {
            a::ProductGraphicsRequest request={};
            result=validation(a::decodeProductGraphics(input,inBytes,hello,request));
            uint64_t sequence=0;
            if(result==kIOReturnSuccess)result=session->graphicsProduct(request,sequence);
            if(result==kIOReturnSuccess){a::SubmitReply value={basic,sequence,0};output(reply,value);}
        }break;
    case a::Selector::Submit:result=kIOReturnUnsupported;break;
    default:result=kIOReturnUnsupported;break;
    }
    if(result==kIOReturnSuccess&&outBytes==sizeof(a::Header))output(reply,basic);
    session->release();return result;
}
