// SPDX-License-Identifier: MIT
// Original bounded SDMA staging/serialization, following Linux commit
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// amdgpu_sdma.c:278-289; amdgpu_ucode.c:891-899 (whole context/control payloads),
// amdgpu_psp.c:810-819,3243-3248,3374-3390 (zeroed GPCOM LOAD_IP_FW image),
// psp_gfx_if.h:94,277-278,315-323,466-490 (command ID, types and offsets).
// Normal auto firmware mode uses PSP: amdgpu_drv.c:164,379-386 and
// amdgpu_ucode.c:591-597. No selection of unverified forced-direct loading.
// https://github.com/torvalds/linux/tree/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu
// Copyright 2016-2025 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
#include "hackynavi/sdma_firmware.hpp"

namespace hackynavi {
namespace {
bool block(const Navi31IpLayout& l, Navi31Block which, uint16_t id, uint8_t major) {
    const IpRegisterBlock& b = l.blocks[static_cast<size_t>(which)];
    return b.hardwareId == id && b.major == major && !b.minor && !b.revision &&
        !b.instance && !b.dieId && b.baseCount && b.baseCount <= kNavi31MaxBases;
}
bool supportedIp(const Navi31IpLayout& l) {
    return l.valid && block(l, Navi31Block::Gc, 11, 11) &&
        block(l, Navi31Block::Mp0, 255, 13) &&
        block(l, Navi31Block::Sdma0, 42, 6) && block(l, Navi31Block::Sdma1, 43, 6);
}
void put32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}
SdmaFirmwareStatus allocationCheck(const VramAllocator& allocator,
    VramAllocationHandle handle, uint64_t required, uint64_t& mc,
    VramAllocatorStatus& allocationStatus) {
    VramRange range = {};
    allocationStatus = allocator.lookup(handle, range);
    if (allocationStatus != VramAllocatorStatus::Ok) return SdmaFirmwareStatus::InvalidAllocation;
    if (range.bytes < required) return SdmaFirmwareStatus::OutsideAllocation;
    uint64_t physical = 0;
    allocationStatus = allocator.addresses(handle, mc, physical);
    if (allocationStatus != VramAllocatorStatus::Ok || (mc & 4095))
        return SdmaFirmwareStatus::InvalidAllocation;
    // allocator.addresses validated the complete allocation in both GPU domains.
    return SdmaFirmwareStatus::Ok;
}
} // namespace
SdmaFirmwareStatus planSdmaFirmware(const uint8_t* data, size_t bytes,
    const Navi31IpLayout& ips, SdmaFirmwarePlan& out, Status& parserStatus) {
    out = SdmaFirmwarePlan{};
    parserStatus = Status::InvalidArgument;
    if (!supportedIp(ips)) return SdmaFirmwareStatus::UnsupportedIp;
    SdmaFirmwareV2 fw = {};
    parserStatus = parseSdmaFirmwareV2(data, bytes, fw);
    if (parserStatus != Status::Ok) return SdmaFirmwareStatus::FirmwareRejected;
    if (fw.header.ipMajor != 6 || fw.header.ipMinor != 0)
        return SdmaFirmwareStatus::UnsupportedFirmware;
    if (!fw.context.size || !fw.control.size) return SdmaFirmwareStatus::EmptySegment;
    if (fw.context.size > kVramTransferMaxBytes || fw.control.size > kVramTransferMaxBytes)
        return SdmaFirmwareStatus::CapacityExceeded;
    if (uint64_t(fw.context.offset) < uint64_t(fw.control.offset) + fw.control.size &&
        uint64_t(fw.control.offset) < uint64_t(fw.context.offset) + fw.context.size)
        return SdmaFirmwareStatus::OverlappingSegments;
    SdmaFirmwarePlan plan = {};
    plan.ucodeVersion = fw.header.ucodeVersion; plan.featureVersion = fw.featureVersion;
    plan.fileSegments[0] = fw.context; plan.fileSegments[1] = fw.control;
    plan.pspTypes[0] = 71; plan.pspTypes[1] = 72;
    plan.stagingOffsets[1] = (uint64_t(fw.context.size) + 4095) & ~uint64_t(4095);
    plan.allocationBytes = plan.stagingOffsets[1] + ((uint64_t(fw.control.size) + 4095) & ~uint64_t(4095));
    plan.valid = true; out = plan;
    return SdmaFirmwareStatus::Ok;
}
SdmaFirmwareStatus stageSdmaFirmware(const uint8_t* data, size_t bytes,
    const Navi31IpLayout& ips, const VramAllocator& allocator,
    VramAllocationHandle allocation, const VramTransferIo& transferIo,
    const HdpIo& hdpIo, HdpState& hdp, StagedSdmaFirmware& staged,
    SdmaFirmwareResult& out) {
    staged.invalidate(); out = SdmaFirmwareResult{};
    SdmaFirmwarePlan plan = {};
    SdmaFirmwareStatus status = planSdmaFirmware(data, bytes, ips, plan, out.parserStatus);
    if (status != SdmaFirmwareStatus::Ok) return status;
    if (!transferIo.read32 || !transferIo.write32 || transferIo.barBytes < 0x1c ||
        !hdpIo.read32 || !hdpIo.write32) return SdmaFirmwareStatus::InvalidArgument;
    if (!hdp.configured()) return SdmaFirmwareStatus::InvalidState;
    uint64_t mc = 0;
    status = allocationCheck(allocator, allocation, plan.allocationBytes, mc, out.allocationStatus);
    if (status != SdmaFirmwareStatus::Ok) return status;
    for (unsigned i = 0; i < 2; ++i) {
        const FirmwareRange& segment = plan.fileSegments[i];
        if (uploadVram(allocator, allocation, plan.stagingOffsets[i], data + segment.offset,
            segment.size, transferIo, out.transfer) != VramTransferStatus::Ok)
            return SdmaFirmwareStatus::TransferFailed;
        ++out.segmentsVerified;
    }
    out.visibilityAttempted = true;
    out.visibilityStatus = flushHdp(hdpIo, hdp, out.visibility);
    if (out.visibilityStatus != HdpStatus::Ok) return SdmaFirmwareStatus::VisibilityFailed;
    staged.plan_ = plan; staged.handle_ = allocation; staged.mc_ = mc; staged.ready_ = true;
    return SdmaFirmwareStatus::Ok;
}
SdmaFirmwareStatus encodeSdmaPspLoadCommand(const StagedSdmaFirmware& staged,
    const VramAllocator& allocator, unsigned segment, uint8_t* destination, size_t bytes) {
    if (!destination || bytes < kSdmaPspCommandBytes || segment > 1)
        return SdmaFirmwareStatus::InvalidArgument;
    if (!staged.ready_) return SdmaFirmwareStatus::InvalidState;
    uint64_t mc = 0;
    VramAllocatorStatus allocationStatus = VramAllocatorStatus::Ok;
    const SdmaFirmwareStatus status = allocationCheck(allocator, staged.handle_,
        staged.plan_.allocationBytes, mc, allocationStatus);
    if (status != SdmaFirmwareStatus::Ok) return status;
    if (mc != staged.mc_) return SdmaFirmwareStatus::InvalidState;
    const uint64_t address = mc + staged.plan_.stagingOffsets[segment];
    for (size_t i = 0; i < kSdmaPspCommandBytes; ++i) destination[i] = 0;
    put32(destination + 8, 6);
    put32(destination + 28, static_cast<uint32_t>(address));
    put32(destination + 32, static_cast<uint32_t>(address >> 32));
    put32(destination + 36, staged.plan_.fileSegments[segment].size);
    put32(destination + 40, staged.plan_.pspTypes[segment]);
    return SdmaFirmwareStatus::Ok;
}
const char* sdmaFirmwareStatusString(SdmaFirmwareStatus status) {
    switch (status) {
    case SdmaFirmwareStatus::Ok: return "ok";
    case SdmaFirmwareStatus::InvalidArgument: return "invalid SDMA firmware arguments";
    case SdmaFirmwareStatus::UnsupportedIp: return "unsupported SDMA/GC/PSP IP layout";
    case SdmaFirmwareStatus::FirmwareRejected: return "SDMA firmware parser rejected input";
    case SdmaFirmwareStatus::UnsupportedFirmware: return "SDMA firmware IP is not 6.0";
    case SdmaFirmwareStatus::EmptySegment: return "empty SDMA firmware segment";
    case SdmaFirmwareStatus::OverlappingSegments: return "overlapping SDMA firmware segments";
    case SdmaFirmwareStatus::CapacityExceeded: return "SDMA firmware exceeds bounded transfer size";
    case SdmaFirmwareStatus::InvalidAllocation: return "SDMA staging handle or address is invalid";
    case SdmaFirmwareStatus::OutsideAllocation: return "SDMA staging allocation too small";
    case SdmaFirmwareStatus::InvalidState: return "SDMA staging or HDP state is invalid";
    case SdmaFirmwareStatus::TransferFailed: return "SDMA staging upload/readback failed";
    case SdmaFirmwareStatus::VisibilityFailed: return "SDMA staging HDP visibility failed";
    }
    return "unknown SDMA firmware status";
}
} // namespace hackynavi
