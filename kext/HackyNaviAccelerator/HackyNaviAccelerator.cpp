// SPDX-License-Identifier: MIT
#include "HackyNaviAccelerator.hpp"
#include "../HackyNavi/HackyNaviAcceleratorProtocol.hpp"
#include <hackynavi/native_abi.hpp>
#include <IOKit/IOUserClient.h>
#include <kern/task.h>
#include <libkern/libkern.h>

OSDefineMetaClassAndStructors(HackyNaviAccelerator, IOAccelerator)

bool HackyNaviAccelerator::init(OSDictionary* properties) {
    ownerLock_=nullptr;owner_=nullptr;stopping_=baseStarted_=false;
    if(!IOAccelerator::init(properties))return false;
    ownerLock_=IOLockAlloc();return ownerLock_!=nullptr;
}

bool HackyNaviAccelerator::start(IOService* provider) {
    using namespace hackynavi::acceleratorbridge;
    if (!provider || !ownerLock_ || getProvider() != provider || provider->isInactive() ||
        strcmp(provider->getMetaClass()->getClassName(), "HackyNaviDevice")) return false;
    IOLockLock(ownerLock_);
    const bool initial=!owner_&&!stopping_&&!baseStarted_;
    IOLockUnlock(ownerLock_);
    if(!initial||!IOAccelerator::start(provider))return false;
    IOLockLock(ownerLock_);
    baseStarted_=true;
    const bool adopt=!owner_&&!stopping_;
    if(adopt){owner_=provider;owner_->retain();}
    IOLockUnlock(ownerLock_);
    if(!adopt){stop(provider);return false;}
    const UInt64 ownerID = provider->getRegistryEntryID();
    const bool propertiesReady = ownerID &&
        setProperty("HackyNaviOwnerRegistryID", ownerID, 64) &&
        setProperty("MetalPluginName", "HackyNaviMetal") &&
        setProperty("MetalPluginClassName", "HNMetalDevice");
    Request request = {sizeof(Request), kVersion, this, ownerID, 0};
    // The core alone registers this exact child while a fresh normal engine
    // Operation excludes retirement. All child state is ready before that
    // synchronous call; no matching-property read is used as authority.
    const IOReturn result = propertiesReady ?
        provider->callPlatformFunction(kPublish, false, &request, nullptr, nullptr, nullptr) :
        kIOReturnNoMemory;
    if (result != kIOReturnSuccess) {
        stop(provider);
        return false;
    }
    return true;
}

void HackyNaviAccelerator::stop(IOService* provider) {
    using namespace hackynavi::acceleratorbridge;
    IOService* owner=nullptr;bool base=false;
    if(ownerLock_) {
        IOLockLock(ownerLock_);
        stopping_=true;owner=owner_;owner_=nullptr;
        base=baseStarted_;baseStarted_=false;
        IOLockUnlock(ownerLock_);
    }
    if (owner) {
        Request request = {sizeof(Request), kVersion, this, owner->getRegistryEntryID(), 0};
        // Withdrawal is pointer-identity bookkeeping, not a hardware request.
        // It must not wait for a normal Operation during owner retirement.
        owner->callPlatformFunction(kWithdraw, false, &request, nullptr, nullptr, nullptr);
        owner->release();
    }
    if(base)IOAccelerator::stop(provider);
}

IOReturn HackyNaviAccelerator::newUserClient(task_t task, void* securityID, UInt32 type,
    OSDictionary* properties, IOUserClient** out) {
    using namespace hackynavi::acceleratorbridge;
    if (!out) return kIOReturnBadArgument;
    *out = nullptr;
    if(type!=hackynavi::nativeabi::kUserClientType)return kIOReturnUnsupported;
    if(!task||task!=current_task()||!ownerLock_||isInactive())return kIOReturnNotPermitted;
    IOLockLock(ownerLock_);
    auto* owner=!stopping_&&baseStarted_?owner_:nullptr;
    if(owner)owner->retain();
    IOLockUnlock(ownerLock_);
    if(!owner)return kIOReturnOffline;
    OpenRequest request={{sizeof(OpenRequest),kVersion,this,owner->getRegistryEntryID(),0},
        task,securityID,properties,nullptr,type,0};
    // Only the caller's real task and the native HNV1 ABI are forwarded. No
    // private Apple accelerator clients, second VM owner or nested engine
    // Operation is introduced here. The core attaches its actual user client
    // to this accelerator and checks the publication generation during start.
    IOReturn result=getProvider()==owner&&!isInactive()?
        owner->callPlatformFunction(kOpen,false,&request,nullptr,nullptr,nullptr):kIOReturnOffline;
    owner->release();
    if(result==kIOReturnSuccess&&(!request.client||!request.identity.generation))result=kIOReturnError;
    if(result==kIOReturnSuccess)*out=request.client;
    else if(request.client){request.client->clientClose();request.client->release();}
    return result;
}

void HackyNaviAccelerator::free() {
    if (owner_) { owner_->release(); owner_ = nullptr; }
    if(ownerLock_){IOLockFree(ownerLock_);ownerLock_=nullptr;}
    IOAccelerator::free();
}
