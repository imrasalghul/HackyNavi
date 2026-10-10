// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_ACCELERATOR_HPP
#define HACKYNAVI_ACCELERATOR_HPP
#include <IOKit/graphics/IOAccelerator.h>
#include <IOKit/IOLocks.h>

// Genuine IOAccelerator child, loaded after IOGraphicsFamily is available.
// The early PCI/engine owner lives in HackyNavi.kext and has no SystemKC link.
class HackyNaviAccelerator : public IOAccelerator {
    OSDeclareDefaultStructors(HackyNaviAccelerator)
public:
    bool init(OSDictionary* properties = nullptr) override;
    bool start(IOService*) override;
    void stop(IOService*) override;
    IOReturn newUserClient(task_t, void*, UInt32, OSDictionary*, IOUserClient**) override;
    void free() override;
private:
    IOLock* ownerLock_;
    IOService* owner_;
    bool stopping_,baseStarted_;
};
#endif
