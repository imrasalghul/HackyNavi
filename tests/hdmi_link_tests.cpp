// SPDX-License-Identifier: MIT
// Offline receiver/MMIO models only. Production paths use real DDC/DMCUB/DIO.
#include "hackynavi/hdmi_link.hpp"
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>
namespace h=hackynavi::hdmitmds;
namespace l=hackynavi::dio;
namespace d=hackynavi::dmcub;
namespace i=hackynavi::hdmiddc;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
const d::PollOptions polls={4,10};
h::Config config(uint32_t pixels=594000,uint8_t depth=8){
    return {pixels,depth,h::Encoding::Rgb,false,{600000,7},{{600000,7},true,true,true,true,false}};
}
struct Protocol {
    uint8_t sink[256]={};std::vector<std::string> events;std::vector<uint32_t> delays;
    size_t calls=0,failAt=0;bool fatal=false,ddcOk=true,sourceOk=true,configured=false,enabled=false;
    bool ignoreConfig=false,ignoreVersion=false,dropAfterEnable=false;uint32_t clock=0;
    Protocol(){sink[1]=1;sink[0x20]=0xa0;}
    h::Io io(){return {this,read,write,configure,enable,disable,ddcHealthy,sourceHealthy,delay};}
    bool good(bool source){++calls;if(calls!=failAt)return true;if(fatal){if(source)sourceOk=false;else ddcOk=false;}return false;}
    static bool read(void* p,uint8_t at,uint8_t& v){auto& s=*static_cast<Protocol*>(p);
        CHECK(s.ddcOk);s.events.push_back("r"+std::to_string(at));v=s.sink[at];return s.good(false);}
    static bool write(void* p,uint8_t at,uint8_t v){auto& s=*static_cast<Protocol*>(p);
        CHECK(s.ddcOk);s.events.push_back("w"+std::to_string(at));
        if(!(s.ignoreConfig && at==0x20) && !(s.ignoreVersion && at==2))s.sink[at]=v;
        return s.good(false);}
    static bool configure(void* p,const l::HdmiConfig& c){auto& s=*static_cast<Protocol*>(p);
        CHECK(s.sourceOk && !s.configured && !s.enabled);s.events.push_back("configure");
        CHECK(l::hdmiCharacterClock(c,s.clock)==l::Status::Ok);s.configured=true;return s.good(true);}
    static bool enable(void* p,uint32_t clock){auto& s=*static_cast<Protocol*>(p);
        CHECK(s.sourceOk && s.configured && !s.enabled && clock==s.clock);
        if(s.sink[2])CHECK(!s.delays.empty() && s.delays.back()==1000);
        s.events.push_back("enable");s.enabled=true;if(s.dropAfterEnable)s.sink[0x20]=0xa0;return s.good(true);}
    static bool disable(void* p){auto& s=*static_cast<Protocol*>(p);CHECK(s.sourceOk);
        s.events.push_back("disable");s.enabled=false;s.configured=false;return s.good(true);}
    static bool ddcHealthy(void* p){return static_cast<Protocol*>(p)->ddcOk;}
    static bool sourceHealthy(void* p){return static_cast<Protocol*>(p)->sourceOk;}
    static void delay(void* p,uint32_t us){auto& s=*static_cast<Protocol*>(p);CHECK(us==1000);s.events.push_back("delay");s.delays.push_back(us);}
};
void planning(){
    h::Plan p{};
    for(uint32_t clock:{148500U,340000U,340001U,594000U,600000U}) {
        auto c=config(clock);CHECK(h::plan(c,p)==h::Status::Ok && p.characterClockKHz==clock);
        CHECK(p.sinkConfig==(clock>340000?3:0));
        c.scrambleLowRates=true;CHECK(h::plan(c,p)==h::Status::Ok && p.sinkConfig==(clock>340000?3:1));
    }
    for(uint8_t depth:{8,10,12,16}) {
        auto c=config(148351,depth);CHECK(h::plan(c,p)==h::Status::Ok);
        CHECK(p.characterClockKHz==(uint64_t(148351)*depth+7)/8);
        c.encoding=h::Encoding::YCbCr444;CHECK(h::plan(c,p)==h::Status::Ok);
        c.sink.deepColor444=false;CHECK(h::plan(c,p)==(depth==8?h::Status::Ok:h::Status::LinkLimits));
    }
    for(unsigned n=0;n<14;++n){auto c=config();
        if(n==0)c.source.maxCharacterClockKHz=340000;
        if(n==1)c.sink.limits.maxCharacterClockKHz=340000;
        if(n==2)c.sink.scdc=false;
        if(n==3){c.pixelClockKHz=148500;c.scrambleLowRates=true;c.sink.scrambleLowRates=false;}
        if(n==4){c.pixelClockKHz=148500;c.bitsPerComponent=10;c.sink.limits.deepColorMask=0;}
        if(n==5){c.pixelClockKHz=148500;c.bitsPerComponent=12;c.source.deepColorMask=1;}
        if(n==6){c.pixelClockKHz=148500;c.encoding=h::Encoding::YCbCr444;c.sink.ycbcr444=false;}
        if(n==7)c.encoding=h::Encoding::YCbCr422;
        if(n==8)c.encoding=h::Encoding::YCbCr420;
        if(n==9)c.bitsPerComponent=9;
        if(n==10)c.bitsPerComponent=16;
        if(n==11)c.pixelClockKHz=0;
        if(n==12)c.source.deepColorMask=8;
        if(n==13)c.sink.limits.maxCharacterClockKHz=UINT32_MAX;
        Protocol model;h::Result r{};CHECK(h::activate(model.io(),c,r)!=h::Status::Ok);CHECK(model.events.empty());
    }
}
void sequences(){
    for(uint32_t clock:{148500U,340000U,340001U,594000U})for(bool low:{false,true}){
        Protocol s;h::Result r{};auto c=config(clock);c.scrambleLowRates=low;
        CHECK(h::activate(s.io(),c,r)==h::Status::Ok && r.activated && !r.recoveryRequired);
        CHECK(r.stage==h::Stage::Complete && r.sourceConfigured && r.sourceEnableAttempted);
        CHECK(s.enabled && s.sink[0x20]==uint8_t(0xa0|(clock>340000?3:low?1:0)));
        CHECK(r.scdcReads==5 && r.scdcWrites==2 && r.sourceCommands==2 && r.delayUs==1000);
        CHECK(s.events==std::vector<std::string>({"r1","configure","w2","r2","r32","w32","r32","delay","enable","r32"}));
        CHECK(h::deactivate(s.io(),true,r)==h::Status::Ok && r.sourceDisabled && r.sinkReset);
        CHECK(!s.enabled && s.sink[0x20]==0xa0 && r.delayUs==1000);
    }
    Protocol legacy;h::Result r{};auto c=config(148500);c.sink.scdc=false;
    CHECK(h::activate(legacy.io(),c,r)==h::Status::Ok && r.scdcReads==0 && r.scdcWrites==0 && r.delayUs==0);
    CHECK(legacy.events==std::vector<std::string>({"configure","enable"}));
    CHECK(h::deactivate(legacy.io(),false,r)==h::Status::Ok && r.sourceDisabled);
    for(uint8_t version:{0,255}){Protocol s;s.sink[1]=version;
        CHECK(h::activate(s.io(),config(),r)==h::Status::UnsupportedReceiver && !r.sourceConfigureAttempted);}
    Protocol newer;newer.sink[1]=2;CHECK(h::activate(newer.io(),config(),r)==h::Status::Ok);
    Protocol frl;auto fc=config();fc.sink.frlCapable=true;frl.sink[0x31]=3;
    CHECK(h::activate(frl.io(),fc,r)==h::Status::UnsupportedReceiver && !r.sourceConfigureAttempted);
    frl.sink[0x31]=0x20;CHECK(h::activate(frl.io(),fc,r)==h::Status::Ok);
}
void faults(){
    Protocol baseline;h::Result r{};CHECK(h::activate(baseline.io(),config(),r)==h::Status::Ok);
    for(bool fatal:{false,true})for(size_t at=1;at<=baseline.calls;++at){
        Protocol s;s.failAt=at;s.fatal=fatal;
        CHECK(h::activate(s.io(),config(),r)!=h::Status::Ok && !r.activated);
        if(!s.ddcOk)CHECK(r.recoveryRequired);
        if(r.sourceConfigureAttempted){CHECK(r.cleanupAttempted);
            CHECK(r.sourceDisabled==s.sourceOk);
            if(!s.sourceOk)CHECK(r.recoveryRequired && !r.sinkReset);
            if(s.sourceOk && s.ddcOk)CHECK(!s.enabled && (!r.sinkMayBeChanged || r.sinkReset));
        }
    }
    for(unsigned kind=0;kind<3;++kind){Protocol s;
        s.ignoreConfig=kind==0;s.ignoreVersion=kind==1;s.dropAfterEnable=kind==2;
        CHECK(h::activate(s.io(),config(),r)==h::Status::VerifyFailed && !r.activated);
        CHECK(r.cleanupAttempted && r.sourceDisabled && !s.enabled);
    }
    Protocol unplug;CHECK(h::activate(unplug.io(),config(),r)==h::Status::Ok);
    unplug.failAt=unplug.calls+2;CHECK(h::deactivate(unplug.io(),true,r)==h::Status::ScdcFailed);
    CHECK(r.sourceDisabled && !r.recoveryRequired && !unplug.enabled);
    Protocol faulted;CHECK(h::activate(faulted.io(),config(),r)==h::Status::Ok);faulted.ddcOk=false;
    CHECK(h::deactivate(faulted.io(),true,r)==h::Status::ScdcFailed && r.sourceDisabled && r.recoveryRequired);
    Protocol source;CHECK(h::activate(source.io(),config(),r)==h::Status::Ok);source.sourceOk=false;
    const auto before=source.events.size();CHECK(h::deactivate(source.io(),true,r)==h::Status::SourceFailed);
    CHECK(source.events.size()==before && !r.sourceDisabled && r.recoveryRequired);
}
void p16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void p32(std::vector<uint8_t>& b,size_t p,uint32_t v){p16(b,p,uint16_t(v));p16(b,p+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t p){uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s;}
std::vector<uint8_t> discovery(){std::vector<uint8_t>b(164,0);
    p32(b,0,0x28211407);p16(b,4,1);p16(b,6,4);p16(b,10,164);p16(b,12,60);p16(b,16,104);
    p32(b,60,0x53445049);p16(b,64,3);p16(b,66,104);p16(b,72,1);p16(b,76,140);p16(b,142,1);
    p16(b,144,271);b[147]=3;b[148]=3;b[149]=2;p32(b,156,0xc0);p32(b,160,0x34c0);
    p16(b,14,sum(b,60));p16(b,8,sum(b,10));return b;}
constexpr uint64_t mailbox=0x40000000;
// Integrates the production adapters with actual DDC FIFO commands, actual
// 64-byte mailbox packets, source MMIO and independently selected PHY/FE/DDC.
struct Device {
    l::Layout ll{};d::Layout dl{};i::Layout il{};l::Link link;d::Inbox inbox;i::Engine ddc;
    std::map<uint32_t,uint32_t> regs;std::vector<uint8_t> memory=std::vector<uint8_t>(d::kMailboxBytes,0);
    uint8_t fifo[i::kFifoBytes]={},sink[256]={};size_t wp=0,rp=0;uint8_t port=4;
    std::vector<std::vector<uint8_t>> firmware;std::vector<std::vector<uint8_t>> wire;
    bool noEnable=false,nack=false,dropConfig=false;uint32_t waitUs=0;
    uint32_t reg(i::Register r)const{return il.registers[port][size_t(r)];}
    uint32_t reg(d::Register r)const{return dl.registers[size_t(r)];}
    uint32_t reg(uint8_t n,l::Register r)const{return ll.registers[n][size_t(r)];}
    Device(){
        auto b=discovery();CHECK(l::buildLayout(b.data(),b.size(),0x100000,ll)==l::Status::Ok);
        CHECK(d::buildLayout(b.data(),b.size(),0x100000,dl)==d::Status::Ok);
        CHECK(i::buildLayout(b.data(),b.size(),0x100000,il)==i::Status::Ok);
        for(size_t n=0;n<l::kLinkCount;++n)for(size_t x=0;x<l::kRegisterCount;++x)regs[ll.registers[n][x]]=0;
        for(size_t x=0;x<d::kRegisterCount;++x)regs[dl.registers[x]]=0;
        for(size_t n=0;n<i::kPorts;++n)for(size_t x=0;x<i::kRegisters;++x)regs[il.registers[n][x]]=0;
        regs[reg(d::Register::Control)]=0x10000;regs[reg(d::Register::BootStatus)]=3;regs[reg(d::Register::AxiSpace)]=3;
        regs[reg(d::Register::Cw4Base)]=0x04000000;regs[reg(d::Register::Cw4Top)]=0x84004000;
        regs[reg(d::Register::Cw4Offset)]=uint32_t(mailbox);regs[reg(d::Register::InboxBase)]=0x64000000;regs[reg(d::Register::InboxSize)]=d::kInboxBytes;
        regs[reg(i::Register::Control)]=2;regs[reg(i::Register::Arbitration)]=0x10;
        regs[reg(i::Register::MemoryPowerControl)]=1;regs[reg(i::Register::MemoryPowerStatus)]=1;
        sink[1]=1;sink[0x20]=0xa0;
        d::Result dr{};CHECK(inbox.open(dl,{this,read,write,readMemory,writeMemory,flush,delay},mailbox,dr)==d::Status::Ok);
        i::Result ir{};CHECK(ddc.open(il,{this,read,write,delay},port,{100000,100},ir)==i::Status::Ok);
        l::Result lr{};CHECK(link.prepare(ll,{this,read,write,delay},{0x310c,0x2220,3,4,1,true},0,4,lr)==l::Status::Ok);
    }
    static bool read(void* c,uint32_t a,uint32_t& v){auto& s=*static_cast<Device*>(c);CHECK(s.regs.count(a));v=s.regs[a];
        if(a==s.reg(i::Register::Data)){CHECK((s.regs[a]&1) && s.rp<i::kFifoBytes);v=(uint32_t(s.fifo[s.rp++])<<8)|1;}
        return true;}
    static bool write(void* c,uint32_t a,uint32_t v){auto& s=*static_cast<Device*>(c);CHECK(s.regs.count(a));const auto old=s.regs[a];s.regs[a]=v;
        if(a==s.reg(d::Register::Wptr)){
            CHECK(v==(old+64)%d::kInboxBytes);s.firmware.emplace_back(s.memory.begin()+old,s.memory.begin()+old+64);
            const auto& cmd=s.firmware.back();CHECK(cmd[0]==0x80);
            if(cmd[1]==1){CHECK(cmd[4]==3 && cmd[3]==60 && cmd[12]==2);
                if(cmd[5]==1 && !s.noEnable)s.regs[s.reg(3,l::Register::BackendEnable)]=1;
                if(cmd[5]==0)s.regs[s.reg(3,l::Register::BackendEnable)]=0;
            }else CHECK(cmd[1]==0 && cmd[3]==12 && cmd[4]==0 && cmd[5]==15 && cmd[6]==3);
            s.regs[s.reg(d::Register::Rptr)]=v;
        }else if(a==s.reg(i::Register::MemoryPowerControl))s.regs[s.reg(i::Register::MemoryPowerStatus)]=v&1;
        else if(a==s.reg(i::Register::Arbitration)){
            if(v&0x200000)s.regs[a]=v&~0x30000cU;
            else if(v&0x100000)s.regs[a]=(v&~0x30000cU)|4;
        }else if(a==s.reg(i::Register::Control)){
            if(v&8)s.regs[s.reg(i::Register::Status)]=0;
            if((v&1) && !(old&1)){
                CHECK(((v>>8)&7)==s.port);const unsigned count=((v>>20)&3)+1;
                CHECK(count==1 || count==2);CHECK(s.wp>=3 && s.fifo[0]==0xa8);
                const uint8_t at=s.fifo[1];s.wire.emplace_back(s.fifo,s.fifo+s.wp);
                if(count==2){CHECK(s.fifo[2]==0xa9 && s.wp==3);
                    const auto t0=s.regs[s.reg(i::Register::Transaction0)],t1=s.regs[s.reg(i::Register::Transaction1)];
                    CHECK(((t0>>16)&1023)==1 && !(t0&1) && !(t0&0x2000));
                    CHECK(((t1>>16)&1023)==1 && (t1&1) && (t1&0x2000));s.fifo[s.wp]=s.sink[at];
                }else{CHECK(s.wp==3);const auto t=s.regs[s.reg(i::Register::Transaction0)];
                    CHECK(((t>>16)&1023)==2 && !(t&1) && (t&0x2000));
                    if(!s.nack && !(s.dropConfig && at==0x20))s.sink[at]=s.fifo[2];}
                s.regs[s.reg(i::Register::Status)]=s.nack?0x104:4;s.regs[a]=v&~1U;
            }
        }else if(a==s.reg(i::Register::Data)){
            if(v&1){if(v&0x80000000)s.rp=(v>>16)&1023;}
            else{if(v&0x80000000)s.wp=(v>>16)&1023;CHECK(s.wp<i::kFifoBytes);s.fifo[s.wp++]=uint8_t(v>>8);}
        }
        return true;
    }
    static bool readMemory(void* c,uint64_t a,uint8_t* b,size_t n){auto& s=*static_cast<Device*>(c);CHECK(a>=mailbox && a+n<=mailbox+d::kInboxBytes);std::memcpy(b,s.memory.data()+size_t(a-mailbox),n);return true;}
    static bool writeMemory(void* c,uint64_t a,const uint8_t* b,size_t n){auto& s=*static_cast<Device*>(c);CHECK(a>=mailbox && a+n<=mailbox+d::kInboxBytes);std::memcpy(s.memory.data()+size_t(a-mailbox),b,n);return true;}
    static bool flush(void*){return true;}
    static void delay(void* c,uint32_t us){static_cast<Device*>(c)->waitUs+=us;}
    h::Status activate(const h::Config& c,h::HardwareResult& r){return h::activate(ddc,link,inbox,c,polls,{this,delay},r);}
    h::Status deactivate(h::HardwareResult& r){return h::deactivate(ddc,link,inbox,true,polls,{this,delay},r);}
};
void production(){
    for(uint32_t rate:{340000U,340001U,594000U}){
        Device s;h::HardwareResult r{};CHECK(s.activate(config(rate),r)==h::Status::Ok);
        CHECK(r.link.activated && s.link.phase()==l::Phase::Enabled && r.source.phyEnableObserved);
        CHECK(s.firmware.size()==2 && s.wire.size()==7);
        CHECK(s.sink[0x20]==(rate>340000?0xa3:0xa0));
        CHECK((s.regs[s.reg(0,l::Register::HdmiControl)]&6)==(rate>340000?6U:0U));
        CHECK(s.regs[s.reg(3,l::Register::HdmiControl)]==0 && s.regs[s.reg(0,l::Register::FifoControl)]==0);
        CHECK(s.regs[s.reg(0,l::Register::HdmiGeneralControl)]&1);
        CHECK(s.deactivate(r)==h::Status::Ok && r.link.sourceDisabled && r.link.sinkReset);
        CHECK(s.link.phase()==l::Phase::Closed && s.sink[0x20]==0xa0);
        i::Result ir{};CHECK(s.ddc.close(ir)==i::Status::Ok && !s.ddc.ownsController());
    }
    Device deep;h::HardwareResult r{};auto c=config(297000,12);c.encoding=h::Encoding::YCbCr444;
    CHECK(deep.activate(c,r)==h::Status::Ok && r.link.configuration.characterClockKHz==445500);
    CHECK((deep.regs[deep.reg(0,l::Register::HdmiControl)]&0x3100001f)==0x2100001f);
    CHECK(deep.firmware[0][12]==4); // ATOM 12bpc code
    deep.nack=true;CHECK(deep.deactivate(r)==h::Status::ScdcFailed && r.link.sourceDisabled && !r.link.recoveryRequired);
    CHECK(deep.ddc.isOpen() && deep.link.phase()==l::Phase::Closed);
    Device failed;failed.noEnable=true;CHECK(failed.activate(config(),r)==h::Status::SourceFailed);
    CHECK(failed.link.needsRecovery() && r.link.recoveryRequired && !r.link.activated && !r.link.sourceDisabled);
    CHECK(failed.sink[0x20]==0xa3); // unknown local state: do not change receiver clock underneath it
    Device dropped;dropped.dropConfig=true;CHECK(dropped.activate(config(),r)==h::Status::VerifyFailed);
    CHECK(dropped.link.phase()==l::Phase::Closed && r.link.sourceDisabled && !r.link.activated);
    Device wrong;i::Result ir{};CHECK(wrong.ddc.close(ir)==i::Status::Ok);wrong.port=3;
    CHECK(wrong.ddc.open(wrong.il,{&wrong,Device::read,Device::write,Device::delay},3,{100000,100},ir)==i::Status::Ok);
    CHECK(wrong.activate(config(),r)==h::Status::InvalidState && wrong.firmware.empty());
}
}
int main(){planning();sequences();faults();production();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
