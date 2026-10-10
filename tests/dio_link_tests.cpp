// SPDX-License-Identifier: MIT
// Offline hardware/firmware model; real register protocol, no real GPU test.
#include "hackynavi/dio_link.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
namespace d=hackynavi::dmcub;
namespace l=hackynavi::dio;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do { ++checks;if(!(x)) {++failures;std::cerr<<__LINE__<<": " #x "\n";} }while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v){put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t p){uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s;}
void fix(std::vector<uint8_t>& b){put16(b,14,sum(b,60));put16(b,8,sum(b,10));}
std::vector<uint8_t> discovery(){
    std::vector<uint8_t> b(164,0);put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,164);
    put16(b,12,60);put16(b,16,104);put32(b,60,0x53445049);put16(b,64,3);put16(b,66,104);
    put16(b,72,1);put16(b,76,140);put16(b,142,1);put16(b,144,271);b[147]=3;b[148]=3;b[149]=2;put32(b,160,0x34c0);fix(b);return b;
}
constexpr uint64_t mailbox=0x40000000;
const d::PollOptions polls={4,10};
d::Route route(uint8_t phy=0,bool hdmi=false){
    const uint16_t enc[]={0x211e,0x221e,0x2120,0x2220,0x2121};
    return {uint16_t(hdmi?0x310c:0x3113),enc[phy],phy,uint8_t((phy+1)%5),uint8_t((phy+3)%5),true};
}
struct Write{uint32_t address,value;};
struct Device {
    l::Layout linkLayout{};d::Layout mailboxLayout{};d::Inbox inbox;
    std::map<uint32_t,uint32_t> regs;
    std::vector<uint8_t> memory=std::vector<uint8_t>(d::kMailboxBytes,0);
    std::vector<std::vector<uint8_t>> commands;std::vector<Write> writes;
    size_t calls=0,failAt=0,dropAt=0,onesAt=0,delays=0;
    bool ignoreEnable=false,ignoreDisable=false,mailboxStall=false,corruptMapping=false;
    Device(){
        auto b=discovery();CHECK(l::buildLayout(b.data(),b.size(),0x100000,linkLayout)==l::Status::Ok);
        CHECK(d::buildLayout(b.data(),b.size(),0x100000,mailboxLayout)==d::Status::Ok);
        for(size_t n=0;n<l::kLinkCount;++n)for(size_t i=0;i<l::kRegisterCount;++i)regs[linkLayout.registers[n][i]]=0;
        for(size_t i=0;i<d::kRegisterCount;++i)regs[mailboxLayout.registers[i]]=0;
        set(d::Register::Control,0x10000);set(d::Register::BootStatus,3);set(d::Register::AxiSpace,3);
        set(d::Register::Cw4Base,0x04000000);set(d::Register::Cw4Top,0x84004000);
        set(d::Register::Cw4Offset,uint32_t(mailbox));set(d::Register::InboxBase,0x64000000);set(d::Register::InboxSize,d::kInboxBytes);
        d::Result r{};CHECK(inbox.open(mailboxLayout,mailboxIo(),mailbox,r)==d::Status::Ok);
    }
    uint32_t reg(uint8_t n,l::Register r)const{return linkLayout.registers[n][size_t(r)];}
    uint32_t reg(d::Register r)const{return mailboxLayout.registers[size_t(r)];}
    void set(uint8_t n,l::Register r,uint32_t v){regs[reg(n,r)]=v;}
    void set(d::Register r,uint32_t v){regs[reg(r)]=v;}
    uint32_t get(uint8_t n,l::Register r)const{return regs.at(reg(n,r));}
    l::Io io(){return {this,read,write,delay};}
    d::Io mailboxIo(){return {this,read,write,readMemory,writeMemory,flush,delay};}
    static bool read(void* p,uint32_t r,uint32_t& v){auto& s=*static_cast<Device*>(p);++s.calls;CHECK(s.regs.count(r));v=s.regs[r];if(s.calls==s.onesAt)v=UINT32_MAX;return s.calls!=s.failAt;}
    static bool write(void* p,uint32_t r,uint32_t v){
        auto& s=*static_cast<Device*>(p);++s.calls;s.writes.push_back({r,v});CHECK(s.regs.count(r));
        if(s.calls==s.dropAt)return true;
        if(r==s.reg(d::Register::Wptr)){
            const auto old=s.regs[r];CHECK(v==(old+64)%d::kInboxBytes);
            s.commands.emplace_back(s.memory.begin()+old,s.memory.begin()+old+64);const auto& c=s.commands.back();
            CHECK(c[0]==0x80 && (c[1]==1 || c[1]==0) && c[4]<5);
            CHECK(c[1]==1?c[3]==60:c[3]==12);
            if(!s.mailboxStall){
                if(c[1]==1) {
                    if(c[5]==1 && !s.ignoreEnable)s.set(c[4],l::Register::BackendEnable,1);
                    if(c[5]==0 && !s.ignoreDisable)s.set(c[4],l::Register::BackendEnable,0);
                    CHECK(c[5]==0 || c[5]==1 || c[5]==11);
                    if(s.corruptMapping)s.set(c[4],l::Register::BackendControl,0);
                } else CHECK(c[5]==0x0f && c[6]==3 && c[7]==4);
                s.set(d::Register::Rptr,v);
            }
        }
        s.regs[r]=v;return s.calls!=s.failAt;
    }
    static bool writeMemory(void* p,uint64_t a,const uint8_t* b,size_t n){auto& s=*static_cast<Device*>(p);CHECK(a>=mailbox && a+n<=mailbox+d::kInboxBytes && n==64);std::memcpy(s.memory.data()+size_t(a-mailbox),b,n);return true;}
    static bool readMemory(void* p,uint64_t a,uint8_t* b,size_t n){auto& s=*static_cast<Device*>(p);CHECK(a>=mailbox && a+n<=mailbox+d::kInboxBytes && n==64);std::memcpy(b,s.memory.data()+size_t(a-mailbox),n);return true;}
    static bool flush(void*){return true;}
    static void delay(void* p,uint32_t us){auto& s=*static_cast<Device*>(p);CHECK(us==10);++s.delays;}
    l::Status prepare(l::Link& link,l::Result& r,uint8_t phy=0,bool h=false){return link.prepare(linkLayout,io(),route(phy,h),uint8_t((phy+2)%5),4,r);}
};
void normal(){
    for(uint8_t phy=0;phy<5;++phy){
        Device s;l::Link link;l::Result r{};const uint8_t fe=(phy+2)%5;
        s.set(phy,l::Register::TmdsControl,0x01010100);s.set(phy,l::Register::BackendControl,0x08080000);
        s.set(phy,l::Register::Scrambler,0x01000001);s.set(phy,l::Register::PhyControl,0x10080);s.set(phy,l::Register::PrbsControl,0x101);
        CHECK(s.prepare(link,r,phy)==l::Status::Ok);CHECK(link.phase()==l::Phase::Prepared && s.commands.empty());
        CHECK(s.get(phy,l::Register::TmdsControl)==0x01010101);
        CHECK(s.get(phy,l::Register::BackendControl)==(0x08080000U|(uint32_t(route(phy).hpd)<<28)|(0x100U<<fe)));
        CHECK(s.get(phy,l::Register::DpConfig)==3 && s.get(phy,l::Register::Scrambler)==0x01000011);
        CHECK(link.enable(s.inbox,540000,polls,r)==l::Status::Ok);CHECK(r.phyEnableObserved && r.phyCommandSubmitted && r.firmwareCommands==1);
        CHECK(s.commands[0][4]==phy && s.commands[0][12]==route(phy).hpd+1);
        for(uint8_t pattern=0;pattern<4;++pattern){
            const size_t before=s.writes.size();CHECK(link.trainingPattern(l::Pattern(pattern),r)==l::Status::Ok);
            CHECK(s.writes.size()==before+4);CHECK(s.writes[before].address==s.reg(phy,l::Register::TrainingPattern));
            CHECK(s.get(phy,l::Register::TrainingPattern)==pattern && !(s.get(phy,l::Register::LinkControl)&0x10));
            CHECK(s.get(phy,l::Register::PhyControl)==0x80 && s.get(phy,l::Register::PrbsControl)==0x100);
        }
        CHECK(link.drive(s.inbox,1,2,3,polls,r)==l::Status::Ok);CHECK(r.firmwareCommands==4 && s.commands.size()==5);
        for(size_t n=1;n<5;++n)CHECK(s.commands[n][5]==11 && s.commands[n][6]==0xd1 && s.commands[n][7]==4);
        CHECK(link.videoPattern(r)==l::Status::Ok);CHECK(link.phase()==l::Phase::VideoPattern);
        CHECK(s.get(phy,l::Register::PanelControl)==0 && s.get(phy,l::Register::FramingControl)==0x10002000);
        CHECK(s.get(phy,l::Register::Scrambler)==0x0101ff11 && (s.get(phy,l::Register::LinkControl)&0x10));
        CHECK(!s.get(fe,l::Register::StreamControl));
        CHECK(link.disable(s.inbox,polls,r)==l::Status::Ok);CHECK(link.phase()==l::Phase::Closed && !r.phyEnableObserved);
        CHECK(!(s.get(phy,l::Register::BackendControl)&0x7f00) && !(s.get(phy,l::Register::BackendEnable)&1));
        CHECK(!(s.get(phy,l::Register::LinkControl)&0x10) && s.commands.back()[7]==0);
    }
    Device s;l::Link link;l::Result r{};CHECK(s.prepare(link,r,3,true)==l::Status::Ok);
    CHECK(s.get(3,l::Register::DpConfig)==0);CHECK((s.get(3,l::Register::BackendControl)&0x70000)==0x30000);
    CHECK(link.enable(s.inbox,148351,polls,r)==l::Status::InvalidState);
    CHECK(link.configureHdmi(s.inbox,{148351,8,false},polls,r)==l::Status::Ok);
    CHECK(link.enable(s.inbox,148351,polls,r)==l::Status::Ok);
    const auto before=s.calls;CHECK(link.trainingPattern(l::Pattern::Tps1,r)==l::Status::InvalidArgument);
    CHECK(link.drive(s.inbox,0,0,0,polls,r)==l::Status::InvalidArgument);CHECK(s.calls==before);
    CHECK(link.disable(s.inbox,polls,r)==l::Status::Ok);
}
void hdmiTests(){
    for(uint8_t depth:{8,10,12,16})for(uint32_t pixels:{148351U,170000U,272000U,340000U}){
        Device s;l::Link link;l::Result r{};const uint8_t phy=4,fe=1;
        uint32_t clock=0;const l::HdmiConfig cfg={pixels,depth,true};
        const auto planned=l::hdmiCharacterClock(cfg,clock);
        if(uint64_t(pixels)*depth>4800000){CHECK(planned==l::Status::InvalidArgument);continue;}
        CHECK(planned==l::Status::Ok && clock==(uint64_t(pixels)*depth+7)/8);
        s.set(fe,l::Register::HdmiControl,0xc0400280);s.set(phy,l::Register::HdmiControl,0xabcdef01);
        s.set(fe,l::Register::FrontendControl,0xd1400043);s.set(fe,l::Register::HdmiVbiControl,0x20001300);
        CHECK(s.prepare(link,r,phy,true)==l::Status::Ok);
        CHECK(link.configureHdmi(s.inbox,cfg,polls,r)==l::Status::Ok);
        CHECK(r.firmwareCommands==1 && s.commands.size()==1 && s.commands[0][4]==fe);
        const uint32_t encoding=depth==8?0:depth==10?1:depth==12?2:3;
        const uint32_t control=0x1b|(encoding<<28)|(encoding?0x1000000:0)|(clock>340000?4:0);
        CHECK(s.get(fe,l::Register::HdmiControl)==(0xc0400280|control));
        CHECK(s.get(phy,l::Register::HdmiControl)==0xabcdef01); // never confuse PHY with FE
        CHECK(s.get(fe,l::Register::FrontendControl)==0x01400043);
        CHECK(s.get(fe,l::Register::HdmiVbiControl)==0x20000331);
        CHECK((s.get(fe,l::Register::HdmiGeneralControl)&1)==1);
        CHECK(link.enable(s.inbox,clock+1,polls,r)==l::Status::InvalidState);
        CHECK(link.enable(s.inbox,clock,polls,r)==l::Status::Ok);
        CHECK(s.get(fe,l::Register::FifoControl)==0 && s.get(fe,l::Register::StreamControl)==0);
        CHECK(link.disable(s.inbox,polls,r)==l::Status::Ok);
        CHECK(s.get(fe,l::Register::HdmiControl)==0xc0400299);
        CHECK((s.get(fe,l::Register::HdmiVbiControl)&0x1031)==0);
    }
    for(uint32_t clock:{340000U,340001U}) {
        Device s;l::Link link;l::Result r{};
        CHECK(s.prepare(link,r,0,true)==l::Status::Ok);
        CHECK(link.configureHdmi(s.inbox,{clock,8,clock>340000},polls,r)==l::Status::Ok);
        CHECK((s.get(2,l::Register::HdmiControl)&6)==(clock>340000?6U:0U));
    }
    Device busy;l::Link rejected;l::Result r{};busy.set(2,l::Register::FifoControl,1);
    CHECK(busy.prepare(rejected,r,0,true)==l::Status::ActiveOutput && busy.writes.empty());
    for(unsigned f=0;f<4;++f){
        Device s;l::Link link;CHECK(s.prepare(link,r,0,true)==l::Status::Ok);
        CHECK(link.configureHdmi(s.inbox,{594000,8,true},polls,r)==l::Status::Ok);
        if(f==0)s.set(2,l::Register::FifoControl,1);
        if(f==1)s.set(2,l::Register::HdmiControl,0x19);
        if(f==2)s.set(2,l::Register::FrontendControl,0x10000000);
        if(f==3)s.ignoreEnable=true;
        CHECK(link.enable(s.inbox,594000,polls,r)!=l::Status::Ok && link.needsRecovery());
    }
    Device baseline;l::Link normal;CHECK(baseline.prepare(normal,r,0,true)==l::Status::Ok);
    baseline.calls=0;CHECK(normal.configureHdmi(baseline.inbox,{594000,8,true},polls,r)==l::Status::Ok);
    for(size_t n=1;n<=baseline.calls;++n){
        Device s;l::Link link;CHECK(s.prepare(link,r,0,true)==l::Status::Ok);s.calls=0;s.failAt=n;
        CHECK(link.configureHdmi(s.inbox,{594000,8,true},polls,r)!=l::Status::Ok && link.needsRecovery());
        const auto before=s.calls;CHECK(link.enable(s.inbox,594000,polls,r)==l::Status::Faulted && s.calls==before);
    }
}
void preconditions(){
    for(unsigned f=0;f<7;++f){
        Device s;l::Link link;l::Result r{};
        if(f==0)s.set(0,l::Register::BackendEnable,1);if(f==1)s.set(2,l::Register::StreamControl,0x10000);
        if(f==2)s.set(0,l::Register::PhyMux,2);if(f==3)s.set(0,l::Register::BackendControl,0x200);
        if(f==4)s.set(4,l::Register::BackendControl,0x400);
        if(f==5)s.set(0,l::Register::PhyControl,0x10);if(f==6)s.set(0,l::Register::PhyControl,0x20);
        CHECK(s.prepare(link,r)!=l::Status::Ok);CHECK(s.writes.empty() && link.phase()==l::Phase::Closed);
    }
    for(size_t n=0;n<l::kLinkCount;++n)for(size_t i=0;i<l::kRegisterCount;++i){
        Device s;l::Link link;l::Result r{};s.linkLayout.registers[n][i]+=4;const auto before=s.calls;
        CHECK(s.prepare(link,r)==l::Status::InvalidArgument);CHECK(s.calls==before && s.writes.empty());
    }
    Device s;l::Link link;l::Result r{};CHECK(s.prepare(link,r)==l::Status::Ok);const auto before=s.calls;
    CHECK(link.enable(s.inbox,148500,polls,r)==l::Status::InvalidArgument);CHECK(link.enable(s.inbox,540000,{1,0},r)==l::Status::InvalidArgument);
    CHECK(link.trainingPattern(l::Pattern::Tps1,r)==l::Status::InvalidState);CHECK(link.videoPattern(r)==l::Status::InvalidState);CHECK(s.calls==before);
    CHECK(link.disable(s.inbox,polls,r)==l::Status::Ok);CHECK(s.commands.empty()); // prepare can unwind without PHY command
}
void failuresTest(){
    for(unsigned f=0;f<5;++f){
        Device s;l::Link link;l::Result r{};CHECK(s.prepare(link,r)==l::Status::Ok);
        if(f==0)s.ignoreEnable=true;if(f==1)s.mailboxStall=true;if(f==2)s.corruptMapping=true;
        if(f==3)s.set(4,l::Register::BackendControl,0x400);if(f==4)s.set(2,l::Register::StreamControl,1);
        CHECK(link.enable(s.inbox,540000,polls,r)!=l::Status::Ok);CHECK(link.needsRecovery());
        if(f==0)CHECK(r.phyCommandSubmitted && !r.phyEnableObserved && r.polls==4 && r.firmwareStatus==d::Status::Ok);
        if(f>=3)CHECK(!r.phyCommandSubmitted);
        const auto before=s.calls;CHECK(link.disable(s.inbox,polls,r)==l::Status::Faulted);CHECK(link.trainingPattern(l::Pattern::Tps1,r)==l::Status::Faulted);CHECK(s.calls==before);
        link.clearAfterVerifiedReset();CHECK(link.phase()==l::Phase::Closed);
    }
    Device s;l::Link link;l::Result r{};CHECK(s.prepare(link,r)==l::Status::Ok);CHECK(link.enable(s.inbox,540000,polls,r)==l::Status::Ok);
    s.ignoreDisable=true;CHECK(link.disable(s.inbox,polls,r)==l::Status::EnableTimeout);CHECK(link.needsRecovery() && r.phyEnableObserved);
    Device probe;l::Link pl;l::Result pr{};probe.calls=0;CHECK(probe.prepare(pl,pr)==l::Status::Ok);const auto total=probe.calls;
    for(size_t i=1;i<=total;++i){
        Device device;l::Link tested;l::Result out{};device.calls=0;device.failAt=i;
        CHECK(device.prepare(tested,out)==l::Status::TransportError);CHECK(tested.needsRecovery());
        const auto before=device.calls;CHECK(tested.trainingPattern(l::Pattern::Tps1,out)==l::Status::Faulted);CHECK(device.calls==before);
    }
    for(const auto& w:probe.writes){
        // Drop a required write, not an idempotent mux=0 programming operation.
        if(w.value==0)continue;
        Device device;l::Link tested;l::Result out{};
        device.calls=0;
        // Locate this actual write's call number in a successful second run.
        size_t point=0;
        for(size_t i=1;i<=total;++i){
            Device candidate;l::Link cl;l::Result cr{};candidate.calls=0;candidate.failAt=i;
            candidate.prepare(cl,cr);
            if(!candidate.writes.empty() && candidate.writes.back().address==w.address && candidate.writes.back().value==w.value && candidate.calls==i){point=i;break;}
        }
        CHECK(point);device.dropAt=point;CHECK(device.prepare(tested,out)==l::Status::VerifyFailed);CHECK(tested.needsRecovery());
    }
}
void layouts(const char* path){
    auto b=discovery();l::Layout layout{};
    CHECK(l::buildLayout(b.data(),b.size(),0x100000,layout)==l::Status::Ok);
    CHECK(layout.registers[0][size_t(l::Register::BackendControl)]==0x155c4);
    CHECK(layout.registers[4][size_t(l::Register::TrainingPattern)]==0x16760);
    CHECK(layout.registers[4][size_t(l::Register::PhyMux)]==0x14f20);
    for(size_t n=0;n<b.size();++n)CHECK(l::buildLayout(b.data(),n,0x100000,layout)!=l::Status::Ok);
    if(path){std::ifstream f(path,std::ios::binary);b={std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};CHECK(l::buildLayout(b.data(),b.size(),0x100000,layout)==l::Status::Ok);}
}
}
int main(int argc,char** argv){normal();preconditions();failuresTest();hdmiTests();layouts(argc>1?argv[1]:nullptr);std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
