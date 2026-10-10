// SPDX-License-Identifier: MIT
// Read-callback simulations only; no hardware is accessed or modified.
#include "hackynavi/boot_display.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
using namespace hackynavi;
using namespace hackynavi::bootdisplay;
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
                CHECK(s.values[s.l.power[p]]==0);
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
        {PipeRegister::TilingConfig,0x80,Status::UnsupportedPlane},
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
        {PipeRegister::InUseHi,0x10000001,Status::UnsupportedAddressMode},
        {PipeRegister::InUseLo,0x02000100,Status::UnsupportedAddressMode},
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
        s.values[s.l.global[global]]=global==3?0:UINT32_MAX-1;
        CHECK(capture(s.l,s.m,s.io(),c)==Status::UnsupportedAddressMode);emptyOutput(c);
    }
    for (uint64_t address:{uint64_t(0),uint64_t(0x1000000000),uint64_t(0x8000000000),uint64_t(0x6fffff000)}) {
        Simulation s;Capture c{};s.activate(0);s.address(0,address);
        CHECK(capture(s.l,s.m,s.io(),c)==Status::OutsideVram);emptyOutput(c);
    }
}
void failuresAndStability() {
    for (size_t at=1;at<=2*kPassReads;++at) for (bool allOnes:{false,true}) {
        Simulation s;Capture c{};s.activate(0);
        if (allOnes)s.allOnesAt=at;else s.failAt=at;
        CHECK(capture(s.l,s.m,s.io(),c)==(allOnes?Status::DeviceUnavailable:Status::TransportError));
        CHECK(c.readsAttempted==at && s.reads.size()==at);emptyOutput(c);
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
        for(uint32_t state:{0x40000000U,0x80000000U,0xc0000000U,0x10000000U,1U}) {
            Simulation s;Capture c{};s.activate(0);s.values[s.l.power[p]]=state;
            CHECK(capture(s.l,s.m,s.io(),c)==Status::PowerUnavailable);emptyOutput(c);
            CHECK(c.failedPipe==p && c.readsAttempted==p*3+1 && s.reads.size()==p*3+1);
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
Layout coldLayout() {
    auto b=fixture();const size_t at=b.size();b.resize(at+16,0);
    put16(b,10,uint16_t(b.size()));put16(b,16,uint16_t(b.size()-60));
    put16(b,66,uint16_t(b.size()-60));put16(b,142,2);
    put16(b,at,255);b[at+3]=2;b[at+4]=13;put32(b,at+8,0x16000);put32(b,at+12,0x16000);fix(b);
    Layout l{};CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::Ok && l.sos==0x58244);
    return l;
}
void gate(Simulation& s,unsigned mask) {
    for(size_t p=0;p<kPipeCount;++p) {
        if(mask&(1U<<p)) {
            s.values[s.l.power[p]]=0x90000000;s.values[s.l.powerConfig[p]]=0x100;
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
    for(unsigned p=0;p<4;++p)for(uint32_t power:{0x10000000U,0x40000000U,0x80000000U,0xc0000000U,0x90000001U}) {
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
            CHECK(capturePostedCold(l,s.m,s.io(),c)==Status::PowerUnavailable && s.reads.size()==at);emptyOutput(c);
        }
    {Simulation s;CHECK(capturePostedCold(s.l,s.m,s.io(),c)==Status::InvalidArgument && s.reads.empty());}
}
}
int main(int argc,char**argv) {
    layoutTests();golden();invalidModes();failuresAndStability();powerTests();preflight();coldPower();
    if(argc>1)realDiscovery(argv[1]);
    std::cout<<checks<<" boot display assertions, "<<failures<<" failures\n";
    return failures?1:0;
}
