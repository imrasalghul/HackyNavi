// SPDX-License-Identifier: MIT
// Read-callback simulations only; no hardware is accessed or modified.
#include "hackynavi/boot_display_capture.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
using namespace hackynavi;
using namespace hackynavi::bootdisplay;
#include "fixtures/boot_display_9eda.hpp"
namespace {
size_t checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << "line " << __LINE__ << ": " #x "\n"; } } while (0)
constexpr size_t kPassReads = 399;
constexpr uint32_t base1 = 0x1000, base2 = 0x10000;
void put16(std::vector<uint8_t>& b, size_t i, uint16_t v) {
    b[i] = static_cast<uint8_t>(v); b[i+1] = static_cast<uint8_t>(v >> 8);
}
void put32(std::vector<uint8_t>& b, size_t i, uint32_t v) {
    put16(b, i, static_cast<uint16_t>(v)); put16(b, i+2, static_cast<uint16_t>(v >> 16));
}
uint16_t sum(const std::vector<uint8_t>& b, size_t i) {
    uint16_t result = 0;
    for (; i < b.size(); ++i) result = static_cast<uint16_t>(result + b[i]);
    return result;
}
void fix(std::vector<uint8_t>& b) { put16(b, 14, sum(b, 60)); put16(b, 8, sum(b, 10)); }
std::vector<uint8_t> fixture(unsigned count = 1, bool wide = false) {
    const size_t recordBytes = 8 + 3 * (wide ? 8 : 4);
    std::vector<uint8_t> b(144 + count * recordBytes, 0);
    put32(b, 0, 0x28211407); put16(b, 4, 1); put16(b, 6, 4);
    put16(b, 10, static_cast<uint16_t>(b.size())); put16(b, 12, 60);
    put16(b, 16, static_cast<uint16_t>(b.size() - 60));
    put32(b, 60, 0x53445049); put16(b, 64, wide ? 4 : 3);
    put16(b, 66, static_cast<uint16_t>(b.size() - 60));
    put16(b, 72, 1); put16(b, 76, 140); b[138] = wide ? 1 : 0;
    put16(b, 142, static_cast<uint16_t>(count));
    for (unsigned n = 0; n < count; ++n) {
        const size_t p = 144 + n * recordBytes;
        put16(b, p, 271); b[p+3] = 3; b[p+4] = 3; b[p+5] = 2;
        put32(b, p+8+(wide?8:4), base1); put32(b, p+8+2*(wide?8:4), base2);
    }
    fix(b); return b;
}
Layout layout() {
    auto b = fixture(); Layout l{};
    CHECK(buildLayout(b.data(), b.size(), 0x100000, l) == Status::Ok);
    return l;
}
MemoryLayout memory() {
    MemoryLayout m{};
    CHECK(deriveMemoryLayout({0x100,0x6ff,0x1000,24ULL<<30,0x8000000000ULL,256ULL<<20},m)
          == MemoryLayoutStatus::Ok);
    return m;
}
struct Simulation {
    Layout l; MemoryLayout m;
    std::map<uint32_t,uint32_t> values;
    std::vector<uint32_t> reads;
    size_t failAt = 0, allOnesAt = 0, mutateAt = 0;
    uint32_t mutateRegister = 0, mutateXor = 0;
    explicit Simulation(const Layout& selected = layout()) : l(selected), m(memory()) {
        for (size_t p=0;p<kPipeCount;++p){values[l.power[p]]=0;values[l.powerConfig[p]]=0;}
        if(l.sos)values[l.sos]=0;
        for (size_t i=0;i<kVgaCount;++i) values[l.vga[i]]=0;
        for (size_t i=0;i<kGlobalRegisterCount;++i) values[l.global[i]]=0;
        values[l.global[0]]=0x100; values[l.global[1]]=0x6ff; values[l.global[2]]=0x1000;
        values[l.global[3]]=0xffffff;
        for (size_t p=0;p<4;++p) {
            for (size_t r=0;r<kPipeRegisterCount;++r) values[l.pipe[p][r]]=0;
            set(p,PipeRegister::Control,3); // Explicit blank plus drained.
        }
    }
    void set(size_t p,PipeRegister r,uint32_t value) { values[l.pipe[p][static_cast<size_t>(r)]]=value; }
    uint32_t reg(size_t p,PipeRegister r) const { return l.pipe[p][static_cast<size_t>(r)]; }
    void address(size_t p,uint64_t mc) {
        for (PipeRegister r : {PipeRegister::AddressLo,PipeRegister::InUseLo,PipeRegister::EarliestLo})
            set(p,r,static_cast<uint32_t>(mc));
        for (PipeRegister r : {PipeRegister::AddressHi,PipeRegister::InUseHi,PipeRegister::EarliestHi})
            set(p,r,static_cast<uint32_t>(mc>>32));
    }
    void activate(size_t p,uint64_t offset=0x02000000) {
        set(p,PipeRegister::Control,0);
        set(p,PipeRegister::SurfaceConfig,8); set(p,PipeRegister::Pitch,2047);
        set(p,PipeRegister::ViewportDimension,(1080U<<16)|1920);
        set(p,PipeRegister::TlbControl,0x19);
        set(p,PipeRegister::ApertureLow,0x4000); set(p,PipeRegister::ApertureHigh,0x1bfff);
        address(p,m.vramStart+offset);
    }
    Io io() { return {this,read}; }
    static bool read(void* context,uint32_t offset,uint32_t& value) {
        auto& s=*static_cast<Simulation*>(context);
        s.reads.push_back(offset);
        CHECK(s.values.count(offset)==1); // No unintended registers are read.
        if (s.reads.size()==s.mutateAt) s.values[s.mutateRegister]^=s.mutateXor;
        value=s.reads.size()==s.allOnesAt?UINT32_MAX:s.values[offset];
        for (size_t p=0;p<kPipeCount;++p) for(size_t r=0;r<kPipeRegisterCount;++r)
            if(offset==s.l.pipe[p][r]) {
                CHECK(s.reads.size()>1 && s.reads[s.reads.size()-2]==s.l.power[p]);
                CHECK((s.values[s.l.power[p]]==0 || s.values[s.l.power[p]]==0x10000000) &&
                      s.values[s.l.powerConfig[p]]<=1);
            }
        return s.reads.size()!=s.failAt;
    }
};
void emptyOutput(const Capture& c) {
    CHECK(!c.complete && c.planeCount==0);
    for (const Plane& p:c.planes)
        CHECK(!p.surfaceMcAddress && !p.footprintBytes && !p.reservation.bytes && !p.reservation.offset);
}
void layoutTests() {
    Layout l=layout();
    CHECK(l.vga[0]==0x4030 && l.vga[5]==0x40ec);
    CHECK(l.power[0]==0x40204 && l.power[3]==0x4021c);
    CHECK(l.global[0]==0x411d4 && l.global[4]==0x411e4);
    CHECK(l.pipe[0][0]==0x417cc && l.pipe[3][0]==0x4221c);
    CHECK(l.pipe[0][10]==0x41828 && l.pipe[3][10]==0x42278);
    CHECK(l.pipe[0][19]==0x419e0 && l.pipe[3][19]==0x42430);
    auto b=fixture();
    for (size_t n=0;n<b.size();++n) {
        l=layout(); CHECK(buildLayout(b.data(),n,0x100000,l)==Status::DiscoveryRejected);
        CHECK(!l.valid && !l.barBytes);
    }
    for (unsigned defect=0;defect<10;++defect) {
        b=fixture(); Status expected=Status::UnsupportedDmu;
        switch(defect) {
        case 0: put16(b,144,270); expected=Status::MissingDmu; break;
        case 1: b[146]=1; break;
        case 2: b[148]=4; break;
        case 3: b[149]=3; break;
        case 4: b[150]=1; break;
        case 5: b[151]=1; break;
        case 6: put16(b,74,1);put16(b,140,1);break;
        case 7: b=fixture(2);break;
        case 8: b=fixture(1,true);break;
        case 9: put32(b,160,UINT32_MAX);expected=Status::OutsideBar;break;
        }
        fix(b); CHECK(buildLayout(b.data(),b.size(),0x100000,l)==expected);
        CHECK(!l.valid);
    }
    b=fixture();
    for (uint64_t bar:{uint64_t(0),uint64_t(3),uint64_t(0x4000),uint64_t(0x42463)}) {
        CHECK(buildLayout(b.data(),b.size(),bar,l)==Status::OutsideBar);CHECK(!l.valid);
    }
    // Distinct discovery base indices cannot alias semantic registers.
    put32(b,156,base2+0x475-0xc);fix(b);
    CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::OutsideBar);
}
void golden() {
    Simulation s;Capture c{};
    CHECK(capture(s.l,s.m,s.io(),c)==Status::Ok);
    CHECK(c.complete && c.planeCount==0 && c.readsAttempted==798 && s.reads.size()==798);
    for (size_t p=0;p<4;++p) s.activate(p,0x02000000+p*0x01000000);
    const auto unchanged=s.values;s.reads.clear();
    CHECK(capture(s.l,s.m,s.io(),c)==Status::Ok);
    CHECK(c.complete && c.planeCount==4 && c.failedPipe==0xff);
    CHECK(s.values==unchanged);
    for (size_t p=0;p<4;++p) {
        const Plane& a=c.planes[p];
        CHECK(a.pipe==p && a.bytesPerPixel==4 && a.pitchPixels==2048);
        CHECK(a.x==0 && a.y==0 && a.width==1920 && a.height==1080);
        CHECK(a.surfaceMcAddress==0x102000000ULL+p*0x01000000);
        CHECK(a.footprintBytes==8847360 && a.reservation.bytes==8847360);
        CHECK(a.reservation.offset==0x02000000+p*0x01000000);
    }
    // Nonzero viewport origin reserves preceding rows too; base is deliberately
    // not a page boundary. MC, physical-page and CPU BAR domains are different.
    Simulation shifted;shifted.activate(2,0x03000100);
    shifted.set(2,PipeRegister::ViewportStart,(7U<<16)|32);
    shifted.set(2,PipeRegister::ViewportDimension,(17U<<16)|100);
    CHECK(capture(shifted.l,shifted.m,shifted.io(),c)==Status::Ok);
    CHECK(c.planeCount==1 && c.planes[0].pipe==2);
    CHECK(c.planes[0].surfaceMcAddress==0x103000100ULL);
    CHECK(c.planes[0].footprintBytes==196608);
    CHECK(c.planes[0].reservation.offset==0x03000000 && c.planes[0].reservation.bytes==200704);
    // All supported single-plane RGB encodings have independently checked bpp.
    const uint32_t formats[]={1,3,8,10,24,26};
    const uint32_t pixelBytes[]={2,2,4,4,8,8};
    for (size_t i=0;i<6;++i) {
        Simulation rgb;rgb.activate(0);rgb.set(0,PipeRegister::SurfaceConfig,formats[i]);
        CHECK(capture(rgb.l,rgb.m,rgb.io(),c)==Status::Ok);
        CHECK(c.planes[0].bytesPerPixel==pixelBytes[i]);
        CHECK(c.planes[0].footprintBytes==uint64_t(2048)*1080*pixelBytes[i]);
    }
    for (uint32_t metadata:{0x200U,0x800U,0xa00U}) {
        Simulation rgb;rgb.activate(0);rgb.set(0,PipeRegister::TilingConfig,metadata);
        CHECK(capture(rgb.l,rgb.m,rgb.io(),c)==Status::Ok);
        CHECK(c.planes[0].footprintBytes==8847360);
        rgb.set(0,PipeRegister::SurfaceControl,2);
        CHECK(capture(rgb.l,rgb.m,rgb.io(),c)==Status::UnsupportedPlane);emptyOutput(c);
    }
    // Last valid scanout page at the end of24GiB VRAM, not a32-bit truncation.
    Simulation high;high.activate(3,high.m.vramBytes-4096);
    high.set(3,PipeRegister::Pitch,63);high.set(3,PipeRegister::ViewportDimension,(16U<<16)|64);
    CHECK(capture(high.l,high.m,high.io(),c)==Status::Ok);
    CHECK(c.planes[0].surfaceMcAddress==0x6fffff000ULL);
    CHECK(c.planes[0].reservation.offset==(24ULL<<30)-4096 && c.planes[0].reservation.bytes==4096);
}
void invalidModes() {
    struct Bad {PipeRegister reg;uint32_t value;Status status;};
    const Bad bad[]={
        {PipeRegister::SurfaceConfig,65,Status::UnsupportedPlane},
        {PipeRegister::SurfaceConfig,8|0x100,Status::UnsupportedPlane},
        {PipeRegister::SurfaceConfig,8|0x400,Status::UnsupportedPlane},
        {PipeRegister::SurfaceConfig,8|0x800,Status::UnsupportedPlane},
        {PipeRegister::TilingConfig,4,Status::UnsupportedPlane},
        {PipeRegister::TilingConfig,0x100,Status::UnsupportedPlane},
        {PipeRegister::TilingConfig,0x180,Status::UnsupportedPlane},
        {PipeRegister::TilingConfig,0x20,Status::UnsupportedPlane},
        {PipeRegister::SurfaceControl,2,Status::UnsupportedPlane},
        {PipeRegister::SurfaceControl,1,Status::UnsupportedPlane},
        {PipeRegister::FlipControl,1,Status::UnsupportedPlane},
        {PipeRegister::FlipControl,0x100,Status::UnsupportedPlane},
        {PipeRegister::FlipControl,0x10000,Status::UnsupportedPlane},
        {PipeRegister::FlipControl2,0x100,Status::UnsupportedPlane},
        {PipeRegister::FlipControl2,0x400,Status::UnsupportedPlane},
        {PipeRegister::Vmid,1,Status::UnsupportedAddressMode},
        {PipeRegister::TlbControl,0,Status::UnsupportedAddressMode},
        {PipeRegister::TlbControl,0x59,Status::UnsupportedAddressMode},
        {PipeRegister::AddressHi,0x10000,Status::UnsupportedAddressMode},
        {PipeRegister::EarliestLo,0x02000100,Status::UnsupportedAddressMode},
        {PipeRegister::Pitch,63,Status::UnsupportedPlane},
        {PipeRegister::Pitch,2046,Status::UnsupportedPlane},
        {PipeRegister::Pitch,2047|0x10000,Status::UnsupportedPlane},
        {PipeRegister::ViewportStart,0x4000,Status::UnsupportedPlane},
        {PipeRegister::ViewportDimension,0,Status::UnsupportedPlane},
        {PipeRegister::ViewportDimension,(1080U<<16)|2049,Status::UnsupportedPlane},
        {PipeRegister::ApertureLow,0x4081,Status::UnsupportedAddressMode},
        {PipeRegister::ApertureHigh,0x4000,Status::UnsupportedAddressMode},
        {PipeRegister::ApertureHigh,0x40000000,Status::UnsupportedAddressMode},
        {PipeRegister::CursorControl,1,Status::UnsupportedPlane},
        {PipeRegister::DmdataControl,4,Status::UnsupportedPlane},
        {PipeRegister::RequestSize,1,Status::UnsupportedPlane},
        {PipeRegister::Control,1,Status::UnsupportedPlane},
        {PipeRegister::Control,4,Status::UnsupportedPlane}
    };
    for (size_t p=0;p<4;++p) for (const Bad& b:bad) {
        Simulation s;Capture c{};s.activate(0);s.activate(p);
        s.set(p,b.reg,b.value);
        CHECK(capture(s.l,s.m,s.io(),c)==b.status);emptyOutput(c);
        CHECK(c.failedPipe==p);
    }
    for (size_t vga=0;vga<6;++vga) {
        Simulation s;Capture c{};s.values[s.l.vga[vga]]=1;
        CHECK(capture(s.l,s.m,s.io(),c)==Status::LegacyVga);emptyOutput(c);
    }
    for (size_t global=0;global<5;++global) {
        Simulation s;Capture c{};s.activate(0);
        s.values[s.l.global[global]]=UINT32_MAX-1;
        CHECK(capture(s.l,s.m,s.io(),c)==Status::UnsupportedAddressMode);emptyOutput(c);
    }
    for (uint64_t address:{uint64_t(0),uint64_t(0x1000000000),uint64_t(0x8000000000),uint64_t(0x6fffff000)}) {
        Simulation s;Capture c{};s.activate(0);s.address(0,address);
        CHECK(capture(s.l,s.m,s.io(),c)==Status::OutsideVram);emptyOutput(c);
    }
}
void linearAddressPolicy() {
    Capture c{};
    // Every individual swizzle/dimension/reserved bit remains checked; the
    // only added data-surface encoding is linear DIM_TYPE2D (bit7).
    for(unsigned bit=0;bit<32;++bit) {
        Simulation s;s.activate(0);s.set(0,PipeRegister::TilingConfig,uint32_t(1)<<bit);
        const bool allowed=bit==7 || bit==9 || bit==11;
        CHECK(capture(s.l,s.m,s.io(),c)==(allowed?Status::Ok:Status::UnsupportedPlane));
        if(!allowed)emptyOutput(c);
    }
    for(unsigned bit=0;bit<32;++bit) {
        Simulation s;s.activate(0);s.set(0,PipeRegister::FlipControl,uint32_t(1)<<bit);
        const bool allowed=bit==1 || (bit>=20 && bit<=29);
        CHECK(capture(s.l,s.m,s.io(),c)==(allowed?Status::Ok:Status::UnsupportedPlane));
        if(!allowed)emptyOutput(c);
    }
    // Ordinary INUSE is latched unless NO_LATCH is set. EARLIEST always
    // decides whether the requested address has stopped being pending.
    for(bool noLatch:{false,true})for(PipeRegister r:{PipeRegister::InUseLo,PipeRegister::InUseHi,
                                                   PipeRegister::EarliestLo,PipeRegister::EarliestHi}) {
        Simulation s;s.activate(0);s.set(0,PipeRegister::FlipControl2,noLatch?0x1000:0);
        s.values[s.reg(0,r)]^=0x100;
        const bool ignored=!noLatch && (r==PipeRegister::InUseLo || r==PipeRegister::InUseHi);
        CHECK(capture(s.l,s.m,s.io(),c)==(ignored?Status::Ok:Status::UnsupportedAddressMode));
        if(!ignored)emptyOutput(c);
    }
    // Timing values cannot hide real pending, update/master locks or stereo.
    for(uint32_t forbidden:{1U,0x100U,0x200U,0x1000U,0x10000U}) {
        Simulation s;s.activate(0);s.set(0,PipeRegister::FlipControl,0x3ff00002|forbidden);
        CHECK(capture(s.l,s.m,s.io(),c)==Status::UnsupportedPlane);emptyOutput(c);
    }
    // Inclusive AGP windows may sit on either side of FB, including 0/0.
    // Even a one-unit overlap at either end remains unconditionally rejected.
    struct Window {uint32_t low,high;bool accepted;};
    for(const Window& w:{Window{0,0,true},{0,0xff,true},{0x700,0x700,true},
                        {0x100,0x100,false},{0x6ff,0x6ff,false},{0xff,0x100,false},
                        {0x6ff,0x700,false},{0x100,0x6ff,false},{0,0xffffff,false},
                        {0xffffff,0,true}}) {
        Simulation s;s.activate(0);s.values[s.l.global[3]]=w.low;s.values[s.l.global[4]]=w.high;
        CHECK(capture(s.l,s.m,s.io(),c)==(w.accepted?Status::Ok:Status::UnsupportedAddressMode));
        if(!w.accepted)emptyOutput(c);
    }
    // No additional TLB mode is admitted on the strength of legacy enum names.
    for(uint32_t tlb=0;tlb<128;++tlb) {
        Simulation s;s.activate(0);s.set(0,PipeRegister::TlbControl,tlb);
        CHECK(capture(s.l,s.m,s.io(),c)==(tlb==0x19?Status::Ok:Status::UnsupportedAddressMode));
        if(tlb!=0x19)emptyOutput(c);
    }
}
void failuresAndStability() {
    for (size_t at=1;at<=2*kPassReads;++at) for (bool allOnes:{false,true}) {
        Simulation s;Capture c{};s.activate(0);
        if (allOnes)s.allOnesAt=at;else s.failAt=at;
        CHECK(capture(s.l,s.m,s.io(),c)==(allOnes?Status::DeviceUnavailable:Status::TransportError));
        CHECK(c.readsAttempted==at && s.reads.size()==at);emptyOutput(c);
        CHECK(c.lastRegister==s.reads.back() && c.lastReadSucceeded==allOnes);
        CHECK(c.lastValue==(allOnes?UINT32_MAX:0U));
    }
    // Any programmed field changing between passes invalidates all intervals.
    Simulation base;base.activate(0);Capture c{};
    CHECK(capture(base.l,base.m,base.io(),c)==Status::Ok);
    for (size_t index=0;index<kPassReads;++index) {
        Simulation s;s.activate(0);s.mutateAt=kPassReads+1;
        s.mutateRegister=base.reads[index];s.mutateXor=1;
        bool power=false;
        for(size_t p=0;p<kPipeCount;++p) power=power || s.mutateRegister==s.l.power[p];
        CHECK(capture(s.l,s.m,s.io(),c)==(power?Status::PowerUnavailable:Status::UnstableSnapshot));
        emptyOutput(c);
    }
    // Volatile status bits can change on an active pipeline without changing
    // the reservation. Blanked pipelines still require drained status twice.
    for (uint32_t dynamic:{2U,8U,0xf0000U,0xf00000U,0x70000000U}) {
        Simulation s;s.activate(0);s.mutateAt=kPassReads+1;
        s.mutateRegister=s.reg(0,PipeRegister::Control);s.mutateXor=dynamic;
        CHECK(capture(s.l,s.m,s.io(),c)==Status::Ok);
    }
    Simulation draining;draining.mutateAt=kPassReads+1;
    draining.mutateRegister=draining.reg(3,PipeRegister::Control);draining.mutateXor=2;
    CHECK(capture(draining.l,draining.m,draining.io(),c)==Status::UnsupportedPlane);emptyOutput(c);
}
void powerTests() {
    for(size_t p=0;p<kPipeCount;++p) {
        for(uint32_t state:{0x40000000U,0x80000000U,0xc0000000U,1U}) {
            Simulation s;Capture c{};s.activate(0);s.values[s.l.power[p]]=state;
            CHECK(capture(s.l,s.m,s.io(),c)==Status::PowerUnavailable);emptyOutput(c);
            const size_t expected=p*3+1;
            CHECK(c.failedPipe==p && c.readsAttempted==expected && s.reads.size()==expected);
        }
    }
    // A gate change at any of the guard/postcheck reads stops the capture
    // immediately. No guarded window access follows the observed power loss.
    Simulation base;Capture c{};base.activate(0);
    CHECK(capture(base.l,base.m,base.io(),c)==Status::Ok);
    for(size_t at=0;at<base.reads.size();++at) {
        for(size_t p=0;p<kPipeCount;++p) if(base.reads[at]==base.l.power[p]) {
            Simulation s;s.activate(0);s.mutateAt=at+1;
            s.mutateRegister=s.l.power[p];s.mutateXor=0x80000000;
            CHECK(capture(s.l,s.m,s.io(),c)==Status::PowerUnavailable);emptyOutput(c);
            CHECK(c.failedPipe==p && c.readsAttempted==at+1 && s.reads.size()==at+1);
            CHECK(c.lastRegister==s.l.power[p]);
        }
    }
}
void preflight() {
    for (unsigned defect=0;defect<14;++defect) {
        Simulation s;Capture c{};Io io=s.io();
        switch(defect) {
        case 0:io.read32=nullptr;break;
        case 1:s.l.valid=false;break;
        case 2:s.l.barBytes=3;break;
        case 3:s.l.vga[5]+=4;break;
        case 4:s.l.global[4]+=4;break;
        case 5:s.l.pipe[3][21]+=4;break;
        case 6:s.l.pipe[3][21]=UINT32_MAX;break;
        case 7:s.m.vramBytes=0;break;
        case 8:s.m.vramPageBaseOffset=UINT64_MAX;break;
        case 9:s.m.mcWindowStart+=4096;break;
        case 10:s.m.mcWindowEndInclusive=s.m.vramStart;break;
        case 11:s.m.vramEndInclusive=UINT64_MAX;break;
        case 12:s.l.power[3]+=4;break;
        case 13:s.l.powerConfig[3]+=4;break;
        }
        CHECK(capture(s.l,s.m,io,c)==Status::InvalidArgument);
        CHECK(s.reads.empty() && c.readsAttempted==0);emptyOutput(c);
        CHECK(!c.lastReadSucceeded && !c.lastValue && c.lastPowerProbe.pipe==0xff &&
              !c.lastPowerProbe.readsSucceededMask);
    }
}
void realDiscovery(const char* path) {
    std::ifstream f(path,std::ios::binary);
    std::vector<uint8_t>b((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
    CHECK(!b.empty());if(b.empty())return;
    Layout l{};CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::Ok);
    // Only register resolution uses this real ROM metadata; the following
    // values remain explicitly simulated and do not claim a live scanout.
    Simulation s(l);s.activate(0);Capture c{};
    CHECK(capture(l,s.m,s.io(),c)==Status::Ok && c.planeCount==1);
}
Layout coldLayout(uint32_t selectedBase1=base1,uint32_t selectedBase2=base2) {
    auto b=fixture();const size_t at=b.size();b.resize(at+16,0);
    put32(b,156,selectedBase1);put32(b,160,selectedBase2);
    put16(b,10,uint16_t(b.size()));put16(b,16,uint16_t(b.size()-60));
    put16(b,66,uint16_t(b.size()-60));put16(b,142,2);
    put16(b,at,255);b[at+3]=2;b[at+4]=13;put32(b,at+8,0x16000);put32(b,at+12,0x16000);fix(b);
    Layout l{};CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::Ok && l.sos==0x58244);
    return l;
}
void targetFirmwareReplay() {
    const Layout l=coldLayout(0xc0,0x34c0);
    const auto initialize=[&](Simulation& s) {
        CHECK(deriveMemoryLayout(target9eda::memory,s.m)==MemoryLayoutStatus::Ok);
        for(size_t r=0;r<kGlobalRegisterCount;++r) {
            CHECK(l.global[r]==target9eda::globalOffsets[r]);
            s.values[l.global[r]]=target9eda::globalValues[r];
        }
        for(size_t r=0;r<kVgaCount;++r) {
            CHECK(l.vga[r]==target9eda::vgaOffsets[r]);s.values[l.vga[r]]=target9eda::vgaValues[r];
        }
        for(size_t p=0;p<kPipeCount;++p) {
            s.values[l.power[p]]=target9eda::powerValues[p];
            s.values[l.powerConfig[p]]=target9eda::powerConfigs[p];
            for(size_t r=0;r<kPipeRegisterCount;++r) {
                CHECK(l.pipe[p][r]==target9eda::pipeOffsets[p][r]);
                s.values[l.pipe[p][r]]=target9eda::pipeValues[p][r];
            }
        }
    };
    // Exact archived hardware values, replayed offline. The corrected parser
    // still rejects firmware TLB0; never turn a stable capture into authority.
    for(bool cold:{false,true}) {
        Simulation s(l);initialize(s);Capture c{};const auto unchanged=s.values;
        CHECK((cold?capturePostedCold(l,s.m,s.io(),c):capture(l,s.m,s.io(),c))==Status::UnsupportedAddressMode);
        CHECK(c.failedPipe==0 && c.snapshotReadComplete && !c.poweredOffMask);
        CHECK(c.readsAttempted==(cold?801U:798U) && s.values==unchanged);emptyOutput(c);
    }
    // Counterfactual variants isolate the independent blockers. These are
    // unit-test inputs, NOT observed registers and NOT proposed MMIO writes.
    {Simulation s(l);initialize(s);Capture c{};s.set(0,PipeRegister::TlbControl,0x19);
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::UnsupportedAddressMode);
        CHECK(c.failedPipe==1 && c.snapshotReadComplete);emptyOutput(c);}
    for(unsigned target=1;target<4;++target) {
        Simulation s(l);initialize(s);Capture c{};s.set(0,PipeRegister::TlbControl,0x19);
        for(unsigned p=1;p<4;++p)s.set(p,PipeRegister::Control,3);
        s.set(target,PipeRegister::Control,target9eda::pipeValues[target][0]);
        s.set(target,PipeRegister::TlbControl,0x19);
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::UnsupportedPlane);
        CHECK(c.failedPipe==target && c.snapshotReadComplete);emptyOutput(c);
    }
    // Once explicit blank+drained is supplied in the artificial variants,
    // linear DIM2D, fixed delays, latched INUSE0 and disjoint AGP0/0 yield the
    // expected 1920x1080 footprint. This is not a target adoption claim.
    {Simulation s(l);initialize(s);Capture c{};s.set(0,PipeRegister::TlbControl,0x19);
        for(unsigned p=1;p<4;++p)s.set(p,PipeRegister::Control,3);
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::Ok);
        CHECK(c.complete && c.planeCount==1 && c.planes[0].surfaceMcAddress==0x8000000000ULL);
        CHECK(c.planes[0].pitchPixels==1920 && c.planes[0].width==1920 && c.planes[0].height==1080);
        CHECK(c.planes[0].footprintBytes==8294400 && c.planes[0].reservation.offset==0 &&
              c.planes[0].reservation.bytes==8294400);}
}
void gate(Simulation& s,unsigned mask,uint32_t offValue=0x90000000) {
    for(size_t p=0;p<kPipeCount;++p) {
        if(mask&(1U<<p)) {
            s.values[s.l.power[p]]=offValue;s.values[s.l.powerConfig[p]]=0x100;
            for(size_t i=0;i<kPipeRegisterCount;++i)s.values[s.l.pipe[p][i]]=UINT32_MAX;
        }else s.activate(p,0x02000000+p*0x01000000);
    }
}
void coldPower() {
    const Layout l=coldLayout();Capture c{};
    for(unsigned mask=0;mask<16;++mask){Simulation s(l);gate(s,mask);
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::Ok && c.complete && c.poweredOffMask==mask);
        size_t onCount=0;for(unsigned p=0;p<4;++p)if(!(mask&(1U<<p)))++onCount;
        CHECK(c.planeCount==onCount);
        CHECK(s.reads.front()==l.sos && s.reads.back()==l.sos);
        for(uint32_t reg:s.reads)for(unsigned p=0;p<4;++p)if(mask&(1U<<p))
            for(uint32_t client:l.pipe[p])CHECK(reg!=client);
        if(mask){s.reads.clear();CHECK(capture(l,s.m,s.io(),c)==Status::PowerUnavailable);emptyOutput(c);}
    }
    for(unsigned mask:{0U,15U}){Simulation baseline(l);gate(baseline,mask);
        CHECK(capturePostedCold(l,baseline.m,baseline.io(),c)==Status::Ok);
        CHECK(baseline.reads.size()==(mask?73U:801U));
        for(size_t at=1;at<=baseline.reads.size();++at)for(bool unavailable:{false,true}) {
            Simulation s(l);gate(s,mask);if(unavailable)s.allOnesAt=at;else s.failAt=at;
            CHECK(capturePostedCold(l,s.m,s.io(),c)==(unavailable?Status::DeviceUnavailable:Status::TransportError));
            CHECK(s.reads.size()==at);emptyOutput(c);CHECK(!c.poweredOffMask);
        }
        for(size_t at=1;at<=baseline.reads.size();++at)if(baseline.reads[at-1]==l.sos) {
            Simulation s(l);gate(s,mask);s.mutateAt=at;s.mutateRegister=l.sos;s.mutateXor=1;
            CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::PriorDriverRequiresReset && s.reads.size()==at);emptyOutput(c);
        }
    }
    for(unsigned p=0;p<4;++p)for(uint32_t config:{0U,1U,0x101U,0x102U,0x80000000U}) {
        Simulation s(l);gate(s,15);s.values[l.powerConfig[p]]=config;
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::PowerUnavailable);emptyOutput(c);
    }
    for(unsigned p=0;p<4;++p)for(uint32_t power:{0x10000000U,0x40000000U,0xc0000000U,0x90000001U}) {
        Simulation s(l);gate(s,15);s.values[l.power[p]]=power;
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::PowerUnavailable);emptyOutput(c);
    }
    {Simulation s(l);for(unsigned p=0;p<4;++p)s.values[l.powerConfig[p]]=1;
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::Ok && !c.poweredOffMask);}
    // Every always-on read in the all-OFF branch can observe a late wake or
    // loss of the completed status; no gated register may follow it.
    Simulation baseline(l);gate(baseline,15);CHECK(capturePostedCold(l,baseline.m,baseline.io(),c)==Status::Ok);
    for(size_t at=1;at<=baseline.reads.size();++at)for(unsigned p=0;p<4;++p)
        if(baseline.reads[at-1]==l.power[p] || baseline.reads[at-1]==l.powerConfig[p]) {
            Simulation s(l);gate(s,15);s.mutateAt=at;s.mutateRegister=baseline.reads[at-1];
            s.mutateXor=s.mutateRegister==l.power[p]?0x80000000:0x100;
            // A new current0/desired1 candidate may read only always-on
            // CONFIG before rejecting this still-requested OFF state.
            const bool candidate=s.mutateRegister==l.power[p] && (at<2 || baseline.reads[at-2]!=l.powerConfig[p]);
            CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::PowerUnavailable && s.reads.size()==at+(candidate?1:0));emptyOutput(c);
        }
    {Simulation s;CHECK(capturePostedCold(s.l,s.m,s.io(),c)==Status::InvalidArgument && s.reads.empty());}
}
void powerEvidence() {
    const Layout l=coldLayout();
    // The target's rejected first status was not included in the old report.
    // Preserve every class of raw rejection without accessing CONFIG or HUBP.
    // These are injected observations, not a live power-state measurement.
    for(uint32_t raw:{1U,0x40000000U,0xc0000000U,0x90000001U,UINT32_MAX}) {
        Simulation s(l);Capture c{};s.values[l.power[0]]=raw;
        CHECK(capturePostedCold(l,s.m,s.io(),c)==
              (raw==UINT32_MAX?Status::DeviceUnavailable:Status::PowerUnavailable));
        CHECK(s.reads.size()==2 && s.reads[0]==l.sos && s.reads[1]==l.power[0]);
        CHECK(c.failedPipe==0 && c.lastRegister==l.power[0] && c.lastReadSucceeded && c.lastValue==raw);
        CHECK(c.lastPowerProbe.pipe==0 && c.lastPowerProbe.readsSucceededMask==1 &&
              c.lastPowerProbe.statusBefore==raw && !c.lastPowerProbe.config && !c.lastPowerProbe.statusAfter);
        emptyOutput(c);
    }
    // Capture individual failed phases, including a callback that assigns an
    // output then reports failure. Failed outputs must never look like data.
    for(size_t phase=0;phase<3;++phase)for(bool unavailable:{false,true}) {
        Simulation s(l);Capture c{};
        s.values[l.powerConfig[0]]=1;
        if(unavailable)s.allOnesAt=phase+2;else s.failAt=phase+2;
        CHECK(capturePostedCold(l,s.m,s.io(),c)==
              (unavailable?Status::DeviceUnavailable:Status::TransportError));
        CHECK(s.reads.size()==phase+2 && c.lastReadSucceeded==unavailable &&
              c.lastValue==(unavailable?UINT32_MAX:0U));
        const PowerProbe& p=c.lastPowerProbe;
        CHECK(p.pipe==0 && p.readsSucceededMask==((1U<<(phase+(unavailable?1:0)))-1));
        CHECK(p.statusBefore==(phase==0 && unavailable?UINT32_MAX:0U));
        CHECK(p.config==(phase==1 && unavailable?UINT32_MAX:phase>1?1U:0U));
        CHECK(p.statusAfter==(phase==2 && unavailable?UINT32_MAX:0U));
        emptyOutput(c);
    }
    // Later invalid CONFIG must carry the failing pipe's values, never a
    // previous successful probe's status/confirmation or validity bits.
    {Simulation s(l);Capture c{};s.values[l.powerConfig[2]]=0x101;
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::PowerUnavailable);
        CHECK(s.reads.size()==9 && c.lastRegister==l.powerConfig[2] && c.lastValue==0x101);
        CHECK(c.lastPowerProbe.pipe==2 && c.lastPowerProbe.readsSucceededMask==3 &&
              c.lastPowerProbe.config==0x101 && !c.lastPowerProbe.statusAfter);
        emptyOutput(c);}
    // Power loss at the confirmation read preserves both observations but
    // stops before any HUBP-window read. The stable snapshot stays untouched.
    {Simulation s(l);Capture c{};s.mutateAt=4;s.mutateRegister=l.power[0];s.mutateXor=0x80000000;
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::PowerUnavailable);
        CHECK(s.reads.size()==4 && c.lastPowerProbe.readsSucceededMask==7 &&
              !c.lastPowerProbe.statusBefore && c.lastPowerProbe.statusAfter==0x80000000);
        CHECK(!c.snapshot.power[0] && !c.snapshot.powerConfig[0]);emptyOutput(c);}
    // A subsequent successful capture resets all prior failure evidence and
    // reports the last actual read (SOS), separate from its last power probe.
    {Simulation s(l);Capture c{};s.values[l.power[0]]=1;
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::PowerUnavailable);
        s.values[l.power[0]]=0;s.reads.clear();
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::Ok && c.complete);
        CHECK(c.lastRegister==l.sos && c.lastReadSucceeded && !c.lastValue);
        CHECK(c.lastPowerProbe.pipe==3 && c.lastPowerProbe.readsSucceededMask==7 &&
              !c.lastPowerProbe.statusBefore && !c.lastPowerProbe.config && !c.lastPowerProbe.statusAfter);}
}
void desiredOnPower() {
    const Layout l=coldLayout();Capture c{};
    // Linux waits on current PGFSM0, not desired bit28, after requesting
    // GATE0 with CONFIG0. Both FORCEON values allow a powered-on observation.
    // This includes all four actual target pairs 0x10000000/CONFIG0.
    for(bool cold:{false,true})for(unsigned p=0;p<4;++p)
        for(uint32_t power:{0U,0x10000000U,0x40000000U,0x50000000U,0x80000000U,
                            0x90000000U,0xc0000000U,0xd0000000U,1U,0x10000001U})
            for(uint32_t config:{0U,1U,2U,0x100U,0x101U,0x102U,0x80000000U}) {
                Simulation s(l);s.values[l.power[p]]=power;s.values[l.powerConfig[p]]=config;
                const bool accepted=((power==0 || power==0x10000000) && config<=1) ||
                                    (cold && (power==0x80000000 || power==0x90000000) && config==0x100);
                const Status status=cold?capturePostedCold(l,s.m,s.io(),c):capture(l,s.m,s.io(),c);
                CHECK(status==(accepted?Status::Ok:Status::PowerUnavailable));
                if(!accepted) {
                    emptyOutput(c);
                    for(uint32_t offset:s.reads)for(uint32_t window:l.pipe[p])CHECK(offset!=window);
                }
            }
    for(uint32_t config:{0U,1U}) {
        const auto initialize=[&](Simulation& s) {
            for(unsigned p=0;p<4;++p) {
                s.activate(p,0x02000000+p*0x01000000);
                s.values[l.power[p]]=0x10000000;s.values[l.powerConfig[p]]=config;
            }
        };
        Simulation baseline(l);initialize(baseline);
        CHECK(capturePostedCold(l,baseline.m,baseline.io(),c)==Status::Ok);
        CHECK(c.complete && c.snapshotReadComplete && c.planeCount==4 && !c.poweredOffMask && baseline.reads.size()==801);
        for(unsigned p=0;p<4;++p)CHECK(c.snapshot.power[p]==0x10000000 && c.snapshot.powerConfig[p]==config);
        for(bool cold:{false,true}) {
            Simulation s(l);initialize(s);
            CHECK((cold?capturePostedCold(l,s.m,s.io(),c):capture(l,s.m,s.io(),c))==Status::Ok);
            CHECK(c.complete && c.snapshotReadComplete && c.planeCount==4 && !c.poweredOffMask);
            CHECK(s.reads.size()==(cold?801U:798U));
            // A format rejection must still expose both completed raw passes.
            s.reads.clear();s.set(0,PipeRegister::SurfaceConfig,0x123456);
            CHECK((cold?capturePostedCold(l,s.m,s.io(),c):capture(l,s.m,s.io(),c))==Status::UnsupportedPlane);
            CHECK(c.snapshotReadComplete && !c.complete && c.planeCount==0);
            CHECK(c.snapshot.pipe[0][static_cast<size_t>(PipeRegister::SurfaceConfig)]==0x123456);
            for(unsigned p=0;p<4;++p)CHECK(c.snapshot.power[p]==0x10000000 && c.snapshot.powerConfig[p]==config);
        }
        for(size_t at=1;at<=baseline.reads.size();++at) {
            for(bool unavailable:{false,true}) {
                Simulation s(l);initialize(s);
                if(unavailable)s.allOnesAt=at;else s.failAt=at;
                CHECK(capturePostedCold(l,s.m,s.io(),c)==(unavailable?Status::DeviceUnavailable:Status::TransportError));
                CHECK(s.reads.size()==at);emptyOutput(c);CHECK(!c.poweredOffMask && !c.snapshotReadComplete);
            }
            for(unsigned p=0;p<4;++p) {
                const uint32_t reg=baseline.reads[at-1];
                if(reg!=l.power[p] && reg!=l.powerConfig[p])continue;
                for(uint32_t delta:reg==l.power[p]?std::vector<uint32_t>{0x40000000U}:
                                                   std::vector<uint32_t>{0x100U,2U}) {
                    Simulation s(l);initialize(s);s.mutateAt=at;s.mutateRegister=reg;s.mutateXor=delta;
                    const Status status=capturePostedCold(l,s.m,s.io(),c);
                    CHECK(status==Status::PowerUnavailable || status==Status::UnstableSnapshot);
                    emptyOutput(c);CHECK(!c.poweredOffMask && !c.snapshotReadComplete);
                    // A gate, intermediate PGFSM or unknown bit invalidates
                    // readability immediately; no HUBP access follows it.
                    for(size_t i=at-1;i<s.reads.size();++i) {
                        bool alwaysOn=false;
                        for(unsigned q=0;q<4;++q)alwaysOn|=s.reads[i]==l.power[q] || s.reads[i]==l.powerConfig[q];
                        CHECK(alwaysOn);
                    }
                }
                if(reg==l.powerConfig[p]) {
                    Simulation s(l);initialize(s);s.mutateAt=at;s.mutateRegister=reg;s.mutateXor=1;
                    const Status status=capturePostedCold(l,s.m,s.io(),c);
                    // A different valid CONFIG observed before the first
                    // sample is data. Later guards compare the retained raw
                    // value; a first sample in pass2 is caught by stable().
                    const size_t first=3+3*p,second=first+kPassReads+1;
                    CHECK(status==(at==first?Status::Ok:Status::UnstableSnapshot));
                    if(at==first)CHECK(c.complete && c.snapshotReadComplete && c.snapshot.powerConfig[p]==(config^1));
                    else {
                        emptyOutput(c);CHECK(!c.snapshotReadComplete);
                        if(at!=second) {
                            CHECK(s.reads.size()==at+1 && s.reads.back()==l.power[p]);
                            CHECK(c.lastPowerProbe.readsSucceededMask==7 && c.lastPowerProbe.config==(config^1));
                        } else CHECK(s.reads.size()==801);
                    }
                }
            }
        }
        // Changing a raw status to another individually accepted ON value
        // still invalidates its triplet or already established snapshot.
        for(size_t at:{size_t(4),size_t(25)}) {
            Simulation s(l);initialize(s);s.mutateAt=at;s.mutateRegister=l.power[0];s.mutateXor=0x10000000;
            CHECK(capturePostedCold(l,s.m,s.io(),c)==(at==4?Status::PowerUnavailable:Status::UnstableSnapshot));
            emptyOutput(c);CHECK(!c.snapshotReadComplete);
        }
    }
}
void powerInventory() {
    const Layout l=coldLayout();PowerInventory out{};
    Simulation baseline(l);
    const uint32_t raw[]={0x10000000,0x40000000,0x90000000,0xf1234567};
    for(unsigned p=0;p<4;++p) {
        baseline.values[l.power[p]]=raw[p];baseline.values[l.powerConfig[p]]=0xdead0000+p;
    }
    CHECK(inventoryPower(l,baseline.io(),out)==Status::Ok);
    CHECK(out.failedPipe==0xff && out.readsAttempted==12 && baseline.reads.size()==12);
    for(unsigned p=0;p<4;++p) {
        const PowerProbe& probe=out.probes[p];
        CHECK(probe.pipe==p && probe.readsSucceededMask==7 && probe.statusBefore==raw[p] &&
              probe.statusAfter==raw[p] && probe.config==0xdead0000+p);
        CHECK(baseline.reads[p*3]==l.power[p] && baseline.reads[p*3+1]==l.powerConfig[p] &&
              baseline.reads[p*3+2]==l.power[p]);
    }
    // Raw inventory does not turn before/after disagreement into permission
    // or stop gathering the remaining always-on fields.
    {Simulation s(l);s.mutateAt=3;s.mutateRegister=l.power[0];s.mutateXor=0x90000000;
        CHECK(inventoryPower(l,s.io(),out)==Status::Ok && s.reads.size()==12);
        CHECK(!out.probes[0].statusBefore && out.probes[0].statusAfter==0x90000000);}
    for(size_t at=1;at<=12;++at)for(bool unavailable:{false,true}) {
        Simulation s(l);if(unavailable)s.allOnesAt=at;else s.failAt=at;
        CHECK(inventoryPower(l,s.io(),out)==(unavailable?Status::DeviceUnavailable:Status::TransportError));
        const unsigned pipe=static_cast<unsigned>((at-1)/3),phase=static_cast<unsigned>((at-1)%3);
        CHECK(out.failedPipe==pipe && out.readsAttempted==at && s.reads.size()==at);
        CHECK(out.lastReadSucceeded==unavailable && out.lastValue==(unavailable?UINT32_MAX:0U));
        CHECK(out.probes[pipe].readsSucceededMask==((1U<<(phase+(unavailable?1:0)))-1));
        const uint32_t fields[]={out.probes[pipe].statusBefore,out.probes[pipe].config,out.probes[pipe].statusAfter};
        CHECK(fields[phase]==(unavailable?UINT32_MAX:0U));
        for(unsigned p=pipe+1;p<4;++p)CHECK(out.probes[p].pipe==0xff && !out.probes[p].readsSucceededMask);
        for(size_t i=0;i<s.reads.size();++i)CHECK(s.reads[i]==baseline.reads[i]);
    }
    for(unsigned defect=0;defect<6;++defect) {
        Simulation s(l);Io io=s.io();
        switch(defect) {
        case 0:io.read32=nullptr;break;
        case 1:s.l.valid=false;break;
        case 2:s.l.barBytes=3;break;
        case 3:s.l.power[0]+=4;break;
        case 4:s.l.powerConfig[3]+=4;break;
        case 5:s.l.powerConfig[0]=UINT32_MAX;break;
        }
        CHECK(inventoryPower(s.l,io,out)==Status::InvalidArgument && s.reads.empty());
        CHECK(!out.readsAttempted && !out.lastReadSucceeded && out.failedPipe==0xff);
        for(const PowerProbe& probe:out.probes)CHECK(probe.pipe==0xff && !probe.readsSucceededMask);
    }
    // Reusing an output never carries prior failure evidence into new reads.
    {Simulation s(l);CHECK(inventoryPower(l,s.io(),out)==Status::Ok && out.readsAttempted==12);
        CHECK(out.failedPipe==0xff && out.lastReadSucceeded && !out.lastValue);}
}
void rawSnapshotEvidence() {
    const Layout l=coldLayout();Capture c{};
    {Simulation s(l);s.activate(0);
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::Ok && c.snapshotReadComplete);}
    // Full observed register passes remain useful when format decoding fails.
    {Simulation s(l);s.activate(0);s.set(0,PipeRegister::SurfaceConfig,UINT32_C(0x123456));
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::UnsupportedPlane);
        CHECK(c.snapshotReadComplete && !c.complete && c.planeCount==0);
        CHECK(c.snapshot.pipe[0][static_cast<size_t>(PipeRegister::SurfaceConfig)]==0x123456);}
    for(size_t at=1;at<=801;++at) {
        Simulation s(l);s.failAt=at;
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::TransportError);
        CHECK(!c.snapshotReadComplete);
    }
    // Fully completed cold capture may skip OFF windows: the flag does not
    // turn those zero-initialized entries into observed register values.
    {Simulation s(l);gate(s,15);
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::Ok && c.snapshotReadComplete);
        CHECK(c.poweredOffMask==15);
        for(unsigned p=0;p<4;++p)for(uint32_t value:c.snapshot.pipe[p])CHECK(value==0);}
}
void coldOffCurrentState() {
    const Layout l=coldLayout();Capture c{};
    // Current PGFSM2 is OFF regardless of desired bit28. Exercise every
    // combination of ON/OFF domains and desired bits, retaining exact raw
    // evidence while never treating skipped zero storage as a HUBP reading.
    for(unsigned off=0;off<16;++off)for(unsigned desired=0;desired<16;++desired) {
        Simulation s(l);gate(s,off,0x80000000);size_t planes=0;
        for(unsigned p=0;p<4;++p) {
            s.values[l.power[p]]|=(desired&(1U<<p))?0x10000000:0;
            if(!(off&(1U<<p))){++planes;s.values[l.powerConfig[p]]=p&1;}
        }
        const auto unchanged=s.values;
        CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::Ok);
        CHECK(c.complete && c.snapshotReadComplete && c.poweredOffMask==off && c.planeCount==planes);
        CHECK(s.values==unchanged);
        for(unsigned p=0;p<4;++p) {
            CHECK(c.snapshot.power[p]==s.values[l.power[p]] && c.snapshot.powerConfig[p]==s.values[l.powerConfig[p]]);
            if(off&(1U<<p)) {
                for(uint32_t value:c.snapshot.pipe[p])CHECK(value==0);
                for(uint32_t reg:s.reads)for(uint32_t window:l.pipe[p])CHECK(reg!=window);
            }
        }
        if(off){s.reads.clear();CHECK(capture(l,s.m,s.io(),c)==Status::PowerUnavailable);emptyOutput(c);}
    }
    for(uint32_t offValue:{0x80000000U,0x90000000U}) {
        // Native RawSnapshot publication must distinguish the skipped windows
        // even when later format rejection leaves poweredOffMask unpublished.
        // Exercise actual capture data; native OS collection serialization is
        // separately source-reviewed and compiled against kernel headers.
        {Simulation s(l);gate(s,14,offValue);s.set(0,PipeRegister::SurfaceConfig,0x123456);
            CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::UnsupportedPlane);
            CHECK(c.snapshotReadComplete && !c.complete && !c.poweredOffMask && !c.planeCount);
            CHECK(c.snapshot.pipe[0][static_cast<size_t>(PipeRegister::SurfaceConfig)]==0x123456);
            for(unsigned p=1;p<4;++p) {
                CHECK(c.snapshot.power[p]==offValue && c.snapshot.powerConfig[p]==0x100);
                for(uint32_t value:c.snapshot.pipe[p])CHECK(value==0);
                for(uint32_t reg:s.reads)for(uint32_t window:l.pipe[p])CHECK(reg!=window);
            }}
        for(unsigned p=0;p<4;++p)for(unsigned bit=0;bit<32;++bit) {
            Simulation s(l);gate(s,15,offValue);s.values[l.powerConfig[p]]^=uint32_t(1)<<bit;
            CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::PowerUnavailable);emptyOutput(c);
            CHECK(!c.poweredOffMask && !c.snapshotReadComplete);
            if(bit==28)continue; // The sole non-current status observation bit.
            Simulation raw(l);gate(raw,15,offValue);raw.values[l.power[p]]^=uint32_t(1)<<bit;
            CHECK(capturePostedCold(l,raw.m,raw.io(),c)==Status::PowerUnavailable);emptyOutput(c);
        }
        // Test every actual read in an all-OFF and a mixed OFF/ON capture.
        // Faulting callbacks assign values but these must stay unobserved.
        for(unsigned mask:{5U,15U}) {
            Simulation baseline(l);gate(baseline,mask,offValue);
            CHECK(capturePostedCold(l,baseline.m,baseline.io(),c)==Status::Ok);
            for(size_t at=1;at<=baseline.reads.size();++at)for(bool ones:{false,true}) {
                Simulation s(l);gate(s,mask,offValue);if(ones)s.allOnesAt=at;else s.failAt=at;
                CHECK(capturePostedCold(l,s.m,s.io(),c)==(ones?Status::DeviceUnavailable:Status::TransportError));
                CHECK(s.reads.size()==at && c.lastReadSucceeded==ones && c.lastValue==(ones?UINT32_MAX:0U));
                emptyOutput(c);CHECK(!c.poweredOffMask && !c.snapshotReadComplete);
            }
        }
        // Either initial desired-bit value is allowed. Once observed, even a
        // change between two permitted OFF encodings invalidates the capture.
        Simulation baseline(l);gate(baseline,15,offValue);
        CHECK(capturePostedCold(l,baseline.m,baseline.io(),c)==Status::Ok);
        for(size_t at=1;at<=baseline.reads.size();++at)for(unsigned p=0;p<4;++p) {
            if(baseline.reads[at-1]!=l.power[p])continue;
            Simulation s(l);gate(s,15,offValue);s.mutateAt=at;s.mutateRegister=l.power[p];s.mutateXor=0x10000000;
            const Status status=capturePostedCold(l,s.m,s.io(),c);
            if(at==2+3*p) {
                CHECK(status==Status::Ok && c.snapshotReadComplete && c.poweredOffMask==15);
                CHECK(c.snapshot.power[p]==(offValue^0x10000000));
            } else {
                CHECK(status==Status::PowerUnavailable || status==Status::UnstableSnapshot);
                emptyOutput(c);CHECK(!c.snapshotReadComplete && !c.poweredOffMask);
            }
            for(uint32_t reg:s.reads)for(unsigned q=0;q<4;++q)for(uint32_t window:l.pipe[q])CHECK(reg!=window);
        }
    }
}
void readJournalEvidence() {
    auto l=layout();l.sos=0x58244;
    const auto prepare=[&](Simulation& s,unsigned off) {
        for(unsigned p=0;p<4;++p)if(off&(1U<<p)) {
            s.values[l.power[p]]=0x90000000;s.values[l.powerConfig[p]]=0x100;
            for(uint32_t reg:l.pipe[p])s.values[reg]=UINT32_MAX;
        }
    };
    const auto verify=[&](const ReadCapture& j,const Simulation& s,size_t failure,size_t ones) {
        CHECK(j.attempted==s.reads.size()&&j.recorded==j.attempted&&!j.truncated);
        for(size_t i=0;i<j.recorded;++i) {
            CHECK(j.entries[i].byteOffset==s.reads[i]);
            CHECK(j.entries[i].returnedValue==(i+1==ones?UINT32_MAX:s.values.at(s.reads[i])));
            const uint32_t bit=1U<<(i%32);
            CHECK(bool(j.callbackSucceeded[i/32]&bit)==(i+1!=failure));
            CHECK(bool(j.nonAllOnes[i/32]&bit)==(i+1!=failure&&i+1!=ones));
        }
        for(size_t i=j.recorded;i<ReadCapture::capacity;++i) {
            CHECK(!j.entries[i].byteOffset&&!j.entries[i].returnedValue);
            CHECK(!(j.callbackSucceeded[i/32]&(1U<<(i%32)))&&!(j.nonAllOnes[i/32]&(1U<<(i%32))));
        }
    };
    for(bool cold:{false,true})for(unsigned off:{0u,5u,15u}) {
        if(!cold&&off)continue;
        Simulation plain(l),captured(l);prepare(plain,off);prepare(captured,off);
        Capture a{},b{};ReadCapture journal{};
        const auto wanted=cold?capturePostedCold(l,plain.m,plain.io(),a):capture(l,plain.m,plain.io(),a);
        CHECK(captureRecorded(l,captured.m,captured.io(),b,&journal,cold)==wanted);
        CHECK(wanted==Status::Ok&&plain.reads==captured.reads&&a.complete==b.complete);
        verify(journal,captured,0,0);
        if(!off)CHECK(journal.recorded==(cold?801:798));
        for(size_t at=1;at<=plain.reads.size();++at)for(bool ones:{false,true}) {
            Simulation original(l),traced(l);prepare(original,off);prepare(traced,off);
            if(ones)original.allOnesAt=traced.allOnesAt=at;else original.failAt=traced.failAt=at;
            const auto status=cold?capturePostedCold(l,original.m,original.io(),a):capture(l,original.m,original.io(),a);
            CHECK(captureRecorded(l,traced.m,traced.io(),b,&journal,cold)==status);
            CHECK(original.reads==traced.reads&&traced.reads.size()==at&&!b.complete);
            CHECK(a.readsAttempted==b.readsAttempted&&a.lastRegister==b.lastRegister&&
                a.lastReadSucceeded==b.lastReadSucceeded&&a.lastValue==b.lastValue);
            verify(journal,traced,ones?0:at,ones?at:0);
        }
    }
    {Simulation s(l);s.activate(0);s.set(0,PipeRegister::TlbControl,0);Capture c{};ReadCapture j{};
        CHECK(captureRecorded(l,s.m,s.io(),c,&j,true)==Status::UnsupportedAddressMode);
        CHECK(!c.complete&&c.snapshotReadComplete&&j.recorded==801);verify(j,s,0,0);}
    {Simulation s(l);Capture c{};ReadCapture j{};j.attempted=99;
        CHECK(captureRecorded(l,s.m,{nullptr,nullptr},c,&j,true)==Status::InvalidArgument);
        CHECK(!j.attempted&&!j.recorded&&s.reads.empty());
        CHECK(captureRecorded(l,s.m,s.io(),c,nullptr,true)==Status::Ok);}
    // A deliberately undersized sink must not truncate or change real reads.
    {Simulation s(l);s.failAt=3;readcapture::Journal<2> j{};
        readcapture::Reader<2> reader(&s,Simulation::read,j);
        for(unsigned n=0;n<4;++n){uint32_t value=77;
            CHECK(readcapture::Reader<2>::read(&reader,l.sos,value)==(n!=2));CHECK(value==0);}
        CHECK(s.reads.size()==4&&j.attempted==4&&j.recorded==2&&j.truncated);
        CHECK(j.callbackSucceeded[0]==3&&j.nonAllOnes[0]==3);}
}

}
int main(int argc,char**argv) {
    layoutTests();golden();invalidModes();failuresAndStability();powerTests();preflight();coldPower();powerEvidence();
    desiredOnPower();powerInventory();rawSnapshotEvidence();coldOffCurrentState();
    linearAddressPolicy();targetFirmwareReplay();readJournalEvidence();
    if(argc>1)realDiscovery(argv[1]);
    std::cout<<checks<<" boot display assertions, "<<failures<<" failures\n";
    return failures?1:0;
}
