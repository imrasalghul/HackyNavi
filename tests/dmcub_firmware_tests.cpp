// SPDX-License-Identifier: MIT
// Offline protocol/memory emulator. Never linked into a kernel or hardware path.
#include "hackynavi/dmcub_firmware.hpp"
#include "gfx_firmware_fixture.hpp"
#include <algorithm>
#include <cstring>
using namespace gfxfw_test;
namespace d=hackynavi::dmcubfw;
namespace {
std::vector<uint8_t> firmware() {
    auto b=common(1,0,3,40,768,1024); put16(b,14,2); put32(b,16,0x07003500);
    put32(b,32,768); put32(b,36,0);
    std::fill(b.begin()+704,b.begin()+768,0);
    put32(b,704,0x444d5542); put32(b,708,47296); put32(b,712,65552);
    put32(b,716,0x07003500); b[720]=1; return b;
}
void header(std::vector<uint8_t>& b,size_t p,uint16_t bytes,uint8_t major,uint8_t minor) {
    put16(b,p,bytes); b[p+2]=major; b[p+3]=minor;
}
std::vector<uint8_t> rom() {
    std::vector<uint8_t> b(4096,0); b[0]=0x55; b[1]=0xaa; b[2]=8;
    put16(b,0x18,0x80); std::memcpy(b.data()+0x80,"PCIR",4);
    put16(b,0x84,0x1002); put16(b,0x86,0x744c); put16(b,0x8a,24); put16(b,0x90,8); b[0x95]=0x80;
    put16(b,0x48,0x100); header(b,0x100,44,2,3); std::memcpy(b.data()+0x104,"ATOM",4);
    put16(b,0x11e,0x190); put16(b,0x120,0x140); header(b,0x190,8,2,1); header(b,0x140,74,2,1);
    uint8_t sum=0; for(auto v:b)sum=uint8_t(sum+v); b[0x60]=uint8_t(0-sum); return b;
}
uint16_t sum16(const std::vector<uint8_t>& b,size_t p) { uint16_t n=0; for(;p<b.size();++p)n=uint16_t(n+b[p]); return n; }
std::vector<uint8_t> discovery() {
    std::vector<uint8_t> b(164,0);
    put32(b,0,0x28211407); put16(b,4,1); put16(b,6,4); put16(b,10,164);
    put16(b,12,60); put16(b,16,104); put32(b,60,0x53445049); put16(b,64,3);
    put16(b,66,104); put16(b,72,1); put16(b,76,140); put16(b,142,1);
    put16(b,144,271); b[147]=3; b[148]=3; b[149]=2; put32(b,160,0x34c0);
    put16(b,14,sum16(b,60)); put16(b,8,sum16(b,10)); return b;
}
constexpr uint32_t reg(uint32_t offset) { return (0x34c0+offset)*4; }
const d::PollOptions polls={3,1};
struct Device {
    Simulation psp;
    d::State state; d::Result result={}; d::Layout layout={};
    std::vector<uint8_t> fw=firmware(), bios=rom(), memory=std::vector<uint8_t>(512*1024,0xab);
    ImageInfo image={}; d::Plan expected={};
    std::map<uint32_t,uint32_t> registers;
    struct Op { bool write; uint32_t address,value; };
    std::vector<Op> accesses;
    uint64_t mc=UINT64_C(0x1230000000);
    size_t failAt=0,onesAt=0,publishes=0,stopRequests=0,boots=0;
    bool stallStop=false,stallPwait=false,stallBoot=false,stallVersion=false,badVersion=false;
    bool failPublish=false,dropWrite=false;
    uint32_t coldSos=0; bool changeColdSos=false;
    Device() {
        const auto disc=discovery(); CHECK(d::buildLayout(disc.data(),disc.size(),0x100000,layout)==d::Status::Ok);
        CHECK(parseImage(bios.data(),bios.size(),0,image)==hackynavi::Status::Ok && image.checksumValid);
        d::Image parsed={}; CHECK(d::parse(fw.data(),fw.size(),parsed)==d::Status::Ok);
        CHECK(d::plan(parsed,uint32_t(bios.size()),expected)==d::Status::Ok);
        for(unsigned i=0;i<45;++i)registers[layout.regs[i]]=0;
        registers[reg(0xca)]=0x10000; registers[reg(0x21a)]=3;
        registers[reg(0x1ad)]=0x80001234; registers[reg(0x1ae)]=0x80005678;
        registers[reg(0x1f6)]=0x10000; registers[reg(0x342)]=0x100;
        CHECK(load()==d::Status::Ok);
    }
    d::Status load() { return d::load(fw.data(),fw.size(),psp.allocator,psp.staging.handle,
        psp.transfer(),psp.hdpIo(),psp.hdp,psp.io(),psp.ring,{3,1},state,result); }
    d::Memory view() { return {memory.data(),memory.size(),mc}; }
    d::Io io() { return {this,read,write,publish,delay}; }
    d::Status boot() { return d::boot(layout,bios.data(),bios.size(),image,view(),io(),polls,state,result); }
    static bool read(void* c,uint32_t address,uint32_t& value) {
        auto& s=*static_cast<Device*>(c); CHECK(s.registers.count(address)==1);
        if(s.changeColdSos && address==s.coldSos && !s.accesses.empty())s.registers[address]=1;
        value=s.registers[address]; s.accesses.push_back({false,address,value});
        if(s.accesses.size()==s.onesAt)value=UINT32_MAX;
        return s.accesses.size()!=s.failAt;
    }
    static bool write(void* c,uint32_t address,uint32_t value) {
        auto& s=*static_cast<Device*>(c); CHECK(s.registers.count(address)==1);
        // PSP-installed secure windows are never written by the host boot path.
        CHECK(address!=reg(0x1ad) && address!=reg(0x1ae) && address!=reg(0x21a));
        s.accesses.push_back({true,address,value});
        if(s.dropWrite)return s.accesses.size()!=s.failAt;
        const uint32_t previous=s.registers[address]; s.registers[address]=value;
        if(address==reg(0x1f8) && value==0x10020000) {
            ++s.stopRequests; s.registers[address]=0x20000;
            if(!s.stallStop)s.registers[reg(0x1ea)]=0xdeaddead;
            if(!s.stallPwait)s.registers[reg(0x1f6)]|=0x100000;
        } else if(address==reg(0x200) && !(value&1) && (previous&1)) {
            ++s.boots; CHECK(s.publishes==1);
            CHECK((s.registers[reg(0x1f6)]&0x90000)==0x90000);
            CHECK((s.registers[reg(0x342)]&0x100)==0);
            s.verifyWindows(); s.registers[reg(0x1f6)]&=~0x100000U;
            if(!s.stallBoot)s.registers[reg(0x1e3)]=3;
        } else if(address==reg(0x1f8) && value==0x10010000 && !s.stallVersion) {
            s.registers[address]=0x10000; s.registers[reg(0x1ea)]=s.badVersion?0x07003501:0x07003500;
        }
        // Fault injection deliberately lets the register mutation land first.
        return s.accesses.size()!=s.failAt;
    }
    void verifyWindows() {
        const uint32_t low[]={0x1bb,0x1bd,0x1bf,0x1c1};
        for(unsigned i=0;i<4;++i) {
            const auto& r=expected.regions[i]; const uint64_t address=mc+r.offset;
            CHECK(registers[reg(low[i])]==uint32_t(address)); CHECK(registers[reg(low[i]+1)]==uint32_t(address>>32));
            CHECK((registers[reg(0x1a8+i)]&0x1fffffff)==0x03000000+i*0x01000000);
            CHECK(registers[reg(0x1b0+i)]==(0x83000000+i*0x01000000+r.bytes));
        }
        CHECK(registers[reg(0x198)]==uint32_t(mc+expected.regions[2].offset));
        CHECK(registers[reg(0x199)]==uint32_t((mc+expected.regions[2].offset)>>32));
        CHECK(registers[reg(0x1a2)]==(0x80000000|uint32_t(expected.regions[2].bytes-1)));
        CHECK(registers[reg(0x1d8)]==0xa0000010); CHECK(registers[reg(0x1d9)]==expected.regions[2].bytes-16);
        CHECK(registers[reg(0x1d4)]==0x64000000 && registers[reg(0x1d5)]==8192);
        CHECK(registers[reg(0x1dc)]==0x64002000 && registers[reg(0x1dd)]==8192);
    }
    static bool publish(void* c) {
        auto& s=*static_cast<Device*>(c); ++s.publishes;
        CHECK((s.registers[reg(0x200)]&1)==1 && !(s.registers[reg(0x1f6)]&0x10000));
        CHECK(std::equal(s.bios.begin(),s.bios.end(),s.memory.begin()));
        CHECK(std::all_of(s.memory.begin()+s.bios.size(),s.memory.begin()+s.expected.totalBytes,[](uint8_t b){return b==0;}));
        CHECK(std::all_of(s.memory.begin()+s.expected.totalBytes,s.memory.end(),[](uint8_t b){return b==0xab;}));
        return !s.failPublish;
    }
    static void delay(void*,uint32_t us) { CHECK(us==1); }
    static bool readMemory(void* c,uint64_t address,uint8_t* out,size_t bytes) {
        auto& s=*static_cast<Device*>(c); if(address<s.mc || address-s.mc>s.memory.size() || bytes>s.memory.size()-size_t(address-s.mc))return false;
        std::memcpy(out,s.memory.data()+size_t(address-s.mc),bytes); return true;
    }
    static bool writeMemory(void* c,uint64_t address,const uint8_t* in,size_t bytes) {
        auto& s=*static_cast<Device*>(c); if(address<s.mc || address-s.mc>s.memory.size() || bytes>s.memory.size()-size_t(address-s.mc))return false;
        std::memcpy(s.memory.data()+size_t(address-s.mc),in,bytes); return true;
    }
    static bool flush(void*) {return true;}
};
void inheritedCold() {
    const auto ip=ips(); psp::Layout pl={}; CHECK(psp::buildLayout(ip,0x100000,pl)==psp::Status::Ok);
    size_t operations=0;
    {
        Device s;s.coldSos=pl.heartbeat;s.registers[pl.heartbeat]=0;
        auto io=s.io();io.publishMemory=nullptr;
        CHECK(d::stopInheritedCold(ip,s.layout,io,polls,s.result)==d::Status::Ok);
        CHECK(s.result.stage==d::Stage::Complete && s.stopRequests==1 && !s.publishes && !s.boots);
        CHECK(s.registers[reg(0x200)]&1);CHECK(!(s.registers[reg(0x1f6)]&0x10000));
        CHECK(s.accesses.front().address==pl.heartbeat && s.accesses.back().address==pl.heartbeat);
        operations=s.accesses.size();
    }
    for(size_t at=1;at<=operations;++at)for(unsigned kind=0;kind<2;++kind) {
        Device s;s.registers[pl.heartbeat]=0;
        if(kind==0)s.failAt=at;else s.onesAt=at;
        const auto result=d::stopInheritedCold(ip,s.layout,s.io(),polls,s.result);
        // All-ones injection affects reads only. Every callback failure stops,
        // including a mutation which landed before its false return.
        if(kind==0 || !s.accesses[at-1].write)CHECK(result!=d::Status::Ok);
        else CHECK(result==d::Status::Ok);
        if(kind==0)CHECK(s.accesses.size()==at);
        CHECK(!s.publishes && !s.boots);
    }
    for(unsigned mode=0;mode<8;++mode) {
        Device s;s.coldSos=pl.heartbeat;s.registers[pl.heartbeat]=0;
        if(mode==0)s.registers[pl.heartbeat]=1;
        if(mode==1)s.changeColdSos=true;
        if(mode==2)s.stallStop=true;
        if(mode==3)s.stallPwait=true;
        if(mode==4)s.registers[reg(0x1f8)]=0x10010000;
        if(mode==5)s.dropWrite=true;
        if(mode==6)s.registers[reg(0x1f6)]=0;
        if(mode==7)s.registers[reg(0x200)]=1;
        CHECK((d::stopInheritedCold(ip,s.layout,s.io(),polls,s.result)==d::Status::Ok)==(mode>=6));
        if(mode==0)CHECK(s.result.writes==0 && s.accesses.size()==1);
        if(mode==1)CHECK(s.result.writes>0);
        if(mode>=6)CHECK(s.stopRequests==0);
        CHECK(!s.publishes && !s.boots);
    }
    Device s;s.registers[pl.heartbeat]=0;auto bad=ip;bad.valid=false;
    CHECK(d::stopInheritedCold(bad,s.layout,s.io(),polls,s.result)==d::Status::UnsupportedLayout);
    CHECK(s.accesses.empty());
}
void parsing() {
    auto fw=firmware(); d::Image image={}; d::Plan plan={};
    CHECK(d::parse(fw.data(),fw.size(),image)==d::Status::Ok);
    CHECK(image.pspImage.offset==256 && image.pspImage.size==768 && image.instruction.offset==512 && image.instruction.size==256);
    CHECK(image.metadataOffset==704 && image.footerBytes==256 && image.bss.size==0);
    CHECK(d::plan(image,4096,plan)==d::Status::Ok);
    CHECK(plan.regions[0].offset==0 && plan.regions[1].offset==4096 && plan.regions[2].offset==20480);
    CHECK(plan.regions[2].bytes==65600 && plan.regions[3].offset==86272 && plan.regions[3].bytes==47296 && plan.totalBytes==135168);
    for(size_t n=0;n<fw.size();++n) { CHECK(d::parse(fw.data(),n,image)!=d::Status::Ok); CHECK(!image.version); }
    CHECK(d::parse(nullptr,fw.size(),image)==d::Status::FirmwareRejected);
    const size_t fields[]={0,4,8,12,16,20,24,32,36,704,708,712,716,720,724,728,732};
    for(auto at:fields) { auto bad=fw; put32(bad,at,UINT32_MAX); CHECK(d::parse(bad.data(),bad.size(),image)!=d::Status::Ok); CHECK(!image.version); }
    for(unsigned pad=0;pad<16;++pad) {
        auto padded=fw; std::copy(fw.begin()+704,fw.begin()+768,padded.begin()+704-pad);
        if(pad)std::fill(padded.begin()+768-pad,padded.begin()+768,0);
        CHECK(d::parse(padded.data(),padded.size(),image)==d::Status::Ok && image.metadataOffset==704-pad);
    }
    auto footer512=fw; footer512.resize(1280,0); put32(footer512,0,1280);
    put32(footer512,20,1024); put32(footer512,32,1024);
    CHECK(d::parse(footer512.data(),footer512.size(),image)==d::Status::Ok && image.footerBytes==512 && image.instruction.size==256);
    CHECK(d::parse(fw.data(),fw.size(),image)==d::Status::Ok);
    CHECK(d::plan(image,0,plan)==d::Status::InvalidArgument);
    CHECK(d::plan(image,UINT32_MAX,plan)==d::Status::InvalidArgument);
    auto disc=discovery(); d::Layout l={};
    for(size_t n=0;n<disc.size();++n)CHECK(d::buildLayout(disc.data(),n,0x100000,l)!=d::Status::Ok);
    CHECK(d::buildLayout(disc.data(),disc.size(),0x100000,l)==d::Status::Ok && l.regs[0]==reg(0x1f6));
}
void normal() {
    Device s; CHECK(s.psp.received.size()==1 && s.psp.received[0].type==51);
    CHECK(s.psp.received[0].bytes==std::vector<uint8_t>(s.fw.begin()+256,s.fw.end()));
    CHECK(!s.state.needsStagingRetention() && s.state.phase()==d::Phase::Loaded);
    CHECK(s.boot()==d::Status::Ok && s.state.ready() && s.state.needsRuntimeRetention());
    CHECK(s.stopRequests==1 && s.boots==1 && s.publishes==1);
    CHECK(s.state.mailboxGpuAddress()==s.mc+4096);
    const auto disc=discovery(); hackynavi::dmcub::Layout l={};
    CHECK(hackynavi::dmcub::buildLayout(disc.data(),disc.size(),0x100000,l)==hackynavi::dmcub::Status::Ok);
    hackynavi::dmcub::Inbox inbox; hackynavi::dmcub::Result result={};
    CHECK(inbox.open(l,{&s,Device::read,Device::write,Device::readMemory,Device::writeMemory,Device::flush,Device::delay},
        s.state.mailboxGpuAddress(),result)==hackynavi::dmcub::Status::Ok);
    CHECK(inbox.close(result)==hackynavi::dmcub::Status::Ok);
    CHECK(d::stop(s.io(),polls,s.state,s.result)==d::Status::Ok && s.state.phase()==d::Phase::Stopped);
    CHECK(!s.state.needsRuntimeRetention() && s.result.firmwareMayBeLoaded && s.stopRequests==2);
    for(auto offset:{0x1af,0x1b0,0x1b1,0x1b2,0x1b3,0x1b4,0x1a2})CHECK(!(s.registers[reg(offset)]&0x80000000));
    CHECK(s.registers[reg(0x1ad)]==0x80001234 && s.registers[reg(0x1ae)]==0x80005678);
    const auto access=s.accesses.size(); CHECK(s.boot()==d::Status::InvalidState && s.accesses.size()==access);
    CHECK(s.load()==d::Status::InvalidState);
}
void failureCases() {
    // Preflight rejects before any MMIO, memory writes or ownership transition.
    for(unsigned n=0;n<7;++n) {
        Device s; auto memory=s.view(); auto l=s.layout; auto io=s.io(); auto p=polls;
        if(n==0)memory.bytes=1; if(n==1)memory.gpuMcAddress++; if(n==2)memory.cpu=s.bios.data();
        if(n==3)l.regs[20]+=4; if(n==4)io.publishMemory=nullptr; if(n==5)p.iterations=0; if(n==6)s.bios[0]^=1;
        CHECK(d::boot(l,s.bios.data(),s.bios.size(),s.image,memory,io,p,s.state,s.result)!=d::Status::Ok);
        CHECK(s.accesses.empty() && !s.publishes && s.state.phase()==d::Phase::Loaded && !s.state.needsRuntimeRetention());
    }
    for(unsigned n=0;n<10;++n) {
        Device s;
        if(n==0)s.stallStop=true; if(n==1)s.stallPwait=true; if(n==2)s.stallBoot=true;
        if(n==3)s.stallVersion=true; if(n==4)s.badVersion=true; if(n==5)s.failPublish=true;
        if(n==6)s.registers[reg(0x1f8)]=0x10030000; if(n==7)s.dropWrite=true;
        if(n==8)s.registers[reg(0x21a)]=4; if(n==9)s.registers[reg(0x1ad)]=0;
        CHECK(s.boot()!=d::Status::Ok && s.state.phase()==d::Phase::Faulted && !s.state.ready());
        CHECK(s.state.needsRuntimeRetention()==(n<8));
        const auto accesses=s.accesses.size(); s.registers[reg(0x1e3)]=3;
        CHECK(s.boot()==d::Status::InvalidState && s.accesses.size()==accesses);
        if(n==0 || n==1 || n==6 || n==7)CHECK(!s.publishes);
    }
    Device baseline; CHECK(baseline.boot()==d::Status::Ok); const auto pattern=baseline.accesses;
    for(size_t n=1;n<=pattern.size();++n) {
        Device s; s.failAt=n;
        CHECK(s.boot()!=d::Status::Ok && s.state.phase()==d::Phase::Faulted && !s.state.ready());
        CHECK(s.accesses.size()==n && s.state.needsRuntimeRetention()==(n>4));
        CHECK(s.result.status==(pattern[n-1].write?d::Status::WriteFailed:d::Status::ReadFailed));
        const auto before=s.accesses.size(); s.failAt=0; s.registers[reg(0x1e3)]=3;
        CHECK(s.boot()==d::Status::InvalidState && s.accesses.size()==before);
        if(!pattern[n-1].write) {
            Device gone; gone.onesAt=n;
            CHECK(gone.boot()==d::Status::DeviceUnavailable && gone.state.phase()==d::Phase::Faulted);
        }
    }
    Device stopped; CHECK(stopped.boot()==d::Status::Ok); stopped.accesses.clear();
    CHECK(d::stop(stopped.io(),polls,stopped.state,stopped.result)==d::Status::Ok); const auto stopPattern=stopped.accesses;
    for(size_t n=1;n<=stopPattern.size();++n) {
        Device s; CHECK(s.boot()==d::Status::Ok); s.accesses.clear(); s.failAt=n;
        CHECK(d::stop(s.io(),polls,s.state,s.result)!=d::Status::Ok);
        CHECK(s.state.phase()==d::Phase::Faulted && s.state.needsRuntimeRetention() && !s.state.ready());
        CHECK(s.accesses.size()==n);
        s.failAt=0; CHECK(d::stop(s.io(),polls,s.state,s.result)==d::Status::Ok && !s.state.needsRuntimeRetention());
    }
    for(unsigned n=0;n<2;++n) {
        Device s; CHECK(s.boot()==d::Status::Ok); s.stallStop=!n; s.stallPwait=n!=0;
        CHECK(d::stop(s.io(),polls,s.state,s.result)==d::Status::StopTimeout && s.state.needsRuntimeRetention());
    }
    for(unsigned n=0;n<5;++n) {
        Simulation s; d::State state; d::Result r={}; const auto fw=firmware();
        if(n==0)s.rejectLoad=1; if(n==1)s.stallLoad=1; if(n==2)s.failPublish=true; if(n==3)s.mismatch=true;
        const auto h=n==4?s.command.handle:s.staging.handle;
        CHECK(d::load(fw.data(),fw.size(),s.allocator,h,s.transfer(),s.hdpIo(),s.hdp,s.io(),s.ring,{3,1},state,r)!=d::Status::Ok);
        CHECK(state.needsStagingRetention()==(n<2)); CHECK(!state.needsRuntimeRetention());
        if(n==4)CHECK(!s.writes && state.phase()==d::Phase::Fresh);
        else CHECK(state.phase()==d::Phase::Faulted);
    }
}
void actual(const char* path,const char* romPath) {
    std::ifstream in(path,std::ios::binary); std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),{});
    d::Image image={}; CHECK(bytes.size()==284432 && d::parse(bytes.data(),bytes.size(),image)==d::Status::Ok);
    CHECK(image.pspImage.size==284176 && image.metadataOffset==284104 && image.firmwareStateBytes==47296 && image.traceBytes==65552);
    CHECK(image.instruction.size==283664 && image.version==0x07003500 && image.footerBytes==256);
    Simulation psp; VramAllocation staging={}; CHECK(psp.allocator.allocate(288*1024,4096,staging)==VramAllocatorStatus::Ok);
    d::State state; d::Result r={};
    CHECK(d::load(bytes.data(),bytes.size(),psp.allocator,staging.handle,psp.transfer(),psp.hdpIo(),psp.hdp,psp.io(),psp.ring,{3,1},state,r)==d::Status::Ok);
    CHECK(psp.received.size()==1 && psp.received[0].type==51);
    CHECK(psp.received[0].bytes==std::vector<uint8_t>(bytes.begin()+256,bytes.end()));
    if(romPath) {
        std::ifstream romFile(romPath,std::ios::binary); std::vector<uint8_t> bios((std::istreambuf_iterator<char>(romFile)),{});
        ImageInfo ri={}; CHECK(findNextImage(bios.data(),bios.size(),0,ri)==hackynavi::Status::Ok && !ri.codeType && ri.checksumValid);
        d::Plan plan={}; CHECK(d::plan(image,uint32_t(ri.size),plan)==d::Status::Ok);
        std::cout<<"Actual legacy VBIOS "<<ri.size<<" bytes; runtime BO "<<plan.totalBytes<<" bytes\n";
        Device s; s.bios.assign(bios.begin()+ri.offset,bios.begin()+ri.offset+ri.size);
        CHECK(parseImage(s.bios.data(),s.bios.size(),0,s.image)==hackynavi::Status::Ok);
        s.expected=plan; CHECK(s.boot()==d::Status::Ok);
    }
}
}
int main(int argc,char** argv) {
    parsing(); normal(); failureCases(); inheritedCold(); if(argc>1)actual(argv[1],argc>2?argv[2]:nullptr);
    std::cout<<checks<<" checks, "<<failures<<" failures\n"; return failures?1:0;
}
