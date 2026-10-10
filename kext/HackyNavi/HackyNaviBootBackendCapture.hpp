// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_NATIVE_BOOT_BACKEND_CAPTURE_HPP
#define HACKYNAVI_NATIVE_BOOT_BACKEND_CAPTURE_HPP
#include "HackyNaviFunctionGroup.hpp"
#include "hackynavi/boot_display_backend.hpp"
#include <IOKit/IOMemoryDescriptor.h>

// Synchronous observation under a genuine normal FunctionGroup operation.
// Returned object owns only immutable evidence, never PCI/DMA/display authority.
// Even success does not exclude autonomous firmware/SMM changes or authorize a
// later transition. No write, firmware command, wake, reset or mapping is made.
class HackyNaviBootBackendCapture : public OSObject {
    OSDeclareDefaultStructors(HackyNaviBootBackendCapture)
public:
    enum class Failure { None,InvalidArgument,Scope,Deadline,Pci,Mapping,Sequence };
    struct Report {
        IOReturn result;Failure failure;
        uint64_t elapsedNanoseconds,barPhysical,barBytes;
        uint32_t pciChecks,mmioReads,lastMmioRegister,lastMmioValue;
        bool classifierAttempted,lastMmioReadPerformed;
        hackynavi::bootbackend::Result classifier;
    };
    // Null only when the evidence object cannot be allocated/initialized.
    // Non-null on rejected capture too: inspect result and snapshot().valid.
    // Inputs must remain immutable for this call. timeoutUs is 1..1,000,000.
    static HackyNaviBootBackendCapture* observe(HackyNaviFunctionGroup::Operation&,
        IOPCIDevice*,IOService* openOwner,IOMemoryMap* actualBar5,
        const uint8_t* discovery,size_t discoveryBytes,const uint8_t* rom,
        size_t romBytes,size_t imageOffset,uint32_t timeoutUs,IOReturn& result);
    bool init() override;
    const Report& report() const {return report_;}
    const hackynavi::bootbackend::Capture& snapshot() const {return capture_;}
private:
    Report report_;
    hackynavi::bootbackend::Capture capture_;
    struct Access;
};
#endif
