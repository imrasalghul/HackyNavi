// SPDX-License-Identifier: MIT
// Offline hardware protocol emulator used by tests only. Never linked into kext.
#ifndef HACKYNAVI_TEST_PSP_RING_FIXTURE_HPP
#define HACKYNAVI_TEST_PSP_RING_FIXTURE_HPP
#include "hackynavi/psp_ring.hpp"
#include <vector>
namespace pspring_test {
using namespace hackynavi;
namespace ring=hackynavi::pspring;
inline void put(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i) p[i]=uint8_t(v>>(8*i)); }
inline uint32_t get(const uint8_t* p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
inline Navi31IpLayout ips() {
    Navi31IpLayout l={}; l.valid=true;
    const uint16_t ids[]={11,34,255,1,42,43}; const uint8_t majors[]={11,3,13,13,6,6};
    for(unsigned i=0;i<6;++i) l.blocks[i]={ids[i],0,0,majors[i],0,0,2,{0x16000,0xa000}};
    return l;
}
constexpr uint32_t reg(uint32_t offset) { return (0x16000+offset)*4; }
constexpr uint64_t arenaOffset=0x400000,arenaBytes=16U*1024U*1024U;
const ring::PollOptions polls={3,2};
struct Event { char kind; uint32_t address,value; };
struct Fixture {
    VramAllocator allocator;
    VramAllocation ringAllocation{},command{},fence{},input{},tmr{};
    ring::Layout layout{};
    ring::State state;
    ring::Result result{};
    std::vector<uint8_t> memory,scratch;
    std::vector<Event> events;
    std::vector<std::vector<uint8_t>> commands,frames;
    uint64_t mcBase=0,physicalBase=0;
    uint32_t index=0,hi=0,wp=0,rp=0,control=0x80000000;
    uint32_t readCalls=0,writeCalls=0,dataReads=0,dataWrites=0,selectors=0,flushes=0,acquires=0;
    uint32_t failRead=0,failWrite=0,onesRead=0,failDataRead=0,failDataWrite=0,failSelector=0;
    uint32_t failFlush=0,failAcquire=0,busyReads=0,mailboxStatus=0,responseStatus=0,tmrSize=0x400000;
    uint32_t corruptResponseId=0,heartbeat=7,delays=0,waits=0,submitCount=0;
    uint64_t firmwareAddress=0;
    bool autoComplete=true,valid=true,forbidden=false,published=false,awaitingDelay=false;
    bool coldMode=false,sosIssued=false,sosDelayed=false;
    uint32_t sosReads=0;
    Fixture():memory(arenaBytes,0xa5),scratch(4096,0xa5) {
        MemoryLayout m={};
        valid=deriveMemoryLayout({0x8000,0x85ff,0x4000,UINT64_C(24)<<30,
            UINT64_C(8)<<30,UINT64_C(256)<<20},m)==MemoryLayoutStatus::Ok;
        valid&=allocator.configure(m,{arenaOffset,arenaBytes},nullptr,0)==VramAllocatorStatus::Ok;
        valid&=allocator.allocate(4096,4096,ringAllocation)==VramAllocatorStatus::Ok;
        valid&=allocator.allocate(4096,4096,command)==VramAllocatorStatus::Ok;
        valid&=allocator.allocate(4096,4096,fence)==VramAllocatorStatus::Ok;
        valid&=allocator.allocate(1024U*1024U,4096,input)==VramAllocatorStatus::Ok;
        valid&=allocator.allocate(0x400000,0x100000,tmr)==VramAllocatorStatus::Ok;
        valid&=allocator.addresses(ringAllocation.handle,mcBase,physicalBase)==VramAllocatorStatus::Ok;
        mcBase-=ringAllocation.range.offset; physicalBase-=ringAllocation.range.offset;
        valid&=ring::buildLayout(ips(),0x80000,layout)==ring::Status::Ok;
        firmwareAddress=mcBase+tmr.range.offset+4096;
    }
    uint8_t* bytes(const VramAllocation& a) { return memory.data()+size_t(a.range.offset-arenaOffset); }
    ring::Io io() { return {this,read,write,flush,acquire,delayUs,delayMs}; }
    ring::Buffers buffers() { return {ringAllocation.handle,command.handle,fence.handle,scratch.data(),scratch.size()}; }
    ring::Status create(const psp::State& sos) {
        return ring::create(layout,sos,allocator,buffers(),io(),polls,state,result);
    }
    size_t selected() {
        const uint64_t offset=(uint64_t(hi)<<31)|(index&0x7fffffff);
        if(!(index&0x80000000) || offset<arenaOffset || offset-arenaOffset>arenaBytes-4 || (offset&3)) {
            forbidden=true; return 0;
        }
        return size_t(offset-arenaOffset);
    }
    static bool read(void* context,uint32_t address,uint32_t& value) {
        auto& s=*static_cast<Fixture*>(context);
        if(address==4) {
            ++s.dataReads; value=get(s.memory.data()+s.selected()); return s.dataReads!=s.failDataRead;
        }
        ++s.readCalls;
        if(s.awaitingDelay) s.forbidden=true;
        if(s.coldMode) {
            if(address==reg(0x63)) value=0x80000000;
            else if(address==reg(0x91)) value=s.sosIssued ? (s.sosReads++ ? 6 : 5) : 0;
            else if(address==reg(0x7a)) value=123;
            else { s.forbidden=true; value=0; }
        } else if(address==s.layout.control) value=s.busyReads ? (--s.busyReads,0) : s.control;
        else if(address==s.layout.wptr) value=s.wp;
        else if(address==s.layout.rptr) value=s.rp;
        else if(address==s.layout.heartbeat) value=s.heartbeat;
        else { s.forbidden=true; value=0; }
        if(s.onesRead==s.readCalls) value=UINT32_MAX;
        s.events.push_back({'r',address,value}); return s.failRead!=s.readCalls;
    }
    void complete() {
        const auto& frame=frames.back();
        uint8_t* cmd=bytes(command);
        const uint32_t id=get(cmd+8);
        put(cmd+864,responseStatus);
        if(id==0x22) put(cmd+928,0x1234);
        if(id==0x20) put(cmd+880,tmrSize);
        if(id==6) { put(cmd+872,uint32_t(firmwareAddress)); put(cmd+876,uint32_t(firmwareAddress>>32)); }
        if(corruptResponseId) put(cmd+8,corruptResponseId);
        put(bytes(fence),get(frame.data()+20)); rp=wp;
    }
    static bool write(void* context,uint32_t address,uint32_t value) {
        auto& s=*static_cast<Fixture*>(context);
        if(address==0x18 || address==0) {
            ++s.selectors; if(address==0x18) s.hi=value; else s.index=value;
            return s.selectors!=s.failSelector;
        }
        if(address==4) {
            ++s.dataWrites; put(s.memory.data()+s.selected(),value); s.published=false;
            return s.dataWrites!=s.failDataWrite;
        }
        ++s.writeCalls; s.events.push_back({'w',address,value});
        if(s.awaitingDelay) s.forbidden=true;
        if(s.coldMode) {
            if(address==reg(0x63)) { if(value==0x20000) { s.sosIssued=true; s.awaitingDelay=true; } }
            else if(address!=reg(0x64)) s.forbidden=true;
        } else if(address==s.layout.control) {
            if(value!=0x20000 && value!=0x30000) s.forbidden=true;
            if(value==0x20000 && !s.published) s.forbidden=true;
            s.control=0x80000000|value|s.mailboxStatus;
            if(value==0x20000) s.wp=s.rp=0;
            s.awaitingDelay=true;
        } else if(address==s.layout.wptr) {
            if(!s.published) s.forbidden=true;
            ++s.submitCount;
            const uint8_t* frame=s.bytes(s.ringAllocation)+s.wp*4;
            s.frames.emplace_back(frame,frame+64);
            s.commands.emplace_back(s.bytes(s.command),s.bytes(s.command)+1024);
            s.wp=value; if(s.autoComplete) s.complete();
        } else if(address==s.layout.ringLo) {
            if(value!=uint32_t(s.mcBase+s.ringAllocation.range.offset)) s.forbidden=true;
        } else if(address==s.layout.ringHi) {
            if(value!=uint32_t((s.mcBase+s.ringAllocation.range.offset)>>32)) s.forbidden=true;
        } else if(address==s.layout.ringSize) { if(value!=4096) s.forbidden=true; }
        else s.forbidden=true;
        return s.writeCalls!=s.failWrite; // Failed callback can have already landed.
    }
    static bool flush(void* context) {
        auto& s=*static_cast<Fixture*>(context); ++s.flushes; s.published=true;
        s.events.push_back({'h',0,0}); return s.flushes!=s.failFlush;
    }
    static bool acquire(void* context) {
        auto& s=*static_cast<Fixture*>(context); ++s.acquires;
        s.events.push_back({'a',0,0}); return s.acquires!=s.failAcquire;
    }
    static void delayUs(void* context,uint32_t us) {
        auto& s=*static_cast<Fixture*>(context); ++s.waits; s.events.push_back({'u',0,us});
    }
    static void delayMs(void* context,uint32_t ms) {
        auto& s=*static_cast<Fixture*>(context); ++s.delays;
        if(ms!=20 || !s.awaitingDelay) s.forbidden=true;
        s.awaitingDelay=false; s.sosDelayed=true; s.events.push_back({'m',0,ms});
    }
};
inline bool loadTestSos(psp::State& loaded) {
    Fixture s; s.coldMode=true;
    VramAllocation staging={};
    if(!s.valid || s.allocator.allocate(psp::kStagingBytes,psp::kStagingBytes,staging)!=VramAllocatorStatus::Ok)
        return false;
    std::vector<uint8_t> firmware(384,0),workspace(psp::kStagingBytes,0);
    put(firmware.data(),uint32_t(firmware.size())); put(firmware.data()+4,36);
    firmware[8]=2; firmware[12]=13; put(firmware.data()+16,123);
    put(firmware.data()+20,128); put(firmware.data()+24,256); put(firmware.data()+32,2);
    put(firmware.data()+36,2); put(firmware.data()+40,123); put(firmware.data()+48,64);
    put(firmware.data()+52,1); put(firmware.data()+56,123); put(firmware.data()+60,64); put(firmware.data()+64,64);
    for(unsigned i=256;i<384;++i) firmware[i]=uint8_t(i*17);
    psp::Layout layout={}; psp::Result result={};
    const psp::Io io={&s,Fixture::read,Fixture::write,Fixture::flush,Fixture::delayUs,Fixture::delayMs};
    return psp::buildLayout(ips(),0x80000,layout)==psp::Status::Ok &&
        psp::loadSos(layout,s.allocator,staging.handle,firmware.data(),firmware.size(),
            workspace.data(),workspace.size(),io,{3,3,2},loaded,result)==psp::Status::Ok && !s.forbidden;
}
} // namespace pspring_test
#endif
