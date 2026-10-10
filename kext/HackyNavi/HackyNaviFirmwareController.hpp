// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_KERNEL_FIRMWARE_CONTROLLER_HPP
#define HACKYNAVI_KERNEL_FIRMWARE_CONTROLLER_HPP
#include "HackyNaviGARTController.hpp"
#include <hackynavi/firmware_resources.hpp>
#include <hackynavi/psp.hpp>
#include <hackynavi/psp_ring.hpp>
#include <hackynavi/gfx_firmware.hpp>
#include <hackynavi/smu.hpp>
#include <hackynavi/dmcub_firmware.hpp>

// Real kernel PSP13 adapter. Uses embedded AMD firmware, owned allocations in
// the GART controller's SAME arena, uncached BAR5 and actual PSP responses.
// No userspace resource service or synthetic completion path is involved.
class HackyNaviFirmwareController : public OSObject {
    OSDeclareDefaultStructors(HackyNaviFirmwareController)
public:
    enum class State { Empty, Prepared, SosLoaded, RingReady, TmrReady, FirmwareLoaded, Quiesced,
                       FailedUnpublished, FaultedRetained, Retired };
    // Requires initialized MMHUB/HDP on this driver-owned, powered device,
    // cold PSP bootloader state, valid firmware/display/training reservations,
    // stopped CP/SDMA and caller-wide exclusion of suspend/reset/removal.
    // Factory only allocates CPU/VRAM bookkeeping; it makes no hardware writes.
    static HackyNaviFirmwareController* withController(HackyNaviGARTController*, IOReturn&);
    IOReturn loadSos(const hackynavi::psp::Options&);
    IOReturn createRing(const hackynavi::pspring::PollOptions&);
    IOReturn queryBootConfiguration(const hackynavi::pspring::PollOptions&, uint32_t&);
    // Uploads the real SOS TOC, obtains the PSP-reported TMR size, allocates
    // owned aligned VRAM and waits for SETUP_TMR's real fence and response.
    IOReturn initializeTmr(const hackynavi::pspring::PollOptions&);
    // Real ordered SMU/SDMA/GC firmware uploads and PSP authentication/load,
    // RLC autoload command and CP/bootload polling. Explicit non-MES profile
    // for revision C8. Success permits GFXHUB setup; CP queues follow later.
    IOReturn loadGraphicsFirmware(const hackynavi::pspring::PollOptions&);
    bool loadedFirmware(uint32_t pspType, hackynavi::gfxfw::LoadedSegment&);
    // Requires every GPU consumer of firmware/TMR already quiesced. Refuses
    // while the SDMA owner exists. Sends DESTROY_TMR and DESTROY_RINGS before
    // releasing their buffers. Faulted hardware needs explicit reset recovery.
    IOReturn retireAfterQuiescence(const hackynavi::pspring::PollOptions&);
    // This does not reset hardware; call only after separately verified reset
    // or power removal proves all PSP/engine references have retired.
    IOReturn releaseAfterDeviceReset();
    State state() const { return state_; }
    const hackynavi::psp::Result& lastSosResult() const { return sosResult_; }
    const hackynavi::pspring::Result& lastRingResult() const { return ringResult_; }
    const hackynavi::gfxfw::Result& lastGraphicsResult() const { return graphicsResult_; }
    const hackynavi::smu::Result& lastSmuResult() const { return smuResult_; }
    const hackynavi::dmcubfw::Result& lastDisplayFirmwareResult() const { return displayFirmwareResult_; }
    bool requiresHardwareRetirement() const { return hardwareHold_; }
protected:
    bool init() override;
    void free() override;
private:
    friend class HackyNaviGraphicsQueue;
    friend class HackyNaviDisplayController;
    enum Slot { SosStage, Ring, Command, Fence, Payload, Tmr, SlotCount };
    IOReturn initialize(HackyNaviGARTController*);
    bool providerReady() const;
    // Lock order is this mutex, then GART mutex. Held for whole protocols and
    // their MM_INDEX window use; callbacks never recursively acquire either.
    IOReturn begin(State);
    IOReturn finish(IOReturn);
    IOReturn fault();
    void holdHardware();
    IOReturn releaseAllocationsLocked();
    hackynavi::psp::Io sosIo();
    hackynavi::pspring::Io ringIo();
    static bool read32(void*, uint32_t, uint32_t&);
    static bool write32(void*, uint32_t, uint32_t);
    static bool flushHdp(void*);
    static bool beforeReadback(void*);
    static void delayUs(void*, uint32_t);
    static void delayMs(void*, uint32_t);

    IOLock* lock_;
    HackyNaviGARTController* gart_;
    uint8_t* workspace_;
    bool lease_, hardwareHold_;
    State state_;
    hackynavi::VramAllocation slots_[SlotCount];
    hackynavi::FirmwareResource sosResource_;
    hackynavi::psp::SosImage sosImage_;
    hackynavi::psp::Layout sosLayout_;
    hackynavi::pspring::Layout ringLayout_;
    hackynavi::psp::State sos_;
    hackynavi::pspring::State ring_;
    hackynavi::psp::Result sosResult_;
    hackynavi::pspring::Result ringResult_;
    uint8_t pciRevision_;
    hackynavi::gfxfw::Files graphicsFiles_;
    hackynavi::gfxfw::Plan graphicsPlan_;
    hackynavi::gfxfw::State graphics_;
    hackynavi::gfxfw::Result graphicsResult_;
    hackynavi::smu::Layout smuLayout_;
    hackynavi::smu::State smu_;
    hackynavi::smu::Result smuResult_;
    hackynavi::FirmwareResource displayResource_;
    hackynavi::dmcubfw::State displayFirmware_;
    hackynavi::dmcubfw::Result displayFirmwareResult_;
};
#endif
