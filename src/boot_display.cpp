// SPDX-License-Identifier: MIT
/*
 * DCN3.2 register definitions/decoding adapted from Linux at
 * 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
 * dcn_3_2_0_offset.h/sh_mask.h; dcn20_hubp.c, dcn30_hubp.c,
 * dcn30_hubbub.c and dcn32_resource.c.
 * Copyright 2012-2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "hackynavi/boot_display.hpp"

namespace hackynavi {
namespace bootdisplay {
namespace {
// Generated offsets in DWORDS. Every VGA control uses BASE_IDX1; every other
// register here uses BASE_IDX2. All4 generated instance offsets were checked:
// HUBP/HUBPREQ/CURSOR0_ instance stride is0xdc DWORDS, not a discovery instance.
const uint32_t vgaOffsets[kVgaCount] = {0xc, 0xe, 0x38, 0x39, 0x3a, 0x3b};
const uint32_t globalOffsets[kGlobalRegisterCount] = {0x475,0x476,0x477,0x478,0x479};
const uint32_t pipeOffsets[kPipeRegisterCount] = {
    0x5f3,0x5e5,0x5e7,0x5e9,0x5ea,0x607,0x61a,0x61b,0x61c,0x609,
    0x60a,0x60b,0x620,0x621,0x624,0x625,0x642,0x634,0x635,0x678,0x684,0x5f1
};
constexpr uint32_t kPipeStride = 0xdc;
// dcn32_hwseq.c167-196: domain0..3 correspond to HUBP0..3; PGFSM0 is
// powered-on and2 powered-off. sh_mask.h2781-2813: PGFSM bits31:30;
// desired power bit28. dcn32_hubp_pg_control writes CONFIG0 for ON and
// waits only for current PGFSM0, independently of desired bit28. FORCEON
// may be0 or1 when GATE is0. Preserve the raw desired bit for stability;
// it neither establishes a completed OFF state nor requires FORCEON1.
// The same current-state rule permits OFF PGFSM2 with desired0 or desired1;
// only an exact GATE1/FORCEON0 request permits the cold OFF-window skip.
const uint32_t powerOffsets[kPipeCount] = {0x81,0x83,0x85,0x87};
constexpr uint64_t kPageMask = 4095;
bool currentOn(uint32_t status) { return status==0 || status==0x10000000; }
bool currentOff(uint32_t status) { return status==0x80000000 || status==0x90000000; }
struct Dmu { uint64_t base1, base2,mp0; unsigned count,mp0Count; bool valid,mp0Valid; };
void collect(const DiscoveryIp& ip, void* context) {
    Dmu& d = *static_cast<Dmu*>(context);
    if(ip.hardwareId==255){++d.mp0Count;
        d.mp0Valid=ip.dieId==0 && ip.instance==0 && ip.major==13 && !ip.minor &&
            !ip.revision && !ip.variant && !ip.subRevision && !ip.harvest && ip.baseAddressCount;
        if(d.mp0Valid)d.mp0=ip.baseAddresses[0];return;}
    if (ip.hardwareId != 271) return;
    ++d.count;
    d.valid = ip.dieId == 0 && ip.instance == 0 && ip.major == 3 &&
        ip.minor == 2 && ip.revision == 0 && !ip.harvest && !ip.subRevision &&
        !ip.variant && ip.baseAddressCount >= 3 &&
        ip.baseAddresses[1] <= UINT32_MAX && ip.baseAddresses[2] <= UINT32_MAX;
    if (d.valid) { d.base1 = ip.baseAddresses[1]; d.base2 = ip.baseAddresses[2]; }
}
bool resolve(uint64_t base, uint32_t reg, uint64_t barBytes, uint32_t& out) {
    if (base > UINT32_MAX / 4U || reg > UINT32_MAX / 4U - base) return false;
    const uint64_t offset = (base + reg) * 4;
    if (barBytes < 4 || offset > barBytes - 4) return false;
    out = static_cast<uint32_t>(offset);
    return true;
}
bool populate(uint64_t base1, uint64_t base2, uint64_t barBytes, Layout& l,uint64_t mp0=UINT64_MAX) {
    l = Layout{}; l.barBytes = barBytes;
    for (size_t i = 0; i < kPipeCount; ++i)
        if (!resolve(base2, powerOffsets[i], barBytes, l.power[i]) ||
            !resolve(base2,powerOffsets[i]-1,barBytes,l.powerConfig[i])) return false;
    if(mp0!=UINT64_MAX && !resolve(mp0,0x91,barBytes,l.sos))return false;
    for (size_t i = 0; i < kVgaCount; ++i)
        if (!resolve(base1, vgaOffsets[i], barBytes, l.vga[i])) return false;
    for (size_t i = 0; i < kGlobalRegisterCount; ++i)
        if (!resolve(base2, globalOffsets[i], barBytes, l.global[i])) return false;
    for (size_t p = 0; p < kPipeCount; ++p)
        for (size_t i = 0; i < kPipeRegisterCount; ++i)
            if (!resolve(base2, pipeOffsets[i] + static_cast<uint32_t>(p) * kPipeStride,
                         barBytes, l.pipe[p][i])) return false;
    // Discovery must not alias legacy controls with another semantic register.
    // The fixed same-base tables are internally unique by construction.
    for (size_t i = 0; i < kVgaCount; ++i) {
        for (size_t p = 0; p < kPipeCount; ++p)
            if (l.vga[i] == l.power[p] || l.vga[i]==l.powerConfig[p]) return false;
        for (size_t j = 0; j < kGlobalRegisterCount; ++j)
            if (l.vga[i] == l.global[j]) return false;
        for (size_t p = 0; p < kPipeCount; ++p)
            for (size_t j = 0; j < kPipeRegisterCount; ++j)
                if (l.vga[i] == l.pipe[p][j]) return false;
    }
    if(l.sos){
        if(l.sos<0x1c || (l.sos>=0x7f000 && l.sos<0x80000))return false;
        for(size_t p=0;p<kPipeCount;++p){
            if(l.sos==l.power[p] || l.sos==l.powerConfig[p])return false;
            for(size_t i=0;i<kPipeRegisterCount;++i)if(l.sos==l.pipe[p][i])return false;
        }
        for(size_t i=0;i<kVgaCount;++i)if(l.sos==l.vga[i])return false;
        for(size_t i=0;i<kGlobalRegisterCount;++i)if(l.sos==l.global[i])return false;
    }
    l.valid = true;
    return true;
}
bool validLayout(const Layout& l) {
    if (!l.valid || (l.vga[0] & 3) || (l.global[0] & 3) ||
        l.vga[0] / 4 < vgaOffsets[0] || l.global[0] / 4 < globalOffsets[0]) return false;
    if(l.sos && ((l.sos&3) || l.sos/4<0x91))return false;
    Layout expected{};
    if (!populate(l.vga[0] / 4 - vgaOffsets[0], l.global[0] / 4 - globalOffsets[0],
                  l.barBytes, expected,l.sos?l.sos/4-0x91:UINT64_MAX)) return false;
    for (size_t i = 0; i < kPipeCount; ++i)
        if (l.power[i] != expected.power[i] || l.powerConfig[i]!=expected.powerConfig[i]) return false;
    for (size_t i = 0; i < kVgaCount; ++i) if (l.vga[i] != expected.vga[i]) return false;
    for (size_t i = 0; i < kGlobalRegisterCount; ++i)
        if (l.global[i] != expected.global[i]) return false;
    for (size_t p = 0; p < kPipeCount; ++p)
        for (size_t i = 0; i < kPipeRegisterCount; ++i)
            if (l.pipe[p][i] != expected.pipe[p][i]) return false;
    return true;
}
template<class Evidence>
Status read(const Io& io, uint32_t offset, uint32_t& value, Evidence& out) {
    out.lastRegister = offset; ++out.readsAttempted;
    out.lastValue = 0; out.lastReadSucceeded = false;
    uint32_t observed = 0;
    if (!io.read32(io.context, offset, observed)) return Status::TransportError;
    value = observed; out.lastValue = observed; out.lastReadSucceeded = true;
    return value == UINT32_MAX ? Status::DeviceUnavailable : Status::Ok;
}
Status readPower(const Layout& l, const Io& io, size_t p, Snapshot& s, Capture& out,bool cold,bool first=false) {
    out.failedPipe = static_cast<uint8_t>(p);
    out.lastPowerProbe = PowerProbe{};
    PowerProbe& probe = out.lastPowerProbe;
    probe.pipe = static_cast<uint8_t>(p);
    uint32_t power=0,config=0,after=0;
    Status status = read(io, l.power[p], power, out);
    if (out.lastReadSucceeded) { probe.statusBefore = power; probe.readsSucceededMask |= 1; }
    if (status != Status::Ok) return status;
    if(!currentOn(power) && (!cold || !currentOff(power)))return Status::PowerUnavailable;
    status=read(io,l.powerConfig[p],config,out);
    if (out.lastReadSucceeded) { probe.config = config; probe.readsSucceededMask |= 2; }
    if(status!=Status::Ok)return status;
    // PGFSM0 plus an ungated request permits an ON observation with either
    // FORCEON value. Do not require the desired bit to match: Linux waits
    // on current PGFSM only, and the target reports desired1/CONFIG0.
    // Explicit GATE, unknown bits and FORCEON+GATE remain rejected for ON.
    if(currentOff(power) ? config!=0x100 : config>1)
        return Status::PowerUnavailable;
    status=read(io,l.power[p],after,out);
    if (out.lastReadSucceeded) { probe.statusAfter = after; probe.readsSucceededMask |= 4; }
    if(status!=Status::Ok)return status;
    if(after!=power)return Status::PowerUnavailable;
    if(!first && (s.power[p]!=power || s.powerConfig[p]!=config))return Status::UnstableSnapshot;
    s.power[p]=power;s.powerConfig[p]=config;
    return Status::Ok;
}
Status snapshot(const Layout& l, const Io& io, Snapshot& s, Capture& out,bool cold) {
    for (size_t p = 0; p < kPipeCount; ++p) {
        const Status status = readPower(l, io, p, s, out,cold,true);
        if (status != Status::Ok) return status;
    }
    out.failedPipe = 0xff;
    for (size_t i = 0; i < kVgaCount; ++i) {
        const Status status = read(io, l.vga[i], s.vga[i], out);
        if (status != Status::Ok) return status;
    }
    for (size_t i = 0; i < kGlobalRegisterCount; ++i) {
        const Status status = read(io, l.global[i], s.global[i], out);
        if (status != Status::Ok) return status;
    }
    for (size_t p = 0; p < kPipeCount; ++p) {
        out.failedPipe = static_cast<uint8_t>(p);
        if(currentOff(s.power[p]))continue;
        for (size_t i = 0; i < kPipeRegisterCount; ++i) {
            Status status = readPower(l, io, p, s, out,cold);
            if (status != Status::Ok) return status;
            status = read(io, l.pipe[p][i], s.pipe[p][i], out);
            if (status != Status::Ok) return status;
        }
        const Status status = readPower(l, io, p, s, out,cold);
        if (status != Status::Ok) return status;
    }
    for (size_t p = 0; p < kPipeCount; ++p) {
        const Status status = readPower(l, io, p, s, out,cold);
        if (status != Status::Ok) return status;
    }
    out.failedPipe = 0xff;
    return Status::Ok;
}
uint32_t get(const uint32_t* p, PipeRegister r) { return p[static_cast<size_t>(r)]; }
bool stable(const Snapshot& a, const Snapshot& b, Capture& out) {
    for (size_t p = 0; p < kPipeCount; ++p)
        if (a.power[p] != b.power[p] || a.powerConfig[p]!=b.powerConfig[p]) {
            out.failedPipe = static_cast<uint8_t>(p); return false; }
    for (size_t i = 0; i < kVgaCount; ++i) if (a.vga[i] != b.vga[i]) return false;
    for (size_t i = 0; i < kGlobalRegisterCount; ++i)
        if (a.global[i] != b.global[i]) return false;
    for (size_t p = 0; p < kPipeCount; ++p) {
        for (size_t i = 0; i < kPipeRegisterCount; ++i) {
            // Live active-pipe request/blank/underflow status can legitimately
            // change. All programming bits stay exact. For a blank pipe both
            // passes must independently report no outstanding request.
            const uint32_t mask = i == static_cast<size_t>(PipeRegister::Control) ?
                ~0x70ff000aU : UINT32_MAX;
            if ((a.pipe[p][i] ^ b.pipe[p][i]) & mask) {
                out.failedPipe = static_cast<uint8_t>(p); return false;
            }
        }
    }
    return true;
}
bool framebufferMatches(const Snapshot& s, const MemoryLayout& m) {
    for (size_t i = 0; i < kGlobalRegisterCount; ++i)
        if (s.global[i] & 0xff000000U) return false;
    if ((uint64_t(s.global[0]) << 24) != m.mcWindowStart ||
        ((uint64_t(s.global[1]) << 24) | 0xffffff) != m.mcWindowEndInclusive ||
        (uint64_t(s.global[2]) << 24) != m.vramPageBaseOffset) return false;
    // DCN_VM_AGP_BOT/TOP describe an inclusive 16MiB-granularity window
    // (hubbub3_init_dchub_sys_ctx). Equal zero values are a real [0,16MiB)
    // window, not a disabled encoding. Its translation cannot affect any
    // admitted framebuffer address when the entire windows are disjoint.
    // Reject overlap independently of AGP_BASE, which we do not observe.
    return s.global[3] > s.global[4] ||
        ((uint64_t(s.global[4]) << 24) | 0xffffff) < m.mcWindowStart ||
        (uint64_t(s.global[3]) << 24) > m.mcWindowEndInclusive;
}
Status decodePlane(const uint32_t* p, const MemoryLayout& m, Plane& plane) {
    const uint32_t config = get(p, PipeRegister::SurfaceConfig);
    // hubp2_program_pixel_format: only these packed RGB formats have one plane.
    switch (config & 0x7f) {
    case 1: case 3: plane.bytesPerPixel = 2; break;
    case 8: case 10: plane.bytesPerPixel = 4; break;
    case 24: case 26: plane.bytesPerPixel = 8; break;
    default: return Status::UnsupportedPlane;
    }
    // NUM_PIPES/NUM_PKRS and related topology are in separate ADDR_CONFIG.
    // META_LINEAR/PIPE_ALIGNED are metadata layout flags; no metadata surface
    // is fetched with both DCC enables clear below (hubp3_dcc_control). Retain
    // SW_MODE0 is linear. DIM_TYPE0/1 are 1D/2D; hubp3_program_tiling,
    // used by DCN3.2, deliberately leaves this field unchanged. Both use
    // the same row footprint here; 3D/reserved dimensions stay unsupported.
    const uint32_t tiling = get(p, PipeRegister::TilingConfig);
    if ((config & ~0x7fU) || (tiling & ~0xa80U) ||
        get(p, PipeRegister::SurfaceControl) != 0 ||
        (get(p, PipeRegister::RequestSize) & 7)) return Status::UnsupportedPlane;
    // PENDING_DELAY and PENDING_MIN_TIME are timing configuration, distinct
    // from SURFACE_FLIP_PENDING. Preserve their exact two-pass stability but
    // allow them when no update is pending/locked. Stereo/GSL/triple remain
    // unsupported. The immediate-flip type bit does not itself imply pending.
    if ((get(p, PipeRegister::FlipControl) & ~0x3ff00002U) ||
        (get(p, PipeRegister::FlipControl2) & ~0x10ffU)) return Status::UnsupportedPlane;
    // hubp3_set_vm_system_aperture_settings programs precisely this policy.
    // TLB0 observed in firmware remains unsupported: legacy PA_ONLY names
    // do not establish DCN3.2's address domain or MC-to-physical conversion.
    if (get(p, PipeRegister::Vmid) != 0 || get(p, PipeRegister::TlbControl) != 0x19)
        return Status::UnsupportedAddressMode;
    const uint32_t hi = get(p, PipeRegister::AddressHi);
    // DCN3.2 uses hubp2_is_flip_pending: no pending bit and EARLIEST_INUSE
    // equals the requested address. Ordinary INUSE is a separate latched
    // view when RAED_NO_LATCH=0; it can be stale without an outstanding flip.
    // If reads explicitly bypass that latch, require agreement as well.
    if ((hi & 0xffff0000U) || get(p, PipeRegister::EarliestHi) != hi ||
        get(p, PipeRegister::EarliestLo) != get(p, PipeRegister::AddressLo))
        return Status::UnsupportedAddressMode;
    if ((get(p, PipeRegister::FlipControl2) & 0x1000) &&
        (get(p, PipeRegister::InUseHi) != hi ||
         get(p, PipeRegister::InUseLo) != get(p, PipeRegister::AddressLo)))
        return Status::UnsupportedAddressMode;
    plane.surfaceMcAddress = (uint64_t(hi) << 32) | get(p, PipeRegister::AddressLo);
    const uint32_t pitch = get(p, PipeRegister::Pitch);
    const uint32_t start = get(p, PipeRegister::ViewportStart);
    const uint32_t dimension = get(p, PipeRegister::ViewportDimension);
    if ((pitch & ~0x3fffU) || (start & ~0x3fff3fffU) ||
        (dimension & ~0x3fff3fffU) || (plane.surfaceMcAddress & 255))
        return Status::UnsupportedPlane;
    // PITCH is pixels minus1; viewport start/dimensions are direct pixels.
    plane.pitchPixels = (pitch & 0x3fff) + 1;
    plane.x = start & 0x3fff; plane.y = start >> 16;
    plane.width = dimension & 0x3fff; plane.height = dimension >> 16;
    if (!plane.width || !plane.height || plane.x >= plane.pitchPixels ||
        plane.width > plane.pitchPixels - plane.x) return Status::UnsupportedPlane;
    // Bound all complete rows from the surface base through the viewport.
    // Products are bounded by14-bit register fields (under2^33 bytes).
    const uint64_t pitchBytes = uint64_t(plane.pitchPixels) * plane.bytesPerPixel;
    if (pitchBytes & 255) return Status::UnsupportedPlane;
    plane.footprintBytes = pitchBytes * (uint64_t(plane.y) + plane.height);
    if (plane.surfaceMcAddress < m.vramStart) return Status::OutsideVram;
    const uint64_t offset = plane.surfaceMcAddress - m.vramStart;
    uint64_t checked = 0;
    if (vramMcAddress(m, offset, plane.footprintBytes, checked) != MemoryLayoutStatus::Ok ||
        checked != plane.surfaceMcAddress) return Status::OutsideVram;
    const uint32_t low = get(p, PipeRegister::ApertureLow);
    const uint32_t high = get(p, PipeRegister::ApertureHigh);
    if ((low | high) & 0xc0000000U || low > high ||
        plane.surfaceMcAddress < (uint64_t(low) << 18) ||
        plane.surfaceMcAddress + plane.footprintBytes - 1 > ((uint64_t(high) << 18) | 0x3ffff))
        return Status::UnsupportedAddressMode;
    const uint64_t end = offset + plane.footprintBytes;
    const uint64_t roundedEnd = (end + kPageMask) & ~kPageMask;
    plane.reservation = {offset & ~kPageMask, roundedEnd - (offset & ~kPageMask)};
    if (vramMcAddress(m, plane.reservation.offset, plane.reservation.bytes, checked) !=
        MemoryLayoutStatus::Ok) return Status::OutsideVram;
    return Status::Ok;
}
} // namespace

Status buildLayout(const uint8_t* data, size_t bytes, uint64_t barBytes, Layout& out) {
    out = Layout{};
    Dmu d{}; DiscoveryInfo info{};
    if (parseDiscovery(data, bytes, info, collect, &d) != DiscoveryStatus::Ok)
        return Status::DiscoveryRejected;
    if (!d.count) return Status::MissingDmu;
    if (!d.valid || d.count != 1 || (d.mp0Count && (!d.mp0Valid || d.mp0Count!=1)) ||
        info.baseAddresses64Bit || info.dieCount != 1 ||
        info.dies[0].id != 0) return Status::UnsupportedDmu;
    Layout l{};
    if (!populate(d.base1, d.base2, barBytes, l,d.mp0Count?d.mp0:UINT64_MAX)) return Status::OutsideBar;
    out = l;
    return Status::Ok;
}
Status inventoryPower(const Layout& l,const Io& io,PowerInventory& out) {
    out=PowerInventory{};out.failedPipe=0xff;
    for(size_t p=0;p<kPipeCount;++p)out.probes[p].pipe=0xff;
    if(!io.read32 || !validLayout(l))return Status::InvalidArgument;
    for(size_t p=0;p<kPipeCount;++p) {
        out.failedPipe=static_cast<uint8_t>(p);
        PowerProbe& probe=out.probes[p];probe.pipe=static_cast<uint8_t>(p);
        uint32_t* values[]={&probe.statusBefore,&probe.config,&probe.statusAfter};
        const uint32_t offsets[]={l.power[p],l.powerConfig[p],l.power[p]};
        for(size_t phase=0;phase<3;++phase) {
            const Status status=read(io,offsets[phase],*values[phase],out);
            if(out.lastReadSucceeded)probe.readsSucceededMask|=static_cast<uint8_t>(1U<<phase);
            if(status!=Status::Ok)return status;
        }
    }
    out.failedPipe=0xff;return Status::Ok;
}
static Status captureImpl(const Layout& l, const MemoryLayout& m, const Io& io, Capture& out,bool cold) {
    out = Capture{}; out.failedPipe = 0xff;
    out.lastPowerProbe.pipe = 0xff;
    uint64_t checked = 0;
    if (!io.read32 || !validLayout(l) || (cold && !l.sos) ||
        vramMcAddress(m, 0, m.vramBytes, checked) != MemoryLayoutStatus::Ok ||
        vramPhysicalAddress(m, 0, m.vramBytes, checked) != MemoryLayoutStatus::Ok ||
        m.mcWindowStart != m.vramStart || (m.mcWindowStart & 0xffffff) ||
        m.mcWindowEndInclusive >= (1ULL << 48) ||
        (m.mcWindowEndInclusive & 0xffffff) != 0xffffff ||
        m.mcWindowEndInclusive < m.vramEndInclusive ||
        (m.vramPageBaseOffset & 0xffffff))
        return Status::InvalidArgument;
    Snapshot second{};
    const auto checkSos=[&]() -> Status {
        uint32_t value=0;const Status status=read(io,l.sos,value,out);
        return status==Status::Ok?(value?Status::PriorDriverRequiresReset:Status::Ok):status;
    };
    Status status=Status::Ok;
    if(cold){status=checkSos();if(status!=Status::Ok)return status;}
    status = snapshot(l, io, out.snapshot, out,cold);
    if (status != Status::Ok) return status;
    if(cold){status=checkSos();if(status!=Status::Ok)return status;}
    status = snapshot(l, io, second, out,cold);
    if (status != Status::Ok) return status;
    if(cold){status=checkSos();if(status!=Status::Ok)return status;}
    if (!stable(out.snapshot, second, out)) return Status::UnstableSnapshot;
    out.snapshotReadComplete = true;
    for (size_t i = 0; i < kVgaCount; ++i)
        if (out.snapshot.vga[i] & 1) return Status::LegacyVga;
    Plane planes[kPipeCount]{}; size_t count = 0;
    for (size_t p = 0; p < kPipeCount; ++p) {
        out.failedPipe = static_cast<uint8_t>(p);
        if(currentOff(out.snapshot.power[p]))continue;
        const uint32_t* first = out.snapshot.pipe[p];
        const uint32_t control = get(first, PipeRegister::Control);
        // Cursor/DMDATA can add independent memory fetches; never ignore them.
        if ((get(first, PipeRegister::CursorControl) & 1) ||
            get(first, PipeRegister::DmdataControl) != 0 || (control & 4))
            return Status::UnsupportedPlane;
        if (control & 1) {
            if (!(control & 2) || !(get(second.pipe[p], PipeRegister::Control) & 2))
                return Status::UnsupportedPlane;
            continue;
        }
        if (!framebufferMatches(out.snapshot, m)) return Status::UnsupportedAddressMode;
        planes[count].pipe = static_cast<uint8_t>(p);
        status = decodePlane(first, m, planes[count]);
        if (status != Status::Ok) return status;
        ++count;
    }
    for (size_t i = 0; i < count; ++i) out.planes[i] = planes[i];
    for(size_t p=0;p<kPipeCount;++p)if(currentOff(out.snapshot.power[p]))out.poweredOffMask|=uint8_t(1U<<p);
    out.planeCount = count; out.complete = true; out.failedPipe = 0xff;
    return Status::Ok;
}
Status capture(const Layout& l,const MemoryLayout& m,const Io& io,Capture& out) {
    return captureImpl(l,m,io,out,false);
}
Status capturePostedCold(const Layout& l,const MemoryLayout& m,const Io& io,Capture& out) {
    return captureImpl(l,m,io,out,true);
}
const char* statusString(Status s) {
    switch (s) {
    case Status::Ok: return "ok";
    case Status::InvalidArgument: return "invalid boot display argument";
    case Status::DiscoveryRejected: return "display discovery rejected";
    case Status::UnsupportedDmu: return "unsupported DMU layout";
    case Status::MissingDmu: return "DMU missing";
    case Status::OutsideBar: return "display register outside BAR";
    case Status::TransportError: return "display register read failed";
    case Status::DeviceUnavailable: return "display register returned all ones";
    case Status::PowerUnavailable: return "HUBP power state or request is unsupported";
    case Status::PriorDriverRequiresReset: return "nonzero PSP SOS requires prior-driver reset";
    case Status::UnstableSnapshot: return "display changed during capture";
    case Status::LegacyVga: return "legacy VGA reservation cannot be derived";
    case Status::UnsupportedPlane: return "display surface footprint is not supported";
    case Status::UnsupportedAddressMode: return "display address translation is not supported";
    case Status::OutsideVram: return "display footprint exceeds VRAM";
    }
    return "unknown boot display status";
}
} // namespace bootdisplay
} // namespace hackynavi
