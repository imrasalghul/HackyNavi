// SPDX-License-Identifier: MIT
// Offline MMIO simulation tests. No firmware is loaded into real hardware.
#include "hackynavi/psp.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <type_traits>
#include <vector>
using namespace hackynavi;
namespace p = hackynavi::psp;
namespace {
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; } } while (0)
static_assert(!std::is_copy_constructible<p::State>::value,"state cannot be copied");
static_assert(!std::is_move_constructible<p::State>::value,"state cannot be moved");
static_assert(!std::is_copy_assignable<p::State>::value,"state cannot be assigned");
constexpr uint32_t commandReg = (0x16000+0x63)*4;
constexpr uint32_t addressReg = (0x16000+0x64)*4;
constexpr uint32_t heartbeatReg = (0x16000+0x91)*4;
constexpr uint32_t versionReg = (0x16000+0x7a)*4;
const p::Options options = {3,3,2};
void put16(std::vector<uint8_t>& b,size_t off,uint16_t value) {
    b[off] = uint8_t(value); b[off+1] = uint8_t(value>>8);
}
void put32(std::vector<uint8_t>& b,size_t off,uint32_t value) {
    for (unsigned i=0;i<4;++i) b[off+i] = uint8_t(value>>(i*8));
}
std::vector<uint8_t> container(bool full = true) {
    const uint32_t all[] = {2,1,4,3,5,6,7,8,9,10,12};
    const size_t count = full ? 11 : 2;
    std::vector<uint8_t> b(256+count*64,0);
    put32(b,0,uint32_t(b.size())); put32(b,4,36);
    put16(b,8,2); put16(b,12,13); put32(b,16,0x123456);
    put32(b,20,uint32_t(b.size()-256)); put32(b,24,256); put32(b,32,uint32_t(count));
    for (size_t i=0;i<count;++i) {
        put32(b,36+i*16,all[i]); put32(b,40+i*16,0x123456);
        put32(b,44+i*16,uint32_t(i*64)); put32(b,48+i*16,64);
        for (size_t j=0;j<64;++j) b[256+i*64+j] = uint8_t(all[i]*13+j);
    }
    return b;
}
Navi31IpLayout ips() {
    Navi31IpLayout l = {}; l.valid = true;
    const uint16_t ids[] = {11,34,255,1,42,43};
    const uint8_t major[] = {11,3,13,13,6,6};
    for (unsigned i=0;i<6;++i) l.blocks[i] = {ids[i],0,0,major[i],0,0,2,{0x16000,0xa000}};
    return l;
}
struct Event { char kind; uint32_t address,value; };
struct Sim {
    std::vector<uint8_t> fw, workspace;
    std::vector<uint32_t> memory;
    std::vector<Event> events;
    VramAllocator allocator;
    VramAllocation allocation;
    p::Layout layout;
    uint64_t mc = 0;
    uint32_t hi=0,index=0, reads=0,writes=0,selectorWrites=0,dataWrites=0,dataReads=0;
    uint32_t flushes=0,sosReads=0,delays=0,waits=0,commandCount=0;
    uint32_t failRead=0,onesRead=0,failWrite=0,failFlush=0;
    uint32_t failDataWrite=0,failDataRead=0,failSelector=0,corruptDataRead=0;
    uint32_t initialHeartbeat=0,unexpectedAtRead=0,heartbeatBase=5,heartbeatNext=6;
    uint32_t forceCommandValue=0x80000000,busyReads=0;
    bool timeoutAfterCommand=false,sosIssued=false,delayed=false,forbidden=false;
    explicit Sim(bool full=false):fw(container(full)),workspace(p::kStagingBytes+16,0xa5),
        memory(p::kStagingBytes/4,0xdeadbeef),allocation{},layout{} {
        MemoryLayout mem = {};
        CHECK(deriveMemoryLayout({0x8000,0x85ff,0x4000,UINT64_C(24)<<30,
            UINT64_C(8)<<30,UINT64_C(256)<<20},mem) == MemoryLayoutStatus::Ok);
        CHECK(allocator.configure(mem,{0x400000,0x400000},nullptr,0) == VramAllocatorStatus::Ok);
        CHECK(allocator.allocate(p::kStagingBytes,p::kStagingBytes,allocation) == VramAllocatorStatus::Ok);
        uint64_t physical = 0; CHECK(allocator.addresses(allocation.handle,mc,physical) == VramAllocatorStatus::Ok);
        CHECK(p::buildLayout(ips(),0x80000,layout) == p::Status::Ok);
    }
    p::Io io() { return {this,read,write,flush,delayUs,delayMs}; }
    size_t selected() {
        const uint64_t offset = (uint64_t(hi)<<31)|(index&0x7fffffff);
        if (!(index&0x80000000) || offset<allocation.range.offset ||
            offset-allocation.range.offset>=p::kStagingBytes || (offset&3)) {
            forbidden=true; return 0;
        }
        return size_t((offset-allocation.range.offset)/4);
    }
    static bool read(void* arg,uint32_t reg,uint32_t& value) {
        auto& s=*static_cast<Sim*>(arg);
        if (reg==4) {
            ++s.dataReads; value=s.memory[s.selected()];
            if (s.dataReads==s.corruptDataRead) value^=1;
            return s.dataReads!=s.failDataRead;
        }
        ++s.reads;
        if (s.sosIssued && !s.delayed) s.forbidden=true;
        if (reg==commandReg) {
            value=(s.timeoutAfterCommand && s.commandCount) ? 0 :
                s.busyReads ? (--s.busyReads,0) : s.forceCommandValue;
        } else if (reg==heartbeatReg) {
            if (s.sosIssued) value=s.sosReads++ ? s.heartbeatNext : s.heartbeatBase;
            else value=(s.unexpectedAtRead==s.reads) ? 9 : s.initialHeartbeat;
        } else if (reg==versionReg) value=0x123456;
        else { s.forbidden=true; value=0; }
        if (s.reads==s.onesRead) value=UINT32_MAX;
        s.events.push_back({'r',reg,value}); return s.reads!=s.failRead;
    }
    static bool write(void* arg,uint32_t reg,uint32_t value) {
        auto& s=*static_cast<Sim*>(arg);
        if (reg==0x18 || reg==0) {
            ++s.selectorWrites;
            if (reg==0x18) s.hi=value; else s.index=value;
            return s.selectorWrites!=s.failSelector;
        }
        if (reg==4) {
            ++s.dataWrites; s.memory[s.selected()]=value; return s.dataWrites!=s.failDataWrite;
        }
        ++s.writes; s.events.push_back({'w',reg,value});
        if (s.sosIssued) s.forbidden=true;
        if (reg==addressReg) { if (value != (s.mc>>20)) s.forbidden=true; }
        else if (reg==commandReg) {
            ++s.commandCount;
            if (value==0x20000) s.sosIssued=true;
        } else s.forbidden=true;
        return s.writes!=s.failWrite; // Ambiguous failed writes can land.
    }
    static bool flush(void* arg) {
        auto& s=*static_cast<Sim*>(arg); ++s.flushes;
        s.events.push_back({'f',0,0});
        // Firmware fixture images are64B. Every byte after that must be zero
        // and the full image must have completed independent readback first.
        bool zeros=true; for(size_t i=16;i<s.memory.size();++i) zeros&=s.memory[i]==0;
        CHECK(zeros);
        CHECK(s.dataWrites==s.flushes*(p::kStagingBytes/4));
        CHECK(s.dataReads==s.dataWrites);
        return s.flushes!=s.failFlush;
    }
    static void delayUs(void* arg,uint32_t us) {
        auto& s=*static_cast<Sim*>(arg); ++s.waits; CHECK(us==options.pollDelayUs);
        s.events.push_back({'u',0,us});
    }
    static void delayMs(void* arg,uint32_t ms) {
        auto& s=*static_cast<Sim*>(arg); ++s.delays;
        CHECK(ms==20 && s.sosIssued); s.delayed=true; s.events.push_back({'m',0,ms});
    }
    p::Status run(p::State& state,p::Result& result,p::Options opts=options) {
        return p::loadSos(layout,allocator,allocation.handle,fw.data(),fw.size(),
            workspace.data(),p::kStagingBytes,io(),opts,state,result);
    }
};
void parseTests() {
    auto b=container(); p::SosImage img;
    CHECK(p::parseSosImage(b.data(),b.size(),img)==p::Status::Ok);
    CHECK(img.count==9 && img.sosVersion==0x123456 && img.toc.offset==384 && img.registerList.offset==576);
    const uint32_t type[]={3,5,2,7,8,9,10,12,1};
    const uint32_t cmd[]={0x80000,0x10000000,0x10000,0xb0000,0xd0000,0xc0000,0xe0000,0x20000000,0x20000};
    const uint32_t offsets[]={448,448,256,640,704,768,832,896,320};
    for(unsigned i=0;i<9;++i) {
        CHECK(img.components[i].firmwareType==type[i] && img.components[i].command==cmd[i]);
        CHECK(img.components[i].sourceType==(i==1?3:type[i]));
        CHECK(img.components[i].image.offset==offsets[i] && img.components[i].image.size==64);
    }
    for(size_t n=0;n<b.size();++n) {
        CHECK(p::parseSosImage(b.data(),n,img)==p::Status::InvalidFirmware); CHECK(img.count==0);
    }
    for(unsigned mode=0;mode<10;++mode) {
        auto bad=b;
        if(mode==0) put16(bad,12,14);
        if(mode==1) put16(bad,14,1);
        if(mode==2) put32(bad,36,0);
        if(mode==3) put32(bad,36,13);
        if(mode==4) put32(bad,36,11);
        if(mode==5) put32(bad,36,1); // Duplicate SOS/missing SYS.
        if(mode==6) put32(bad,48,0);
        if(mode==7) put32(bad,44,64); // Overlap SOS.
        if(mode==8) put32(bad,36+3*16,7); // Duplicate SOC, missing KDB with SPL.
        if(mode==9) put32(bad,36,4); // Duplicate TOC, missing SYS.
        const auto result=p::parseSosImage(bad.data(),bad.size(),img);
        CHECK(result==(mode<5?p::Status::UnsupportedFirmware:p::Status::InvalidFirmware));
        CHECK(img.count==0);
    }
    auto noKdb=container(false); put32(noKdb,36,5);
    CHECK(p::parseSosImage(noKdb.data(),noKdb.size(),img)==p::Status::InvalidFirmware);
}
void layoutTests() {
    p::Layout l;
    CHECK(p::buildLayout(ips(),0x80000,l)==p::Status::Ok);
    CHECK(l.command==commandReg && l.address==addressReg && l.heartbeat==heartbeatReg && l.version==versionReg);
    CHECK(p::buildLayout(ips(),heartbeatReg+3,l)==p::Status::OutsideBar && !l.valid);
    CHECK(p::buildLayout(ips(),heartbeatReg+4,l)==p::Status::Ok);
    for(unsigned block=0;block<6;++block) for(unsigned f=0;f<8;++f) {
        auto ip=ips(); auto& x=ip.blocks[block];
        if(f==0) ++x.hardwareId; if(f==1) ++x.major; if(f==2) ++x.minor; if(f==3) ++x.revision;
        if(f==4) ++x.dieId; if(f==5) ++x.instance; if(f==6) x.baseCount=0; if(f==7) x.baseCount=9;
        CHECK(p::buildLayout(ip,0x80000,l)==p::Status::UnsupportedLayout && !l.valid);
    }
    auto ip=ips(); ip.blocks[2].basesDwords[0]=UINT32_MAX;
    CHECK(p::buildLayout(ip,UINT64_MAX,l)==p::Status::OutsideBar && !l.valid);
    ip=ips(); ip.blocks[2].basesDwords[0]=0x7f000/4-0x63;
    CHECK(p::buildLayout(ip,0x80000,l)==p::Status::OutsideBar && !l.valid);
}
void success() {
    for(bool full:{false,true}) {
        Sim s(full); p::State state; p::Result r;
        s.busyReads=2;
        CHECK(s.run(state,r)==p::Status::Ok);
        CHECK(state.phase()==p::Phase::Loaded && r.stage==p::Stage::Complete);
        CHECK(r.sosVersion==0x123456 && r.baselineHeartbeat==5 && r.observedHeartbeat==6);
        CHECK(r.completedComponents==(full?9U:2U));
        CHECK(s.commandCount==r.completedComponents && s.flushes==r.completedComponents);
        CHECK(r.stagingMcAddress==s.mc && r.commandValue==0x20000 && r.componentType==1);
        CHECK(r.stagingWritesMayHaveOccurred && r.mailboxWritesMayHaveOccurred && r.sosCommandMayHaveOccurred);
        CHECK(r.transfer.bytesVerified==p::kStagingBytes && r.transfer.bytesWritten==p::kStagingBytes);
        CHECK(r.reads==s.reads && r.writes==s.writes && s.delays==1 && s.waits==2 && !s.forbidden);
        const uint32_t wanted[]={0x80000,0x10000000,0x10000,0xb0000,0xd0000,
                                 0xc0000,0xe0000,0x20000000,0x20000};
        unsigned at=0;
        for(size_t e=0;e<s.events.size();++e) if(s.events[e].kind=='w' && s.events[e].address==commandReg) {
            CHECK(s.events[e].value==(full?wanted[at]:(at?0x20000U:0x10000U)));
            CHECK(e>=2 && s.events[e-1].kind=='w' && s.events[e-1].address==addressReg);
            CHECK(s.events[e-2].kind=='f'); ++at;
        }
        CHECK(at==r.completedComponents);
        for(size_t i=p::kStagingBytes;i<s.workspace.size();++i) CHECK(s.workspace[i]==0xa5);
        const auto count=s.events.size(); CHECK(s.run(state,r)==p::Status::InvalidState);
        CHECK(s.events.size()==count && state.phase()==p::Phase::Loaded);
        // The final command must be followed by delay, baseline, fresh
        // heartbeat, and version, without a fabricated bootloader ACK.
        size_t n=s.events.size();
        CHECK(s.events[n-5].kind=='w' && s.events[n-5].value==0x20000);
        CHECK(s.events[n-4].kind=='m' && s.events[n-3].address==heartbeatReg);
        CHECK(s.events[n-2].address==heartbeatReg && s.events[n-1].address==versionReg);
    }
}
void preflight() {
    for(unsigned mode=0;mode<18;++mode) {
        Sim s; p::State state; p::Result r; auto io=s.io(); auto o=options;
        auto handle=s.allocation.handle; size_t bytes=p::kStagingBytes;
        p::Status wanted=p::Status::InvalidArgument;
        if(mode==0) io.read32=nullptr; if(mode==1) io.write32=nullptr;
        if(mode==2) io.flushHdp=nullptr; if(mode==3) io.delayUs=nullptr; if(mode==4) io.delayMs=nullptr;
        if(mode==5) o.bootloaderPolls=0; if(mode==6) o.heartbeatPolls=1000001;
        if(mode==7) o.pollDelayUs=0; if(mode==8) o.pollDelayUs=1001;
        if(mode==9) { o.bootloaderPolls=10001; o.pollDelayUs=1000; }
        if(mode==10) bytes=p::kStagingBytes-1;
        if(mode==11) { ++handle.serial; wanted=p::Status::InvalidAllocation; }
        if(mode==12) { ++s.layout.address; wanted=p::Status::UnsupportedLayout; }
        if(mode==13) { s.layout.barBytes=heartbeatReg+3; wanted=p::Status::UnsupportedLayout; }
        if(mode==14) { s.layout.valid=false; wanted=p::Status::UnsupportedLayout; }
        if(mode==15) { put32(s.fw,36,9); wanted=p::Status::InvalidFirmware; }
        if(mode==16) { put16(s.fw,12,14); wanted=p::Status::UnsupportedFirmware; }
        if(mode==17) { CHECK(s.allocator.release(handle)==VramAllocatorStatus::Ok); wanted=p::Status::InvalidAllocation; }
        CHECK(p::loadSos(s.layout,s.allocator,handle,s.fw.data(),s.fw.size(),s.workspace.data(),
            bytes,io,o,state,r)==wanted);
        CHECK(s.events.empty() && !s.dataWrites && !s.selectorWrites && state.phase()==p::Phase::Fresh);
        CHECK(!r.stagingWritesMayHaveOccurred && !r.mailboxWritesMayHaveOccurred);
    }
    Sim s; p::State state; p::Result r;
    CHECK(p::loadSos(s.layout,s.allocator,s.allocation.handle,s.workspace.data(),256,s.workspace.data(),
        p::kStagingBytes,s.io(),options,state,r)==p::Status::InvalidArgument);
    CHECK(s.events.empty());
    CHECK(s.allocator.release(s.allocation.handle)==VramAllocatorStatus::Ok);
    CHECK(s.allocator.allocate(4096,4096,s.allocation)==VramAllocatorStatus::Ok);
    CHECK(s.run(state,r)==p::Status::InvalidAllocation && s.events.empty());
}
void hardwareFailures() {
    Sim gold; p::State good; p::Result gr; CHECK(gold.run(good,gr)==p::Status::Ok);
    for(unsigned i=1;i<=gold.reads;++i) for(bool ones:{false,true}) {
        Sim s; p::State state; p::Result r;
        if(ones) s.onesRead=i; else s.failRead=i;
        CHECK(s.run(state,r)==(ones?p::Status::DeviceUnavailable:p::Status::ReadFailed));
        CHECK(s.reads==i && state.phase()==p::Phase::Faulted && !s.forbidden);
        const auto events=s.events.size(); CHECK(s.run(state,r)==p::Status::InvalidState);
        CHECK(s.events.size()==events);
    }
    for(unsigned i=1;i<=gold.writes;++i) {
        Sim s; p::State state; p::Result r; s.failWrite=i;
        CHECK(s.run(state,r)==p::Status::WriteFailed && s.writes==i);
        CHECK(state.phase()==p::Phase::Faulted && r.mailboxWritesMayHaveOccurred);
        CHECK(s.delays==(i==4?1U:0U) && !s.forbidden);
        CHECK(r.sosCommandMayHaveOccurred==(i==4));
    }
    for(unsigned i=1;i<=2;++i) {
        Sim s; p::State state; p::Result r; s.failFlush=i;
        CHECK(s.run(state,r)==p::Status::HdpFlushFailed && s.flushes==i);
        CHECK(s.commandCount==i-1 && state.phase()==p::Phase::Faulted);
    }
    // Owned transport already has exhaustive DWORD failure testing. Exercise
    // first/middle/end boundaries here to prove propagation stops mailbox use.
    for(uint32_t at:{1U,131072U,262144U}) for(unsigned kind=0;kind<4;++kind) {
        Sim s; p::State state; p::Result r;
        if(kind==0) s.failDataWrite=at; if(kind==1) s.failDataRead=at;
        if(kind==2) s.corruptDataRead=at; if(kind==3) s.failSelector=at;
        CHECK(s.run(state,r)==p::Status::UploadFailed);
        CHECK(!s.writes && !s.flushes && !s.commandCount && state.phase()==p::Phase::Faulted);
        CHECK(r.transfer.status==(kind==2?VramTransferStatus::VerifyMismatch:VramTransferStatus::TransportError));
        CHECK(!r.mailboxWritesMayHaveOccurred && !s.forbidden);
    }
}
void waitAndPriorState() {
    for(uint32_t value:{0U,0x80000001U,0x80010000U}) {
        Sim s; p::State state; p::Result r; s.forceCommandValue=value;
        CHECK(s.run(state,r)==p::Status::BootloaderTimeout);
        CHECK(s.waits==2 && !s.dataWrites && !s.writes && state.phase()==p::Phase::Faulted);
    }
    {
        Sim s; p::State state; p::Result r; s.timeoutAfterCommand=true;
        CHECK(s.run(state,r)==p::Status::BootloaderTimeout);
        CHECK(r.stage==p::Stage::BootloaderComplete && r.commandMayHaveOccurred);
        CHECK(s.commandCount==1 && s.flushes==1 && r.completedComponents==0);
        CHECK(state.phase()==p::Phase::Faulted && s.waits==2 && !s.sosIssued);
    }
    for(unsigned mode=0;mode<3;++mode) {
        Sim s; p::State state; p::Result r;
        if(mode==0) s.heartbeatNext=s.heartbeatBase;
        if(mode==1) { s.heartbeatBase=0; s.heartbeatNext=0; }
        if(mode==2) s.heartbeatNext=0; // A change to dead zero is not proof of SOS life.
        CHECK(s.run(state,r)==p::Status::HeartbeatTimeout);
        CHECK(s.delays==1 && s.sosReads==4 && s.waits==2 && r.completedComponents==1);
        CHECK(state.phase()==p::Phase::Faulted && !s.forbidden);
    }
    Sim s; p::State state; p::Result r; s.initialHeartbeat=42;
    CHECK(s.run(state,r)==p::Status::AlreadyRunning);
    CHECK(r.sosVersion==0x123456 && r.observedHeartbeat==42 && state.phase()==p::Phase::AlreadyRunning);
    CHECK(s.reads==2 && !s.writes && !s.selectorWrites && !s.dataWrites && !s.flushes);
    const auto n=s.events.size(); CHECK(s.run(state,r)==p::Status::InvalidState && s.events.size()==n);
    Sim t; p::State ts; t.unexpectedAtRead=2;
    CHECK(t.run(ts,r)==p::Status::UnexpectedSos);
    CHECK(!t.dataWrites && !t.writes && ts.phase()==p::Phase::Faulted);
}
void corpus(const char* path) {
    std::ifstream f(path,std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});
    CHECK(!bytes.empty()); p::SosImage image;
    CHECK(p::parseSosImage(bytes.data(),bytes.size(),image)==p::Status::Ok);
    CHECK(image.count==4 && image.components[0].firmwareType==3 && image.components[1].firmwareType==5);
    CHECK(image.components[1].sourceType==3 && image.components[0].image.offset==image.components[1].image.offset);
    CHECK(image.components[2].firmwareType==2 && image.components[3].firmwareType==1);
    CHECK(image.toc.size==2304 && image.registerList.size==2400);
}
} // namespace
int main(int argc,char** argv) {
    parseTests(); layoutTests(); success(); preflight(); hardwareFailures(); waitAndPriorState();
    if(argc>1) corpus(argv[1]);
    std::cout<<checks<<" assertions, "<<failures<<" failures\n";
    return failures?1:0;
}
