// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_PSP_TA_HPP
#define HACKYNAVI_PSP_TA_HPP
#include "hackynavi/firmware.hpp"
namespace hackynavi { namespace pspta {
enum class Kind : uint8_t { Hdcp=0, Dtm=1 };
constexpr uint32_t kSharedBytes=16384;
enum class Status { Ok, InvalidArgument, InvalidContainer, UnsupportedProfile,
    InvalidEntry, DuplicateEntry, Overlap, MissingApplication };
struct Application { uint32_t version; FirmwareRange image; };
struct Image {
    FirmwareHeader header;
    Application hdcp,dtm;
};
// Bounded PSP13 TA v2.0 container metadata, with the observed 36-byte header
// and 256-byte payload start. All declared entries must form a nonoverlapping
// complete payload partition. HDCP and DTM must both exist. No borrowed pointer,
// firmware authentication, upload, session or inherited-HDCP authority results.
// Output is zero on failure; an input/output memory overlap is rejected without
// touching either object. Caller selects the actual family firmware file.
Status parse(const uint8_t*,size_t,Image&);
const char* statusString(Status);
} }
#endif
