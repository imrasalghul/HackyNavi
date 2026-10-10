// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>
#include "hackynavi/native_abi.hpp"
class HackyNaviDevice;
class HackyNaviClientSession;

// Bounded copied messages only. The session reacquires actual paired hardware
// authority; a connection/handle never grants MMIO or arbitrary command access.
class HackyNaviUserClient : public IOUserClient {
    OSDeclareDefaultStructors(HackyNaviUserClient)
public:
    bool initWithTask(task_t,void*,UInt32,OSDictionary*) override;
    bool start(IOService*) override;
    void stop(IOService*) override;
    void free() override;
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t,IOExternalMethodArguments*,
        IOExternalMethodDispatch* = nullptr,OSObject* = nullptr,void* = nullptr) override;
    IOReturn startResult() const {return startResult_;}
private:
    friend class HackyNaviDevice;
    bool bindAccelerator(HackyNaviDevice*,IOService*,uint64_t);
    IOReturn closeSession();
    IOReturn dispatchCopied(uint32_t,const void*,size_t,void*,size_t);
    IOLock* stateLock_;
    task_t task_;
    HackyNaviDevice* owner_;
    IOService* accelerator_;
    uint64_t acceleratorGeneration_;
    HackyNaviClientSession* session_;
    hackynavi::nativeabi::HelloReply hello_;
    IOReturn startResult_,closeResult_;
    bool negotiated_,closed_,closing_;
};
