// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_EDID_ACQUIRE_HPP
#define HACKYNAVI_EDID_ACQUIRE_HPP
#include "hackynavi/edid.hpp"
#include "hackynavi/dp_aux.hpp"
#include "hackynavi/hdmi_ddc.hpp"
namespace hackynavi { namespace edidacq {
enum class Status { Ok, InvalidArgument, InvalidState, NoSpace, TransportFailed,
    EdidRejected, ChangedWhileReading };
enum class Stage { None, Read, Verify, Parse, ReleaseTransport, Complete };
struct Result {
    Status status;
    Stage stage;
    edid::Status edidStatus;
    size_t expectedBlocks, blocksRead, verifiedBlocks, readCalls, bytes;
    uint8_t lastBlock;
    bool complete;
};
struct Io {
    void* context;
    // Returns true only for an exact 128-byte block. The caller serializes
    // the physical connection across the entire capture, including verification.
    bool (*readBlock)(void*,uint8_t block,uint8_t* bytes);
};
// Acquire the base and every effective advertised extension, honoring the
// first-extension HF-EEODB count. Verify a second complete capture is byte
// identical before parsing. No partial capture or fabricated mode is success.
// Buffer is scratch until complete=true; only result.bytes bytes are valid on
// success. Monitor remains unchanged on error. Arguments must not overlap.
Status capture(const Io&,uint8_t* buffer,size_t capacity,edid::Monitor&,Result&);
struct HardwareResult {
    Result capture;
    dpaux::Status auxStatus, stopStatus;
    hdmiddc::Status ddcStatus;
    dpaux::Result aux, stop;
    hdmiddc::Result ddc;
    uint32_t packets;
    bool stopAttempted, stopCompleted;
};
// Adapters use actual already-open engines, leaving ownership with the caller.
// AUX reads consume partial ACK payloads exactly once and finish each block
// with a real I2C STOP. The outer owner must close the engine and handle any
// ambiguous hardware ownership before publishing a successful capture.
Status capture(dpaux::Engine&,uint8_t*,size_t,edid::Monitor&,HardwareResult&);
Status capture(hdmiddc::Engine&,uint8_t*,size_t,edid::Monitor&,HardwareResult&);
const char* statusString(Status);
} }
#endif
