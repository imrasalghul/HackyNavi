// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_HUBP_APERTURE_HPP
#define HACKYNAVI_HUBP_APERTURE_HPP
#include "hubp_scanout.hpp"
namespace hackynavi { namespace hubpaperture {
enum class Status { Ok,InvalidArgument,DiscoveryRejected,PowerUnavailable,
    ClockUnavailable,ActivePipe,ForeignControl,AddressMode,TransportError,
    DeviceUnavailable,VerifyFailed };
struct Result {
    Status status;
    hubpscanout::Status layoutStatus;
    uint32_t reads,writes,lastRegister,lastValue;
    bool writeAttempted;
};
// Native caller holds the real device/power/reset exclusion, owns the selected
// HUBP clock and blanked/drained pipe, and retains all hardware owners BEFORE
// dispatch: even a failed write may have reached the device. This function
// neither grants ownership nor retires DMA. The caller supplies deadline- and
// revocation-checked real MMIO callbacks and a verified live MemoryLayout.
//
// Programs only this HUBP's direct-VRAM system aperture and L1 access mode.
// Global FB routing must already match MemoryLayout and AGP must be disabled.
// No VMID changes, requestor policy, fetch enable, timing or link activation.
// Addresses are built from discovery, never accepted from a caller register
// array. Each gated access is preceded by always-on power observations. Writes
// are followed by exact readback; failure requires owner retention/reset.
Status program(const uint8_t* discovery,size_t discoveryBytes,uint64_t barBytes,
    const hubpscanout::Io&,uint8_t pipe,const MemoryLayout&,Result&);
} }
#endif
