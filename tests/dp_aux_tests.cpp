// SPDX-License-Identifier: MIT
// MMIO peripheral simulation only. These tests do not access a GPU or sink.
#include "hackynavi/dp_aux.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
using namespace hackynavi;
using namespace hackynavi::dpaux;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr<<__LINE__<<": " #x "\n"; } } while (0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v) { b[p]=static_cast<uint8_t>(v);b[p+1]=static_cast<uint8_t>(v>>8); }
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v) { put16(b,p,static_cast<uint16_t>(v));put16(b,p+2,static_cast<uint16_t>(v>>16)); }
uint16_t sum(const std::vector<uint8_t>& b,size_t p) { uint16_t s=0;for(;p<b.size();++p)s=static_cast<uint16_t>(s+b[p]);return s; }
void fix(std::vector<uint8_t>& b) { put16(b,14,sum(b,60));put16(b,8,sum(b,10)); }
std::vector<uint8_t> fixture() {
    std::vector<uint8_t> b(164,0);
    put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,164);
    put16(b,12,60);put16(b,16,104);put32(b,60,0x53445049);put16(b,64,3);
    put16(b,66,104);put16(b,72,1);put16(b,76,140);put16(b,142,1);
    put16(b,144,271);b[147]=3;b[148]=3;b[149]=2;put32(b,160,0x34c0);fix(b);return b;
}
Layout layout() { const auto b=fixture();Layout l{};CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::Ok);return l; }
struct Reply {
    uint8_t code;
    std::vector<uint8_t> data;
    uint32_t flags;
    bool done;
    Reply(uint8_t c=0,std::vector<uint8_t> d={},uint32_t f=0,bool completed=true):code(c),data(d),flags(f),done(completed){}
};
struct Access { bool write; uint32_t reg,value; };
struct Simulation {
    Layout l;uint8_t engine;
    std::map<uint32_t,uint32_t> registers;
    std::vector<Access> accesses;
    std::vector<uint32_t> delays;
    std::vector<uint8_t> tx,rx;
    std::vector<std::vector<uint8_t>> packets;
    std::vector<Reply> replies;
    size_t rxIndex=0,failAt=0,onesAt=0,dropAt=0;
    bool resetStuck=false,doneStuck=false,denyGrant=false,loseAtGo=false,clearRequest=false;
    bool rxCountOverride=false;uint8_t rxCount=0;
    bool preparingReply=false;unsigned preparationReads=0;
    explicit Simulation(uint8_t e=0,const Layout& selected=layout()):l(selected),engine(e) {
        for(size_t r=0;r<kRegisterCount;++r) registers[l.registers[e][r]]=0;
        set(Register::GpioMask,0x40000111);set(Register::GpioAux,0x0207f155);
        set(Register::Control,0x20010000); // disconnected-HPD bypass is cleared
        set(Register::RxControl1,0x40008177);
        set(Register::Interrupt,0x6666); // unrelated W1C ACKs must not echo
    }
    uint32_t reg(Register r) const {return l.registers[engine][static_cast<size_t>(r)];}
    void set(Register r,uint32_t v) {registers[reg(r)]=v;}
    uint32_t get(Register r) const {return registers.at(reg(r));}
    Io io() {return {this,read,write,delay};}
    static bool read(void* p,uint32_t a,uint32_t& v) {
        Simulation& s=*static_cast<Simulation*>(p);
        CHECK(s.registers.count(a)==1);v=s.registers[a];
        if(a==s.reg(Register::Data) && !(v&1) && s.preparingReply)
            CHECK(++s.preparationReads==1); // REG_UPDATE_SEQ_3: one read only.
        if(a==s.reg(Register::Data) && (v&1)) {
            // RMW switching the old receive window back to transmit mode can
            // sample DATA after the previous FIFO reply has been consumed.
            // This is not an additional valid wire byte.
            v=(v&~0xff00U)|(static_cast<uint32_t>(s.rxIndex<s.rx.size()?s.rx[s.rxIndex++]:0)<<8);
        }
        s.accesses.push_back({false,a,v});
        if(s.accesses.size()==s.onesAt)v=UINT32_MAX;
        return s.accesses.size()!=s.failAt;
    }
    static bool write(void* p,uint32_t a,uint32_t v) {
        Simulation& s=*static_cast<Simulation*>(p);
        CHECK(s.registers.count(a)==1);s.accesses.push_back({true,a,v});
        if(s.accesses.size()==s.dropAt)return true;
        const uint32_t old=s.registers[a];s.registers[a]=v;
        if(a==s.reg(Register::Control)) {
            if(!s.resetStuck)s.registers[a]=(v&~0x20U)|((v&0x10)?0x20U:0);
        } else if(a==s.reg(Register::Arbitration)) {
            if(v&0x20000)s.registers[a]&=~0x3000cU;
            else if(v&0x10000) {
                s.registers[a]=(v&~0xcU)|(s.denyGrant?8U:4U);
                if(s.clearRequest)s.registers[a]&=~0x10000U;
            }
        } else if(a==s.reg(Register::Interrupt)) {
            CHECK(!(v&0x2220)); // LS/GTC ACKs are not our interrupt ownership.
            if((v&2)&&!s.doneStuck)s.set(Register::Status,0);
            s.preparingReply=false;
        } else if(a==s.reg(Register::Data)) {
            if(!(v&1)) {
                if(v&0x80000000)s.tx.clear();
                s.tx.push_back(static_cast<uint8_t>(v>>8));
            } else if(!(old&1)){s.rxIndex=0;s.preparingReply=false;}
        } else if(a==s.reg(Register::SwControl) && (v&1)) {
            const size_t length=(v>>16)&31;
            CHECK(s.tx.size()==length);
            s.packets.push_back(s.tx);
            const size_t index=s.packets.size()-1;
            Reply r=index<s.replies.size()?s.replies[index]:Reply();
            s.rx.clear();s.rx.push_back(static_cast<uint8_t>(r.code<<4));
            s.rx.insert(s.rx.end(),r.data.begin(),r.data.end());s.rxIndex=0;
            s.preparingReply=true;s.preparationReads=0;
            s.set(Register::Status,r.flags|(r.done?1U:0)|
                static_cast<uint32_t>((s.rxCountOverride?s.rxCount:s.rx.size())<<24));
            if(r.done)s.registers[a]&=~1U;
            if(s.loseAtGo)s.set(Register::Arbitration,8);
        }
        return s.accesses.size()!=s.failAt; // Ambiguous writes actually execute.
    }
    static void delay(void* p,uint32_t us) {static_cast<Simulation*>(p)->delays.push_back(us);}
};
Request readRequest(uint32_t address=0x12345,uint8_t length=4) {return {Operation::NativeRead,address,length,false,nullptr};}
void layoutTests() {
    auto b=fixture();Layout l=layout();
    CHECK(l.registers[0][0]==0x15040);CHECK(l.registers[4][0]==0x15200);
    CHECK(l.registers[0][9]==0x17640);CHECK(l.registers[4][9]==0x17680);
    CHECK(l.registers[0][10]==0x17774 && l.registers[4][10]==0x17774);
    for(size_t n=0;n<b.size();++n) {Layout out{};CHECK(buildLayout(b.data(),n,0x100000,out)==Status::DiscoveryRejected);CHECK(!out.valid);}
    CHECK(buildLayout(b.data(),b.size(),0x17777,l)==Status::OutsideBar);CHECK(!l.valid);
    CHECK(buildLayout(b.data(),b.size(),0x17778,l)==Status::Ok);
    b[148]=4;fix(b);CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::UnsupportedDmu);
    b=fixture();put16(b,144,42);fix(b);CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::MissingDmu);
    b=fixture();put32(b,160,0xffffffff);fix(b);CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::OutsideBar);
}
void successTests() {
    for(uint8_t e=0;e<5;++e) {
        Simulation s(e);Engine engine;Result r{};
        const uint32_t oldMask=s.get(Register::GpioMask),oldAux=s.get(Register::GpioAux);
        CHECK(engine.open(s.l,s.io(),e,static_cast<uint8_t>(4-e),r)==Status::Ok);
        CHECK(engine.isOpen() && engine.ownsPins() && !engine.needsRecovery());
        CHECK((s.get(Register::GpioMask)&0x10101)==0x10000);
        CHECK((s.get(Register::GpioAux)&(1U<<(12+e)))==0);
        CHECK(s.get(Register::RxControl0)==0x103d1010);
        CHECK(s.get(Register::TxControl)==0x21c7a);
        CHECK(s.get(Register::RxControl1)==0x40004577);
        CHECK((s.get(Register::Control)&0x710131)==(static_cast<uint32_t>(4-e)<<20|1U));
        s.replies.push_back(Reply(0,{0xff,0x01,0x80,0x57}));
        CHECK(engine.transfer(readRequest(),r)==Status::Ok);
        CHECK(r.submitted && r.completed && r.reply==0 && r.length==4);
        CHECK(r.data[0]==0xff && r.data[1]==1 && r.data[2]==0x80 && r.data[3]==0x57);
        CHECK(s.packets[0]==std::vector<uint8_t>({0x91,0x23,0x45,3}));
        std::vector<uint8_t> data(16);for(size_t i=0;i<16;++i)data[i]=static_cast<uint8_t>(i*11);
        const Request write{Operation::NativeWrite,0x100,16,false,data.data()};
        CHECK(engine.transfer(write,r)==Status::Ok);CHECK(r.length==0);
        std::vector<uint8_t> expected{0x80,1,0,15};expected.insert(expected.end(),data.begin(),data.end());
        CHECK(s.packets[1]==expected);
        CHECK(engine.close(r)==Status::Ok);CHECK(!engine.ownsPins());
        CHECK(s.get(Register::GpioMask)==oldMask);CHECK(s.get(Register::GpioAux)==oldAux);
        CHECK(!(s.get(Register::Arbitration)&0x10000));
        const size_t n=s.accesses.size();CHECK(engine.close(r)==Status::Ok);CHECK(s.accesses.size()==n);
    }
    Simulation s;Engine e;Result r{};s.clearRequest=true;
    CHECK(e.open(s.l,s.io(),0,0,r)==Status::Ok);
    s.replies={Reply(0,{7})};CHECK(e.transfer(readRequest(0,1),r)==Status::Ok);
    CHECK(r.data[0]==7 && e.close(r)==Status::Ok);
}
void i2cTests() {
    Simulation s;Engine e;Result r{};CHECK(e.open(s.l,s.io(),0,0,r)==Status::Ok);
    const uint8_t offset=0x80;
    const Request write{Operation::I2cWrite,0x50,1,true,&offset};
    CHECK(e.transfer(write,r)==Status::Ok);
    CHECK(s.packets[0]==std::vector<uint8_t>({0x40,0,0x50,0,0x80}));
    const size_t n=s.accesses.size();CHECK(e.transfer(readRequest(),r)==Status::Busy);CHECK(s.accesses.size()==n);
    s.replies.push_back(Reply());s.replies.push_back(Reply(0,{0x12,0x34}));
    const Request read{Operation::I2cRead,0x50,2,true,nullptr};
    CHECK(e.transfer(read,r)==Status::Ok);CHECK(r.data[0]==0x12 && r.data[1]==0x34);
    CHECK(s.packets[1]==std::vector<uint8_t>({0x50,0,0x50,1}));
    CHECK(e.close(r)==Status::Ok);CHECK(s.packets.size()==3);
    CHECK(s.packets[2]==std::vector<uint8_t>({0,0,0x50})); // Address-only STOP

    Simulation a;Engine ae;CHECK(ae.open(a.l,a.io(),0,1,r)==Status::Ok);
    a.replies={Reply(0,{0}),Reply(8),Reply()};
    CHECK(ae.transferWithRetries(write,r)==Status::Ok);CHECK(r.attempts==3);
    CHECK(a.packets[1]==std::vector<uint8_t>({0x60,0,0x50}));
    CHECK(a.packets[2]==a.packets[1]);CHECK(a.delays.size()==2);
    CHECK(ae.close(r)==Status::Ok);

    Simulation d;Engine de;CHECK(de.open(d.l,d.io(),0,1,r)==Status::Ok);
    d.replies={Reply(8),Reply()};CHECK(de.transferWithRetries(write,r)==Status::Ok);
    CHECK(d.packets[1]==std::vector<uint8_t>({0x60,0,0x50}));CHECK(de.close(r)==Status::Ok);
}
void replyTests() {
    struct Case {uint8_t reply;uint32_t flags;Status status;};
    const Case cases[]={{1,0,Status::Nack},{2,0,Status::Deferred},{3,0,Status::InvalidReply},
        {4,0,Status::InvalidReply},{8,0,Status::InvalidReply},{0,0x200,Status::HpdDisconnected},
        {0,0x10,Status::ReplyTimeout},{0,0x80,Status::ReplyTimeout},
        {0,0x100,Status::InvalidReply},{0,0x400,Status::InvalidReply},
        {0,0x4000,Status::InvalidReply},{0,0x100000,Status::InvalidReply},
        {0,0x400000,Status::InvalidReply},{0,0x800000,Status::InvalidReply},
        {0,0x800,Status::InvalidReply},{0,0x20000,Status::InvalidReply},
        {0,0x40000,Status::InvalidReply},{0,0x80000,Status::InvalidReply}};
    for(const auto& c:cases) {
        Simulation s;Engine e;Result r{};CHECK(e.open(s.l,s.io(),0,0,r)==Status::Ok);
        s.replies={Reply(c.reply,{},c.flags)};CHECK(e.transfer(readRequest(),r)==c.status);
        CHECK(e.isOpen() && r.completed);CHECK(e.close(r)==Status::Ok);
    }
    Simulation s;Engine e;Result r{};CHECK(e.open(s.l,s.io(),0,0,r)==Status::Ok);
    s.replies={Reply(0,{1,2},0x1000),Reply(0,{1,2,3,4,5})};
    CHECK(e.transfer(readRequest(),r)==Status::ShortReply);CHECK(r.length==2 && r.data[1]==2);
    CHECK(e.transfer(readRequest(),r)==Status::InvalidReply);CHECK(r.length==0);
    CHECK(e.close(r)==Status::Ok);
    for(uint8_t n:{uint8_t(0),uint8_t(18),uint8_t(31)}) {
        Simulation c;Engine ce;CHECK(ce.open(c.l,c.io(),0,0,r)==Status::Ok);
        c.rxCountOverride=true;c.rxCount=n;
        CHECK(ce.transfer(readRequest(),r)==Status::InvalidReply);CHECK(ce.close(r)==Status::Ok);
    }
    // Full16-byte reply is legal; one-byte native-write ACKM is not.
    Simulation m;Engine me;CHECK(me.open(m.l,m.io(),0,0,r)==Status::Ok);
    std::vector<uint8_t> payload(16);for(size_t i=0;i<16;++i)payload[i]=static_cast<uint8_t>(i*17);
    m.replies={Reply(0,payload),Reply(0,{0})};
    CHECK(me.transfer(readRequest(0,16),r)==Status::Ok);CHECK(r.length==16);
    for(size_t i=0;i<16;++i)CHECK(r.data[i]==payload[i]);
    const Request wr{Operation::NativeWrite,0,16,false,payload.data()};
    CHECK(me.transfer(wr,r)==Status::InvalidReply);CHECK(me.close(r)==Status::Ok);
}
void retryTests() {
    Simulation s;Engine e;Result r{};CHECK(e.open(s.l,s.io(),0,0,r)==Status::Ok);
    s.replies={Reply(2),Reply(2),Reply(0,{1,2,3,4})};
    CHECK(e.transferWithRetries(readRequest(),r)==Status::Ok);CHECK(r.attempts==3);
    CHECK(s.packets[0]==s.packets[1] && s.packets[1]==s.packets[2]);
    CHECK(s.delays==std::vector<uint32_t>({300,300}));CHECK(e.close(r)==Status::Ok);
    Simulation d;Engine de;CHECK(de.open(d.l,d.io(),0,0,r)==Status::Ok);
    d.replies.assign(32,Reply(2));CHECK(de.transferWithRetries(readRequest(),r)==Status::RetryExhausted);
    CHECK(r.attempts==32 && d.packets.size()==32 && d.delays.size()==31);CHECK(de.close(r)==Status::Ok);
    Simulation t;Engine te;CHECK(te.open(t.l,t.io(),0,0,r)==Status::Ok);
    t.replies.assign(3,Reply(0,{},0x80));CHECK(te.transferWithRetries(readRequest(),r)==Status::ReplyTimeout);
    CHECK(r.attempts==3);CHECK(te.close(r)==Status::Ok);
    // Short I2C reads have consumed the sink pointer; do not replay them.
    Simulation p;Engine pe;CHECK(pe.open(p.l,p.io(),0,0,r)==Status::Ok);
    p.replies={Reply(0,{0x11}),Reply(0,{0x22,0x33})};
    const Request rd{Operation::I2cRead,0x50,2,false,nullptr};
    CHECK(pe.transferWithRetries(rd,r)==Status::ShortReply);CHECK(r.attempts==1 && r.data[0]==0x11);
    // Conservative ownership survives partial replies; actual STOP is required.
    p.replies[1]=Reply();CHECK(pe.close(r)==Status::Ok);CHECK(p.packets.size()==2);
}
void invalidTests() {
    Simulation s;Engine e;Result r{};
    CHECK(e.transfer(readRequest(),r)==Status::Closed);
    CHECK(e.open(s.l,s.io(),5,0,r)==Status::InvalidArgument);CHECK(s.accesses.empty());
    CHECK(e.open(s.l,s.io(),0,5,r)==Status::InvalidArgument);CHECK(s.accesses.empty());
    Layout damaged=s.l;damaged.registers[4][5]+=4;
    CHECK(e.open(damaged,s.io(),0,0,r)==Status::InvalidArgument);CHECK(s.accesses.empty());
    CHECK(e.open(s.l,s.io(),0,0,r)==Status::Ok);const size_t n=s.accesses.size();
    CHECK(e.open(s.l,s.io(),0,0,r)==Status::Busy);
    Request q=readRequest();q.length=0;CHECK(e.transfer(q,r)==Status::InvalidArgument);
    q=readRequest();q.length=17;CHECK(e.transfer(q,r)==Status::InvalidArgument);
    q=readRequest();q.mot=true;CHECK(e.transfer(q,r)==Status::InvalidArgument);
    q=readRequest(0xfffff,2);CHECK(e.transfer(q,r)==Status::InvalidArgument);
    q={Operation::I2cRead,0x80,1,false,nullptr};CHECK(e.transfer(q,r)==Status::InvalidArgument);
    q={Operation::NativeWrite,0,1,false,nullptr};CHECK(e.transfer(q,r)==Status::InvalidArgument);
    q={Operation::I2cWriteStatus,0x50,1,false,nullptr};CHECK(e.transfer(q,r)==Status::InvalidArgument);
    q.operation=static_cast<Operation>(0xff);CHECK(e.transfer(q,r)==Status::InvalidArgument);
    CHECK(s.accesses.size()==n);CHECK(e.close(r)==Status::Ok);
    for(uint32_t arb:{8U,12U,0x10004U}) {
        Simulation b;Engine be;b.set(Register::Arbitration,arb);
        CHECK(be.open(b.l,b.io(),0,0,r)==Status::Busy);CHECK(!be.ownsPins());
        for(const Access& a:b.accesses)CHECK(!a.write);
    }
    for(Register reg:{Register::Control,Register::SwControl,Register::Status}) {
        Simulation b;Engine be;b.set(reg,reg==Register::Control?0x10:reg==Register::SwControl?1:2);
        CHECK(be.open(b.l,b.io(),0,0,r)==Status::Busy);CHECK(!be.ownsPins());
        for(const Access& a:b.accesses)CHECK(!a.write);
    }
}
void faultTests() {
    Simulation reference;Engine re;Result r{};
    CHECK(re.open(reference.l,reference.io(),0,0,r)==Status::Ok);
    const size_t openCount=reference.accesses.size();
    reference.replies={Reply(0,{1,2,3,4})};
    CHECK(re.transfer(readRequest(),r)==Status::Ok);
    const size_t transferCount=reference.accesses.size()-openCount;
    CHECK(re.close(r)==Status::Ok);
    const size_t closeCount=reference.accesses.size()-openCount-transferCount;
    for(size_t stage=0;stage<3;++stage) for(size_t fail=1;fail<=(stage==0?openCount:stage==1?transferCount:closeCount);++fail) {
        Simulation s;Engine e;
        if(stage)CHECK(e.open(s.l,s.io(),0,0,r)==Status::Ok);
        if(stage==2){s.replies={Reply(0,{1,2,3,4})};CHECK(e.transfer(readRequest(),r)==Status::Ok);}
        s.failAt=s.accesses.size()+fail;s.replies={Reply(0,{1,2,3,4})};
        const Status st=stage==0?e.open(s.l,s.io(),0,0,r):stage==1?e.transfer(readRequest(),r):e.close(r);
        CHECK(st==Status::TransportError);CHECK(e.needsRecovery() && e.ownsPins());
        const size_t n=s.accesses.size();CHECK(e.transfer(readRequest(),r)==Status::Faulted);
        CHECK(e.close(r)==Status::Faulted);CHECK(s.accesses.size()==n);
        e.clearAfterVerifiedReset();CHECK(!e.ownsPins());
    }
    for(size_t i=0;i<openCount;++i) if(!reference.accesses[i].write) {
        Simulation s;Engine e;s.onesAt=i+1;
        CHECK(e.open(s.l,s.io(),0,0,r)==Status::DeviceUnavailable);CHECK(e.needsRecovery());
    }
    // Every receive-path read returning an unavailable device faults without
    // another MMIO operation. No data from such a reply is reported successful.
    for(size_t i=openCount;i<openCount+transferCount;++i) if(!reference.accesses[i].write) {
        Simulation s;Engine e;CHECK(e.open(s.l,s.io(),0,0,r)==Status::Ok);
        s.onesAt=i+1;s.replies={Reply(0,{1,2,3,4})};
        CHECK(e.transfer(readRequest(),r)==Status::DeviceUnavailable);CHECK(e.needsRecovery());
        CHECK(s.accesses.size()==i+1);
    }
    // Silent GPIO programming loss is detected before the engine is published.
    for(size_t i=0;i<openCount;++i) if(reference.accesses[i].write &&
        (reference.accesses[i].reg==reference.reg(Register::GpioMask) ||
         reference.accesses[i].reg==reference.reg(Register::GpioAux))) {
        Simulation s;Engine e;s.dropAt=i+1;
        CHECK(e.open(s.l,s.io(),0,0,r)==Status::VerifyFailed);CHECK(e.needsRecovery());
    }
    Simulation reset;Engine rst;reset.resetStuck=true;
    CHECK(rst.open(reset.l,reset.io(),0,0,r)==Status::ResetTimeout);
    CHECK(rst.needsRecovery() && r.polls==11 && reset.delays.size()==11);
    Simulation busy;Engine b;busy.denyGrant=true;
    CHECK(b.open(busy.l,busy.io(),0,0,r)==Status::OwnershipLost);CHECK(b.needsRecovery());
    Simulation timeout;Engine t;CHECK(t.open(timeout.l,timeout.io(),0,0,r)==Status::Ok);
    timeout.replies={Reply(0,{},0,false)};
    CHECK(t.transfer(readRequest(),r)==Status::CompletionTimeout);CHECK(t.needsRecovery());
    CHECK(r.submitted && !r.completed && r.polls==241 && timeout.delays.size()==240);
    Simulation ack;Engine a;CHECK(a.open(ack.l,ack.io(),0,0,r)==Status::Ok);
    ack.doneStuck=true;ack.set(Register::Status,1);
    CHECK(a.transfer(readRequest(),r)==Status::CompletionTimeout);CHECK(!r.submitted && ack.packets.empty());
    Simulation lost;Engine le;CHECK(le.open(lost.l,lost.io(),0,0,r)==Status::Ok);
    lost.loseAtGo=true;CHECK(le.transfer(readRequest(),r)==Status::OwnershipLost);
    CHECK(le.needsRecovery() && r.submitted && r.completed);
    // A request that auto-cleared on grant is not proof of release. Dropping
    // DONE_USING must be detected by the still-SW grant status.
    Simulation release;Engine qe;release.clearRequest=true;
    CHECK(qe.open(release.l,release.io(),0,0,r)==Status::Ok);
    // close: owned read, GPIO R/W/R twice, arbitration R/W then verification.
    release.dropAt=release.accesses.size()+9;
    CHECK(qe.close(r)==Status::VerifyFailed);CHECK(qe.needsRecovery());
}
void realDiscovery(const char* path) {
    std::ifstream f(path,std::ios::binary);std::vector<uint8_t>b((std::istreambuf_iterator<char>(f)),{});
    CHECK(!b.empty());Layout l{};CHECK(buildLayout(b.data(),b.size(),0x100000,l)==Status::Ok);
    CHECK(l.registers[0][0]==0x15040 && l.registers[4][10]==0x17774);
    Simulation s(3,l);Engine e;Result r{};CHECK(e.open(l,s.io(),3,1,r)==Status::Ok);
    s.replies={Reply(0,{0x14})};CHECK(e.transfer(readRequest(0,1),r)==Status::Ok);
    CHECK(r.data[0]==0x14);CHECK(e.close(r)==Status::Ok);
}
}
int main(int argc,char** argv) {
    layoutTests();successTests();i2cTests();replyTests();retryTests();invalidTests();faultTests();
    if(argc==2)realDiscovery(argv[1]);
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
