// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_HDMI_LINK_HPP
#define HACKYNAVI_HDMI_LINK_HPP
#include "hackynavi/dio_link.hpp"
#include "hackynavi/hdmi_ddc.hpp"
namespace hackynavi { namespace hdmitmds {
enum class Status { Ok,InvalidArgument,InvalidState,UnsupportedFormat,
    UnsupportedReceiver,LinkLimits,ScdcFailed,SourceFailed,VerifyFailed };
enum class Stage { Preflight,Capabilities,SourceConfigure,SinkConfigure,
    SourceEnable,Verify,Complete,SourceDisable,SinkDisable };
enum class Encoding { Rgb,YCbCr444,YCbCr422,YCbCr420 };
// Mask bits 0/1/2 authorize 10/12/16bpc respectively; 8bpc is mandatory.
// These are actual board limits and validated EDID capabilities, not guesses.
struct Limits { uint32_t maxCharacterClockKHz; uint8_t deepColorMask; };
struct SinkCaps { Limits limits; bool scdc,scrambleLowRates,ycbcr444,
    deepColor444,frlCapable; };
struct Config { uint32_t pixelClockKHz; uint8_t bitsPerComponent;
    Encoding encoding; bool scrambleLowRates; Limits source; SinkCaps sink; };
struct Plan { dio::HdmiConfig source; uint32_t characterClockKHz;
    uint8_t sinkConfig; };
Status plan(const Config&,Plan&);
struct Result { Status status; Stage stage; Plan configuration;
    uint32_t scdcReads,scdcWrites,sourceCommands,delayUs;
    uint8_t sinkVersion,lastScdcOffset,lastScdcValue;
    bool sourceConfigureAttempted,sourceConfigured,sourceEnableAttempted;
    bool sinkMayBeChanged,cleanupAttempted,sourceDisabled,sinkReset;
    bool recoveryRequired,activated; };
// Exact single-byte SCDC transactions, no automatic write retry. Failed I/O
// can have taken effect. Healthy callbacks indicate whether that transport is
// still safe to use. Source enable/disable must observe actual DIG_ENABLE.
struct Io { void* context;
    bool (*readScdc)(void*,uint8_t,uint8_t&);
    bool (*writeScdc)(void*,uint8_t,uint8_t);
    bool (*sourceConfigure)(void*,const dio::HdmiConfig&);
    bool (*sourceEnable)(void*,uint32_t);
    bool (*sourceDisable)(void*);
    bool (*ddcHealthy)(void*); bool (*sourceHealthy)(void*);
    void (*delayUs)(void*,uint32_t); };
// Cold native HDMI-TMDS only. Validates explicit sink/board TMDS/deep-color
// limits; >340MHz needs SCDC scrambling and 1/40 ratio; exactly340MHz keeps
// the legacy ratio at BOTH ends. Optional low-rate scrambling needs the
// sink's advertised support. FRL-active receivers and 420/422 are rejected.
// Reads SCDC version, writes/reads source version and TMDS_CONFIG, observes
// the minimum1ms settling delay, programs actual DIO/DMCUB and checks sink
// configuration again. FIFO stays off and AVMUTE asserted: this is PHY/link
// configuration, not video, sink lock, scanout or an AVI/audio implementation.
// Caller owns PLL, frontend, OTG/timing, FIFO and connector; source PLL/depth
// ratio must match Config. It holds shared display/firmware/PM/reset exclusion
// for the entire operation. Source is Prepared and the routed DDC is open.
Status activate(const Io&,const Config&,Result&);
// Caller has stopped pixel stream/OTG/FIFO. Retires source with actual DIG0
// observation first, then returns an advertised SCDC sink to legacy TMDS.
// A disconnected/NACKing sink does not make local PHY retirement uncertain:
// inspect sourceDisabled separately from status/sinkReset. Ambiguous local
// source or DDC ownership still requires retention by the kernel owner.
Status deactivate(const Io&,bool scdcPresent,Result&);
struct Delay { void* context; void (*delayUs)(void*,uint32_t); };
struct HardwareResult { Result link; hdmiddc::Status ddcStatus;
    hdmiddc::Result ddc; dio::Status sourceStatus; dio::Result source; };
// Production binding to the actual DCN3.2 hardware I2C engine and DIO/DMCUB.
// Rejects wrong connector/DDC route, closed/faulted transports and invalid
// source phase. Caller closes the DDC engine afterwards, including failures.
Status activate(hdmiddc::Engine&,dio::Link&,dmcub::Inbox&,const Config&,
    const dmcub::PollOptions&,const Delay&,HardwareResult&);
Status deactivate(hdmiddc::Engine&,dio::Link&,dmcub::Inbox&,bool scdcPresent,
    const dmcub::PollOptions&,const Delay&,HardwareResult&);
const char* statusString(Status);
} }
#endif
