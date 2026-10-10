// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_AUDIO_QUIESCE_HPP
#define HACKYNAVI_AUDIO_QUIESCE_HPP
#include <stdint.h>
namespace hackynavi { namespace audioquiesce {
constexpr uint8_t kMaxStreams=30;
enum class Status { Ok,InvalidArgument,Revoked,ReadFailed,WriteFailed,DeviceUnavailable,
    UnsupportedVersion,UnsupportedCapabilities,OutsideBar,Timeout,ClockFailure,
    StateChanged,FlushInProgress };
enum class Stage { Preflight,Capabilities,Interrupts,Streams,CommandDma,PositionDma,
    VerifyStopped,ControllerReset,ResetHold,Complete };
struct Io {
    void* context;
    // The native caller holds the actual graphics/audio ownership gate for the
    // entire call. valid must consult that scope's current revocation state,
    // live function1 ownership/D0 and BAR0 mapping; a supplied readiness flag
    // is not sufficient. This protocol never acquires ownership itself.
    bool (*valid)(void*);
    bool (*read8)(void*,uint32_t,uint8_t&);
    bool (*read16)(void*,uint32_t,uint16_t&);
    bool (*read32)(void*,uint32_t,uint32_t&);
    bool (*write8)(void*,uint32_t,uint8_t);
    bool (*write16)(void*,uint32_t,uint16_t);
    bool (*write32)(void*,uint32_t,uint32_t);
    bool (*nowUs)(void*,uint64_t&); // Actual monotonic time, independent of HDA WALLCLK.
    bool (*delayUs)(void*,uint32_t); // Sleepable; may return late, never fabricate time.
};
struct Result {
    Status status;
    Stage stage;
    bool touched,complete,initiallyInReset,streamCountsKnown,lastReadSucceeded;
    uint8_t inputStreams,outputStreams,bidirectionalStreams,streamCount,stream;
    uint8_t lastWidth;
    uint16_t gcap;
    uint32_t reads,writes,lastOffset,lastValue,stoppedStreamMask;
    uint64_t elapsedUs;
};
// Stop the inherited HDA controller and leave CRST asserted. Actual GCAP counts
// select all input/output/bidirectional descriptors, without fallback counts.
// Requires HDA1.0 byte-addressable controls (the AMD HDMI profile); does not
// support controllers requiring DWORD-only SD_CTL writes. SD_STS/STATESTS and
// ring status W1C bytes are never written. All poll phases and the entire call
// use measured deadlines; totalTimeoutUs is a caller budget in1..1000000us.
// The protocol also caps each stop/reset poll at100ms. A timeout is failure,
// not evidence that hardware stopped. Read/all-ones/revocation errors abort
// without further MMIO. A failed write may have landed: touched remains true.
// If already in reset, only GCTL/WAKEEN are accessed; default-valued stream
// registers are not represented as independently observed stopped engines.
// Success is an HDA-controller observation under the still-held native gate.
// It does not disable PCI BME, drain PCI Transactions Pending, prove whole-GPU
// retirement, stop firmware/SMM, or authorize freeing any DMA allocation.
// Never release retained resources on error or reuse this result after unlock.
Status stop(const Io&,uint64_t barBytes,uint32_t totalTimeoutUs,Result&);
const char* statusString(Status);
} }
#endif
