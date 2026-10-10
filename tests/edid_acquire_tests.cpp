// SPDX-License-Identifier: MIT
// Host-only EDID/peripheral models. Production adapters execute real MMIO.
#include "hackynavi/edid_acquire.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>
namespace e=hackynavi::edid;
namespace q=hackynavi::edidacq;
namespace a=hackynavi::dpaux;
namespace d=hackynavi::hdmiddc;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void checksum(std::vector<uint8_t>& bytes,size_t block) {
    unsigned sum=0;for(size_t i=0;i<127;++i)sum+=bytes[block*128+i];bytes[block*128+127]=uint8_t(0-sum);
}
std::vector<uint8_t> fixture(size_t blocks) {
    std::vector<uint8_t> bytes(blocks*128,0);
    for(size_t i=1;i<7;++i)bytes[i]=255;
    bytes[18]=1;bytes[19]=4;bytes[20]=0x80;bytes[126]=uint8_t(blocks-1);
    for(size_t i=38;i<54;++i)bytes[i]=1;
    for(size_t b=1;b<blocks;++b) {bytes[b*128]=2;bytes[b*128+1]=3;bytes[b*128+2]=6;
        bytes[b*128+4]=0x41;bytes[b*128+5]=16;}
    for(size_t b=0;b<blocks;++b)checksum(bytes,b);
    return bytes;
}
struct Blocks {
    std::vector<uint8_t> bytes;std::vector<uint8_t> reads;
    size_t failCall=0,changeCall=0;
    explicit Blocks(size_t blocks):bytes(fixture(blocks)){}
    static bool read(void* p,uint8_t block,uint8_t* out) {
        auto& s=*static_cast<Blocks*>(p);s.reads.push_back(block);
        if(s.reads.size()==s.failCall)return false;
        CHECK((size_t(block)+1)*128<=s.bytes.size());
        if((size_t(block)+1)*128>s.bytes.size())return false;
        std::memcpy(out,s.bytes.data()+size_t(block)*128,128);
        if(s.reads.size()==s.changeCall) {out[12]^=1;out[127]=uint8_t(out[127]+(out[12]&1?-1:1));}
        return true;
    }
    q::Io io(){return {this,read};}
};
void generic() {
    uint8_t buffer[e::kMaxBlocks*128]={};q::Result r{};e::Monitor m{};
    for(size_t n=1;n<=e::kMaxBlocks;++n) {
        Blocks s(n);CHECK(q::capture(s.io(),buffer,sizeof(buffer),m,r)==q::Status::Ok);
        CHECK(r.complete && r.bytes==n*128 && r.blocksRead==n && r.verifiedBlocks==n && r.readCalls==2*n);
        CHECK(r.expectedBlocks==n && m.blocks==n && r.stage==q::Stage::Complete);
        CHECK(std::memcmp(buffer,s.bytes.data(),n*128)==0);
        for(size_t i=0;i<s.reads.size();++i)CHECK(s.reads[i]==i%n);
    }
    for(size_t fail=1;fail<=8;++fail) {
        Blocks s(4);s.failCall=fail;m.serial=0xfade;const auto before=m;
        CHECK(q::capture(s.io(),buffer,sizeof(buffer),m,r)==q::Status::TransportFailed);
        CHECK(!r.complete && !r.bytes && r.readCalls==fail && std::memcmp(&m,&before,sizeof(m))==0);
    }
    for(size_t change=4;change<=6;++change) {
        Blocks s(3);s.changeCall=change;m.serial=0xfade;const auto before=m;
        CHECK(q::capture(s.io(),buffer,sizeof(buffer),m,r)==q::Status::ChangedWhileReading);
        CHECK(!r.complete && r.stage==q::Stage::Verify && r.readCalls==change);
        CHECK(std::memcmp(&m,&before,sizeof(m))==0);
    }
    Blocks upward(3);upward.bytes[126]=1;upward.bytes[130]=7;
    upward.bytes[132]=0xe2;upward.bytes[133]=0x78;upward.bytes[134]=2;
    checksum(upward.bytes,0);checksum(upward.bytes,1);
    CHECK(q::capture(upward.io(),buffer,sizeof(buffer),m,r)==q::Status::Ok && r.expectedBlocks==3);
    Blocks downward(2);downward.bytes[126]=255;downward.bytes[130]=7;
    downward.bytes[132]=0xe2;downward.bytes[133]=0x78;downward.bytes[134]=1;
    checksum(downward.bytes,0);checksum(downward.bytes,1);
    CHECK(q::capture(downward.io(),buffer,256,m,r)==q::Status::Ok && r.bytes==256 && downward.reads.size()==4);
    Blocks tooMany(2);tooMany.bytes[126]=255;checksum(tooMany.bytes,0);
    CHECK(q::capture(tooMany.io(),buffer,sizeof(buffer),m,r)==q::Status::EdidRejected);
    CHECK(r.edidStatus==e::Status::TooManyBlocks && r.readCalls==2);
    for(size_t n=0;n<512;++n) {
        Blocks s(4);CHECK(q::capture(s.io(),buffer,n,m,r)==q::Status::NoSpace && !r.complete);
        CHECK(r.readCalls==(n<128?0:n<256?1:2));
    }
    for(size_t b=0;b<4;++b) {
        Blocks s(4);s.bytes[b*128+127]^=1;
        CHECK(q::capture(s.io(),buffer,sizeof(buffer),m,r)==q::Status::EdidRejected);
        CHECK(r.edidStatus==e::Status::BadChecksum && !r.complete && r.stage==q::Stage::Read);
    }
    Blocks malformed(2);malformed.bytes[130]=5;checksum(malformed.bytes,1);
    CHECK(q::capture(malformed.io(),buffer,sizeof(buffer),m,r)==q::Status::EdidRejected);
    CHECK(r.edidStatus==e::Status::BadExtension && r.stage==q::Stage::Parse);
    Blocks s(1);CHECK(q::capture({nullptr,nullptr},buffer,sizeof(buffer),m,r)==q::Status::InvalidArgument);
    CHECK(q::capture(s.io(),nullptr,sizeof(buffer),m,r)==q::Status::InvalidArgument && s.reads.empty());
}
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v){put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16));}
std::vector<uint8_t> discovery() {
    std::vector<uint8_t> b(164,0);
    put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,164);put16(b,12,60);put16(b,16,104);
    put32(b,60,0x53445049);put16(b,64,3);put16(b,66,104);put16(b,72,1);put16(b,76,140);put16(b,142,1);
    put16(b,144,271);b[147]=3;b[148]=3;b[149]=2;put32(b,156,0xc0);put32(b,160,0x34c0);
    uint16_t sum=0;for(size_t i=60;i<b.size();++i)sum=uint16_t(sum+b[i]);put16(b,14,sum);
    sum=0;for(size_t i=10;i<b.size();++i)sum=uint16_t(sum+b[i]);put16(b,8,sum);return b;
}
struct AuxDevice {
    a::Layout layout{};a::Engine engine;std::map<uint32_t,uint32_t> regs;
    std::vector<uint8_t> bytes,tx,rx;std::vector<std::vector<uint8_t>> packets;
    size_t rxIndex=0,offset=0;uint8_t segment=0,shortLimit=16;
    size_t nackAt=0,failAt=0;bool active=false;unsigned stops=0,segments=0;
    explicit AuxDevice(size_t blocks):bytes(fixture(blocks)) {
        const auto b=discovery();CHECK(a::buildLayout(b.data(),b.size(),0x100000,layout)==a::Status::Ok);
        for(const auto& row:layout.registers)for(auto reg:row)regs[reg]=0;
        a::Result r{};CHECK(engine.open(layout,{this,read,write,delay},3,2,r)==a::Status::Ok);
    }
    uint32_t reg(a::Register r)const{return layout.registers[3][size_t(r)];}
    static void delay(void*,uint32_t){}
    static bool read(void* p,uint32_t reg,uint32_t& value) {
        auto& s=*static_cast<AuxDevice*>(p);CHECK(s.regs.count(reg));value=s.regs[reg];
        if(reg==s.reg(a::Register::Data)&&(value&1)) {
            // The next packet's initial Data RMW may sample beyond the old
            // reply before switching direction and resetting the FIFO index.
            value=(value&~0xff00U)|(uint32_t(s.rxIndex<s.rx.size()?s.rx[s.rxIndex++]:0)<<8);
        }return true;
    }
    static bool write(void* p,uint32_t reg,uint32_t value) {
        auto& s=*static_cast<AuxDevice*>(p);CHECK(s.regs.count(reg));const auto old=s.regs[reg];s.regs[reg]=value;
        if(reg==s.reg(a::Register::Control))s.regs[reg]=(value&~0x20U)|((value&0x10)?0x20U:0);
        else if(reg==s.reg(a::Register::Arbitration))s.regs[reg]=(value&0x20000)?value&~0x3000cU:(value&0x10000)?(value&~0xcU)|4:value;
        else if(reg==s.reg(a::Register::Interrupt)){if(value&2)s.regs[s.reg(a::Register::Status)]=0;}
        else if(reg==s.reg(a::Register::Data)) {
            if(!(value&1)){if(value&0x80000000)s.tx.clear();s.tx.push_back(uint8_t(value>>8));}
            else if(!(old&1))s.rxIndex=0;
        } else if(reg==s.reg(a::Register::SwControl)&&(value&1)) {
            CHECK(s.tx.size()==((value>>16)&31));CHECK(s.tx.size()>=3);s.packets.push_back(s.tx);
            const uint8_t command=s.tx[0]&0xf0;const uint8_t slave=s.tx[2];
            CHECK(s.tx[1]==0 && (s.tx[0]&15)==0 && (slave==0x30 || slave==0x50));
            s.rx={0};
            if(s.packets.size()==s.nackAt)s.rx[0]=0x40;
            else if(s.tx.size()==3) {
                CHECK(command==0);s.segment=0;s.active=false;++s.stops;
            } else {
                s.active=true;const size_t length=size_t(s.tx[3])+1;
                if(command==0x40) {
                    CHECK(s.tx.size()==5 && length==1);
                    if(slave==0x30){s.segment=s.tx[4];++s.segments;}else s.offset=s.tx[4];
                } else {
                    CHECK(command==0x50 && slave==0x50 && s.tx.size()==4);
                    const size_t n=std::min(length,size_t(s.shortLimit));const size_t at=size_t(s.segment)*256+s.offset;
                    CHECK(at+n<=s.bytes.size());s.rx.insert(s.rx.end(),s.bytes.begin()+at,s.bytes.begin()+at+n);s.offset+=n;
                }
            }
            s.rxIndex=0;s.regs[s.reg(a::Register::Status)]=1|(uint32_t(s.rx.size())<<24);s.regs[reg]&=~1U;
            if(s.packets.size()==s.failAt)return false; // GO may have landed.
        }return true;
    }
};
void auxAdapter() {
    uint8_t buffer[1024]={};e::Monitor m{};q::HardwareResult r{};
    for(uint8_t n: {uint8_t(16),uint8_t(7),uint8_t(1)}) {
        AuxDevice s(4);s.shortLimit=n;
        CHECK(q::capture(s.engine,buffer,sizeof(buffer),m,r)==q::Status::Ok);
        CHECK(r.capture.complete && r.capture.bytes==512 && r.stopCompleted && s.stops==8 && s.segments==4);
        CHECK(!s.active && s.engine.isOpen() && r.packets==s.packets.size());
        CHECK(std::memcmp(buffer,s.bytes.data(),512)==0);
        CHECK(s.packets.front()==std::vector<uint8_t>({0x40,0,0x50,0,0}));
        CHECK(s.packets.back()==std::vector<uint8_t>({0,0,0x50}));
        a::Result closed{};CHECK(s.engine.close(closed)==a::Status::Ok && !s.engine.ownsPins());
    }
    for(size_t fail=1;fail<=20;++fail) {
        AuxDevice s(2);s.nackAt=fail;m.serial=0xface;const auto before=m;
        CHECK(q::capture(s.engine,buffer,sizeof(buffer),m,r)==q::Status::TransportFailed);
        CHECK(!r.capture.complete && std::memcmp(&m,&before,sizeof(m))==0);
        CHECK(r.stopAttempted && s.engine.isOpen());
        a::Result closed{};CHECK(s.engine.close(closed)==a::Status::Ok && !s.active);
    }
    AuxDevice zero(1);zero.shortLimit=0;
    CHECK(q::capture(zero.engine,buffer,sizeof(buffer),m,r)==q::Status::TransportFailed);
    CHECK(r.auxStatus==a::Status::ShortReply && r.stopCompleted && zero.packets.size()==3);
    AuxDevice ambiguous(1);ambiguous.failAt=2;
    CHECK(q::capture(ambiguous.engine,buffer,sizeof(buffer),m,r)==q::Status::TransportFailed);
    CHECK(ambiguous.engine.needsRecovery() && ambiguous.engine.ownsPins() && ambiguous.packets.size()==2);
    CHECK(!r.stopAttempted && !r.capture.complete);
    a::Engine closed;CHECK(q::capture(closed,buffer,sizeof(buffer),m,r)==q::Status::InvalidState && !r.packets);
}
struct DdcDevice {
    d::Layout layout{};d::Engine engine;std::map<uint32_t,uint32_t> regs;
    std::vector<uint8_t> bytes;uint8_t fifo[d::kFifoBytes]={};size_t wi=0,ri=0,commands=0;
    size_t nackAt=0,failAt=0;unsigned segmentWrites=0;
    explicit DdcDevice(size_t blocks):bytes(fixture(blocks)) {
        const auto b=discovery();CHECK(d::buildLayout(b.data(),b.size(),0x100000,layout)==d::Status::Ok);
        for(const auto& row:layout.registers)for(auto reg:row)regs[reg]=0;
        d::Result r{};CHECK(engine.open(layout,{this,read,write,delay},2,{100000,100},r)==d::Status::Ok);
    }
    uint32_t reg(d::Register r)const{return layout.registers[2][size_t(r)];}
    static void delay(void*,uint32_t){}
    static bool read(void* p,uint32_t reg,uint32_t& value) {
        auto& s=*static_cast<DdcDevice*>(p);CHECK(s.regs.count(reg));value=s.regs[reg];
        if(reg==s.reg(d::Register::Data)){CHECK(s.ri<d::kFifoBytes);value=uint32_t(s.fifo[s.ri++])<<8|1;}
        return true;
    }
    static bool write(void* p,uint32_t reg,uint32_t value) {
        auto& s=*static_cast<DdcDevice*>(p);CHECK(s.regs.count(reg));const auto old=s.regs[reg];s.regs[reg]=value;
        if(reg==s.reg(d::Register::MemoryPowerControl))s.regs[s.reg(d::Register::MemoryPowerStatus)]=value&1;
        else if(reg==s.reg(d::Register::Arbitration))s.regs[reg]=(value&0x200000)?value&~0x30000cU:(value&0x100000)?(value&~0x30000cU)|4:value;
        else if(reg==s.reg(d::Register::Data)) {
            if(value&1){if(value&0x80000000)s.ri=(value>>16)&1023;}
            else {if(value&0x80000000)s.wi=(value>>16)&1023;CHECK(s.wi<d::kFifoBytes);s.fifo[s.wi++]=uint8_t(value>>8);}
        } else if(reg==s.reg(d::Register::Control)) {
            if(value&8)s.regs[s.reg(d::Register::Status)]=0;
            if((value&1)&&!(old&1)) {
                ++s.commands;const size_t count=((value>>20)&3)+1;CHECK(count==2 || count==3);
                size_t pos=0,offset=0,segment=0;
                for(size_t i=0;i<count;++i) {
                    const auto desc=s.regs[s.reg(d::Register::Transaction0)+uint32_t(i)*4];
                    const size_t n=(desc>>16)&1023;const uint8_t address=s.fifo[pos++];
                    CHECK((desc&0x1100)==0x1100 && bool(desc&0x2000)==(i+1==count));
                    if(desc&1) {
                        CHECK(i+1==count && address==0xa1 && n==128 && pos==s.wi);
                        const size_t at=segment*256+offset;CHECK(at+n<=s.bytes.size());
                        std::memcpy(s.fifo+pos,s.bytes.data()+at,n);
                    } else {
                        CHECK(n==1);if(address==0x60){segment=s.fifo[pos++];++s.segmentWrites;}
                        else {CHECK(address==0xa0);offset=s.fifo[pos++];}
                    }
                }
                s.regs[s.reg(d::Register::Status)]=s.commands==s.nackAt?0x104:4;s.regs[reg]&=~1U;
                if(s.commands==s.failAt)return false;
            }
        }return true;
    }
};
void ddcAdapter() {
    uint8_t buffer[1024]={};e::Monitor m{};q::HardwareResult r{};
    DdcDevice s(4);CHECK(q::capture(s.engine,buffer,sizeof(buffer),m,r)==q::Status::Ok);
    CHECK(r.capture.complete && r.capture.bytes==512 && s.commands==8 && s.segmentWrites==4);
    CHECK(std::memcmp(buffer,s.bytes.data(),512)==0 && s.engine.isOpen());
    d::Result closed{};CHECK(s.engine.close(closed)==d::Status::Ok && !s.engine.ownsController());
    for(size_t fail=1;fail<=8;++fail) {
        DdcDevice failed(4);failed.nackAt=fail;m.serial=0xface;const auto before=m;
        CHECK(q::capture(failed.engine,buffer,sizeof(buffer),m,r)==q::Status::TransportFailed);
        CHECK(r.ddcStatus==d::Status::Nack && r.packets==fail && !r.capture.complete);
        CHECK(std::memcmp(&m,&before,sizeof(m))==0 && failed.engine.isOpen());
        CHECK(failed.engine.close(closed)==d::Status::Ok);
    }
    DdcDevice ambiguous(1);ambiguous.failAt=1;
    CHECK(q::capture(ambiguous.engine,buffer,sizeof(buffer),m,r)==q::Status::TransportFailed);
    CHECK(ambiguous.engine.needsRecovery() && ambiguous.engine.ownsController() && !r.capture.complete);
    d::Engine unopened;CHECK(q::capture(unopened,buffer,sizeof(buffer),m,r)==q::Status::InvalidState && !r.packets);
}
}
int main(){generic();auxAdapter();ddcAdapter();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
