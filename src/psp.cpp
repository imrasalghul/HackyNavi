// SPDX-License-Identifier: MIT
// Bootloader sequence adapted from Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// amdgpu/psp_v13_0.c:200-374; amdgpu_psp.c:2952-3049,4657-4674;
// amdgpu_psp.h:95-108; amdgpu_ucode.h:118-132;
// include/asic_reg/mp/mp_13_0_0_offset.h:35-38,81-82,127-128.
// SPL source selection independently agrees with Linux v6.6:233-235 and
// v6.12:261-263. CPU validation, owned staging and fail-stop state are port policy.
// Copyright 2016, 2020 Advanced Micro Devices, Inc.
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
#include "hackynavi/psp.hpp"

namespace hackynavi {
namespace psp {
struct Access { static void set(State& state, Phase phase) { state.phase_ = phase; } };
namespace {
constexpr uint32_t kReady = 0x80000000;
bool interval(const void* p, size_t bytes) {
    return p && bytes && bytes - 1 <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(p);
}
bool overlap(const void* a, size_t an, const void* b, size_t bn) {
    const auto av = reinterpret_cast<uintptr_t>(a), bv = reinterpret_cast<uintptr_t>(b);
    return av <= bv ? bv - av < an : av - bv < bn;
}
bool inBar(uint32_t offset, uint64_t bytes) {
    return !(offset & 3) && bytes >= 4 && offset <= bytes - 4;
}
bool validLayout(const Layout& l) {
    if (!l.valid || l.command < 0x63*4 || l.barBytes < 0x1c) return false;
    const uint64_t base = l.command - 0x63*4;
    const uint64_t offsets[] = {base+0x63*4,base+0x64*4,base+0x91*4,base+0x7a*4};
    const uint32_t actual[] = {l.command,l.address,l.heartbeat,l.version};
    for (unsigned i = 0; i < 4; ++i) {
        if (offsets[i] > UINT32_MAX || offsets[i] != actual[i] || !inBar(actual[i],l.barBytes) ||
            actual[i] == 0 || actual[i] == 4 || actual[i] == 0x18 ||
            (actual[i] >= 0x7f000 && actual[i] < 0x80000)) return false;
    }
    return true;
}
Status finish(Result& out, Status status) { out.status = status; return status; }
Status fault(State& state, Result& out, Status status) {
    Access::set(state,Phase::Faulted); return finish(out,status);
}
bool read(const Layout&, const Io& io, uint32_t reg, uint32_t& value, Result& out) {
    out.lastRegister = reg; ++out.reads;
    if (!io.read32(io.context,reg,value)) { finish(out,Status::ReadFailed); return false; }
    out.lastValue = value;
    if (value == UINT32_MAX) { finish(out,Status::DeviceUnavailable); return false; }
    return true;
}
bool write(const Io& io, uint32_t reg, uint32_t value, Result& out) {
    out.lastRegister = reg; out.lastValue = value; ++out.writes;
    out.mailboxWritesMayHaveOccurred = true;
    if (!io.write32(io.context,reg,value)) { finish(out,Status::WriteFailed); return false; }
    return true;
}
Status bootWait(const Layout& l, const Io& io, const Options& options, Result& out) {
    for (uint32_t i = 0; i < options.bootloaderPolls; ++i) {
        uint32_t value = 0; ++out.polls;
        if (!read(l,io,l.command,value,out)) return out.status;
        if (value == kReady) return Status::Ok;
        if (i + 1 < options.bootloaderPolls) io.delayUs(io.context,options.pollDelayUs);
    }
    // No low-byte status decoding is invented for13.0.0; Linux's exact
    // comparison also times out if the ready bit has other bits set.
    return Status::BootloaderTimeout;
}
} // namespace

Status buildLayout(const Navi31IpLayout& ips, uint64_t barBytes, Layout& out) {
    out = Layout{};
    if (!ips.valid) return Status::UnsupportedLayout;
    const uint16_t ids[] = {11,34,255,1,42,43};
    const uint8_t major[] = {11,3,13,13,6,6};
    for (unsigned i = 0; i < 6; ++i) {
        const auto& b = ips.blocks[i];
        if (b.hardwareId != ids[i] || b.major != major[i] || b.minor || b.revision ||
            b.dieId || b.instance || !b.baseCount || b.baseCount > kNavi31MaxBases)
            return Status::UnsupportedLayout;
    }
    const uint32_t raw[] = {0x63,0x64,0x91,0x7a};
    uint32_t offsets[4] = {};
    for (unsigned i = 0; i < 4; ++i) {
        ResolvedRegister r = {};
        if (resolveNavi31Register(ips,Navi31Block::Mp0,0,raw[i],barBytes,r) != IpLayoutStatus::Ok)
            return Status::OutsideBar;
        offsets[i] = r.mmioByteOffset;
    }
    const Layout l = {true,barBytes,offsets[0],offsets[1],offsets[2],offsets[3]};
    if (!validLayout(l)) return Status::OutsideBar;
    out = l; return Status::Ok;
}
Status parseSosImage(const uint8_t* data, size_t bytes, SosImage& out) {
    out = SosImage{};
    if (!interval(data,bytes)) return Status::InvalidFirmware;
    PspFirmwareV2 container = {};
    if (parsePspFirmwareV2(data,bytes,container) != hackynavi::Status::Ok)
        return Status::InvalidFirmware;
    if (container.header.ipMajor != 13 || container.header.ipMinor != 0)
        return Status::UnsupportedFirmware;
    const PspFirmwareDescriptor* images[13] = {};
    for (uint32_t i = 0; i < container.descriptorCount; ++i) {
        const auto& d = container.descriptors[i];
        // MP0v13 has no IPKEYMGR boot callback. Reject rather than silently
        // skip a future container with a dependency this implementation lacks.
        if (!d.type || d.type >= 13 || d.type == 11) return Status::UnsupportedFirmware;
        if (images[d.type] || !d.image.size || d.image.size > kStagingBytes)
            return Status::InvalidFirmware;
        for (uint32_t j = 0; j < i; ++j) {
            const auto& prev = container.descriptors[j].image;
            const uint64_t a = d.image.offset, b = prev.offset;
            if (a < b + prev.size && b < a + d.image.size) return Status::InvalidFirmware;
        }
        images[d.type] = &d;
    }
    if (!images[1] || !images[2] || (images[5] && !images[3])) return Status::InvalidFirmware;
    SosImage result = {};
    const uint32_t order[] = {3,5,2,7,8,9,10,12,1};
    const uint32_t command[] = {0x80000,0x10000000,0x10000,0xb0000,0xd0000,
                               0xc0000,0xe0000,0x20000000,0x20000};
    for (unsigned i = 0; i < kMaxBootComponents; ++i) {
        const uint32_t type = order[i];
        if (!images[type]) continue;
        // Exact source protocol: SPL is presence-gated by its descriptor but
        // psp_v13_0_bootloader_load_spl submits KDB data (not PSP14's SPL data).
        const uint32_t sourceType = type == 5 ? 3 : type;
        result.components[result.count++] = {type,sourceType,command[i],images[sourceType]->image};
    }
    result.sosVersion = images[1]->version;
    if (images[4]) result.toc = images[4]->image;
    if (images[6]) result.registerList = images[6]->image;
    out = result; return Status::Ok;
}
Status loadSos(const Layout& layout, const VramAllocator& allocator,
    VramAllocationHandle handle, const uint8_t* firmware, size_t firmwareBytes,
    uint8_t* workspace, size_t workspaceBytes, const Io& io, const Options& options,
    State& state, Result& out) {
    out = Result{}; out.stage = Stage::Preflight;
    if (state.phase() != Phase::Fresh) return finish(out,Status::InvalidState);
    if (!validLayout(layout)) return finish(out,Status::UnsupportedLayout);
    if (!io.read32 || !io.write32 || !io.flushHdp || !io.delayUs || !io.delayMs ||
        !options.bootloaderPolls || options.bootloaderPolls > 1000000 ||
        !options.heartbeatPolls || options.heartbeatPolls > 1000000 ||
        !options.pollDelayUs || options.pollDelayUs > 1000 ||
        uint64_t(options.bootloaderPolls)*options.pollDelayUs > 10000000 ||
        uint64_t(options.heartbeatPolls)*options.pollDelayUs > 10000000 ||
        workspaceBytes < kStagingBytes || !interval(workspace,workspaceBytes) ||
        !interval(firmware,firmwareBytes) || overlap(firmware,firmwareBytes,workspace,workspaceBytes))
        return finish(out,Status::InvalidArgument);
    SosImage image = {};
    const auto parsed = parseSosImage(firmware,firmwareBytes,image);
    if (parsed != Status::Ok) return finish(out,parsed);
    VramRange range = {}; uint64_t mc = 0, physical = 0;
    if (allocator.lookup(handle,range) != VramAllocatorStatus::Ok || range.bytes < kStagingBytes ||
        allocator.addresses(handle,mc,physical) != VramAllocatorStatus::Ok ||
        (mc & (kStagingBytes-1)) || mc >= (UINT64_C(1)<<48) ||
        kStagingBytes > (UINT64_C(1)<<48)-mc)
        return finish(out,Status::InvalidAllocation);
    out.stagingMcAddress = mc;
    Access::set(state,Phase::Running);
    out.stage = Stage::InitialSos;
    uint32_t heartbeat = 0;
    if (!read(layout,io,layout.heartbeat,heartbeat,out)) return fault(state,out,out.status);
    out.observedHeartbeat = heartbeat;
    if (heartbeat) {
        out.stage = Stage::SosVersion;
        if (!read(layout,io,layout.version,out.sosVersion,out)) return fault(state,out,out.status);
        Access::set(state,Phase::AlreadyRunning); return finish(out,Status::AlreadyRunning);
    }
    const VramTransferIo transferIo = {io.context,layout.barBytes,io.read32,io.write32};
    for (uint32_t i = 0; i < image.count; ++i) {
        const auto& c = image.components[i];
        out.componentIndex = i; out.componentType = c.firmwareType;
        out.sourceType = c.sourceType; out.commandValue = c.command;
        out.stage = Stage::InitialSos;
        if (!read(layout,io,layout.heartbeat,heartbeat,out)) return fault(state,out,out.status);
        if (heartbeat) return fault(state,out,Status::UnexpectedSos);
        out.stage = Stage::BootloaderReady;
        auto status = bootWait(layout,io,options,out);
        if (status != Status::Ok) return fault(state,out,status);
        out.stage = Stage::Upload;
        for (size_t j = 0; j < c.image.size; ++j) workspace[j] = firmware[c.image.offset+j];
        for (size_t j = c.image.size; j < kStagingBytes; ++j) workspace[j] = 0;
        const auto uploaded = uploadVram(allocator,handle,0,workspace,kStagingBytes,transferIo,out.transfer);
        out.stagingWritesMayHaveOccurred |= out.transfer.dataWritesMayHaveOccurred;
        if (uploaded != VramTransferStatus::Ok) return fault(state,out,Status::UploadFailed);
        out.stage = Stage::HdpFlush;
        if (!io.flushHdp(io.context)) return fault(state,out,Status::HdpFlushFailed);
        out.stage = Stage::PublishAddress;
        if (!write(io,layout.address,uint32_t(mc>>20),out)) return fault(state,out,out.status);
        out.stage = Stage::IssueCommand;
        out.commandMayHaveOccurred = true;
        const bool sos = c.firmwareType == 1;
        if (sos) out.sosCommandMayHaveOccurred = true;
        if (!write(io,layout.command,c.command,out)) {
            // Command failure can be ambiguous; preserve the required delay
            // before returning resources to a caller's recovery path.
            if (sos) io.delayMs(io.context,20);
            return fault(state,out,out.status);
        }
        if (!sos) {
            out.stage = Stage::BootloaderComplete;
            status = bootWait(layout,io,options,out);
            if (status != Status::Ok) return fault(state,out,status);
            ++out.completedComponents;
            continue;
        }
        out.stage = Stage::SosDelay; io.delayMs(io.context,20);
        out.stage = Stage::SosBaseline;
        if (!read(layout,io,layout.heartbeat,out.baselineHeartbeat,out)) return fault(state,out,out.status);
        out.stage = Stage::SosHeartbeat;
        bool changed = false;
        for (uint32_t j = 0; j < options.heartbeatPolls; ++j) {
            ++out.polls;
            if (!read(layout,io,layout.heartbeat,heartbeat,out)) return fault(state,out,out.status);
            out.observedHeartbeat = heartbeat;
            if (heartbeat && heartbeat != out.baselineHeartbeat) { changed = true; break; }
            if (j+1 < options.heartbeatPolls) io.delayUs(io.context,options.pollDelayUs);
        }
        if (!changed) return fault(state,out,Status::HeartbeatTimeout);
        out.stage = Stage::SosVersion;
        if (!read(layout,io,layout.version,out.sosVersion,out)) return fault(state,out,out.status);
        ++out.completedComponents;
    }
    out.stage = Stage::Complete; Access::set(state,Phase::Loaded);
    return finish(out,Status::Ok);
}
const char* statusString(Status status) {
    switch (status) {
    case Status::Ok: return "Ok";
    case Status::AlreadyRunning: return "AlreadyRunning";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::InvalidState: return "InvalidState";
    case Status::UnsupportedLayout: return "UnsupportedLayout";
    case Status::OutsideBar: return "OutsideBar";
    case Status::InvalidFirmware: return "InvalidFirmware";
    case Status::UnsupportedFirmware: return "UnsupportedFirmware";
    case Status::InvalidAllocation: return "InvalidAllocation";
    case Status::ReadFailed: return "ReadFailed";
    case Status::WriteFailed: return "WriteFailed";
    case Status::DeviceUnavailable: return "DeviceUnavailable";
    case Status::BootloaderTimeout: return "BootloaderTimeout";
    case Status::HeartbeatTimeout: return "HeartbeatTimeout";
    case Status::UnexpectedSos: return "UnexpectedSos";
    case Status::UploadFailed: return "UploadFailed";
    case Status::HdpFlushFailed: return "HdpFlushFailed";
    }
    return "Unknown";
}
} // namespace psp
} // namespace hackynavi
