// SPDX-License-Identifier: MIT
// Synthetic MMIO/I2C peripheral tests; no physical GPU or monitor accessed.
#include "hackynavi/hdmi_ddc.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
using namespace hackynavi;
using namespace hackynavi::hdmiddc;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void put16(std::vector<uint8_t>&b,size_t p,uint16_t v){b[p]=static_cast<uint8_t>(v);b[p+1]=static_cast<uint8_t>(v>>8);}
void put32(std::vector<uint8_t>&b,size_t p,uint32_t v){put16(b,p,static_cast<uint16_t>(v));put16(b,p+2,static_cast<uint16_t>(v>>16));}
uint16_t sum(const std::vector<uint8_t>&b,size_t p){uint16_t s=0;for(;p<b.size();++p)s=static_cast<uint16_t>(s+b[p]);return s;}
void fix(std::vector<uint8_t>&b){put16(b,14,sum(b,60));put16(b,8,sum(b,10));}
std::vector<uint8_t> fixture(){std::vector<uint8_t>b(164,0);
    put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,164);put16(b,12,60);put16(b,16,104);
    put32(b,60,0x53445049);put16(b,64,3);put16(b,66,104);put16(b,72,1);put16(b,76,140);put16(b,142,1);
    put16(b,144,271);b[147]=3;b[148]=3;b[149]=2;put32(b,156,0xc0);put32(b,160,0x34c0);fix(b);return b;}
Layout layout(){const auto b=fixture();Layout l{};CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::Ok);return l;}
struct Access {bool write;uint32_t address,value;};
struct WireMessage {uint8_t address;bool read;uint16_t bytes;std::vector<uint8_t> data;};
struct Simulation {
    Layout l;uint8_t port;std::map<uint32_t,uint32_t> registers;
    std::vector<Access> accesses;std::vector<uint32_t> delays;
    std::vector<std::vector<WireMessage>> commands;
    uint8_t fifo[kFifoBytes]={};size_t writeIndex=0,readIndex=0;
    std::vector<uint8_t> reply;
    size_t failAt=0,onesAt=0,dropAt=0;
    bool powerStuck=false,denyGrant=false,resetStuck=false,noDone=false,loseGrant=false;
    unsigned stuckRelease=0,releaseCalls=0,busySamples=0,zeroSamples=0;
    uint32_t replyStatus=4;bool running=false;
    explicit Simulation(uint8_t p=0,const Layout& selected=layout()):l(selected),port(p){
        for(size_t r=0;r<kRegisters;++r)registers[l.registers[p][r]]=0;
        set(Register::Setup,0x00210230);set(Register::Speed,0x81220110);
        set(Register::Arbitration,0x10);
        set(Register::Control,2);set(Register::MemoryPowerControl,0x41);set(Register::MemoryPowerStatus,1);
        set(Register::GpioMask,0x40010101);set(Register::GpioEnable,0x101);
        set(Register::GpioAux,0x20000055);set(Register::PhyAux,0x800001ff);
    }
    uint32_t reg(Register r)const{return l.registers[port][static_cast<size_t>(r)];}
    uint32_t get(Register r)const{return registers.at(reg(r));}
    void set(Register r,uint32_t v){registers[reg(r)]=v;}
    Io io(){return {this,read,write,delay};}
    static bool read(void* c,uint32_t a,uint32_t&v){Simulation&s=*static_cast<Simulation*>(c);
        CHECK(s.registers.count(a)==1);v=s.registers[a];
        if(a==s.reg(Register::Status) && s.running){
            if(s.zeroSamples){--s.zeroSamples;v=0;}
            else if(s.busySamples){--s.busySamples;v=0x40001;}
            else if(s.noDone)v=0;
            else{v=s.replyStatus;s.running=false;s.set(Register::Status,v);s.set(Register::Control,s.get(Register::Control)&~1U);}
        }
        if(a==s.reg(Register::Data)){
            CHECK(s.get(Register::Data)&1);CHECK(s.readIndex<kFifoBytes);
            v=static_cast<uint32_t>(s.fifo[s.readIndex++])<<8|1;
        }
        s.accesses.push_back({false,a,v});if(s.accesses.size()==s.onesAt)v=UINT32_MAX;
        return s.accesses.size()!=s.failAt;
    }
    static bool write(void*c,uint32_t a,uint32_t v){Simulation&s=*static_cast<Simulation*>(c);
        CHECK(s.registers.count(a)==1);s.accesses.push_back({true,a,v});
        if(s.accesses.size()==s.dropAt)return true;
        const uint32_t old=s.registers[a];s.registers[a]=v;
        if(a==s.reg(Register::MemoryPowerControl)){
            if(!s.powerStuck)s.set(Register::MemoryPowerStatus,v&1);
        }else if(a==s.reg(Register::Arbitration)){
            if(v&0x200000){++s.releaseCalls;
                if(s.releaseCalls<=s.stuckRelease)s.registers[a]=(v&~0x30000cU)|4;
                else s.registers[a]=v&~0x30000cU;
            }else if(v&0x100000)s.registers[a]=(v&~0x30000cU)|(s.denyGrant?8U:4U);
        }else if(a==s.reg(Register::Control)){
            if((v&8) && !s.resetStuck){s.set(Register::Status,0);s.running=false;}
            if((v&1) && !(old&1)){
                CHECK((v&0x700)==static_cast<uint32_t>(s.port)<<8);
                const size_t count=((v>>20)&3)+1;size_t pos=0;std::vector<WireMessage> command;
                for(size_t i=0;i<count;++i){
                    const uint32_t d=s.registers[s.reg(Register::Transaction0)+static_cast<uint32_t>(i)*4];
                    CHECK((d&0x1100)==0x1100);CHECK(bool(d&0x2000)==(i+1==count));
                    const uint16_t n=static_cast<uint16_t>((d>>16)&1023);const bool read=d&1;
                    CHECK(pos<s.writeIndex);const uint8_t addr=s.fifo[pos++];CHECK(bool(addr&1)==read);
                    WireMessage message{static_cast<uint8_t>(addr>>1),read,n,{}};
                    if(!read){CHECK(pos+n<=s.writeIndex);message.data.assign(s.fifo+pos,s.fifo+pos+n);pos+=n;}
                    else CHECK(i+1==count);
                    command.push_back(message);
                }
                CHECK(pos==s.writeIndex);s.commands.push_back(command);
                if(command.back().read){CHECK(pos+command.back().bytes<=kFifoBytes);
                    for(size_t i=0;i<command.back().bytes;++i)s.fifo[pos+i]=i<s.reply.size()?s.reply[i]:static_cast<uint8_t>(i*7+3);}
                s.running=true;
                if(s.loseGrant)s.set(Register::Arbitration,8);
            }
        }else if(a==s.reg(Register::Data)){
            if(v&1){if(v&0x80000000)s.readIndex=(v>>16)&1023;}
            else{if(v&0x80000000)s.writeIndex=(v>>16)&1023;CHECK(s.writeIndex<kFifoBytes);
                s.fifo[s.writeIndex++]=static_cast<uint8_t>(v>>8);}
        }
        return s.accesses.size()!=s.failAt;
    }
    static void delay(void*c,uint32_t us){static_cast<Simulation*>(c)->delays.push_back(us);}
};
const Clock clock100{100000,100};
void layoutTests(){auto b=fixture();Layout l=layout();
    CHECK(l.registers[0][0]==0x14d8c);CHECK(l.registers[4][0]==0x14dac);
    CHECK(l.registers[0][11]==0x4ec);CHECK(l.registers[4][11]==0x4ec);
    CHECK(l.registers[0][16]==0x17774);CHECK(l.registers[4][14]==0x17680);
    for(size_t n=0;n<b.size();++n){CHECK(buildLayout(b.data(),n,0x100000,l)==Status::DiscoveryRejected);CHECK(!l.valid);}
    CHECK(buildLayout(b.data(),b.size(),0x17777,l)==Status::OutsideBar);CHECK(!l.valid);
    CHECK(buildLayout(b.data(),b.size(),0x17778,l)==Status::Ok);
    b[149]=1;fix(b);CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::UnsupportedDmu);
    b=fixture();put16(b,144,11);fix(b);CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::MissingDmu);
    b=fixture();put32(b,156,0x34c0+0x1e98-0x7b);fix(b);
    CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::OutsideBar); // malicious alias
}
void clockTests(){Speed s{};
    CHECK(calculateSpeed(clock100,0,s)==Status::Ok);CHECK(s.referenceKHz==100000 && s.inputKHz==50000 && s.prescaler==500);
    CHECK(calculateSpeed({27000,50},0,s)==Status::Ok && s.prescaler==270);
    CHECK(calculateSpeed({27000,100},0x0264,s)==Status::Ok && s.referenceKHz==100000 && s.prescaler==500);
    CHECK(calculateSpeed({27000,400},0x0430,s)==Status::Ok && s.inputKHz==12000 && s.prescaler==30);
    CHECK(calculateSpeed({1,100},0,s)==Status::InvalidClock && !s.prescaler);
    CHECK(calculateSpeed({1000000,1},0,s)==Status::InvalidClock);
    CHECK(calculateSpeed({1000000,1},0x7f01,s)==Status::Ok && s.prescaler==7);
    CHECK(calculateSpeed({0,100},0,s)==Status::InvalidClock);
    CHECK(calculateSpeed({100000,0},0,s)==Status::InvalidClock);
    CHECK(calculateSpeed({100000,401},0,s)==Status::InvalidClock);
    CHECK(calculateSpeed(clock100,UINT32_MAX,s)==Status::InvalidClock);
}
void successTests(){for(uint8_t p=0;p<5;++p){
    Simulation s(p);Engine e;Result r{};const auto original=s.registers;
    CHECK(e.open(s.l,s.io(),p,clock100,r)==Status::Ok);CHECK(e.isOpen() && !e.needsRecovery());
    CHECK(s.get(Register::Speed)==0x01f40212);CHECK((s.get(Register::Setup)&0xff000054)==0x03000044);
    CHECK((s.get(Register::GpioMask)&0x11111)==0x1010);CHECK(!(s.get(Register::MemoryPowerStatus)&1));
    CHECK(s.delays==std::vector<uint32_t>({3000,2000}));s.delays.clear();
    s.busySamples=2;s.zeroSamples=1;
    CHECK(e.readEdidBlock(0,r)==Status::Ok && r.bytes==128 && r.submitted && r.completed);
    CHECK(r.polls==4 && s.delays==std::vector<uint32_t>({1,1,1}));
    for(size_t i=0;i<128;++i)CHECK(r.data[i]==static_cast<uint8_t>(i*7+3));
    CHECK(s.commands[0].size()==2);CHECK(s.commands[0][0].address==0x50 && s.commands[0][0].data==std::vector<uint8_t>({0}));
    CHECK(s.commands[0][1].address==0x50 && s.commands[0][1].read);
    CHECK(e.readEdidBlock(5,r)==Status::Ok && r.bytes==128);
    CHECK(s.commands[1].size()==3 && s.commands[1][0].address==0x30);
    CHECK(s.commands[1][0].data==std::vector<uint8_t>({2}) && s.commands[1][1].data==std::vector<uint8_t>({128}));
    s.reply={0x01,0x03};CHECK(e.readScdc(0x20,2,r)==Status::Ok && r.bytes==2 && r.data[0]==1 && r.data[1]==3);
    CHECK(s.commands[2][0].address==0x54 && s.commands[2][0].data==std::vector<uint8_t>({0x20}));
    const uint8_t config=3;CHECK(e.writeScdc(0x20,&config,1,r)==Status::Ok && r.bytes==0);
    CHECK(s.commands[3].size()==1 && s.commands[3][0].data==std::vector<uint8_t>({0x20,3}));
    CHECK(e.close(r)==Status::Ok && !e.ownsController());
    CHECK((s.get(Register::Arbitration)&0x10)==0x10);
    for(Register reg:{Register::Setup,Register::Speed,Register::GpioMask,Register::GpioAux,Register::PhyAux,Register::MemoryPowerControl})
        CHECK(s.get(reg)==original.at(s.reg(reg)));
    const size_t n=s.accesses.size();CHECK(e.close(r)==Status::Ok && s.accesses.size()==n);
}}
void commandTests(){Simulation s;Engine e;Result r{};CHECK(e.open(s.l,s.io(),0,clock100,r)==Status::Ok);
    uint8_t a=7,b=8,c=9;const Message four[]={{0x10,false,1,&a},{0x20,false,1,&b},{0x30,false,1,&c},{0x40,true,8,nullptr}};
    CHECK(e.transfer(four,4,r)==Status::Ok && r.bytes==8);CHECK(s.commands[0].size()==4);
    const Message probe{0x55,false,0,nullptr};CHECK(e.transfer(&probe,1,r)==Status::Ok);
    CHECK(s.commands[1][0].address==0x55 && s.commands[1][0].bytes==0);
    const Message full{0x50,true,537,nullptr};CHECK(e.transfer(&full,1,r)==Status::Ok && r.bytes==537);
    const size_t n=s.accesses.size();
    Message bad=full;bad.bytes=538;CHECK(e.transfer(&bad,1,r)==Status::InvalidArgument);
    bad={0x80,false,0,nullptr};CHECK(e.transfer(&bad,1,r)==Status::InvalidArgument);
    bad={0x50,false,1,nullptr};CHECK(e.transfer(&bad,1,r)==Status::InvalidArgument);
    bad={0x50,true,0,nullptr};CHECK(e.transfer(&bad,1,r)==Status::InvalidArgument);
    Message readFirst[]={{0x50,true,1,nullptr},{0x54,false,1,&a}};
    CHECK(e.transfer(readFirst,2,r)==Status::InvalidArgument);
    CHECK(e.transfer(nullptr,1,r)==Status::InvalidArgument);CHECK(e.transfer(four,5,r)==Status::InvalidArgument);
    CHECK(e.readScdc(255,2,r)==Status::InvalidArgument);CHECK(e.writeScdc(255,&a,2,r)==Status::InvalidArgument);
    CHECK(s.accesses.size()==n);CHECK(e.close(r)==Status::Ok);
}
void statusTests(){struct Case{uint32_t bits;Status status;};
    const Case cases[]={{0x100,Status::Nack},{0x20,Status::WireTimeout},{0x10,Status::Aborted},
        {0x40,Status::Interrupted},{0x80,Status::BufferOverflow},{0x104,Status::Nack}};
    for(const auto&c:cases){Simulation s;Engine e;Result r{};CHECK(e.open(s.l,s.io(),0,clock100,r)==Status::Ok);
        s.replyStatus=c.bits;CHECK(e.readScdc(0,1,r)==c.status && r.completed && r.bytes==0);
        CHECK(e.close(r)==Status::Ok);}
    Simulation s;Engine e;Result r{};CHECK(e.open(s.l,s.io(),0,{100000,400},r)==Status::Ok);
    s.noDone=true;CHECK(e.readScdc(0,1,r)==Status::CompletionTimeout);CHECK(e.needsRecovery() && !r.completed && r.submitted);
    CHECK(r.polls==r.timeoutUs && r.timeoutUs==3040); // idle0 is never treated as success
    Simulation d;Engine de;CHECK(de.open(d.l,d.io(),0,clock100,r)==Status::Ok);
    d.loseGrant=true;CHECK(de.readScdc(0,1,r)==Status::OwnershipLost && de.needsRecovery());
    Simulation busy;Engine be;busy.set(Register::Arbitration,4);
    CHECK(be.open(busy.l,busy.io(),0,clock100,r)==Status::Busy && !be.ownsController());
    for(const Access&a:busy.accesses)CHECK(!a.write);
    Simulation power;Engine pe;power.powerStuck=true;
    CHECK(pe.open(power.l,power.io(),0,clock100,r)==Status::PowerTimeout && pe.needsRecovery());CHECK(r.polls==5);
    Simulation clock;Engine ce;CHECK(ce.open(clock.l,clock.io(),0,clock100,r)==Status::Ok);
    clock.set(Register::TimeBase,0x0264);CHECK(ce.readScdc(0,1,r)==Status::InvalidClock && ce.needsRecovery());
    Simulation reset;Engine re;CHECK(re.open(reset.l,reset.io(),0,clock100,r)==Status::Ok);
    reset.resetStuck=true;reset.set(Register::Status,4);CHECK(re.readScdc(0,1,r)==Status::VerifyFailed && !r.submitted);
    for(unsigned stuck:{1U,2U}){Simulation a;Engine ae;a.stuckRelease=stuck;
        CHECK(ae.open(a.l,a.io(),0,clock100,r)==Status::Ok);
        CHECK(ae.close(r)==(stuck==1?Status::Ok:Status::VerifyFailed));CHECK(a.releaseCalls==2);
        CHECK(ae.needsRecovery()==(stuck==2));}
}
void faultTests(){Simulation reference;Engine re;Result r{};CHECK(re.open(reference.l,reference.io(),0,clock100,r)==Status::Ok);
    const size_t openCount=reference.accesses.size();CHECK(re.readScdc(0x20,4,r)==Status::Ok);
    const size_t transferCount=reference.accesses.size()-openCount;CHECK(re.close(r)==Status::Ok);
    const size_t closeCount=reference.accesses.size()-openCount-transferCount;
    for(size_t stage=0;stage<3;++stage)for(size_t step=1;step<=(stage==0?openCount:stage==1?transferCount:closeCount);++step){
        Simulation s;Engine e;
        if(stage)CHECK(e.open(s.l,s.io(),0,clock100,r)==Status::Ok);
        if(stage==2)CHECK(e.readScdc(0x20,4,r)==Status::Ok);
        s.failAt=s.accesses.size()+step;
        const Status status=stage==0?e.open(s.l,s.io(),0,clock100,r):stage==1?e.readScdc(0x20,4,r):e.close(r);
        CHECK(status==Status::TransportError && e.needsRecovery());const size_t n=s.accesses.size();
        CHECK(e.readScdc(0,1,r)==Status::Faulted);CHECK(e.close(r)==Status::Faulted);CHECK(s.accesses.size()==n);
        e.clearAfterVerifiedReset();CHECK(!e.ownsController());
    }
    for(size_t i=0;i<openCount+transferCount;++i)if(!reference.accesses[i].write){
        Simulation s;Engine e;
        if(i>=openCount)CHECK(e.open(s.l,s.io(),0,clock100,r)==Status::Ok);
        s.onesAt=i+1;
        CHECK((i<openCount?e.open(s.l,s.io(),0,clock100,r):e.readScdc(0x20,4,r))==Status::DeviceUnavailable);
        CHECK(e.needsRecovery() && s.accesses.size()==i+1);
    }
    // Silently dropped pin/prescale writes cannot publish an open controller.
    for(size_t i=0;i<openCount;++i)if(reference.accesses[i].write &&
        (reference.accesses[i].address==reference.reg(Register::Speed) ||
         reference.accesses[i].address==reference.reg(Register::GpioAux))){
        Simulation s;Engine e;s.dropAt=i+1;CHECK(e.open(s.l,s.io(),0,clock100,r)==Status::VerifyFailed);CHECK(e.needsRecovery());}
}
void realDiscovery(const char* path){std::ifstream f(path,std::ios::binary);std::vector<uint8_t>b((std::istreambuf_iterator<char>(f)),{});
    CHECK(!b.empty());Layout l{};CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::Ok);
    CHECK(l.registers[0][11]==0x4ec && l.registers[4][0]==0x14dac);
    Simulation s(4,l);Engine e;Result r{};CHECK(e.open(l,s.io(),4,clock100,r)==Status::Ok);
    CHECK(e.readEdidBlock(2,r)==Status::Ok && r.bytes==128);CHECK(e.close(r)==Status::Ok);}
}
int main(int argc,char**argv){layoutTests();clockTests();successTests();commandTests();statusTests();faultTests();
    if(argc==2)realDiscovery(argv[1]);std::cout<<checks<<" HDMI DDC checks, "<<failures<<" failures\n";return failures?1:0;}
