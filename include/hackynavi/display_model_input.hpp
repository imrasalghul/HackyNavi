// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_DISPLAY_MODEL_INPUT_HPP
#define HACKYNAVI_DISPLAY_MODEL_INPUT_HPP
#include "rom.hpp"
#include "ip_layout.hpp"
namespace hackynavi { namespace displaymodel {
enum class Status { Ok,InvalidArgument,RomRejected,UnsupportedBoard,DiscoveryRejected,
    OutsideBar,Revoked,TransportError,DeviceUnavailable,Timeout,ClockWentBackwards,
    ChangedReference,UnsupportedReference };
struct BoardInfo {
    uint16_t vendorId,deviceId,subsystemVendorId,subsystemDeviceId;
    TableInfo firmwareTable,displayControllerTable,vramTable;
    uint32_t firmwareRevision,crystalKHz,i2cKHz;
    uint32_t rawMclkChange100ns,rawSrExit100ns,rawSrEnterExit100ns;
    uint8_t memoryType,channelCount,channelWidthRaw,maxDisplayPipes;
    uint16_t channelWidthBytes;
};
// Exact selected legacy Navi31 ROM image; Firmware3.4/3.5, DCE4.5 and VRAM3.0.
// Reports firmware metadata, not live strap/clock measurements or authenticity.
// Raw latency overrides remain uninterpreted: pinned Linux's *10 conversion
// disagrees with atomfirmware.h's 100ns units; profile construction rejects
// nonzero overrides until that discrepancy is resolved explicitly.
Status parseBoard(const uint8_t*,size_t,const ImageInfo&,BoardInfo&);
struct ReferenceInfo {
    uint32_t pllRequestRegister,timerControlRegister,rawPllRequest,rawTimerControl;
    uint32_t dccgReferenceKHz,dchubReferenceKHz,programmedVcoKHz;
    uint8_t timerDivider;
};
struct Result { Status status;uint32_t reads,lastRegister,lastValue;uint64_t elapsedUs; };
struct Io {
    void* context;
    bool (*valid)(void*);
    bool (*read32)(void*,uint32_t,uint32_t&);
    uint64_t (*timeUs)(void*);
};
// Read-only, four actual MMIO reads: two identical observations of PLL request
// and enabled DCHUB timer. DCCG source follows the DCN32 XTAL contract, verified
// against this ROM's 100MHz crystal. VCO is derived from programmed FbMult,
// NOT a measured running clock. No zero-register/default-VCO fallback.
// Caller must retain the actual provider/BAR5 MMIO map and enforce readable
// D0 and actual global exclusion.
// valid must test immediate revocation; timeUs must read a real monotonic clock.
// No reads of gated HUBP/pipe windows and no write callback is accepted.
Status captureReferences(const uint8_t* discovery,size_t,uint64_t bar5Bytes,
    const BoardInfo&,const Io&,uint32_t timeoutUs,ReferenceInfo&,Result&);
// Pure derivation for the userspace boundary to recheck serialized observations.
// This cannot prove the supplied raw values came from hardware.
Status decodeReferences(const BoardInfo&,uint32_t rawPll,uint32_t rawTimer,ReferenceInfo&);
} }
#endif
