// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_KERNEL_SDMA_QUEUE_HPP
#define HACKYNAVI_KERNEL_SDMA_QUEUE_HPP
#include "HackyNaviGARTController.hpp"
#include <hackynavi/sdma_queue.hpp>

// Kernel-backed SDMA6 queue0. The ring, pointers, fence and copy payloads are
// actual prepared DMA allocations mapped by the retained GART controller.
// No simulated backend or CPU copy fallback is selected by this class.
class HackyNaviSDMAQueue : public OSObject {
    OSDeclareDefaultStructors(HackyNaviSDMAQueue)
public:
    enum class State { Empty, Prepared, Running, FaultedRetained, Stopped };
    struct CopyTestResult {
        bool completed;
        uint32_t bytesCompared, firstMismatch;
        uint8_t expected, observed;
    };
    // Requires an active BothEnabled GART, loaded/running SDMA6 firmware,
    // powered/awake engines, and exclusive queue0/control ownership. This
    // factory does not establish firmware or power readiness. It exclusively
    // leases the controller's Data+Writeback buffers and prevents GART teardown
    // until stop or separately verified reset releases that lease.
    static HackyNaviSDMAQueue* withController(HackyNaviGARTController*,
                                             uint32_t engine, IOReturn& result);
    IOReturn start();
    // Synchronous, bounded submissions over the owned Data buffer, excluding
    // its first 4KiB command ring. Offsets are bytes within that allocation,
    // never caller-supplied GPU virtual/physical addresses. No overlapping copy.
    IOReturn copyData(uint64_t sourceOffset, uint64_t destinationOffset,
                      uint32_t bytes, const hackynavi::sdmaqueue::PollOptions&);
    IOReturn fillData(uint64_t destinationOffset, uint32_t value, uint32_t bytes,
                      const hackynavi::sdmaqueue::PollOptions&);
    // Initializes two distinct owned pages, submits a real 4KiB GPU copy,
    // waits for its device-written fence, then compares the actual DMA result.
    // This routine has not been executed on the target hardware.
    IOReturn runCopyTest(const hackynavi::sdmaqueue::PollOptions&, CopyTestResult&);
    IOReturn stop(const hackynavi::sdmaqueue::PollOptions&);
    // Bookkeeping ONLY after a separate verified reset/power-off proves the
    // GPU cannot reference these mappings. Does not itself reset the device.
    IOReturn releaseAfterDeviceReset();
    State state() const { return state_; }
    const hackynavi::sdmaqueue::Result& lastResult() const { return result_; }
protected:
    bool init() override;
    void free() override;
private:
    IOReturn initialize(HackyNaviGARTController*, uint32_t);
    bool providerReady() const;
    bool accessAllowed(uint32_t offset) const;
    bool dataRange(uint64_t offset, uint32_t bytes) const;
    IOReturn copyLocked(uint64_t source, uint64_t destination, uint32_t bytes,
                       const hackynavi::sdmaqueue::PollOptions&);
    IOReturn faultLocked();
    IOReturn endOperation(IOReturn);
    void holdHardware();
    static bool read32(void*, uint32_t, uint32_t&);
    static bool write32(void*, uint32_t, uint32_t);
    static bool publish(void*);
    static bool syncCpu(void*);
    static void delayUs(void*, uint32_t);
    hackynavi::sdmaqueue::Io io();

    IOLock* lock_;
    HackyNaviGARTController* gart_;
    uint8_t* dataCpu_;
    uint64_t dataBytes_, dataGpu_;
    bool storageLease_, hardwareHold_, mmioActive_;
    State state_;
    hackynavi::sdmaqueue::Config config_;
    hackynavi::sdmaqueue::Memory memory_;
    hackynavi::sdmaqueue::State queue_;
    hackynavi::sdmaqueue::Result result_;
};
#endif
