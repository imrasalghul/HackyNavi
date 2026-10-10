// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_FIRMWARE_RESOURCES_HPP
#define HACKYNAVI_FIRMWARE_RESOURCES_HPP
#include <stddef.h>
#include <stdint.h>

namespace hackynavi {
enum class FirmwareResourceId { PspSos, Sdma, Smu, GcPfp, GcMe, GcMec, GcImu, GcRlc, Dmcub, PspTa };
struct FirmwareResource {
    const uint8_t* data;
    size_t bytes;
    const char* filename;
    const char* sha256;
};
// Defined by scripts/embed-firmware.py's generated translation unit. Inputs
// are size/SHA256 checked against firmware/manifest.json before compilation.
// Immutable bytes live with the kext, including before a userspace firmware
// service is available. No missing-resource substitution or filesystem I/O.
// Successful lookup supplies bytes only, not hardware authentication/load.
bool getFirmwareResource(FirmwareResourceId, FirmwareResource&);
}
#endif
