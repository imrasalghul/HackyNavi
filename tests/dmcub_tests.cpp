// SPDX-License-Identifier: MIT
// Offline MMIO/firmware peripheral simulation; no physical GPU is exercised.
#include "hackynavi/dmcub.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
namespace d=hackynavi::dmcub;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; std::cerr<<__LINE__<<": " #x "\n"; } } while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v) { b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8); }
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v) { put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16)); }
uint16_t get16(const std::vector<uint8_t>& b,size_t p) {return uint16_t(b[p])|(uint16_t(b[p+1])<<8);}
uint32_t get32(const std::vector<uint8_t>& b,size_t p) {return uint32_t(get16(b,p))|(uint32_t(get16(b,p+2))<<16);}
uint16_t sum(const std::vector<uint8_t>& b,size_t p) { uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s; }
void fix(std::vector<uint8_t>& b) {put16(b,14,sum(b,60));put16(b,8,sum(b,10));}
std::vector<uint8_t> discovery() {
    std::vector<uint8_t> b(164,0);
    put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,164);
    put16(b,12,60);put16(b,16,104);put32(b,60,0x53445049);put16(b,64,3);
    put16(b,66,104);put16(b,72,1);put16(b,76,140);put16(b,142,1);
    put16(b,144,271);b[147]=3;b[148]=3;b[149]=2;put32(b,160,0x34c0);fix(b);return b;
}
d::Layout layout() {const auto b=discovery();d::Layout l{};CHECK(d::buildLayout(b.data(),b.size(),0x100000,l)==d::Status::Ok);return l;}
void table(std::vector<uint8_t>& b,size_t p,uint16_t size,uint8_t major,uint8_t minor) {
    put16(b,p,size);b[p+2]=major;b[p+3]=minor;
}
std::vector<uint8_t> rom() {
    std::vector<uint8_t> b(4096,0);b[0]=0x55;b[1]=0xaa;b[2]=8;
    put16(b,0x18,0x80);std::memcpy(b.data()+0x80,"PCIR",4);
    put16(b,0x84,0x1002);put16(b,0x86,0x744c);put16(b,0x8a,24);put16(b,0x90,8);b[0x95]=0x80;
    put16(b,0x48,0x100);table(b,0x100,44,2,3);std::memcpy(b.data()+0x104,"ATOM",4);
    put16(b,0x11e,0x190);put16(b,0x120,0x140);table(b,0x190,8,2,1);table(b,0x140,74,2,1);
    put16(b,0x144+12*2,0x200);put16(b,0x144+22*2,0x300);
    table(b,0x200,84,2,1);
    const uint8_t shifts[]={0,8,16,24,26};
    for(size_t i=0;i<5;++i) {
        const size_t h=0x204+i*8,c=0x22c+i*8;
        put32(b,h,0x5db5);b[h+4]=shifts[i];b[h+6]=uint8_t(i+1);
        put32(b,c,0x5d91+uint32_t(i)*4);b[c+4]=8;b[c+6]=uint8_t(0x90+i);
    }
    table(b,0x300,112,1,5);b[0x306]=4;
    const uint16_t ids[]={0x3113,0x3213,0x3313,0x340c}, enc[]={0x211e,0x2121,0x221e,0x2220};
    const uint8_t mux[]={3,4,2,0};
    for(size_t i=0;i<4;++i) {
        const size_t p=0x308+i*16,r=0x348+i*10;
        put16(b,p,ids[i]);put16(b,p+2,uint16_t(72+i*10));put16(b,p+4,enc[i]);put16(b,p+12,uint16_t(1U<<i));
        b[r]=1;b[r+1]=4;b[r+2]=uint8_t(0x90+mux[i]);b[r+4]=2;b[r+5]=4;b[r+6]=mux[i]+1;b[r+7]=1;b[r+8]=0xff;
    }
    return b;
}
hackynavi::ImageInfo image(const std::vector<uint8_t>& b) {
    hackynavi::ImageInfo i{};CHECK(hackynavi::parseImage(b.data(),b.size(),0,i)==hackynavi::Status::Ok);return i;
}
d::Route route(unsigned n=3) {
    auto b=rom();const auto i=image(b);d::Route r{};
    CHECK(d::parseRoute(b.data(),b.size(),i,layout(),uint8_t(n),r)==d::Status::Ok);return r;
}
constexpr uint64_t mailbox=UINT64_C(0x1230040000);
const d::PollOptions polls={4,10};
struct Device {
    d::Layout l=layout();std::map<uint32_t,uint32_t> regs;
    std::vector<uint8_t> memory=std::vector<uint8_t>(d::kMailboxBytes,0),last;
    std::vector<char> operations;
    size_t accesses=0,failAt=0,onesAt=0,writes=0,delays=0,vrWrites=0,vrReads=0,flushes=0;
    bool stall=false,dropPublish=false,rejectWrite=false,rejectRead=false,rejectFlush=false,corrupt=false;
    bool changeMap=false,changePointer=false,badRptr=false;
    uint32_t pendingTarget=0;
    explicit Device(uint32_t initial=0) {
        for(size_t i=0;i<d::kRegisterCount;++i)regs[l.registers[i]]=0;
        set(d::Register::Control,0x10000);set(d::Register::BootStatus,3);set(d::Register::AxiSpace,3);
        set(d::Register::Cw4Base,0x04000000);set(d::Register::Cw4Top,0x84004000);
        set(d::Register::Cw4Offset,uint32_t(mailbox));set(d::Register::Cw4OffsetHigh,uint32_t(mailbox>>32));
        set(d::Register::InboxBase,0x64000000);set(d::Register::InboxSize,d::kInboxBytes);
        set(d::Register::Wptr,initial);set(d::Register::Rptr,initial);
    }
    uint32_t reg(d::Register r) const {return l.registers[static_cast<size_t>(r)];}
    void set(d::Register r,uint32_t v) {regs[reg(r)]=v;}
    uint32_t get(d::Register r) const {return regs.at(reg(r));}
    d::Io io() {return {this,read,write,readMemory,writeMemory,flush,delay};}
    static bool read(void* p,uint32_t r,uint32_t& v) {
        auto& s=*static_cast<Device*>(p);CHECK(s.regs.count(r)==1);v=s.regs[r];++s.accesses;s.operations.push_back('r');
        if(s.accesses==s.onesAt)v=UINT32_MAX;
        return s.accesses!=s.failAt;
    }
    static bool write(void* p,uint32_t r,uint32_t v) {
        auto& s=*static_cast<Device*>(p);CHECK(r==s.reg(d::Register::Wptr));++s.accesses;++s.writes;
        CHECK(s.vrWrites==s.vrReads && s.flushes==s.vrWrites);s.operations.push_back('w');
        const uint32_t old=s.get(d::Register::Wptr);CHECK(v==(old+64)%d::kInboxBytes);
        s.last.assign(s.memory.begin()+old,s.memory.begin()+old+64);
        // Simulated firmware consumes only the VRAM command selected by WPTR.
        if(s.last[0]==76) {
            CHECK(s.last[1]==0 && s.last[2]==0 && s.last[3]==52);
            for(size_t i=4;i<64;++i)CHECK(s.last[i]==0);
        } else {
            CHECK(s.last[0]==0x80 && !s.last[2] && (s.last[1]<=2));
            CHECK(s.last[3]==(s.last[1]==0?12:s.last[1]==1?60:16));
        }
        for(size_t i=4+s.last[3];i<64;++i)CHECK(s.last[i]==0);
        if(!s.dropPublish)s.regs[r]=v;
        s.pendingTarget=v;
        if(!s.stall && !s.dropPublish)s.set(d::Register::Rptr,s.badRptr?(v+64)%d::kInboxBytes:v);
        return s.accesses!=s.failAt;
    }
    static bool writeMemory(void* p,uint64_t a,const uint8_t* b,size_t n) {
        auto& s=*static_cast<Device*>(p);++s.vrWrites;s.operations.push_back('V');
        CHECK(n==64 && a==mailbox+s.get(d::Register::Wptr));
        std::memcpy(s.memory.data()+size_t(a-mailbox),b,n);return !s.rejectWrite;
    }
    static bool readMemory(void* p,uint64_t a,uint8_t* b,size_t n) {
        auto& s=*static_cast<Device*>(p);++s.vrReads;s.operations.push_back('R');CHECK(s.flushes==s.vrWrites);
        CHECK(n==64 && a==mailbox+s.get(d::Register::Wptr));
        std::memcpy(b,s.memory.data()+size_t(a-mailbox),n);if(s.corrupt)b[63]^=1;return !s.rejectRead;
    }
    static bool flush(void* p) {
        auto& s=*static_cast<Device*>(p);++s.flushes;s.operations.push_back('F');
        if(s.changeMap)s.set(d::Register::Cw4Offset,uint32_t(mailbox)+256);
        // Applied after readback by delay-free read hook in the external test.
        if(s.changePointer)s.set(d::Register::Rptr,64);
        return !s.rejectFlush;
    }
    static void delay(void* p,uint32_t us) {auto& s=*static_cast<Device*>(p);CHECK(us==10);++s.delays;}
    d::Status open(d::Inbox& q,d::Result& r) {return q.open(l,io(),mailbox,r);}
    d::Status enable(d::Inbox& q,d::Result& r) {return q.transmitter(route(),{d::Action::Enable,4,148500},polls,r);}
};
void layoutAndRoutes() {
    const auto b=discovery();d::Layout l{};CHECK(d::buildLayout(b.data(),b.size(),0x100000,l)==d::Status::Ok);
    CHECK(l.registers[0]==0xdad8);CHECK(l.registers[size_t(d::Register::Wptr)]==0xda58);
    for(size_t n=0;n<b.size();++n)CHECK(d::buildLayout(b.data(),n,0x100000,l)!=d::Status::Ok);
    auto bad=b;bad[149]=1;fix(bad);CHECK(d::buildLayout(bad.data(),bad.size(),0x100000,l)==d::Status::UnsupportedDmu);
    CHECK(d::buildLayout(b.data(),b.size(),16,l)==d::Status::OutsideBar);
    auto rb=rom();auto img=image(rb);l=layout();d::Route r{};
    const uint8_t phy[]={0,4,1,3},gpio[]={3,4,2,0};
    for(uint8_t i=0;i<4;++i) {
        CHECK(d::parseRoute(rb.data(),rb.size(),img,l,i,r)==d::Status::Ok);
        CHECK(r.phy==phy[i] && r.ddc==gpio[i] && r.hpd==gpio[i] && r.hpdActiveHigh);
    }
    CHECK(d::parseRoute(rb.data(),rb.size(),img,l,4,r)==d::Status::InvalidArgument);
    for(size_t pos : {size_t(0x308+6),size_t(0x348+3),size_t(0x348+6),size_t(0x348+7),size_t(0x204+3*8+4),size_t(0x22c+3*8)}) {
        auto damaged=rb;damaged[pos]^=0x40;
        CHECK(d::parseRoute(damaged.data(),damaged.size(),img,l,0,r)!=d::Status::Ok);
        CHECK(!r.connectorObject && !r.encoderObject);
    }
    auto swapped=rb;swapped[0x348+6]=1;
    CHECK(d::parseRoute(swapped.data(),swapped.size(),img,l,0,r)==d::Status::Ok);
    CHECK(r.phy==0 && r.ddc==3 && r.hpd==0); // mapping must be independent
    auto dup=rb;dup[0x20c+6]=1;CHECK(d::parseRoute(dup.data(),dup.size(),img,l,3,r)==d::Status::UnsupportedRoute);
    for(size_t n=0;n<rb.size();n+=17)CHECK(d::parseRoute(rb.data(),n,img,l,0,r)!=d::Status::Ok);
}
void commands() {
    Device s;d::Inbox q;d::Result r{};CHECK(s.open(q,r)==d::Status::Ok);CHECK(s.writes==0);
    CHECK(s.enable(q,r)==d::Status::Ok);CHECK(r.slotWritten && r.submitted && r.consumed && r.polls==1);
    CHECK(get32(s.last,0)==0x3c000180 && get32(s.last,4)==0x04030103);
    CHECK(get32(s.last,8)==14850 && get32(s.last,12)==0xf60c0001);
    for(size_t i=16;i<64;++i)CHECK(s.last[i]==0);
    CHECK(q.transmitter(route(),{d::Action::Disable,0,0},polls,r)==d::Status::Ok);
    CHECK(s.last[5]==0 && s.last[7]==0 && get32(s.last,8)==0);
    CHECK(q.transmitter(route(1),{d::Action::Enable,2,810000},polls,r)==d::Status::Ok);
    CHECK(get32(s.last,4)==0x02000104 && get32(s.last,8)==81000 && get32(s.last,12)==0xf4130005);
    CHECK(q.pixelClock(route(),{1485000,23,2,0,1},polls,r)==d::Status::Ok);
    CHECK(get32(s.last,0)==0x10000280 && get32(s.last,4)==1485000 && get32(s.last,8)==0x01032017 && get32(s.last,12)==2);
    CHECK(q.setupEncoder(route(),{3,4,10,148500},polls,r)==d::Status::Ok);
    CHECK(get32(s.last,0)==0x0c000080 && get32(s.last,4)==0x04030f03 && get32(s.last,8)==18562 && s.last[12]==3);
    CHECK(q.setupEncoder(route(0),{0,4,12,148500},polls,r)==d::Status::Ok);
    CHECK(get32(s.last,4)==0x04000f00 && get32(s.last,8)==14850 && s.last[12]==4);
    CHECK(q.transmitter(route(),{d::Action::Enable,4,148351},polls,r)==d::Status::Ok);
    CHECK(get32(s.last,8)==14835); // fractional-refresh modes use ABI truncation
    CHECK(q.dpDrive(route(0),{4,1,2,3,540000},polls,r)==d::Status::Ok);
    CHECK(get32(s.last,4)==0x04d10b00 && get32(s.last,8)==54000 && get32(s.last,12)==0xf6130004);
    for(unsigned i=0;i<260;++i)CHECK(s.enable(q,r)==d::Status::Ok);
    CHECK(s.get(d::Register::Wptr)==(268*64)%d::kInboxBytes);
    CHECK(q.close(r)==d::Status::Ok);CHECK(!q.isOpen() && !q.pending());
    CHECK(s.enable(q,r)==d::Status::Closed);
    Device wrap(d::kInboxBytes-64);d::Inbox q2;CHECK(wrap.open(q2,r)==d::Status::Ok);
    CHECK(wrap.enable(q2,r)==d::Status::Ok);CHECK(r.wptr==0 && r.rptr==0);
}
void preconditions() {
    for(const auto reg : {d::Register::Control,d::Register::Control2,d::Register::BootStatus,d::Register::AxiSpace,
        d::Register::Cw4Base,d::Register::Cw4Top,d::Register::Cw4Offset,d::Register::Cw4OffsetHigh,
        d::Register::InboxBase,d::Register::InboxSize,d::Register::Wptr,d::Register::Rptr}) {
        Device s;d::Inbox q;d::Result r{};
        s.set(reg,reg==d::Register::Control2?1:reg==d::Register::Control?0:reg==d::Register::BootStatus?s.get(reg)^1:s.get(reg)^4);
        CHECK(s.open(q,r)!=d::Status::Ok);CHECK(!q.isOpen() && !q.needsRecovery());CHECK(s.writes==0 && s.vrWrites==0);
    }
    for(size_t i=0;i<d::kRegisterCount;++i) {
        Device s;d::Inbox q;d::Result r{};s.l.registers[i]+=4;
        CHECK(s.open(q,r)==d::Status::InvalidArgument);CHECK(s.accesses==0);
    }
    Device s;d::Inbox q;d::Result r{};CHECK(q.open(s.l,s.io(),mailbox+1,r)==d::Status::InvalidArgument);
    CHECK(s.open(q,r)==d::Status::Ok);const auto before=s.accesses;
    auto bad=route();bad.phy=0;CHECK(q.transmitter(bad,{d::Action::Enable,4,148500},polls,r)==d::Status::InvalidArgument);
    CHECK(q.transmitter(route(),{d::Action::Enable,2,148500},polls,r)==d::Status::InvalidArgument);
    CHECK(q.transmitter(route(0),{d::Action::Enable,4,148500},polls,r)==d::Status::InvalidArgument);
    CHECK(q.transmitter(route(),{d::Action::Disable,4,148500},polls,r)==d::Status::InvalidArgument);
    CHECK(q.transmitter(route(),{d::Action::Enable,4,600010},polls,r)==d::Status::InvalidArgument);
    CHECK(q.pixelClock(route(),{1485000,11,0,0,0},polls,r)==d::Status::InvalidArgument);
    CHECK(q.pixelClock(route(),{1485000,23,4,0,0},polls,r)==d::Status::InvalidArgument);
    CHECK(q.setupEncoder(route(),{5,4,8,148500},polls,r)==d::Status::InvalidArgument);
    CHECK(q.setupEncoder(route(),{0,4,6,148500},polls,r)==d::Status::InvalidArgument);
    CHECK(q.setupEncoder(route(),{0,4,16,600000},polls,r)==d::Status::InvalidArgument);
    CHECK(q.dpDrive(route(),{4,0,0,0,540000},polls,r)==d::Status::InvalidArgument);
    CHECK(q.dpDrive(route(0),{4,3,1,0,540000},polls,r)==d::Status::InvalidArgument);
    CHECK(q.dpDrive(route(0),{4,0,0,1,810000},polls,r)==d::Status::InvalidArgument);
    CHECK(q.transmitter(route(),{d::Action::Enable,4,148500},{2000001,1},r)==d::Status::InvalidArgument);
    CHECK(s.accesses==before && !s.vrWrites);
}
void faults() {
    for(unsigned f=0;f<9;++f) {
        Device s;d::Inbox q;d::Result r{};CHECK(s.open(q,r)==d::Status::Ok);
        if(f==0)s.rejectWrite=true;if(f==1)s.rejectFlush=true;if(f==2)s.rejectRead=true;
        if(f==3)s.corrupt=true;if(f==4)s.changeMap=true;if(f==5)s.changePointer=true;
        if(f==6)s.stall=true;if(f==7)s.dropPublish=true;if(f==8)s.badRptr=true;
        CHECK(s.enable(q,r)!=d::Status::Ok);CHECK(q.needsRecovery());
        if(f<6)CHECK(!s.writes && !r.submitted);else CHECK(s.writes==1 && r.submitted && !r.consumed);
        const auto before=s.accesses;CHECK(s.enable(q,r)==d::Status::Faulted);CHECK(q.close(r)==d::Status::Faulted);CHECK(s.accesses==before);
        if(f==6) { CHECK(q.pending());s.set(d::Register::Rptr,s.pendingTarget);CHECK(q.wait(polls,r)==d::Status::Ok);CHECK(r.consumed && !q.pending() && q.needsRecovery()); }
        q.clearAfterVerifiedReset();CHECK(!q.pending() && !q.needsRecovery() && !q.isOpen());
    }
    Device good;d::Inbox first;d::Result rr{};CHECK(good.open(first,rr)==d::Status::Ok);
    good.accesses=0;CHECK(good.enable(first,rr)==d::Status::Ok);const size_t count=good.accesses;
    for(size_t i=1;i<=count;++i) {
        Device s;d::Inbox q;d::Result r{};CHECK(s.open(q,r)==d::Status::Ok);s.accesses=0;s.failAt=i;
        CHECK(s.enable(q,r)==d::Status::TransportError);CHECK(q.needsRecovery());const auto before=s.accesses;
        CHECK(s.enable(q,r)==d::Status::Faulted);CHECK(s.accesses==before);
    }
    Device s;d::Inbox q;d::Result r{};CHECK(s.open(q,r)==d::Status::Ok);s.onesAt=s.accesses+1;
    CHECK(s.enable(q,r)==d::Status::DeviceUnavailable);CHECK(q.needsRecovery() && !r.submitted);
}
void fams() {
    Device good; d::Inbox first; d::Result r{};
    CHECK(first.checkIdle(r)==d::Status::Closed);
    CHECK(first.disableFams(polls,r)==d::Status::Closed && !r.submitted);
    CHECK(good.open(first,r)==d::Status::Ok);
    CHECK(first.checkIdle(r)==d::Status::Ok && first.isOpen() && !good.writes && !good.vrWrites);
    good.accesses=0;
    CHECK(first.disableFams(polls,r)==d::Status::Ok);
    const size_t count=good.accesses;
    CHECK(r.slotWritten && r.submitted && r.consumed && !first.pending());
    CHECK(get32(good.last,0)==0x3400004c);
    for(size_t i=4;i<64;++i)CHECK(good.last[i]==0);
    for(unsigned f=0;f<9;++f) {
        Device s;d::Inbox q;CHECK(s.open(q,r)==d::Status::Ok);
        if(f==0)s.rejectWrite=true;if(f==1)s.rejectFlush=true;if(f==2)s.rejectRead=true;
        if(f==3)s.corrupt=true;if(f==4)s.changeMap=true;if(f==5)s.changePointer=true;
        if(f==6)s.stall=true;if(f==7)s.dropPublish=true;if(f==8)s.badRptr=true;
        CHECK(q.disableFams(polls,r)!=d::Status::Ok && q.needsRecovery());
        CHECK(!r.consumed);
        if(f<6)CHECK(!s.writes && !r.submitted);else CHECK(s.writes==1 && r.submitted);
        const auto before=s.accesses;
        CHECK(q.disableFams(polls,r)==d::Status::Faulted && s.accesses==before);
        if(f==6) {
            CHECK(q.checkIdle(r)==d::Status::Faulted);
            s.set(d::Register::Rptr,s.pendingTarget);
            CHECK(q.wait(polls,r)==d::Status::Ok && r.consumed && q.needsRecovery());
        }
    }
    for(size_t i=1;i<=count;++i) {
        Device s;d::Inbox q;CHECK(s.open(q,r)==d::Status::Ok);s.accesses=0;s.failAt=i;
        CHECK(q.disableFams(polls,r)==d::Status::TransportError && q.needsRecovery());
        const auto before=s.accesses;
        CHECK(q.disableFams(polls,r)==d::Status::Faulted && s.accesses==before);
    }
    { Device s; d::Inbox q; CHECK(s.open(q,r)==d::Status::Ok);
      const auto before=s.accesses;
      CHECK(q.disableFams({0,10},r)==d::Status::InvalidArgument && s.accesses==before);
      s.set(d::Register::Cw4Offset,uint32_t(mailbox)+256);
      CHECK(q.checkIdle(r)==d::Status::MappingMismatch && q.needsRecovery());
      CHECK(!s.writes && !s.vrWrites);
    }
}
std::vector<uint8_t> file(const char* path) {std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
void supplied(const char* discoveryPath,const char* romPath) {
    const auto b=file(discoveryPath);d::Layout l{};CHECK(d::buildLayout(b.data(),b.size(),0x100000,l)==d::Status::Ok);
    if(!romPath)return;
    const auto rb=file(romPath);hackynavi::ImageInfo i{};
    CHECK(hackynavi::findNextImage(rb.data(),rb.size(),0,i)==hackynavi::Status::Ok);
    const uint8_t phys[]={0,4,1,3},gpio[]={3,4,2,0};
    for(uint8_t n=0;n<4;++n) {d::Route r{};CHECK(d::parseRoute(rb.data(),rb.size(),i,l,n,r)==d::Status::Ok);CHECK(r.phy==phys[n] && r.hpd==gpio[n] && r.ddc==gpio[n]);}
}
}
int main(int argc,char** argv) {
    layoutAndRoutes();commands();preconditions();faults();fams();if(argc>1)supplied(argv[1],argc>2?argv[2]:nullptr);
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
