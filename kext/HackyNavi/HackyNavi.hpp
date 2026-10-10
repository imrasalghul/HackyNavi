// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_OBSERVER_HPP
#define HACKYNAVI_OBSERVER_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>

class IOPCIDevice;

// This service attaches only to IOResources. It observes PCI publication without
// becoming a GPU driver or exposing a user client.
class HackyNavi : public IOService {
    OSDeclareDefaultStructors(HackyNavi)

public:
    bool init(OSDictionary* properties = nullptr) override;
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;
    void free() override;

private:
    static bool matched(void* target, void* reference, IOService* service,
                        IONotifier* notifier);
    void capture(IOPCIDevice* device);
    void removeNotifier();

    IOLock* lock_;
    IONotifier* notifier_;
    OSDictionary* snapshots_;
    bool stopping_;
};

#endif
