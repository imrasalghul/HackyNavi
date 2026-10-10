// SPDX-License-Identifier: MIT
// Host protocol/fault-injection tests only; no GPU observation or execution.
#include "hackynavi/gfx11_color_topology.hpp"
#include <stdio.h>
#include <string.h>
#include <vector>
#include <array>
using namespace hackynavi::colortopology;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("line %u: %s\n",unsigned(__LINE__),#x);}}while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v) {b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v) {put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16));}
void fix(std::vector<uint8_t>& b) {
    uint16_t sum=0;for(size_t i=60;i<b.size();++i)sum=uint16_t(sum+b[i]);put16(b,14,sum);
    sum=0;for(size_t i=10;i<b.size();++i)sum=uint16_t(sum+b[i]);put16(b,8,sum);
}
std::vector<uint8_t> discovery(bool indirect) {
    std::vector<uint8_t> b(144+7*40,0);
    put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,uint16_t(b.size()));
    put16(b,12,60);put16(b,16,uint16_t(b.size()-60));put32(b,60,0x53445049);
    put16(b,64,3);put16(b,66,uint16_t(b.size()-60));put16(b,72,1);put16(b,76,140);put16(b,142,7);
    const unsigned ids[]={11,34,255,1,42,43,108},major[]={11,3,13,13,6,6,4};
    for(unsigned n=0;n<7;++n) {
        const size_t p=144+n*40;put16(b,p,uint16_t(ids[n]));b[p+3]=8;b[p+4]=uint8_t(major[n]);
        if(n==6)b[p+5]=3;
        for(unsigned j=0;j<8;++j)put32(b,p+8+j*4,(n+1)*0x1000+j*0x100);
    }
    put32(b,144+6*40+12,0x14); // RSMU window
    put32(b,144+6*40+16,indirect?0x20000:0xd20); // NBIF BASE2
    fix(b);return b;
}
Layout layout(bool indirect=false) {
    const auto d=discovery(indirect);Layout out{};
    CHECK(buildLayout(d.data(),d.size(),0x80000,out)==Status::Ok);
    CHECK(out.gbAddrConfig==(0x1000+0x13de)*4);
    CHECK(out.revisionStrap==((indirect?0x20000:0xd20)+0x16)*4);
    CHECK(out.index==0x50 && out.data==0x54 && out.indexHi==0x58);return out;
}
bool zero(const Snapshot& s) {
    Snapshot expected{};return !memcmp(&s,&expected,sizeof(s));
}
struct Event {char kind;uint32_t address,value;};
struct Harness {
    Layout l;
    Identity id{0x744c1002,0x030000c8,0x10021002,0,0xe0000000,6,0,0};
    uint32_t gb=0x344,strap=0x01000000,high=0,selector=0;
    unsigned calls=0,failAt=0,changeAt=0,onesAt=0;
    uint32_t changeMask=0;
    char changedKind='g';
    bool failed=false;
    std::vector<Event> events;
    explicit Harness(bool indirect=false):l(layout(indirect)) {}
    bool step(char kind,uint32_t address,uint32_t value=0) {
        CHECK(!failed);events.push_back({kind,address,value});++calls;
        if(calls==failAt){failed=true;return false;}return true;
    }
    static bool identity(void* c,Identity& out) {
        auto& h=*static_cast<Harness*>(c);if(!h.step('p',0))return false;
        out=h.id;
        if(h.calls==h.changeAt && h.changedKind=='p')out.subsystem^=h.changeMask;
        if(h.calls==h.onesAt)out.vendorDevice=UINT32_MAX;
        return true;
    }
    static bool read(void* c,uint32_t address,uint32_t& out) {
        auto& h=*static_cast<Harness*>(c);if(!h.step('r',address))return false;
        if(address==h.l.gbAddrConfig)out=h.gb;
        else if(address==h.l.revisionStrap)out=h.strap;
        else if(address==h.l.indexHi)out=h.high;
        else if(address==h.l.index)out=h.selector;
        else {CHECK(address==h.l.data && h.selector==h.l.revisionStrap);out=h.strap;}
        if(h.calls==h.changeAt)out^=h.changeMask;
        if(h.calls==h.onesAt)out=UINT32_MAX;
        return true;
    }
    static bool write(void* c,uint32_t address,uint32_t value) {
        auto& h=*static_cast<Harness*>(c);
        CHECK(address==h.l.index && value==h.l.revisionStrap);
        h.selector=value; // A failed write can already have reached hardware.
        return h.step('w',address,value);
    }
    Io io(){return {this,identity,read,write};}
    Status run(Snapshot& s,Trace& t){return capture(l,io(),s,t);}
};
void layoutTests() {
    Layout l=layout();const auto d=discovery(false);
    for(size_t n=0;n<d.size();++n){CHECK(buildLayout(d.data(),n,0x80000,l)==Status::DiscoveryRejected);CHECK(!l.barBytes);}
    for(unsigned n=0;n<7;++n)for(unsigned field: {2u,4u,5u,6u,7u}) {
        auto bad=d;bad[144+n*40+field]^=1;fix(bad);
        CHECK(buildLayout(bad.data(),bad.size(),0x80000,l)!=Status::Ok);CHECK(!l.barBytes);
    }
    for(unsigned index: {0u,1u,2u}) {
        auto bad=d;put32(bad,144+(index?6*40:0)+8+(index==1?4:index==2?8:0),UINT32_MAX);fix(bad);
        CHECK(buildLayout(bad.data(),bad.size(),0x80000,l)!=Status::Ok);CHECK(!l.barBytes);
    }
    CHECK(buildLayout(d.data(),d.size(),0x50,l)!=Status::Ok);
    Harness h;Snapshot s{};Trace t{};
    for(unsigned i=0;i<5;++i) for(uint32_t address: {0u,4u,0x18u,0x7f000u,0x7fffcu,3u}) {
        Layout bad=h.l;
        switch(i){case 0:bad.gbAddrConfig=address;break;case 1:bad.revisionStrap=address;break;
        case 2:bad.index=address;break;case 3:bad.data=address;break;default:bad.indexHi=address;break;}
        CHECK(capture(bad,h.io(),s,t)==Status::InvalidLayout);CHECK(zero(s));CHECK(h.calls==0);
    }
    Layout bad=h.l;bad.revisionStrap=bad.index;
    CHECK(capture(bad,h.io(),s,t)==Status::InvalidLayout);
    for(unsigned n=0;n<3;++n){Io io=h.io();if(n==0)io.identity=nullptr;else if(n==1)io.read32=nullptr;else io.write32=nullptr;
        CHECK(capture(h.l,io,s,t)==Status::InvalidArgument);CHECK(h.calls==0);}
}
void captures() {
    for(bool indirect: {false,true}) {
        Harness h(indirect);Snapshot s{};Trace t{};CHECK(h.run(s,t)==Status::Ok);
        CHECK(s.topology.gb_addr_config==h.gb && s.topology.revision_strap==h.strap);
        CHECK(t.identitiesAttempted==4 && t.snapshotsCompleted==2);
        CHECK(t.readsAttempted==(indirect?8u:4u));
        CHECK(t.selectorWritesAttempted==(indirect?2u:0u));
        CHECK(t.selectorWritesCompleted==t.selectorWritesAttempted && t.postingReadsCompleted==t.selectorWritesAttempted);
        const unsigned count=h.calls;
        const unsigned stride=indirect?7:4;
        for(unsigned sample=0;sample<2;++sample) {
            const unsigned at=sample*stride;
            CHECK(h.events[at].kind=='p' && h.events[at+1].address==h.l.gbAddrConfig);
            if(indirect) {
                CHECK(h.events[at+2].kind=='r' && h.events[at+2].address==h.l.indexHi);
                CHECK(h.events[at+3].kind=='w' && h.events[at+3].address==h.l.index);
                CHECK(h.events[at+4].kind=='r' && h.events[at+4].address==h.l.index);
                CHECK(h.events[at+5].kind=='r' && h.events[at+5].address==h.l.data);
            } else CHECK(h.events[at+2].kind=='r' && h.events[at+2].address==h.l.revisionStrap);
            CHECK(h.events[at+stride-1].kind=='p');
        }
        for(unsigned n=1;n<=count;++n) {
            Harness failed(indirect);failed.failAt=n;
            CHECK(failed.run(s,t)==Status::TransportError);CHECK(failed.calls==n);CHECK(zero(s));
            if(failed.events.back().kind=='w')CHECK(t.selectorWritesAttempted==t.selectorWritesCompleted+1);
        }
        for(unsigned n=1;n<=count;++n) {
            if(h.events[n-1].kind=='w')continue;
            Harness bad(indirect);bad.onesAt=n;CHECK(bad.run(s,t)!=Status::Ok);CHECK(zero(s));CHECK(bad.calls==n);
        }
        // Full raw equality includes unused/address-configuration bits, not
        // just the extracted packer count and ASIC revision fields.
        const unsigned secondGb=indirect?9:6,secondStrap=indirect?13:7;
        for(unsigned bit=0;bit<32;++bit)for(unsigned at: {secondGb,secondStrap}) {
            Harness changed(indirect);changed.changeAt=at;changed.changeMask=1u<<bit;
            CHECK(changed.run(s,t)!=Status::Ok);CHECK(zero(s));
        }
        for(unsigned n=0;n<count;++n)if(h.events[n].kind=='p')for(unsigned bit=0;bit<32;++bit) {
            Harness changed(indirect);changed.changedKind='p';changed.changeAt=n+1;changed.changeMask=1u<<bit;
            CHECK(changed.run(s,t)==Status::Changed);CHECK(zero(s));
        }
    }
    for(unsigned bit=0;bit<32;++bit) {
        Harness h(true);h.high=1u<<bit;Snapshot s{};Trace t{};
        CHECK(h.run(s,t)==Status::IndirectHighNotZero);CHECK(t.selectorWritesAttempted==0);CHECK(zero(s));
    }
    for(unsigned field=0;field<8;++field) {
        Harness h;Snapshot s{};Trace t{};
        switch(field) {
        case 0:h.id.vendorDevice=0x744d1002;break;
        case 1:h.id.revisionClass=0x030000c9;break;
        case 2:h.id.subsystem=UINT32_MAX;break;
        case 3:h.id.bar4=4;break;
        case 4:h.id.bar5=0;break;
        case 5:h.id.command=2;break;
        case 6:h.id.headerType=1;break;
        default:h.id.reserved=1;break;
        }
        CHECK(h.run(s,t)==Status::DeviceUnavailable);CHECK(h.calls==1);CHECK(zero(s));
    }
    // Every admitted numeric address profile/revision passes the protocol;
    // these are host inputs, not claimed observations of any GPU.
    for(unsigned pipe=0;pipe<=6;++pipe)for(unsigned pack=0;pack<=5;++pack)for(unsigned rev=0;rev<16;++rev) {
        Harness h;h.gb=pipe|(pack<<8);h.strap=rev<<24;Snapshot s{};Trace t{};
        const bool expected=h.gb && pack<=pipe && pipe-pack<=2 && rev<15;
        CHECK((h.run(s,t)==Status::Ok)==expected);if(!expected)CHECK(zero(s));
    }
}
}
int main(){layoutTests();captures();printf("gfx11_color_topology: %u checks, %u failures\n",checks,failures);return failures?1:0;}
