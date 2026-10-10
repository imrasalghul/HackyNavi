// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_SCANOUT_SURFACE_HPP
#define HACKYNAVI_SCANOUT_SURFACE_HPP
#include "HackyNaviDisplayController.hpp"
#include "HackyNaviFunctionGroup.hpp"
#include "hackynavi/hubp_scanout.hpp"
#include "hackynavi/hubp_aperture.hpp"
#include "hackynavi/pixel_pipeline.hpp"

// Native allocation and blanked-pipeline owner. This is not an IOFramebuffer
// mode implementation: no API here enables fetch, OTG or a physical stream.
// The later modeset transaction must own measured bandwidth/DET/RQ/DLG/TTU,
// pixel PLL/timing/link and platform-console handoff before exposing pixels.
class HackyNaviScanoutSurface : public OSObject {
    OSDeclareDefaultStructors(HackyNaviScanoutSurface)
public:
    enum class State { Empty, Allocated, Initialized, Prepared,
        FaultedRetained, Retired };
    enum class Stage { None, Allocation, Upload, Visibility, Pixel, Aperture,
        Surface, StopSurface, StopPixel, Release, Complete, VerifyClocks };
    struct Config {
        uint32_t width,height,pitchPixels;
        uint8_t pipe,outputBpc;
        hackynavi::hubpscanout::Format format;
    };
    struct Report {
        IOReturn result; Stage stage;
        uint64_t allocationBytes,initializedBytes,elapsedUs;
        uint32_t lastRegister,lastValue,reads,writes;
        bool writeAttempted,retentionRequired;
        hackynavi::VramTransferResult upload;
        hackynavi::HdpResult hdp;
        hackynavi::pixelpipeline::Result pixel;
        hackynavi::hubpaperture::Result aperture;
        hackynavi::hubpscanout::Result hubp;
        HackyNaviDisplayController::ProgrammedClocksResult globalClocks[2];
    };
    // Caller already holds the actual FunctionGroup scope AND installs the
    // GART::Access guard before descendant locks; no nested guard acquisition.
    // Uses Display's existing reservation-aware GART allocator.
    // Owns one private allocation, not a raw GPU address or another owner's BO.
    // Retains Display/Firmware/GART/provider/maps and leases Display until
    // explicit release. Allocation and construction perform no hardware writes.
    static HackyNaviScanoutSurface* withDisplay(HackyNaviDisplayController*,
        HackyNaviFunctionGroup::Operation&,const Config&,IOReturn&);
    // Sequential complete initialization, including allocation padding. Each
    // immutable kernel source chunk is <=2MiB and is independently read back.
    // No scanout publication until every byte has passed readback + HDP flush.
    // Callers provide kernel-owned accessible data; no userspace pointer ABI.
    IOReturn upload(HackyNaviFunctionGroup::Operation&,uint64_t offset,
        const uint8_t* data,size_t bytes,uint32_t timeoutUs,Report&);
    // Requires this Display's completed private DISPCLK/DPPCLK journals and
    // fresh same-generation programming readbacks for both clocks, owned DTO
    // and powered domain. Programs HUBP clock + DPP/MPC/OPP, direct-VRAM system
    // aperture and surface while OTG is stopped and HUBP blank/drained. Does
    // not invent bandwidth records or unblank. Retains before the first write.
    IOReturn prepareBlanked(HackyNaviFunctionGroup::Operation&,uint32_t timeoutUs,Report&);
    // Fresh HUBP blank/drain, then verified MPC disconnect and clock disable.
    // No stream was started by this object. A faulted hardware publication is
    // terminal and needs the typed independent reset proof below.
    IOReturn retireBlanked(HackyNaviFunctionGroup::Operation&,uint32_t timeoutUs,Report&);
    IOReturn retireBlanked(HackyNaviFunctionGroup::RetirementScope&,uint32_t timeoutUs,Report&);
    // Only when no pipe/address publication was attempted and every completed
    // CPU upload passed readback/flush. An ambiguous CPU write retains too.
    IOReturn releaseUnpublished(HackyNaviFunctionGroup::Operation&);
    IOReturn releaseUnpublished(HackyNaviFunctionGroup::RetirementScope&);
    // Caller holds the separate reset-retirement gate through the SAME typed
    // proof transfer. Never fabricate retirement from a normal Operation.
    IOReturn releaseAfterVerifiedReset(HackyNaviFunctionGroup::RetirementScope&,
        const HackyNaviResetController::RetirementProof&);
    // Snapshot getters require the same outer normal/retirement exclusion;
    // Display uses the retention getter under its own owner mutex too.
    bool requiresHardwareRetirement() const {return hardwareHold_;}
    State state() const {return state_;}
    uint64_t byteCount() const {return allocation_.range.bytes;}
protected:
    bool init() override;
    void free() override;
private:
    IOReturn initialize(HackyNaviDisplayController*,HackyNaviFunctionGroup::Operation&,const Config&);
    bool begin(HackyNaviFunctionGroup::Operation*,HackyNaviFunctionGroup::RetirementScope*,uint32_t,Report&);
    bool scopeValid() const;
    IOReturn retireBlankedImpl(HackyNaviFunctionGroup::Operation*,HackyNaviFunctionGroup::RetirementScope*,uint32_t,Report&);
    IOReturn releaseUnpublishedImpl(HackyNaviFunctionGroup::Operation*,HackyNaviFunctionGroup::RetirementScope*);
    IOReturn finish(IOReturn,Report&);
    bool ready();
    bool power();
    void holdHardware();
    IOReturn releaseLocked();
    static bool rawRead(void*,uint32_t,uint32_t&);
    static bool rawWrite(void*,uint32_t,uint32_t);
    static bool pipeRead(void*,uint32_t,uint32_t&);
    static bool pipeWrite(void*,uint32_t,uint32_t);
    static void delay(void*,uint32_t);
    static uint64_t timeUs();
    HackyNaviDisplayController* display_;
    HackyNaviGARTController* gart_; // retained through display_
    HackyNaviFunctionGroup::Operation* scope_; // only while a method holds locks
    HackyNaviFunctionGroup::RetirementScope* retirement_; // cleanup only
    Report* report_;
    Config config_;
    hackynavi::VramAllocation allocation_;
    hackynavi::hubpscanout::Surface surface_;
    hackynavi::hubpscanout::Layout hubpLayout_;
    hackynavi::hubpscanout::Controller hubp_;
    hackynavi::pixelpipeline::Layout pixelLayout_;
    hackynavi::pixelpipeline::Controller pixel_;
    State state_;
    bool lease_,hardwareHold_;
    // Exact first stable ON observation persists through this surface lifetime.
    uint32_t powerStatus_,powerConfig_;bool powerValid_;
    uint64_t initializedBytes_,start_,last_;
    uint32_t budget_;
};
#endif
