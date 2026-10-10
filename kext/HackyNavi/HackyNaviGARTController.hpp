// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_GART_CONTROLLER_HPP
#define HACKYNAVI_GART_CONTROLLER_HPP
#include "HackyNaviGARTStorage.hpp"
#include "hackynavi/gart.hpp"
#include "hackynavi/hdp.hpp"
#include "hackynavi/vram_transfer.hpp"
#include <libkern/c++/OSObject.h>
#include <IOKit/IOLocks.h>

class IOMemoryMap;
class HackyNaviSDMAQueue;
class HackyNaviFirmwareController;
class HackyNaviGraphicsQueue;
class HackyNaviDisplayController;
class OSData;

// Real kernel memory and MMIO integration. No simulated backend is selected by
// this class. Its caller establishes device power/quiescence and the owned
// VRAM arena; opening PCI or reading ROM metadata alone is insufficient.
class HackyNaviGARTController : public OSObject {
    OSDeclareDefaultStructors(HackyNaviGARTController)
public:
    enum class State { Empty, Prepared, Uploaded, MmEnabled, BothEnabled,
                       FaultedRetained, Retired };
    struct Config {
        hackynavi::MemoryLayout memory;
        hackynavi::VramRange ownedArena;
        uint64_t apertureStart;
        HackyNaviGARTStorage::Options storage;
        hackynavi::gart::PollOptions poll;
    };

    // Retains the already-open provider, its open owner, and its assigned
    // uncached BAR5 map in kernel_task. Copies the validated discovery input
    // for subsequent IP owners. The caller supplies a kernel map, never a user map.
    // Copies the arena/reservations into a private stable allocator. Allocates
    // and binds real DMA storage plus VRAM table/default-page ranges; no MMIO
    // occurs until upload(). Failure here returns null and unwinds allocation.
    static HackyNaviGARTController* withDevice(IOPCIDevice*, IOService* openOwner,
        IOMemoryMap* uncachedBar5, const uint8_t* discovery, size_t discoveryBytes,
        const Config&, const hackynavi::VramRange* reservations, size_t count,
        IOReturn& result);

    // All operations run in a sleepable client context, outside workloop gates
    // and interrupt/spinlock context. The lock serializes THIS controller;
    // caller excludes every other MM_INDEX, NBIO and hub user/reset/power event.
    // upload writes and reads back the table and zeroed VRAM default page.
    IOReturn upload();
    // Requires initialized memory controller/common IP, powered MMHUB,
    // enabled PCI bus mastering and quiesced clients. MMHUB can precede full
    // PSP/GC firmware initialization. PCI command bits are not changed here.
    IOReturn enableMmhub();
    // Additionally requires CP idle and completed RLC bootload, powered GFX
    // held awake, no KIQ/MES/GFXOFF path. Discovery-ready is not that proof.
    IOReturn enableGfxhub();
    IOReturn flush();

    // Caller first stops submissions and proves ALL GPU users quiescent, with
    // each enabled hub still powered and PCI decode/bus mastering still enabled.
    // Flushes/disables hubs before releasing
    // DMA mappings or VRAM ranges. A partial failure retains all resources.
    IOReturn retireAfterQuiescence();
    // This does NOT reset hardware. Only call after a separately completed
    // reset/power-off proves the GPU can no longer reference any allocation.
    IOReturn releaseAfterDeviceReset();

    // Before the first programming callback this object retains itself. That
    // hold survives both active operation and failures, and is dropped ONLY by
    // successful retirement or releaseAfterDeviceReset. Releasing a caller's
    // reference therefore cannot free live GPU mappings after a failed enable.
    // The service must also defer PCI close/domain teardown while this hold
    // exists; retaining openOwner does not prevent its stop() closing PCI.
    bool requiresHardwareRetirement() const { return hardwareHold_; }
    State state() const { return state_; }
    const hackynavi::gart::Result& lastGartResult() const { return gartResult_; }
    const hackynavi::HdpResult& lastHdpResult() const { return hdpResult_; }
    const hackynavi::VramTransferResult& lastTransferResult() const { return transferResult_; }
    // Borrowed CPU/DMA views require external lifetime/CPU-GPU synchronization;
    // valid until retirement. GPU VA is derived from the actual bound PTE index.
    const HackyNaviDMABuffer* buffer(HackyNaviGARTStorage::Region) const;
    bool gpuAddress(HackyNaviGARTStorage::Region, uint64_t& out) const;
    IOReturn synchronizeForDevice(HackyNaviGARTStorage::Region);
    IOReturn synchronizeForCPU(HackyNaviGARTStorage::Region);

protected:
    bool init() override;
    void free() override;
private:
    friend class HackyNaviSDMAQueue;
    friend class HackyNaviFirmwareController;
    friend class HackyNaviGraphicsQueue;
    friend class HackyNaviDisplayController;
    // One SDMA or graphics queue exclusively leases shared Data/Writeback.
    // Retirement cannot bypass the lease, even if its caller mistakenly asks
    // to tear GART down before stopping the producer. The queue retains us;
    // this pointer is borrowed and cleared only after verified producer stop.
    IOReturn acquireQueueStorage(OSObject* owner);
    IOReturn releaseQueueStorage(OSObject* owner);
    IOReturn publishQueueWrites(OSObject* owner);
    IOReturn initialize(IOPCIDevice*, IOService*, IOMemoryMap*, const uint8_t*, size_t,
                        const Config&, const hackynavi::VramRange*, size_t);
    IOReturn enable(hackynavi::gart::Hub);
    bool disable(hackynavi::gart::Hub);
    bool invalidate(hackynavi::gart::Hub);
    void holdHardware();
    IOReturn fault();
    IOReturn endOperation(IOReturn);
    IOReturn releaseAllocations();
    bool ioReady(uint32_t) const;
    static bool read32(void*, uint32_t, uint32_t&);
    static bool write32(void*, uint32_t, uint32_t);
    static void delayUs(void*, uint32_t);
    static bool hdpFlush(void*);
    hackynavi::gart::Io gartIo();
    hackynavi::HdpIo hdpIo();
    hackynavi::VramTransferIo transferIo();

    IOLock* lock_;
    IOPCIDevice* device_;
    IOService* openOwner_;
    OSObject* queueOwner_;
    // Firmware uses the same allocator and mutex as GART. The borrowed owner
    // retains this controller and prevents retirement while PSP/TMR is live.
    HackyNaviFirmwareController* firmwareOwner_;
    HackyNaviDisplayController* displayOwner_;
    OSData* discovery_;
    bool firmwareReady_;
    IOMemoryMap* registerMap_;
    volatile uint32_t* registers_;
    uint64_t registerBytes_;
    bool mmioActive_, hardwareHold_, mmMayBeEnabled_, gfxMayBeEnabled_;
    State state_;
    Config config_;
    hackynavi::Navi31IpLayout ipLayout_;
    hackynavi::HdpLayout hdpLayout_;
    hackynavi::HdpState hdpState_;
    hackynavi::VramAllocator allocator_;
    hackynavi::VramAllocation table_, scratch_;
    HackyNaviGARTStorage storage_;
    hackynavi::gart::Config gartConfig_;
    hackynavi::gart::Result gartResult_;
    hackynavi::HdpResult hdpResult_;
    hackynavi::VramTransferResult transferResult_;
};
#endif
