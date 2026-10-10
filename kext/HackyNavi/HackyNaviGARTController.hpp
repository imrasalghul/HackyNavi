// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_GART_CONTROLLER_HPP
#define HACKYNAVI_GART_CONTROLLER_HPP
#include "HackyNaviGARTStorage.hpp"
#include "HackyNaviResetController.hpp"
#include "hackynavi/gart.hpp"
#include "hackynavi/hdp.hpp"
#include "hackynavi/vram_transfer.hpp"
#include "hackynavi/gfx11_color_topology.hpp"
#include "hackynavi/gfx11_graphics_topology.hpp"
#include <libkern/c++/OSObject.h>
#include <IOKit/IOLocks.h>

class IOMemoryMap;
class HackyNaviSDMAQueue;
class HackyNaviFirmwareController;
class HackyNaviGraphicsQueue;
class HackyNaviDisplayController;
class HackyNaviScanoutSurface;
class HackyNaviVMContext;
class OSData;

// Real kernel memory and MMIO integration. No simulated backend is selected by
// this class. Its caller establishes device power/quiescence and the owned
// VRAM arena; opening PCI or reading ROM metadata alone is insufficient.
class HackyNaviGARTController : public OSObject {
    OSDeclareDefaultStructors(HackyNaviGARTController)
public:
    // Lexical authority for every descendant hardware/DMA access. The actual
    // FunctionGroup scope owns the outer gate; this binding takes no mutex.
    // Never nest guards on the same controller or let one outlive its scope.
    class Access {
        friend class HackyNaviGARTController;
    public:
        Access(HackyNaviGARTController&, HackyNaviFunctionGroup::Operation&);
        Access(HackyNaviGARTController&, HackyNaviFunctionGroup::RetirementScope&);
        // Partial startup can fail before a GART exists. Null remains invalid;
        // it never grants access to an owner or substitutes retirement proof.
        Access(HackyNaviGARTController*, HackyNaviFunctionGroup::RetirementScope&);
        ~Access();
        bool valid() const;
        bool normal() const;
        bool retiring(const HackyNaviFunctionGroup::RetirementScope&) const;
    private:
        Access(const Access&)=delete;
        Access& operator=(const Access&)=delete;
        void bind(HackyNaviGARTController&);
        HackyNaviGARTController* gart_;
        HackyNaviFunctionGroup::Operation* operation_;
        HackyNaviFunctionGroup::RetirementScope* retirement_;
        thread_t thread_;
    };
    bool accessReady() const;
    bool normalAccessReady() const;
    // The bound lexical guard must contain this exact issued scope, not just
    // another valid-looking retirement for matching PCI providers.
    bool retirementAccessReady(const HackyNaviFunctionGroup::RetirementScope&) const;
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
    // factoryTeardownFailure separately reports failed cleanup; the parent must
    // preserve that terminal DMA-domain error after this factory returns null.
    static HackyNaviGARTController* withDevice(HackyNaviFunctionGroup::Operation&, IOPCIDevice*, IOService* openOwner,
        IOMemoryMap* uncachedBar5, const uint8_t* discovery, size_t discoveryBytes,
        const Config&, const hackynavi::VramRange* reservations, size_t count,
        IOReturn& result, IOReturn* factoryTeardownFailure = nullptr);

    // All operations run in a sleepable client context, outside workloop gates
    // and interrupt/spinlock context. The lock serializes THIS controller;
    // caller excludes every other MM_INDEX, NBIO and hub user/reset/power event.
    // upload writes and reads back the table and zeroed VRAM default page.
    IOReturn upload();
    // Requires initialized memory controller/common IP, powered MMHUB,
    // disabled PCI bus mastering and quiesced clients. The CPU installs the
    // owned mapping before the outer owner enables BME and starts PSP/GC
    // firmware. PCI command bits are not changed by this controller.
    IOReturn enableMmhub();
    // Requires PCI bus mastering enabled, CP idle and completed RLC bootload, powered GFX
    // held awake, no KIQ/MES/GFXOFF path. Discovery-ready is not that proof.
    IOReturn enableGfxhub();
    IOReturn flush();

    // One-shot live topology capture, not an automatic startup action. Caller
    // installs the normal Access for this exact Operation before taking any
    // descendant mutex. Firmware/GC must already be ready and BothEnabled.
    // Two complete matching PCI/GB/NBIO snapshots are retained privately with
    // this controller's creation generation. Numeric output is diagnostic only.
    // Failed attempted reads/selector writes seal this GART; no retry/rollback.
    IOReturn captureColorTopology(HackyNaviFunctionGroup::Operation&,
        uint64_t absoluteDeadlineUs, HNColorTopology& diagnostic);
    IOReturn copyColorTopology(HackyNaviFunctionGroup::Operation&, HNColorTopology& diagnostic);
    // Extends this same owner's completed color capture with firmware-declared
    // GC maxima and two observed harvested-mask sweeps. No automatic caller.
    // GRBM selector attempts/restoration remain privately journaled; a failed
    // attempted call is terminal and performs no best-effort cleanup writes.
    IOReturn captureGraphicsTopology(HackyNaviFunctionGroup::Operation&,
        uint64_t absoluteDeadlineUs, hackynavi::graphicstopology::Facts& diagnostic);
    IOReturn copyGraphicsTopology(HackyNaviFunctionGroup::Operation&,
        hackynavi::graphicstopology::Facts& diagnostic);

    // Caller first stops submissions and proves ALL GPU users quiescent, with
    // each enabled hub still powered and PCI decode/bus mastering still enabled.
    // Flushes/disables hubs before releasing
    // DMA mappings or VRAM ranges. A partial failure retains all resources.
    IOReturn retireAfterQuiescence();
    // Terminal VM registry transfer. Validates this exact provider/open owner/
    // BAR map against the reset issuer, permanently closes VM admission, and
    // retires every registered context through the SAME typed proof. The caller
    // holds the outer device retirement gate through all releases and the reset
    // owner's finishRetirementTransfer(). This never infers DMA retirement from
    // a completed job, disabled VM context or empty queue.
    IOReturn retireVmContextsAfterVerifiedReset(const HackyNaviResetController::RetirementProof&);
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
    friend class HackyNaviScanoutSurface;
    friend class HackyNaviVMContext;
    // One SDMA or graphics queue exclusively leases shared Data/Writeback.
    // Retirement cannot bypass the lease, even if its caller mistakenly asks
    // to tear GART down before stopping the producer. The queue retains us;
    // this pointer is borrowed and cleared only after verified producer stop.
    IOReturn acquireQueueStorage(OSObject* owner);
    IOReturn releaseQueueStorage(OSObject* owner);
    IOReturn publishQueueWrites(OSObject* owner);
    IOReturn initialize(HackyNaviFunctionGroup::Operation&, IOPCIDevice*, IOService*, IOMemoryMap*, const uint8_t*, size_t,
                        const Config&, const hackynavi::VramRange*, size_t);
    IOReturn enable(hackynavi::gart::Hub);
    bool disable(hackynavi::gart::Hub);
    bool invalidate(hackynavi::gart::Hub);
    void holdHardware();
    IOReturn fault();
    IOReturn endOperation(IOReturn);
    IOReturn releaseAllocations();
    IOReturn synchronizeDeviceLocked(HackyNaviGARTStorage::Region);
    IOReturn synchronizeCpuLocked(HackyNaviGARTStorage::Region);
    IOReturn synchronizeDeviceBuffer(HackyNaviDMABuffer&);
    IOReturn synchronizeCpuBuffer(HackyNaviDMABuffer&);
    // Shared mutex held. Preserve the first VM or GART DMA/page-storage
    // teardown error even after direct release/failed factory cleanup empties
    // the registry and allocation ledger. Terminal for this GART epoch.
    void latchVmStorageFailureLocked(IOReturn);
    bool hasVmOwners() const; // caller holds lock_; VMID1..7 leases
    bool ioReady(uint32_t) const;
    static bool read32(void*, uint32_t, uint32_t&);
    static bool write32(void*, uint32_t, uint32_t);
    static void delayUs(void*, uint32_t);
    static bool hdpFlush(void*);
    hackynavi::gart::Io gartIo();
    hackynavi::HdpIo hdpIo();
    hackynavi::VramTransferIo transferIo();
    bool colorTopologyReady(const HackyNaviFunctionGroup::Operation&) const;
    bool colorTopologyIoReady() const;
    static bool colorTopologyIdentity(void*, hackynavi::colortopology::Identity&);
    static bool colorTopologyRead(void*, uint32_t, uint32_t&);
    static bool colorTopologyWrite(void*, uint32_t, uint32_t);
    bool graphicsTopologyIoReady() const;
    static bool graphicsTopologyIdentity(void*, hackynavi::colortopology::Identity&);
    static bool graphicsTopologyRead(void*, uint32_t, uint32_t&);
    static bool graphicsTopologyWrite(void*, uint32_t, uint32_t);

    IOLock* lock_;
    Access* access_;
    thread_t accessThread_;
    IOPCIDevice* device_;
    IOService* openOwner_;
    OSObject* queueOwner_;
    // Firmware uses the same allocator and mutex as GART. The borrowed owner
    // retains this controller and prevents retirement while PSP/TMR is live.
    HackyNaviFirmwareController* firmwareOwner_;
    HackyNaviDisplayController* displayOwner_;
    // Each registry entry strongly retains its VM, which retains us. The SAME
    // lock protects leasing, page trees/BOs and shared invalidate engine17.
    // Explicit pre-exposure release or typed reset retirement breaks the
    // ownership cycle; final client release cannot race snapshot acquisition.
    // A lease survives context disable/ACK until actual resource retirement.
    HackyNaviVMContext* vmOwners_[7];
    bool vmAdmissionClosed_, vmRetirementActive_;
    IOReturn vmRetirementFailure_;
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
    // Serialized by lock_: preparation resets this single-use workspace before
    // each synchronous execution. Keep its ~2 KiB step array off kernel stack.
    hackynavi::gart::Plan gartPlan_;
    hackynavi::HdpResult hdpResult_;
    hackynavi::VramTransferResult transferResult_;
    // Private provenance. No numeric diagnostic or public portable layout can
    // populate this record or grant a future VM color-target lease.
    uint64_t ownerGeneration_, colorTopologyGeneration_, colorTopologyDeadline_;
    HackyNaviFunctionGroup::Operation* colorTopologyScope_;
    bool colorTopologyAttempted_, colorTopologyValid_;
    hackynavi::colortopology::Layout colorTopologyLayout_;
    hackynavi::colortopology::Snapshot colorTopologySnapshot_;
    hackynavi::colortopology::Trace colorTopologyTrace_;
    hackynavi::colortopology::Status colorTopologyStatus_;
    uint64_t graphicsTopologyGeneration_,graphicsTopologyDeadline_;
    HackyNaviFunctionGroup::Operation* graphicsTopologyScope_;
    bool graphicsTopologyAttempted_,graphicsTopologyValid_;
    uint8_t graphicsTopologyPhase_; // 1 color-before, 2 harvesting, 3 color-after
    hackynavi::graphicstopology::Metadata graphicsTopologyMetadata_;
    hackynavi::graphicstopology::Layout graphicsTopologyLayout_;
    hackynavi::graphicstopology::Snapshot graphicsTopologySnapshot_;
    hackynavi::graphicstopology::Trace graphicsTopologyTrace_;
    hackynavi::graphicstopology::Status graphicsTopologyStatus_;
    hackynavi::colortopology::Snapshot graphicsTopologyColor_[2];
    hackynavi::colortopology::Trace graphicsTopologyColorTrace_[2];
};
#endif
