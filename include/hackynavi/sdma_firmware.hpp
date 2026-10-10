// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_SDMA_FIRMWARE_HPP
#define HACKYNAVI_SDMA_FIRMWARE_HPP
#include "hackynavi/firmware.hpp"
#include "hackynavi/ip_layout.hpp"
#include "hackynavi/vram_transfer.hpp"
#include "hackynavi/hdp.hpp"

namespace hackynavi {
constexpr size_t kSdmaPspCommandBytes = 1024;
enum class SdmaFirmwareStatus {
    Ok, InvalidArgument, UnsupportedIp, FirmwareRejected, UnsupportedFirmware,
    EmptySegment, OverlappingSegments, CapacityExceeded, InvalidAllocation,
    OutsideAllocation, InvalidState, TransferFailed, VisibilityFailed
};
struct SdmaFirmwarePlan {
    bool valid;
    uint32_t ucodeVersion, featureVersion;
    FirmwareRange fileSegments[2]; // TH0 context, then TH1 control.
    uint32_t pspTypes[2];          // 71 and 72, shared across both SDMA engines.
    uint64_t stagingOffsets[2];   // Each 4 KiB aligned, relative to one handle.
    uint64_t allocationBytes;
};
struct SdmaFirmwareResult {
    Status parserStatus;
    VramAllocatorStatus allocationStatus;
    VramTransferResult transfer; // Last attempted segment transfer.
    HdpStatus visibilityStatus;
    HdpResult visibility;
    unsigned segmentsVerified;
    bool visibilityAttempted;
};
class StagedSdmaFirmware {
public:
    StagedSdmaFirmware() : ready_(false), plan_{}, handle_{}, mc_(0) {}
    StagedSdmaFirmware(const StagedSdmaFirmware&) = delete;
    StagedSdmaFirmware& operator=(const StagedSdmaFirmware&) = delete;
    // This means uploaded/readback-verified/HDP-visible, never PSP accepted,
    // authenticated, loaded into an engine, or ready to execute a queue.
    bool stagedForPsp() const { return ready_; }
    void invalidate() { ready_ = false; plan_ = SdmaFirmwarePlan{}; handle_ = VramAllocationHandle{}; mc_ = 0; }
private:
    bool ready_;
    SdmaFirmwarePlan plan_;
    VramAllocationHandle handle_;
    uint64_t mc_;
    friend SdmaFirmwareStatus stageSdmaFirmware(const uint8_t*, size_t, const Navi31IpLayout&,
        const VramAllocator&, VramAllocationHandle, const VramTransferIo&,
        const HdpIo&, HdpState&, StagedSdmaFirmware&, SdmaFirmwareResult&);
    friend SdmaFirmwareStatus encodeSdmaPspLoadCommand(const StagedSdmaFirmware&,
        const VramAllocator&, unsigned, uint8_t*, size_t);
};

// Caller selects the authentic, permitted amdgpu/sdma_6_0_0.bin resource.
// This parser verifies bounds and exact header2.0/IP6.0, not provenance, CRC or
// PSP signature. Requires a successfully built immutable Navi31IpLayout with
// GC11.0.0, MP0 13.0.0 and both SDMA6.0.0 blocks. Rejects empty/overlapping
// segments and segments above the bounded uploadVram maximum. No allocation.
// Plan offsets/sizes come from firmware, not jump-table lengths or constants.
// out is zero on failure; parserStatus preserves the underlying parse result.
SdmaFirmwareStatus planSdmaFirmware(const uint8_t* data, size_t bytes,
    const Navi31IpLayout& ips, SdmaFirmwarePlan& out, Status& parserStatus);

// Uses an ALREADY OWNED live allocator handle covering plan.allocationBytes.
// Does not allocate, release or expose an arbitrary GPU address callback.
// Parses again before any MMIO, uploads each full segment with independent
// readback using uploadVram, then performs real flushHdp. Commands are available
// only after both transfers and visibility succeed. Padding is not uploaded or
// submitted to PSP. On any failure staged is invalidated, and partial VRAM
// writes may remain; caller keeps the allocation owned/quiescent for recovery.
//
// Inherits uploadVram and HDP ownership/power/serialization contracts. Caller
// serializes the entire operation, keeps source immutable/non-overlapping with
// outputs, and prevents reset, suspend, allocator mutations and other MM-window
// users. HDP must already be configured for this device. The source is not
// retained. Output state/allocation must outlive all generated PSP commands.
SdmaFirmwareStatus stageSdmaFirmware(const uint8_t* data, size_t bytes,
    const Navi31IpLayout& ips, const VramAllocator& allocator,
    VramAllocationHandle allocation, const VramTransferIo& transferIo,
    const HdpIo& hdpIo, HdpState& hdp, StagedSdmaFirmware& staged,
    SdmaFirmwareResult& out);

// Writes one explicit little-endian 1024-byte GPCOM command after rechecking
// the same live handle and MC address. TH0 index0, TH1 index1. All bytes except
// cmd_id=6 and LOAD_IP_FW address/size/type are zero, matching pinned Linux's
// acquire/prep path (including zero buf_size/version despite ABI comments).
// Preflight failure leaves destination unchanged. Destination must not overlap
// state/allocator. No hardware access or PSP submission occurs here.
//
// Caller must upload this command to owned PSP command memory and use the real
// initialized KM ring, verified fence/response protocol and reserved TMR. Keep
// staging unmodified/owned until PSP has retired the load. Reset, power loss,
// mutation or reuse of staging invalidates it even if its handle remains live;
// caller must invalidate this state. LOAD_IP_FW acceptance stages firmware in
// TMR; Navi31 RLC autoload and bootload/idle completion are separate prerequisites
// before unhalting SDMA. This API does not supply a false firmware-ready flag.
SdmaFirmwareStatus encodeSdmaPspLoadCommand(const StagedSdmaFirmware& staged,
    const VramAllocator& allocator, unsigned segment, uint8_t* destination, size_t bytes);
const char* sdmaFirmwareStatusString(SdmaFirmwareStatus status);
} // namespace hackynavi
#endif
