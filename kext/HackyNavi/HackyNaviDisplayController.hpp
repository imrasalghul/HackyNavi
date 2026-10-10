// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_KERNEL_DISPLAY_CONTROLLER_HPP
#define HACKYNAVI_KERNEL_DISPLAY_CONTROLLER_HPP
#include "HackyNaviFirmwareController.hpp"
#include <hackynavi/dmcub.hpp>
#include <hackynavi/dp_aux.hpp>
#include <hackynavi/hdmi_ddc.hpp>
#include <hackynavi/hdmi_link.hpp>
#include <hackynavi/dp_training.hpp>
#include <hackynavi/edid_acquire.hpp>

// Kernel owner of real DMCUB runtime DMA and physical display transports.
// Caller holds device-wide display/power/reset/removal exclusion, establishes
// DCN clocks, and withdraws inherited display clients before boot. No user
// client is exposed here. Firmware command consumption is not a modeset fence.
class HackyNaviDisplayController : public OSObject {
    OSDeclareDefaultStructors(HackyNaviDisplayController)
public:
    enum class State { Empty, Prepared, Ready, FaultedRetained, Retired };
    // Copies and revalidates the selected legacy image and its PCI subsystem
    // identity, builds connector routes, leases dedicated Display GART storage.
    // No hardware writes. Firmware must have completed PSP DMUB51 loading.
    static HackyNaviDisplayController* withFirmware(HackyNaviFirmwareController*,
        const uint8_t* rom, size_t bytes, const hackynavi::ImageInfo&, IOReturn&);
    IOReturn boot(const hackynavi::dmcubfw::PollOptions&);
    bool route(uint8_t path, hackynavi::dmcub::Route&);
    uint8_t pathCount() const { return paths_; }
    IOReturn pixelClock(uint8_t path, const hackynavi::dmcub::PixelClock&,
        const hackynavi::dmcub::PollOptions&);
    IOReturn setupEncoder(uint8_t path, const hackynavi::dmcub::Encoder&,
        const hackynavi::dmcub::PollOptions&);
    // Source PLL and DCN clocks must already be programmed by the modeset
    // owner. Opens actual ROM-selected AUX, prepares DIO and trains against
    // real receiver lane status. Success leaves a trained idle DP link;
    // pixel stream/OTG/scanout activation follows in the modeset owner.
    IOReturn trainDp(uint8_t path, uint8_t frontend, const hackynavi::dptrain::Config&,
        const hackynavi::dmcub::PollOptions&, hackynavi::dptrain::HardwareResult&);
    // The modeset owner first stops the pixel stream. Powers down the DP sink
    // where reachable, disables the source and confirms DIG_ENABLE is clear.
    IOReturn disableDp(uint8_t path, const hackynavi::dmcub::PollOptions&);
    // The modeset owner programs PLL/depth, DCN clocks and stream timing
    // first, with this frontend's FIFO and OTG/pixel stream stopped. Actual
    // ROM-routed DDC/SCDC, encoder setup and PHY enable/readback follow here.
    // Config contains validated EDID/board limits; clock is the board's ATOM
    // I2C reference. Success leaves AVMUTE asserted and FIFO off, not scanout.
    IOReturn enableHdmi(uint8_t path, uint8_t frontend,
        const hackynavi::hdmiddc::Clock&, const hackynavi::hdmitmds::Config&,
        const hackynavi::dmcub::PollOptions&, hackynavi::hdmitmds::HardwareResult&);
    // Stop stream/OTG/FIFO first. Uses the saved route/DDC clock and SCDC
    // capability from enableHdmi; an unreachable sink is reported in out,
    // independently of confirmed local PHY retirement. No new EDID guesses.
    IOReturn disableHdmi(uint8_t path, const hackynavi::dmcub::PollOptions&,
        hackynavi::hdmitmds::HardwareResult&);
    // One serialized batch on the ROM-selected AUX engine. The final request
    // must end any MOT chain; real STOP/release follows even a protocol NACK.
    // Results retain the actual wire reply; ambiguous I/O retains ownership.
    IOReturn transferAux(uint8_t path, const hackynavi::dpaux::Request*, size_t count,
        hackynavi::dpaux::Result* results);
    // Clock.crystalKHz must come from this board's ATOM clock table. Complete
    // repeated-START messages and final STOP use the actual shared I2C engine.
    IOReturn transferHdmi(uint8_t path, const hackynavi::hdmiddc::Clock&,
        const hackynavi::hdmiddc::Message*, size_t count, hackynavi::hdmiddc::Result&);
    IOReturn readHdmiEdid(uint8_t path, const hackynavi::hdmiddc::Clock&, uint8_t block,
        hackynavi::hdmiddc::Result&);
    // Reads and rechecks every effective EDID block through this ROM path's
    // physical DDC or I2C-over-AUX engine. The HDMI clock is ignored for DP.
    // raw must initially be null; on success the caller owns the returned
    // OSData reference. raw/monitor stay unchanged on any error, including
    // failed pin release. No partial capture is published as complete EDID.
    IOReturn readEdid(uint8_t path, const hackynavi::hdmiddc::Clock&,
        OSData*& raw, hackynavi::edid::Monitor&, hackynavi::edidacq::HardwareResult&);
    // The modeset owner first stops every driver-created pixel stream. This
    // closes owned AUX/DDC and disables owned DIO links before retiring DMCUB.
    // Inherited boot scanout must be blanked/drained by its separate owner;
    // downstream inherited OTG/PHY state is not a DMCUB DMA reference.
    // Stops local transports, requests STOP_FW, observes PWAIT, asserts reset
    // and disables runtime windows before releasing the Display lease.
    IOReturn stop(const hackynavi::dmcubfw::PollOptions&);
    // Performs no reset: caller must have independently verified full device
    // reset/power removal, including physical display engines and DMA.
    IOReturn releaseAfterDeviceReset();
    State state() const { return state_; }
    bool requiresHardwareRetirement() const { return hardwareHold_; }
    const hackynavi::dmcub::Result& lastInboxResult() const { return inboxResult_; }
protected:
    bool init() override;
    void free() override;
private:
    IOReturn initialize(HackyNaviFirmwareController*, const uint8_t*, size_t, const hackynavi::ImageInfo&);
    void lockOwners();
    IOReturn finish(IOReturn);
    IOReturn fault();
    bool providerReady(bool retiring = false) const;
    bool validPath(uint8_t, uint8_t connector = 0) const;
    IOReturn beginPath(uint8_t, uint8_t connector = 0);
    IOReturn inboxStatus(hackynavi::dmcub::Status);
    IOReturn hdmiOperation(uint8_t, const hackynavi::hdmiddc::Clock&,
        const hackynavi::hdmiddc::Message*, size_t, bool edid, uint8_t block,
        hackynavi::hdmiddc::Result&);
    IOReturn releaseLeaseLocked();
    IOReturn disableDpLocked(uint8_t, const hackynavi::dmcub::PollOptions&);
    IOReturn disableHdmiLocked(uint8_t, const hackynavi::dmcub::PollOptions&,
        hackynavi::hdmitmds::HardwareResult&);
    void clearLinkMetadata(uint8_t);
    static bool read32(void*, uint32_t, uint32_t&);
    static bool write32(void*, uint32_t, uint32_t);
    static bool readMemory(void*, uint64_t, uint8_t*, size_t);
    static bool writeMemory(void*, uint64_t, const uint8_t*, size_t);
    static bool publish(void*);
    static void delayUs(void*, uint32_t);
    bool memoryRange(uint64_t, size_t) const;
    hackynavi::dmcubfw::Io firmwareIo();
    hackynavi::dmcub::Io inboxIo();
    IOLock* lock_;
    HackyNaviFirmwareController* firmware_;
    HackyNaviGARTController* gart_;
    OSData* rom_;
    bool lease_, hardwareHold_;
    State state_;
    uint8_t paths_;
    hackynavi::ImageInfo image_;
    hackynavi::dmcub::Route routes_[hackynavi::kMaxDisplayPaths];
    bool routeValid_[hackynavi::kMaxDisplayPaths];
    hackynavi::dmcubfw::Layout firmwareLayout_;
    hackynavi::dmcubfw::Memory memory_;
    hackynavi::dmcub::Layout inboxLayout_;
    hackynavi::dmcub::Inbox inbox_;
    hackynavi::dmcub::Result inboxResult_;
    hackynavi::dpaux::Layout auxLayout_;
    hackynavi::dpaux::Engine aux_;
    hackynavi::hdmiddc::Layout ddcLayout_;
    hackynavi::hdmiddc::Engine ddc_;
    hackynavi::dio::Layout dioLayout_;
    hackynavi::dio::Link links_[hackynavi::kMaxDisplayPaths];
    hackynavi::dio::Result linkResult_;
    enum class LinkKind : uint8_t { None, Dp, Hdmi };
    LinkKind linkKinds_[hackynavi::kMaxDisplayPaths];
    hackynavi::hdmiddc::Clock hdmiClocks_[hackynavi::kMaxDisplayPaths];
    bool hdmiScdc_[hackynavi::kMaxDisplayPaths];
};
#endif
