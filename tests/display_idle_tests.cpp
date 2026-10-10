// SPDX-License-Identifier: MIT
// Read-only host register model. No hardware quiescence is inferred.
#include "hackynavi/display_idle.hpp"
#include <iostream>
#include <map>
#include <vector>
namespace d=hackynavi::displayidle;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v){put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t p){uint16_t v=0;for(;p<b.size();++p)v=uint16_t(v+b[p]);return v;}
void checksum(std::vector<uint8_t>& b){put16(b,14,sum(b,60));put16(b,8,sum(b,10));}
std::vector<uint8_t> discovery(){
    std::vector<uint8_t> b(168,0);put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,168);
    put16(b,12,60);put16(b,16,108);put32(b,60,0x53445049);put16(b,64,3);put16(b,66,108);
    put16(b,72,1);put16(b,76,140);put16(b,142,1);put16(b,144,271);b[147]=4;b[148]=3;b[149]=2;
    put32(b,160,0x34c0);put32(b,164,0x8000);checksum(b);return b;
}
const uint32_t masks[]={0x10001,0x10000,0xf,15,15,15,3,15,0x7f};
const uint32_t idle[]={0,0,0xb,15,15,15,1,15,15};
const d::Status bad[]={d::Status::ActiveTiming,d::Status::ActiveTiming,d::Status::OutstandingFetch,
    d::Status::ConnectedCombiner,d::Status::ConnectedCombiner,d::Status::ConnectedCombiner,d::Status::BusyCombiner,
    d::Status::ConnectedCombiner,d::Status::ForeignLock};
struct Device {
    d::Layout layout={};std::map<uint32_t,uint32_t> regs;unsigned reads=0,failAt=0,onesAt=0;
    unsigned changeAt=0;uint32_t changeRegister=0,changeValue=0;std::vector<uint32_t> observed;
    Device(){auto b=discovery();CHECK(d::buildLayout(b.data(),b.size(),0x100000,layout)==d::Status::Ok);
        for(size_t n=0;n<4;++n)for(size_t r=0;r<d::kRegisterCount;++r)
            regs[layout.registers[n][r]]=r<9?idle[r]:0;}
    d::Io io(){return {this,read};}
    static bool read(void* p,uint32_t a,uint32_t& v){auto& x=*static_cast<Device*>(p);++x.reads;
        if(x.reads==x.changeAt)x.regs[x.changeRegister]=x.changeValue;
        x.observed.push_back(a);
        CHECK(x.regs.count(a)==1);v=x.reads==x.onesAt?UINT32_MAX:x.regs[a];return x.reads!=x.failAt;}
    void gated(size_t pipe){regs[layout.registers[pipe][9]]=0x100;regs[layout.registers[pipe][10]]=0x90000000;
        // An access to the gated window is a test failure, even though real
        // hardware may read zero rather than raising a transport error.
        regs.erase(layout.registers[pipe][2]);}
};
void currentPowerStates(){
    d::Result r={};
    for(uint32_t on:{0u,0x10000000u})for(uint32_t cfg:{0u,1u})for(uint32_t off:{0x80000000u,0x90000000u}){
        auto prepare=[&](Device& x,unsigned mask){
            for(size_t p=0;p<4;++p){x.regs[x.layout.registers[p][9]]=cfg;x.regs[x.layout.registers[p][10]]=on;
                if(mask&(1U<<p)){x.gated(p);x.regs[x.layout.registers[p][10]]=off;}}
        };
        for(unsigned mask=0;mask<16;++mask){Device x;prepare(x,mask);
            CHECK(d::verify(x.layout,x.io(),r)==d::Status::Ok && r.poweredOffMask==mask);
            for(size_t p=0;p<4;++p){bool sawControl=false;
                for(uint32_t reg:x.observed)if(reg==x.layout.registers[p][2])sawControl=true;
                CHECK(sawControl==!(mask&(1U<<p)));}
        }
        for(unsigned mask:{0u,15u}){
            Device base;prepare(base,mask);CHECK(d::verify(base.layout,base.io(),r)==d::Status::Ok);
            for(unsigned at=1;at<=base.observed.size();++at)for(bool ones:{false,true}){
                Device x;prepare(x,mask);if(ones)x.onesAt=at;else x.failAt=at;
                CHECK(d::verify(x.layout,x.io(),r)==(ones?d::Status::DeviceUnavailable:d::Status::ReadFailed));
                CHECK(x.reads==at);
            }
            for(size_t p=0;p<4;++p)for(unsigned bit=0;bit<32;++bit){
                Device x;prepare(x,mask);x.regs[x.layout.registers[p][10]]^=1U<<bit;
                CHECK(d::verify(x.layout,x.io(),r)==(bit==28?d::Status::Ok:d::Status::PowerUnavailable));
                Device c;prepare(c,mask);c.regs[c.layout.registers[p][9]]^=1U<<bit;
                CHECK(d::verify(c.layout,c.io(),r)==(!mask&&bit==0?d::Status::Ok:d::Status::PowerUnavailable));
            }
            for(size_t p=0;p<4;++p){bool seenStatus=false,seenConfig=false;
                for(unsigned at=1;at<=base.observed.size();++at){const auto reg=base.observed[at-1];
                    const bool state=reg==base.layout.registers[p][10],config=reg==base.layout.registers[p][9];
                    if(!state&&!config)continue;
                    bool& seen=state?seenStatus:seenConfig;if(!seen){seen=true;continue;}
                    Device x;prepare(x,mask);x.changeAt=at;x.changeRegister=reg;
                    x.changeValue=x.regs[reg]^(state?0x10000000U:1U);
                    const auto status=d::verify(x.layout,x.io(),r);
                    CHECK(status==d::Status::PowerChanged || status==d::Status::PowerUnavailable);
                    CHECK(x.reads<=at+2);
                }
            }
        }
        // Desired bit set must never skip the real HUBP drain check.
        for(size_t p=0;p<4;++p){Device x;prepare(x,0);x.regs[x.layout.registers[p][2]]=0;
            CHECK(d::verify(x.layout,x.io(),r)==d::Status::OutstandingFetch);}
    }
}
}
int main(){
    currentPowerStates();
    Device x;d::Result r={};CHECK(d::verify(x.layout,x.io(),r)==d::Status::Ok&&r.reads==72&&r.poweredOffMask==0);
    for(size_t n=0;n<4;++n){CHECK(x.layout.registers[n][0]==(0x34c0+0x1b41+n*0x80)*4);
        CHECK(x.layout.registers[n][2]==(0x34c0+0x5f3+n*0xdc)*4);
        CHECK(x.layout.registers[n][3]==(0x8000+n*0x15)*4);
        CHECK(x.layout.registers[n][7]==(0x8000+0x3d8+n*4)*4);
        CHECK(x.layout.registers[n][9]==(0x34c0+0x80+n*2)*4);
        CHECK(x.layout.registers[n][10]==(0x34c0+0x81+n*2)*4);
        for(size_t reg=0;reg<9;++reg)for(unsigned bit=0;bit<32;++bit){
            Device a;a.regs[a.layout.registers[n][reg]]^=1u<<bit;
            const auto status=d::verify(a.layout,a.io(),r);
            if(masks[reg]&(1u<<bit)){CHECK(status==bad[reg]);CHECK(r.pipe==n&&size_t(r.reg)==reg);}
            else CHECK(status==d::Status::Ok);
        }
    }
    for(unsigned at=1;at<=72;++at)for(bool ones:{false,true}){Device a;if(ones)a.onesAt=at;else a.failAt=at;
        CHECK(d::verify(a.layout,a.io(),r)==(ones?d::Status::DeviceUnavailable:d::Status::ReadFailed));CHECK(r.reads==at);}
    // Every on/off combination must still observe all downstream OTG/MPC
    // controls. Only the proven-off HUBP control window may be skipped.
    for(unsigned off=0;off<16;++off){Device a;unsigned offCount=0;
        for(size_t n=0;n<4;++n)if(off&(1u<<n)){a.gated(n);++offCount;}
        CHECK(d::verify(a.layout,a.io(),r)==d::Status::Ok);
        CHECK(r.poweredOffMask==off&&r.reads==72-offCount);
        for(size_t n=0;n<4;++n)if(off&(1u<<n))for(size_t reg=0;reg<9;++reg)if(reg!=2){
            Device b;b.gated(n);b.regs[b.layout.registers[n][reg]]^=masks[reg]&(~masks[reg]+1);
            CHECK(d::verify(b.layout,b.io(),r)==bad[reg]);
        }
    }
    for(size_t n=0;n<4;++n){
        // Requested/actual state disagreement, transitional FSM states,
        // FORCEON+GATE and unknown bits are never settled-off evidence.
        for(uint32_t state:{0x40000000u,0x80000000u,0xc0000000u,0xd0000000u,1u}){
            Device a;a.regs[a.layout.registers[n][10]]=state;
            CHECK(d::verify(a.layout,a.io(),r)==d::Status::PowerUnavailable&&r.pipe==n);
        }
        for(uint32_t config:{0x100u,0x101u,2u,0x10000u}){Device a;a.regs[a.layout.registers[n][9]]=config;
            CHECK(d::verify(a.layout,a.io(),r)==d::Status::PowerUnavailable);}
        for(uint32_t config:{0u,1u,0x101u,0x102u}){Device a;a.gated(n);a.regs[a.layout.registers[n][9]]=config;
            CHECK(d::verify(a.layout,a.io(),r)==d::Status::PowerUnavailable);}
        Device forced;forced.regs[forced.layout.registers[n][9]]=1;
        CHECK(d::verify(forced.layout,forced.io(),r)==d::Status::Ok);
        // A changed state between bracketing status reads must reject before
        // accessing this HUBP, even if the new state is itself a valid value.
        Device changed;changed.changeAt=unsigned(n*15+3);changed.changeRegister=changed.layout.registers[n][10];
        changed.changeValue=0x90000000;
        CHECK(d::verify(changed.layout,changed.io(),r)==d::Status::PowerChanged&&r.reads==changed.changeAt);
        // Compare both post-pipe and final-pass power snapshots, including an
        // otherwise valid switch from ordinary-on to forced-on policy.
        for(unsigned at:{unsigned(n*15+13),unsigned(61+n*3)}){Device a;
            a.changeAt=at;a.changeRegister=a.layout.registers[n][9];a.changeValue=1;
            CHECK(d::verify(a.layout,a.io(),r)==d::Status::PowerChanged&&r.pipe==n);}
    }
    for(unsigned at=1;at<=68;++at){Device a;for(size_t n=0;n<4;++n)a.gated(n);a.failAt=at;
        CHECK(d::verify(a.layout,a.io(),r)==d::Status::ReadFailed&&r.reads==at);}
    for(size_t n=0;n<4;++n)for(size_t reg=0;reg<d::kRegisterCount;++reg){Device a;a.layout.registers[n][reg]^=1;
        CHECK(d::verify(a.layout,a.io(),r)==d::Status::InvalidArgument&&r.reads==0);}
    auto b=discovery();d::Layout l={};
    for(size_t bytes=0;bytes<b.size();++bytes)CHECK(d::buildLayout(b.data(),bytes,0x100000,l)==d::Status::DiscoveryRejected);
    b[149]=1;checksum(b);CHECK(d::buildLayout(b.data(),b.size(),0x100000,l)==d::Status::UnsupportedDmu&&!l.valid);
    b=discovery();CHECK(d::buildLayout(b.data(),b.size(),4,l)==d::Status::OutsideBar&&!l.valid);
    // This base aliases OTG0 clock and MPCC0 top selection. Their expected
    // bit predicates are compatible, so rejecting only contradictory values
    // would allow a false successful observation of two distinct controls.
    b=discovery();put32(b,164,0x5045);checksum(b);
    CHECK(d::buildLayout(b.data(),b.size(),0x100000,l)==d::Status::OutsideBar&&!l.valid);
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
