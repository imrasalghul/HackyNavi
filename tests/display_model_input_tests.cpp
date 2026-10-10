// SPDX-License-Identifier: MIT
// Wire-format/MMIO host models only. Optional argument parses a real ROM;
// runtime reference values below remain test data, never target measurements.
#include "hackynavi/display_model_input.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
namespace m=hackynavi::displaymodel;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void p16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void p32(std::vector<uint8_t>& b,size_t p,uint32_t v){p16(b,p,uint16_t(v));p16(b,p+2,uint16_t(v>>16));}
void header(std::vector<uint8_t>& b,size_t p,uint16_t size,uint8_t major,uint8_t minor){p16(b,p,size);b[p+2]=major;b[p+3]=minor;}
void checksum(std::vector<uint8_t>& b){b[0x60]=0;unsigned sum=0;for(auto v:b)sum+=v;b[0x60]=uint8_t(0-sum);}
std::vector<uint8_t> rom(){
    std::vector<uint8_t> b(4096,0);b[0]=0x55;b[1]=0xaa;b[2]=8;p16(b,0x18,0x80);
    std::memcpy(&b[0x80],"PCIR",4);p16(b,0x84,0x1002);p16(b,0x86,0x744c);p16(b,0x8a,24);p16(b,0x90,8);b[0x95]=0x80;
    p16(b,0x48,0x100);header(b,0x100,44,2,3);std::memcpy(&b[0x104],"ATOM",4);
    p16(b,0x118,0x148c);p16(b,0x11a,0x2422);p16(b,0x11e,0x190);p16(b,0x120,0x140);
    header(b,0x190,8,2,1);header(b,0x140,74,2,1);
    p16(b,0x144+4*2,0x600);header(b,0x600,108,3,4);p32(b,0x604,0x12345678);
    p16(b,0x144+27*2,0x800);header(b,0x800,212,4,5);p16(b,0x80c,10000);p16(b,0x80e,5000);p16(b,0x826,0x744c);b[0x82a]=4;
    p16(b,0x144+28*2,0x200);header(b,0x200,132,3,0);b[0x214]=1;b[0x218]=0x70;b[0x219]=24;b[0x21a]=4;
    checksum(b);return b;
}
hackynavi::ImageInfo image(const std::vector<uint8_t>& b){hackynavi::ImageInfo r={};CHECK(hackynavi::parseImage(b.data(),b.size(),0,r)==hackynavi::Status::Ok);return r;}
uint16_t sum(const std::vector<uint8_t>& b,size_t start){uint16_t s=0;for(;start<b.size();++start)s=uint16_t(s+b[start]);return s;}
void fixDiscovery(std::vector<uint8_t>& b){p16(b,14,sum(b,60));p16(b,8,sum(b,10));}
std::vector<uint8_t> discovery(){
    std::vector<uint8_t> b(144+7*20,0);p32(b,0,0x28211407);p16(b,4,1);p16(b,6,4);p16(b,10,uint16_t(b.size()));
    p16(b,12,60);p16(b,16,uint16_t(b.size()-60));p32(b,60,0x53445049);p16(b,64,3);p16(b,66,uint16_t(b.size()-60));
    p16(b,72,1);p16(b,76,140);p16(b,142,7);
    const uint16_t hw[]={11,34,255,1,42,43,271};const uint8_t major[]={11,3,13,13,6,6,3};
    for(unsigned n=0;n<7;++n){const size_t p=144+n*20;p16(b,p,hw[n]);b[p+3]=3;b[p+4]=major[n];b[p+5]=n==6?2:0;
        p32(b,p+8,0x1000+n*0x1000);p32(b,p+12,0x2000+n*0x1000);p32(b,p+16,n==6?0x6000:0x3000+n*0x1000);}
    fixDiscovery(b);return b;
}
struct Device {
    std::vector<uint8_t> romBytes=rom(),ip=discovery();m::BoardInfo board={};m::ReferenceInfo refs={};m::Result result={};
    uint32_t pll=43,timer=0x1002;unsigned reads=0,failAt=0,onesAt=0,changeAt=0,validations=0,revokeAt=0;
    uint64_t ticks=100;unsigned clocks=0,backwardsAt=0,expireAt=0;uint32_t budget=1000;
    Device(){CHECK(m::parseBoard(romBytes.data(),romBytes.size(),image(romBytes),board)==m::Status::Ok);}
    m::Status capture(uint64_t bar=0x100000){return m::captureReferences(ip.data(),ip.size(),bar,board,{this,valid,read,time},budget,refs,result);}
    static bool valid(void* p){auto& x=*static_cast<Device*>(p);++x.validations;return !x.revokeAt||x.validations<x.revokeAt;}
    static bool read(void* p,uint32_t address,uint32_t& value){auto& x=*static_cast<Device*>(p);++x.reads;
        CHECK(address==0x16e37*4||address==(0x6000+0x525)*4);value=address==0x16e37*4?x.pll:x.timer;
        if(x.reads==x.changeAt)value^=1;if(x.reads==x.onesAt)value=UINT32_MAX;return x.reads!=x.failAt;}
    static uint64_t time(void* p){auto& x=*static_cast<Device*>(p);++x.clocks;
        if(x.clocks==x.backwardsAt)return x.ticks-1;if(x.clocks==x.expireAt)x.ticks+=x.budget;return ++x.ticks;}
};
}
int main(int argc,char** argv){
    Device baseline;CHECK(baseline.capture()==m::Status::Ok);CHECK(baseline.reads==4&&baseline.result.reads==4);
    CHECK(baseline.refs.dccgReferenceKHz==100000&&baseline.refs.dchubReferenceKHz==50000&&baseline.refs.programmedVcoKHz==4300000);
    CHECK(baseline.board.subsystemVendorId==0x148c&&baseline.board.channelCount==24&&baseline.board.channelWidthBytes==2&&baseline.board.i2cKHz==50000);
    const auto good=rom();const auto selected=image(good);
    for(size_t bytes=0;bytes<good.size();++bytes){m::BoardInfo out={};CHECK(m::parseBoard(good.data(),bytes,selected,out)!=m::Status::Ok);CHECK(!out.crystalKHz);}
    for(size_t offset:{size_t(0x802),size_t(0x803),size_t(0x80c),size_t(0x80e),size_t(0x826),size_t(0x82a),size_t(0x218),size_t(0x219),size_t(0x21a)}) {
        auto b=good;b[offset]^=1;checksum(b);m::BoardInfo out={};
        // A nonzero changed I2C frequency is metadata, not a fixed ASIC value.
        if(offset==0x80e)CHECK(m::parseBoard(b.data(),b.size(),selected,out)==m::Status::Ok);
        else CHECK(m::parseBoard(b.data(),b.size(),selected,out)!=m::Status::Ok);
    }
    for(uint8_t raw=0;raw<32;++raw){auto b=good;b[0x21a]=raw;checksum(b);m::BoardInfo out={};
        CHECK((m::parseBoard(b.data(),b.size(),selected,out)==m::Status::Ok)==(raw==4));}
    {auto b=good;p32(b,0x838,123);p32(b,0x83c,456);p32(b,0x840,789);checksum(b);m::BoardInfo out={};
        CHECK(m::parseBoard(b.data(),b.size(),selected,out)==m::Status::Ok);
        CHECK(out.rawMclkChange100ns==123&&out.rawSrExit100ns==456&&out.rawSrEnterExit100ns==789);}
    for(unsigned at=1;at<=4;++at){Device x;x.failAt=at;CHECK(x.capture()==m::Status::TransportError);CHECK(x.reads==at&&!x.refs.programmedVcoKHz);}
    for(unsigned at=1;at<=4;++at){Device x;x.onesAt=at;CHECK(x.capture()==m::Status::DeviceUnavailable);CHECK(x.reads==at&&!x.refs.programmedVcoKHz);}
    for(unsigned at:{3u,4u}){Device x;x.changeAt=at;CHECK(x.capture()==m::Status::ChangedReference);CHECK(!x.refs.programmedVcoKHz);}
    for(unsigned at=1;at<=baseline.validations;++at){Device x;x.revokeAt=at;CHECK(x.capture()==m::Status::Revoked);CHECK(x.validations==at&&!x.refs.programmedVcoKHz);}
    for(unsigned at=2;at<=baseline.clocks;++at){Device x;x.expireAt=at;CHECK(x.capture()==m::Status::Timeout);CHECK(!x.refs.programmedVcoKHz);}
    for(unsigned at=2;at<=baseline.clocks;++at){Device x;x.backwardsAt=at;CHECK(x.capture()==m::Status::ClockWentBackwards);CHECK(!x.refs.programmedVcoKHz);}
    for(uint32_t divider=0;divider<16;++divider){Device x;x.timer=0x1000|divider;CHECK((x.capture()==m::Status::Ok)==(divider==2));}
    {Device x;x.timer=2;CHECK(x.capture()==m::Status::UnsupportedReference);}
    {Device x;x.pll=0;CHECK(x.capture()==m::Status::UnsupportedReference);}
    {Device x;x.pll=43|0x80000000;CHECK(x.capture()==m::Status::Ok);CHECK(x.refs.programmedVcoKHz==4350000);}
    {Device x;CHECK(x.capture(0x16e37*4+3)==m::Status::OutsideBar&&x.reads==0);}
    {Device x;CHECK(x.capture(0x16e37*4+4)==m::Status::Ok);}
    {Device x;x.ip[144+6*20+5]=1;fixDiscovery(x.ip);CHECK(x.capture()==m::Status::DiscoveryRejected&&x.reads==0);}
    {Device x;x.budget=0;CHECK(x.capture()==m::Status::InvalidArgument&&x.reads==0);}
    if(argc==2){std::ifstream file(argv[1],std::ios::binary);std::vector<uint8_t> b((std::istreambuf_iterator<char>(file)),{});CHECK(!b.empty());
        hackynavi::ImageInfo i={};bool found=false;size_t offset=0;
        while(hackynavi::findNextImage(b.data(),b.size(),offset,i)==hackynavi::Status::Ok){m::BoardInfo out={};
            if(m::parseBoard(b.data(),b.size(),i,out)==m::Status::Ok){found=true;std::cout<<"ROM board: image="<<i.offset<<" crystalKHz="<<out.crystalKHz<<" i2cKHz="<<out.i2cKHz<<" channels="<<unsigned(out.channelCount)<<" widthBytes="<<out.channelWidthBytes<<" latencyRaw="<<out.rawMclkChange100ns<<","<<out.rawSrExit100ns<<","<<out.rawSrEnterExit100ns<<"\n";}
            offset=i.offset+i.size;
        }CHECK(found);
    }
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
