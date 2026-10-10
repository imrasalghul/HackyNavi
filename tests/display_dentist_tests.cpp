// SPDX-License-Identifier: MIT
// Offline arithmetic and transport fault injection, never target evidence.
#include "hackynavi/display_dentist.hpp"
#include <cstring>
#include <iostream>
#include <vector>
namespace d=hackynavi::displaydentist;
namespace m=hackynavi::displaymodel;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void p16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void p32(std::vector<uint8_t>& b,size_t p,uint32_t v){p16(b,p,uint16_t(v));p16(b,p+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t p){uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s;}
void fix(std::vector<uint8_t>& b){p16(b,14,sum(b,60));p16(b,8,sum(b,10));}
std::vector<uint8_t> discovery(){
    std::vector<uint8_t> b(144+7*20,0);p32(b,0,0x28211407);p16(b,4,1);p16(b,6,4);p16(b,10,uint16_t(b.size()));
    p16(b,12,60);p16(b,16,uint16_t(b.size()-60));p32(b,60,0x53445049);p16(b,64,3);p16(b,66,uint16_t(b.size()-60));
    p16(b,72,1);p16(b,76,140);p16(b,142,7);
    const uint16_t hw[]={11,34,255,1,42,43,271};const uint8_t major[]={11,3,13,13,6,6,3};
    for(unsigned n=0;n<7;++n){const size_t p=144+n*20;p16(b,p,hw[n]);b[p+3]=3;b[p+4]=major[n];b[p+5]=n==6?2:0;
        p32(b,p+8,0x1000+n*0x1000);p32(b,p+12,0x2000+n*0x1000);p32(b,p+16,0x3000+n*0x1000);}
    fix(b);return b;
}
struct Device {
    std::vector<uint8_t> ip=discovery();d::Layout layout={};m::BoardInfo board={};m::ReferenceInfo refs={};
    d::Observation out={};d::Result result={};
    uint32_t pll=43,dentist=0x180000|16|(16<<8),dfs[5]={16,20,64,32,0};
    unsigned reads=0,failAt=0,onesAt=0,changeAt=0,validations=0,revokeAt=0,clocks=0,backAt=0,expireAt=0;
    uint64_t ticks=100;uint32_t budget=1000,mask=0xf;
    Device(){
        CHECK(d::buildLayout(ip.data(),ip.size(),0x100000,layout)==d::Status::Ok);
        board.vendorId=0x1002;board.deviceId=0x744c;board.crystalKHz=100000;board.i2cKHz=50000;
        board.memoryType=0x70;board.channelCount=24;board.channelWidthRaw=4;board.channelWidthBytes=2;board.maxDisplayPipes=4;
        CHECK(m::decodeReferences(board,pll,0x1002,refs)==m::Status::Ok);refs.pllRequestRegister=layout.pll;
    }
    d::Status run(){return d::capture(layout,board,refs,mask,{this,valid,read,time},budget,out,result);}
    static bool valid(void* p){auto& x=*static_cast<Device*>(p);++x.validations;return x.validations!=x.revokeAt;}
    static uint64_t time(void* p){auto& x=*static_cast<Device*>(p);++x.clocks;
        if(x.clocks==x.backAt)return x.ticks-1;if(x.clocks==x.expireAt)x.ticks+=x.budget;return ++x.ticks;}
    static bool read(void* p,uint32_t address,uint32_t& value){auto& x=*static_cast<Device*>(p);++x.reads;
        bool found=false;if(address==x.layout.pll){value=x.pll;found=true;}
        else if(address==x.layout.dentist){value=x.dentist;found=true;}
        else for(size_t i=0;i<5;++i)if(address==x.layout.dfs[i]){value=x.dfs[i];found=true;}
        CHECK(found);if(x.reads==x.onesAt)value=UINT32_MAX;if(x.reads==x.changeAt)value^=1;
        return found&&x.reads!=x.failAt;
    }
};
}
int main(){
    // Independent range table and boundary properties, including no clamp.
    const uint32_t starts[]={8,64,96,126,128},values[]={8,64,128,248},steps[]={1,2,4,264};
    for(unsigned range=0;range<4;++range)for(uint32_t did=starts[range];did<starts[range+1];++did){
        uint32_t out=0;CHECK(d::decodeDivider(did,out)==d::Status::Ok);
        CHECK(out==values[range]+(did-starts[range])*steps[range]);
    }
    for(uint32_t did:{0u,1u,7u,128u,255u,UINT32_MAX}){uint32_t out=42;CHECK(d::decodeDivider(did,out)==d::Status::UnsupportedDivider&&out==0);}
    for(uint32_t vco:{3600000u,4300000u,4350000u})for(uint32_t required=1000;required<=1950000;required+=97){
        d::Preset p={};const auto s=d::plan(vco,required,p);
        if(s==d::Status::Ok){
            CHECK(p.requiredKHz==required&&p.programmedKHz>=required&&p.programmedKHz<=1950000);
            CHECK(p.requestPresetKHz>=required&&p.requestMHz==p.requestPresetKHz/1000);
            CHECK(uint64_t(vco)*4/p.scaledDivider==p.programmedKHz);
            // The selected legal divider is maximal without underclocking.
            if(p.dividerId<127){uint32_t next=0;CHECK(d::decodeDivider(p.dividerId+1,next)==d::Status::Ok);
                CHECK(uint64_t(vco)*4/next<required);}
        }else CHECK(s==d::Status::UnrepresentableFrequency);
    }
    {d::Preset p={};CHECK(d::plan(4300000,1075000,p)==d::Status::Ok);CHECK(p.dividerId==16&&p.requestMHz==1075&&p.programmedKHz==1075000);}
    {d::Preset p={};CHECK(d::plan(4300000,30000,p)==d::Status::Ok);CHECK(p.dividerId==127&&p.programmedKHz==33593);}
    {d::Preset p={};CHECK(d::plan(4300000,33594,p)==d::Status::Ok);CHECK(p.dividerId==126&&p.programmedKHz==69354);}
    for(uint32_t vco:{0u,1u,UINT32_MAX}){d::Preset p={};CHECK(d::plan(vco,1950001,p)!=d::Status::Ok&&!p.programmedKHz);}
    {d::Preset p={};CHECK(d::plan(1,0,p)==d::Status::InvalidArgument);CHECK(d::plan(1000,501,p)==d::Status::UnrepresentableFrequency);}
    Device baseline;CHECK(baseline.run()==d::Status::Ok);CHECK(baseline.reads==14&&baseline.result.reads==14);
    CHECK(baseline.out.validClockMask==0xf&&baseline.out.programmedVcoKHz==4300000);
    CHECK(baseline.out.programmedKHz[0]==1075000&&baseline.out.programmedKHz[1]==860000&&baseline.out.programmedKHz[4]==0);
    CHECK(baseline.out.dispChangeDone&&baseline.out.dppChangeDone&&baseline.out.dispDividerAgrees);
    {Device x;x.mask=31;CHECK(x.run()==d::Status::UnsupportedDivider&&!x.out.validClockMask);}
    {Device x;x.mask=31;x.dfs[4]=127;CHECK(x.run()==d::Status::Ok&&x.out.programmedKHz[4]==33593);}
    {Device x;x.dentist=0;CHECK(x.run()==d::Status::Ok&&!x.out.dispChangeDone&&!x.out.dppChangeDone&&!x.out.dispDividerAgrees);}
    for(unsigned at=1;at<=14;++at){Device x;x.failAt=at;CHECK(x.run()==d::Status::ReadFailed);CHECK(x.reads==at&&!x.out.validClockMask);}
    for(unsigned at=1;at<=14;++at){Device x;x.onesAt=at;CHECK(x.run()==d::Status::DeviceUnavailable);CHECK(!x.out.validClockMask);}
    for(unsigned at=8;at<=14;++at){Device x;x.changeAt=at;CHECK(x.run()==d::Status::ChangedProgramming&&!x.out.validClockMask);}
    for(unsigned at=1;at<=baseline.validations;++at){Device x;x.revokeAt=at;CHECK(x.run()==d::Status::Revoked&&!x.out.validClockMask);}
    for(unsigned at=2;at<=baseline.clocks;++at){Device x;x.backAt=at;CHECK(x.run()==d::Status::ClockWentBackwards&&!x.out.validClockMask);}
    for(unsigned at=2;at<=baseline.clocks;++at){Device x;x.expireAt=at;CHECK(x.run()==d::Status::Timeout&&!x.out.validClockMask);}
    {Device x;x.pll^=1;CHECK(x.run()==d::Status::ReferenceMismatch&&!x.out.validClockMask);}
    {Device x;++x.refs.programmedVcoKHz;CHECK(x.run()==d::Status::ReferenceMismatch&&x.reads==0);}
    {Device x;x.refs.rawPllRequest|=0x80000000;x.pll=x.refs.rawPllRequest;x.refs.programmedVcoKHz=4350000;
        CHECK(x.run()==d::Status::Ok&&x.out.programmedKHz[0]==1087500);}
    for(uint32_t mask:{0u,32u,UINT32_MAX}){Device x;x.mask=mask;CHECK(x.run()==d::Status::InvalidArgument&&x.reads==0);}
    {Device x;x.budget=2000001;CHECK(x.run()==d::Status::InvalidArgument&&x.reads==0);}
    {Device x;x.layout.dfs[0]+=4;CHECK(x.run()==d::Status::InvalidArgument&&x.reads==0);}
    {Device x;d::Layout l={};CHECK(d::buildLayout(x.ip.data(),x.ip.size(),0x16e75*4+3,l)==d::Status::OutsideBar&&!l.valid);
        CHECK(d::buildLayout(x.ip.data(),x.ip.size(),0x16e75*4+4,l)==d::Status::Ok);}
    {Device x;x.ip[144+6*20+5]=1;fix(x.ip);d::Layout l={};CHECK(d::buildLayout(x.ip.data(),x.ip.size(),0x100000,l)==d::Status::DiscoveryRejected);}
    {Device x;for(size_t n=0;n<x.ip.size();++n){d::Layout l={};CHECK(d::buildLayout(x.ip.data(),n,0x100000,l)!=d::Status::Ok&&!l.valid);}}
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
