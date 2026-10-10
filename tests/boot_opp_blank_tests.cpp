// SPDX-License-Identifier: MIT
// Actual production protocol with numeric register callbacks; no hardware run.
#include "hackynavi/boot_opp_blank.hpp"
#include "fixtures/boot_backend_fixture.hpp"
#include <iostream>
#include <utility>
namespace o=hackynavi::bootopp;
namespace b=hackynavi::bootbackend;
namespace f=backend_fixture;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
// Checked independently against generated DCN3.2 register names by the runner.
const uint16_t offsets[]={0x1854,0x1856,0x185a,0x1859,0x1858,0x1857,
    0x185b,0x188c,0x1840,0x1b2b,0x1b36,0x1b41,0x1b85};
uint32_t address(o::Register r,uint8_t n) {
    return uint32_t((0x34c0+offsets[size_t(r)]+n*(size_t(r)>=9?0x80u:0x5au))*4);
}
struct ReadEvent {uint32_t address;size_t writes;};
struct Device : f::Device {
    uint8_t opp,timing;
    size_t writes=0,delays=0,oppPermissions=0,writePermissions=0;
    size_t failWrite=0,mismatchWrite=0,denyAccess=0,denyWritePermission=0;
    size_t mutateRead=0;uint32_t mutateValue=0;
    bool landFailedWrite=false,neverSettle=false,revokeAfterDelay=false,revoked=false,changeBackend=false;
    uint32_t pendingPolls=0;
    std::vector<std::pair<uint32_t,uint32_t>> sent;
    std::vector<ReadEvent> trace;
    Device(uint8_t output=3,uint8_t tg=1):f::Device(2,tg,output,0,2),opp(output),timing(tg) {
        for(uint8_t n=0;n<4;++n)for(unsigned r=0;r<11;++r)regs[address(o::Register(r),n)]=0;
        regs[address(o::Register::Clock,opp)]=3;
        dimensions(3840,2160);
        regs[address(o::Register::Dimensions,opp)]=(32u<<16)|16;
        regs[address(o::Register::Offset,opp)]=0x10002;
        regs[address(o::Register::Blue,opp)]=0xaaaa5555;
        regs[address(o::Register::Green,opp)]=0x5555aaaa;
        regs[address(o::Register::Red,opp)]=0x12345670;
    }
    void dimensions(uint32_t width,uint32_t height) {
        regs[address(o::Register::Horizontal,timing)]=(100u<<16)|(100+width);
        regs[address(o::Register::Vertical,timing)]=(50u<<16)|(50+height);
    }
    static bool valid(void* p) {
        auto& s=*static_cast<Device*>(p);
        return !s.revoked&&f::Device::valid(static_cast<f::Device*>(&s));
    }
    static bool canRead(void* p,b::Bank bank,uint8_t n,uint32_t a) {
        auto& s=*static_cast<Device*>(p);return f::Device::canRead(static_cast<f::Device*>(&s),bank,n,a);
    }
    static bool read(void* p,uint32_t a,uint32_t& value) {
        auto& s=*static_cast<Device*>(p);s.trace.push_back({a,s.writes});
        if(s.changeBackend&&s.writes==7&&a==f::address(b::Bank::Link,0,0))
            s.set(b::Bank::Combiner,0,4,1); // Still classified, but not the captured raw topology.
        const bool ok=f::Device::read(static_cast<f::Device*>(&s),a,value);
        if(s.reads==s.mutateRead)value=s.mutateValue;
        if(a==address(o::Register::Pending,s.opp)&&s.writes==7) {
            if(s.neverSettle)value=1;
            else if(s.pendingPolls){--s.pendingPolls;value=1;}
        }
        return ok;
    }
    static bool access(void* p,o::Register r,uint8_t n,uint32_t a,bool write) {
        auto& s=*static_cast<Device*>(p);++s.oppPermissions;
        if(size_t(r)>=o::kRegisters||n>=4||a!=address(r,n)||
            n!=(size_t(r)>=9?s.timing:s.opp)||(write&&size_t(r)>=6))s.missing=true;
        if(write&&++s.writePermissions==s.denyWritePermission)return false;
        return s.oppPermissions!=s.denyAccess;
    }
    static bool write(void* p,uint32_t a,uint32_t value) {
        auto& s=*static_cast<Device*>(p);++s.writes;s.sent.push_back({a,value});
        if(s.writes!=s.failWrite||s.landFailedWrite)s.regs[a]=value;
        if(s.writes==s.mismatchWrite)s.regs[a]^=1;
        return s.writes!=s.failWrite;
    }
    static void delay(void* p,uint32_t) {auto& s=*static_cast<Device*>(p);++s.delays;if(s.revokeAfterDelay)s.revoked=true;}
    o::Io io(){return {{this,valid,canRead,read},access,write,delay};}
    o::Status run(o::Controller& c,o::Result& r,o::Poll poll={4,10}) {
        return c.blank(disc.data(),disc.size(),0x100000,bios.data(),bios.size(),0,io(),poll,r);
    }
};
void failure(Device& d,o::Status expected,bool touched) {
    o::Controller c;o::Result r{};CHECK(d.run(c,r)==expected);
    CHECK(c.phase()==o::Phase::Faulted&&!r.blanked&&r.writesMayHaveOccurred==touched);
    CHECK(c.writesMayHaveOccurred()==touched&&!d.missing);
    const auto reads=d.reads,writes=d.writes,guards=d.guards,perms=d.oppPermissions;
    CHECK(d.run(c,r)==o::Status::InvalidState);
    CHECK(d.reads==reads&&d.writes==writes&&d.guards==guards&&d.oppPermissions==perms);
}
void positive() {
    // Independent source and sink instances; smallest and largest encodable extents.
    for(const auto shape:{std::pair<uint32_t,uint32_t>{1,1},{3840,2160},{16383,16383}})
    for(const auto route:{std::pair<uint8_t,uint8_t>{3,1},{0,3}}) {
        Device d(route.first,route.second);d.dimensions(shape.first,shape.second);
        const uint32_t control=0x00a53120;d.regs[address(o::Register::Control,d.opp)]=control;
        o::Controller c;o::Result r{};CHECK(d.run(c,r)==o::Status::Ok);
        CHECK(r.width==shape.first&&r.height==shape.second&&r.opp==d.opp&&r.timing==d.timing);
        CHECK(c.phase()==o::Phase::Blanked&&r.blanked&&r.writesMayHaveOccurred&&!d.missing);
        const o::Register order[]={o::Register::Dimensions,o::Register::Offset,o::Register::Blue,
            o::Register::Green,o::Register::Red,o::Register::Control,o::Register::Dimensions};
        const uint32_t size=(shape.first<<16)|shape.second;
        const uint32_t values[]={size,0,0,0,0,(control&~0x71u)|0x41u,size};
        CHECK(d.sent.size()==7&&r.writes==7&&r.polls==1&&d.delays==0);
        for(unsigned i=0;i<7;++i)CHECK(d.sent[i]==std::make_pair(address(order[i],d.opp),values[i]));
        const auto reads=d.reads;CHECK(d.run(c,r)==o::Status::InvalidState);CHECK(d.reads==reads&&d.writes==7);
    }
    Device delayed;delayed.pendingPolls=2;o::Controller c;o::Result r{};
    CHECK(delayed.run(c,r)==o::Status::Ok&&r.polls==3&&delayed.delays==2);
}
void admission() {
    {Device d;o::Controller c;o::Result r{};CHECK(d.run(c,r,{0,1})==o::Status::InvalidArgument);
        CHECK(c.phase()==o::Phase::Fresh&&d.reads==0&&d.writes==0);CHECK(d.run(c,r)==o::Status::Ok);}
    {Device d;o::Controller c;o::Result r{};CHECK(d.run(c,r,{100001,1})==o::Status::InvalidArgument);CHECK(d.reads==0);}
    {Device d;d.disc[0]^=1;failure(d,o::Status::DiscoveryRejected,false);CHECK(d.reads==0);}
    {Device d;f::put32(d.disc,164,0x34c0+0x1854);f::fix(d.disc);
        failure(d,o::Status::CaptureRejected,false);CHECK(d.writes==0);}
    {Device d;o::Controller c;o::Result r{};CHECK(c.blank(d.disc.data(),d.disc.size(),4,
        d.bios.data(),d.bios.size(),0,d.io(),{4,10},r)==o::Status::OutsideBar);CHECK(d.reads==0&&d.writes==0);}
    {Device d;d.bios[0]^=1;failure(d,o::Status::CaptureRejected,false);CHECK(d.reads==0);}
    {Device d;d.set(b::Bank::Link,0,0,1);failure(d,o::Status::CaptureRejected,false);CHECK(d.reads==1);}
    for(auto reg:{o::Register::Clock,o::Register::Pending,o::Register::Control,o::Register::Format}) {
        Device d;d.regs[address(reg,d.opp)]=reg==o::Register::Clock?1:reg==o::Register::Format?0x10000:1;
        failure(d,o::Status::UnsupportedState,false);
    }
    for(uint32_t h:{0x00640064u,0x00640063u,0x80640065u,0x00648065u,0x00004000u}) {
        Device d;d.regs[address(o::Register::Horizontal,d.timing)]=h;failure(d,o::Status::UnsupportedState,false);
    }
    {Device d;d.regs[address(o::Register::Vertical,d.timing)]=0x00320032;failure(d,o::Status::UnsupportedState,false);}
    {Device d;d.regs[address(o::Register::Control,d.opp)]=0x80000000;failure(d,o::Status::UnsupportedState,false);}
    {Device d;d.denyPermission=1;failure(d,o::Status::CaptureRejected,false);CHECK(d.reads==0);}
    {Device d;d.denyAccess=1;failure(d,o::Status::AccessDenied,false);CHECK(d.writes==0);}
    for(unsigned missing=0;missing<6;++missing) {
        Device d;o::Controller c;o::Result r{};auto io=d.io();
        switch(missing){case 0:io.backend.valid=nullptr;break;case 1:io.backend.canRead=nullptr;break;
            case 2:io.backend.read32=nullptr;break;case 3:io.canAccess=nullptr;break;
            case 4:io.write32=nullptr;break;case 5:io.delayUs=nullptr;break;}
        CHECK(c.blank(d.disc.data(),d.disc.size(),0x100000,d.bios.data(),d.bios.size(),0,io,{4,10},r)==o::Status::InvalidArgument);
        CHECK(c.phase()==o::Phase::Fresh&&d.reads==0&&d.writes==0);
    }
}
void failuresAtBoundaries() {
    Device reference;o::Controller c;o::Result r{};CHECK(reference.run(c,r)==o::Status::Ok);
    // Real source sequence has seven potentially delivered writes. Both lost
    // and landed failures retain the one-shot journal without any retry.
    for(size_t at=1;at<=7;++at)for(bool landed:{false,true}) {
        Device d;d.failWrite=at;d.landFailedWrite=landed;failure(d,o::Status::WriteFailed,true);CHECK(d.writes==at);
    }
    for(size_t at=1;at<=7;++at) {
        Device d;d.mismatchWrite=at;failure(d,o::Status::ReadbackMismatch,true);CHECK(d.writes==at);
    }
    for(size_t at:{size_t(1),size_t(2),size_t(13),size_t(14)}) {
        Device d;d.denyWritePermission=at;failure(d,o::Status::AccessDenied,at>1);
        CHECK(d.writes==at/2); // permission before and after each delivery
    }
    // Select semantic transport boundaries rather than repeat every broad
    // backend-capture test: initial clock, pre-first-write, immediate readback,
    // post-write guard, final topology and final pending read.
    std::vector<size_t> selected={235,248,reference.trace.size()};
    for(size_t i=0;i<reference.trace.size();++i) {
        if(reference.trace[i].writes==1){selected.push_back(i+1);selected.push_back(i+2);break;}
    }
    for(size_t at:selected)for(bool allOnes:{false,true}) {
        Device d;if(allOnes)d.onesAt=at;else d.failAt=at;
        const bool touched=reference.trace[at-1].writes!=0;
        failure(d,allOnes?o::Status::DeviceUnavailable:o::Status::ReadFailed,touched);
        CHECK(d.reads==at);
    }
    {Device d;d.neverSettle=true;failure(d,o::Status::Timeout,true);CHECK(d.delays==3&&d.writes==7);}
    {Device d;d.pendingPolls=1;d.revokeAfterDelay=true;failure(d,o::Status::AccessDenied,true);CHECK(d.delays==1);}
    {Device d;d.changeBackend=true;failure(d,o::Status::Changed,true);}
    {Device d;d.mutateRead=reference.trace.size();d.mutateValue=1;failure(d,o::Status::Changed,true);}
    // Mutating a running-clock read after the first delivered write must
    // prevent the second write even if the new value is independently valid.
    for(size_t i=0;i<reference.trace.size();++i)if(reference.trace[i].writes==1&&
        reference.trace[i].address==address(o::Register::TimingClock,reference.timing)) {
        Device d;d.mutateRead=i+1;d.mutateValue=0x10103;failure(d,o::Status::Changed,true);CHECK(d.writes==1);break;
    }
}
}
int main(){positive();admission();failuresAtBoundaries();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
