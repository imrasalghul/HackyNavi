// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_EDID_HPP
#define HACKYNAVI_EDID_HPP
#include <stddef.h>
#include <stdint.h>
namespace hackynavi { namespace edid {
constexpr size_t kBlockBytes=128, kMaxBlocks=8, kMaxModes=64;
enum class Status { Ok, InvalidArgument, Truncated, BadHeader, BadChecksum,
    UnsupportedVersion, AnalogDisplay, TooManyBlocks, BadExtension, BadTiming,
    ConflictingCapabilities, TooManyModes };
struct Timing {
    uint32_t pixelClockKHz;
    uint16_t width,height,hTotal,hSyncStart,hSyncEnd,vTotal,vSyncStart,vSyncEnd;
    bool hPositive,vPositive,preferred,native;
    uint8_t vic; // Zero means an explicitly advertised detailed timing.
};
struct Monitor {
    uint16_t manufacturer,product;
    uint32_t serial;
    uint8_t version,revision,blocks;
    bool hdmi,scdc,scrambleLowRates,ycbcr444,deepColor444,frlCapable;
    uint8_t deepColorMask; // Bits 0/1/2: 10/12/16 bits per component.
    // Zero means unspecified, never permission to assume 600MHz.
    uint32_t maxTmdsCharacterClockKHz;
    uint8_t modeCount;
    uint16_t skippedTimings,skippedExtensions;
    Timing modes[kMaxModes];
};
// Validate the available block-aligned prefix and report acquisition length.
// A base with extensions initially returns blocks=2, final=false; the first
// extension can override the count with HF-EEODB. Once final=true, blocks is
// the exact complete-capture length (bounded by kMaxBlocks). Output parameters
// are unchanged on error. Each available block checksum is validated.
Status requiredBlocks(const uint8_t*,size_t capturedBytes,size_t& blocks,bool& final);
// Parses one immutable complete EDID capture, including every effective
// extension (base count or first-extension HF-EEODB override). Validates all
// block checksums before decoding.
// Supports EDID1.3/1.4 digital sinks, progressive separate-sync DTDs and common
// CTA RGB/444 VICs (720p,1080p,2160p). Unsupported timings are counted, never
// replaced by guessed modes. Established/standard timings are counted but not
// synthesized. Unknown extensions are counted. CTA 420-only
// timings are not exposed as RGB. No transport, allocation or hardware I/O.
// Output is unchanged on error. Input and output must not overlap.
Status parse(const uint8_t*,size_t bytes,Monitor&);
const char* statusString(Status);
} }
#endif
