// SPDX-License-Identifier: MIT
// Shared host-only graphics bring-up emulator. Never linked into production.
#ifndef HACKYNAVI_TEST_GFX_FIRMWARE_FIXTURE_HPP
#define HACKYNAVI_TEST_GFX_FIRMWARE_FIXTURE_HPP
#include "hackynavi/gfx_firmware.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <vector>
namespace gfxfw_test {
using namespace hackynavi;
namespace g=hackynavi::gfxfw;
unsigned checks=0,failures=0;
#define CHECK(x) do { ++::gfxfw_test::checks; if(!(x)) { ++::gfxfw_test::failures; std::cerr<<__LINE__<<": " #x "\n"; } } while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v) { b[p]=uint8_t(v); b[p+1]=uint8_t(v>>8); }
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v) { for(unsigned i=0;i<4;++i)b[p+i]=uint8_t(v>>(8*i)); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24; }
Navi31IpLayout ips() {
    Navi31IpLayout l={}; l.valid=true; const uint16_t ids[]={11,34,255,1,42,43};
    const uint8_t majors[]={11,3,13,13,6,6};
    for(unsigned i=0;i<6;++i) l.blocks[i]={ids[i],0,0,majors[i],0,0,2,{0x16000,0xa000}};
    l.blocks[0].basesDwords[0]=0x1260; return l;
}
std::vector<uint8_t> common(uint16_t major,uint16_t minor,uint16_t ip,uint32_t header,uint32_t code,uint32_t total) {
    std::vector<uint8_t> b(total,0);
    put32(b,0,total); put32(b,4,header); put16(b,8,major); put16(b,10,minor); put16(b,12,ip);
    put32(b,16,7); put32(b,20,code); put32(b,24,256);
    for(size_t i=256;i<b.size();++i) b[i]=uint8_t(i*53+(i>>3));
    return b;
}
struct Fixtures {
    std::vector<uint8_t> b[7];
    Fixtures() {
        b[0]=common(2,1,13,44,16,272);
        b[1]=common(2,0,6,64,32,288); put32(b[1],36,16); put32(b[1],48,272); put32(b[1],52,16);
        for(unsigned i=2;i<=4;++i) {
            b[i]=common(2,0,11,60,16,288); put32(b[i],36,16); put32(b[i],40,256);
            put32(b[i],44,16); put32(b[i],48,272); put32(b[i],52,0x1000);
        }
        b[5]=common(1,0,11,48,32,288); put32(b[5],32,16); put32(b[5],36,0); put32(b[5],40,16); put32(b[5],44,16);
        b[6]=common(2,3,11,204,16,384); const unsigned off[]={116,132,148,156,164,180,196};
        for(unsigned i=0;i<7;++i) { put32(b[6],off[i],16); put32(b[6],off[i]+4,272+i*16); }
    }
    g::Files files() const { return {{b[0].data(),b[0].size()},{b[1].data(),b[1].size()},
        {b[2].data(),b[2].size()},{b[3].data(),b[3].size()},{b[4].data(),b[4].size()},
        {b[5].data(),b[5].size()},{b[6].data(),b[6].size()}}; }
};
namespace p=hackynavi::psp;
namespace pr=hackynavi::pspring;
constexpr uint32_t coldCommand=(0x16000+0x63)*4, heartbeat=(0x16000+0x91)*4;
constexpr uint32_t cpStat=(0x1260+0xf40)*4,bootStat=(0xa000+0x4e82)*4,mesControl=(0xa000+0x2807)*4;
struct Loaded { uint32_t id,type; std::vector<uint8_t> bytes; };
struct Simulation {
    VramAllocator allocator; VramAllocation sos={},ringBuffer={},command={},fence={},staging={},tmr={};
    std::vector<uint8_t> memory,workspace,scratch;
    std::map<uint32_t,uint32_t> registers;
    std::vector<Loaded> received;
    HdpState hdp; p::State sosState; pr::State ring; pr::Layout layout={};
    uint64_t mcBase=0,tmrMc=0; uint32_t high=0,index=0,heartReads=0;
    size_t accesses=0,failAccess=0,writes=0,stagingWrites=0,flushes=0;
    unsigned rejectLoad=0,stallLoad=0,loadCount=0,autoloads=0;
    bool sosIssued=false,badBoot=false,busyCp=false,onesStatus=false,badMesAfter=false;
    bool mismatch=false,failPublish=false,failed=false;
    static const uint64_t vramStart=0x400000;
    Simulation():memory(8U<<20,0),workspace(p::kStagingBytes,0),scratch(4096,0) {
        MemoryLayout m={};
        CHECK(deriveMemoryLayout({0x8000,0x85ff,0x4000,UINT64_C(24)<<30,
            UINT64_C(8)<<30,UINT64_C(256)<<20},m)==MemoryLayoutStatus::Ok);
        CHECK(allocator.configure(m,{vramStart,memory.size()},nullptr,0)==VramAllocatorStatus::Ok);
        CHECK(allocator.allocate(p::kStagingBytes,p::kStagingBytes,sos)==VramAllocatorStatus::Ok);
        CHECK(allocator.allocate(4096,4096,ringBuffer)==VramAllocatorStatus::Ok);
        CHECK(allocator.allocate(4096,4096,command)==VramAllocatorStatus::Ok);
        CHECK(allocator.allocate(4096,4096,fence)==VramAllocatorStatus::Ok);
        CHECK(allocator.allocate(270336,4096,staging)==VramAllocatorStatus::Ok);
        CHECK(allocator.allocate(1U<<20,1U<<20,tmr)==VramAllocatorStatus::Ok);
        uint64_t mc=0,physical=0;
        CHECK(allocator.addresses(sos.handle,mc,physical)==VramAllocatorStatus::Ok); mcBase=mc-sos.range.offset;
        CHECK(allocator.addresses(tmr.handle,tmrMc,physical)==VramAllocatorStatus::Ok);
        registers[0x430c]=24576; registers[mesControl]=0x40000000;
        HdpResult hr={}; CHECK(configureHdp({true,0x44b4,0x44b8,0x430c,0x7f000,0x7f004,0x100000},hdpIo(),hdp,hr)==HdpStatus::Ok);
        auto fw=common(2,0,13,36,128,384); put32(fw,32,2);
        put32(fw,36,2); put32(fw,40,7); put32(fw,44,0); put32(fw,48,64);
        put32(fw,52,1); put32(fw,56,7); put32(fw,60,64); put32(fw,64,64);
        p::Layout cold={}; p::Result cr={};
        CHECK(p::buildLayout(ips(),0x100000,cold)==p::Status::Ok);
        CHECK(p::loadSos(cold,allocator,sos.handle,fw.data(),fw.size(),workspace.data(),workspace.size(),
            {this,read,write,flush,delayUs,delayMs},{3,3,1},sosState,cr)==p::Status::Ok);
        CHECK(pr::buildLayout(ips(),0x100000,layout)==pr::Status::Ok);
        registers[layout.control]=0x80000000;
        pr::Result rr={};
        CHECK(pr::create(layout,sosState,allocator,{ringBuffer.handle,command.handle,fence.handle,
            scratch.data(),scratch.size()},io(),{3,1},ring,rr)==pr::Status::Ok);
        CHECK(pr::submitLoadToc(io(),ring,staging.handle,0,16,rr)==pr::Status::Ok);
        CHECK(pr::wait(io(),ring,{3,1},rr)==pr::Status::Ok);
        CHECK(pr::submitSetupTmr(io(),ring,tmr.handle,rr)==pr::Status::Ok);
        CHECK(pr::wait(io(),ring,{3,1},rr)==pr::Status::Ok && ring.tmrReady());
        accesses=writes=stagingWrites=flushes=0; received.clear();
    }
    size_t pos(uint64_t relative) {
        if(relative<vramStart || relative-vramStart>memory.size()-4 || (relative&3)) {
            failed=true; return 0;
        }
        return size_t(relative-vramStart);
    }
    size_t mcPos(uint64_t address) { return pos(address-mcBase); }
    size_t selected() { if(!(index&0x80000000))failed=true; return pos((uint64_t(high)<<31)|(index&0x7fffffff)); }
    uint64_t address(size_t p) { return uint64_t(le32(&memory[p]))|uint64_t(le32(&memory[p+4]))<<32; }
    void execute(uint32_t wp) {
        const uint32_t old=registers[layout.wptr]; registers[layout.wptr]=wp;
        const size_t frame=pos(ringBuffer.range.offset)+old*4;
        const size_t cmd=mcPos(address(frame)), done=mcPos(address(frame+12));
        const uint32_t id=le32(&memory[cmd+8]),sequence=le32(&memory[frame+20]);
        if(id==6) {
            ++loadCount; const uint32_t type=le32(&memory[cmd+40]), bytes=le32(&memory[cmd+36]);
            const size_t input=mcPos(address(cmd+28));
            if(input>memory.size() || bytes>memory.size()-input) { failed=true; return; }
            received.push_back({id,type,std::vector<uint8_t>(memory.begin()+input,memory.begin()+input+bytes)});
            if(stallLoad==loadCount) return;
            if(rejectLoad==loadCount) put32(memory,cmd+864,1);
            const uint64_t result=tmrMc+uint64_t(type)*4096;
            put32(memory,cmd+872,uint32_t(result)); put32(memory,cmd+876,uint32_t(result>>32));
        } else if(id==0x20) put32(memory,cmd+880,1U<<20);
        else if(id==0x21) { ++autoloads; received.push_back({id,0,{}}); }
        put32(memory,done,sequence); registers[layout.rptr]=wp;
    }
    static bool read(void* context,uint32_t reg,uint32_t& v) {
        auto& s=*static_cast<Simulation*>(context); ++s.accesses;
        if(reg==4) {
            const size_t at=s.selected(); v=le32(&s.memory[at]);
            if(s.mismatch && at>=s.staging.range.offset-vramStart && at<s.staging.range.offset-vramStart+s.staging.range.bytes) v^=1;
        } else if(reg==coldCommand) v=0x80000000;
        else if(reg==heartbeat) v=s.sosIssued?5+(s.heartReads++?1:0):0;
        else if(reg==(0x16000+0x7a)*4) v=7;
        else if(reg==cpStat) v=s.onesStatus?UINT32_MAX:s.busyCp?1:0;
        else if(reg==bootStat) v=s.autoloads && !s.badBoot?0x80000000:0;
        else if(reg==mesControl && s.badMesAfter && s.loadCount) v=0;
        else v=s.registers[reg];
        return s.accesses!=s.failAccess;
    }
    static bool write(void* context,uint32_t reg,uint32_t v) {
        auto& s=*static_cast<Simulation*>(context); ++s.accesses; ++s.writes;
        if(reg==0x18)s.high=v; else if(reg==0)s.index=v;
        else if(reg==4) {
            const size_t at=s.selected(); put32(s.memory,at,v);
            if(at>=s.staging.range.offset-vramStart && at<s.staging.range.offset-vramStart+s.staging.range.bytes) ++s.stagingWrites;
        } else if(reg==coldCommand && v==0x20000)s.sosIssued=true;
        else if(reg==s.layout.control && (v==0x20000 || v==0x30000))s.registers[reg]=0x80000000;
        else if(reg==s.layout.wptr)s.execute(v);
        else { s.registers[reg]=v; if(reg==0x7f000) { ++s.flushes; if(s.failPublish)return false; } }
        return s.accesses!=s.failAccess;
    }
    static bool flush(void* context) { auto& s=*static_cast<Simulation*>(context); HdpResult r={}; return flushHdp(s.hdpIo(),s.hdp,r)==HdpStatus::Ok; }
    static bool before(void*) { return true; }
    static void delayUs(void*,uint32_t) {}
    static void delayMs(void*,uint32_t) {}
    HdpIo hdpIo() { return {this,read,write}; }
    VramTransferIo transfer() { return {this,0x100000,read,write}; }
    pr::Io io() { return {this,read,write,flush,before,delayUs,delayMs}; }
    g::Status run(const g::Files& files,g::State& state,g::Result& r,VramAllocationHandle input={}) {
        return g::load(files,ips(),0xc8,allocator,input.owner?input:staging.handle,
            transfer(),hdpIo(),hdp,io(),ring,{3,1},state,r);
    }
};

} // namespace gfxfw_test
#endif
