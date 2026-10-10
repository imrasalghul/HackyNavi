// SPDX-License-Identifier: MIT
// Pure protocol/fault tests. Optional actual ROM discovery input is metadata,
// not observed hardware harvesting. There is no GPU backend in this test.
#include "hackynavi/gfx11_graphics_topology.hpp"
#include <stdio.h>
#include <string.h>
#include <fstream>
#include <iterator>
#include <vector>
using namespace hackynavi::graphicstopology;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("line %u: %s\n",unsigned(__LINE__),#x);}}while(0)
const Metadata declaration={6,2,4,4};
const Layout locations={0x100,0x104,0x108,0x10c,0x110,0x114,0x118,0x80000};
struct Event {bool write;uint32_t address,value;};
struct Model {
    uint32_t selector=0xe0000000,saHw=0,saUser=0,rbHw=0,rbUser=0;
    uint32_t hw[kSlots]={},user[kSlots]={};
    unsigned calls=0,failAt=0,changeAt=0,onesAt=0;uint32_t changeMask=0;
    bool failed=false;std::vector<Event> events;
    bool step(bool write,uint32_t address,uint32_t value) {
        CHECK(!failed);events.push_back({write,address,value});++calls;
        if(calls==failAt){failed=true;return false;}return true;
    }
    static bool read(void* p,uint32_t a,uint32_t& v) {
        auto& m=*static_cast<Model*>(p);if(!m.step(false,a,0))return false;
        if(a==locations.selector)v=m.selector;
        else if(a==locations.saHardware)v=m.saHw;else if(a==locations.saUser)v=m.saUser;
        else if(a==locations.rbHardware)v=m.rbHw;else if(a==locations.rbUser)v=m.rbUser;
        else {
            CHECK((m.selector&0xe0000000)==0x40000000);
            const unsigned se=(m.selector>>16)&255,sa=(m.selector>>8)&255;
            CHECK(se<6 && sa<2);CHECK(!(((m.saHw|m.saUser)>>8)&(1u<<(se*2+sa))));
            CHECK(a==locations.wgpHardware || a==locations.wgpUser);
            v=a==locations.wgpHardware?m.hw[se*2+sa]:m.user[se*2+sa];
        }
        if(m.calls==m.changeAt)v^=m.changeMask;
        if(m.calls==m.onesAt)v=UINT32_MAX;
        return true;
    }
    static bool write(void* p,uint32_t a,uint32_t v) {
        auto& m=*static_cast<Model*>(p);CHECK(a==locations.selector);CHECK(validSelector(v,declaration));
        m.selector=v; // a reported failure may already have reached the selector
        return m.step(true,a,v);
    }
    Status run(Snapshot& s,Trace& t){return capture(declaration,locations,{this,read,write},s,t);}
};
bool zero(const Snapshot& s){Snapshot z{};return !memcmp(&z,&s,sizeof(s));}
void protocol() {
    Model original;Snapshot s{};Trace t{};CHECK(original.run(s,t)==Status::Ok);
    CHECK(t.sweepsCompleted==2 && t.restoreWritesAttempted==2 && t.restoreWritesCompleted==2);
    CHECK(t.selectorWritesAttempted==26 && t.selectorWritesCompleted==26);
    CHECK(original.selector==0xe0000000 && s.raw.observedSa==4095);
    CHECK(s.facts.maxSe==6 && s.facts.maxSaPerSe==2 && s.facts.maxRenderBackends==24);
    CHECK(s.facts.activeCu==96 && s.facts.activeSe==6 && s.facts.minGoodCuPerSa==8 && s.facts.activeRbMask==0xffffff);
    for(unsigned n=0;n<12;++n)CHECK(s.facts.cuMask[n]==255);
    const unsigned count=original.calls;
    for(unsigned n=1;n<=count;++n) {
        Model m;m.failAt=n;CHECK(m.run(s,t)==Status::TransportError);CHECK(m.calls==n);CHECK(zero(s));
        if(m.events.back().write)CHECK(t.selectorWritesAttempted==t.selectorWritesCompleted+1);
    }
    for(unsigned n=1;n<=count;++n)if(!original.events[n-1].write) {
        Model m;m.onesAt=n;CHECK(m.run(s,t)==Status::DeviceUnavailable);CHECK(m.calls==n);CHECK(zero(s));
        // Any failed read, including post-write readback, must be the LAST
        // callback. There is no attempted best-effort selector restoration.
        CHECK(!m.events.back().write);
    }
    // Full raw stability covers reserved fields as well as harvested bits.
    // Changing one observation can fail immediately or after the second
    // complete sweep, but never publishes the changed candidate.
    for(unsigned n=1;n<=count;++n)if(!original.events[n-1].write)for(unsigned bit=0;bit<32;++bit) {
        Model m;m.changeAt=n;m.changeMask=1u<<bit;
        const Status status=m.run(s,t);CHECK(status!=Status::Ok);CHECK(zero(s));
    }
    // A caller's original selector can be specific or broadcast. Restore the
    // exact validated value, never assume Linux's usual broadcast default.
    for(unsigned se=0;se<6;++se)for(unsigned sa=0;sa<2;++sa)for(uint32_t instance:{0u,0x40000000u}) {
        Model m;m.selector=instance|(se<<16)|(sa<<8);const auto prior=m.selector;
        CHECK(m.run(s,t)==Status::Ok);CHECK(m.selector==prior);CHECK(t.originalSelector==prior);
    }
    for(uint32_t bad:{1u,0x1000000u,0x60000u,0x200u,0x80010000u,0x20000100u}) {
        Model m;m.selector=bad;CHECK(m.run(s,t)==Status::InvalidSelector);CHECK(m.calls==1);CHECK(t.selectorWritesAttempted==0);
    }
    Model absent;absent.saHw=0xfff00;CHECK(absent.run(s,t)==Status::NoActiveUnits);
    CHECK(t.selectorWritesAttempted==0 && t.sweepsCompleted==2);CHECK(zero(s));
}
void harvesting() {
    // Every whole-SE and individual-SA subset: compare raw-mask derivation
    // against independently enumerated CU and RB sets, including no units.
    for(unsigned enabled=0;enabled<4096;++enabled) {
        Model m;m.saHw=(~enabled&0xfff)<<8;
        for(unsigned slot=0;slot<12;++slot) {
            m.hw[slot]=(1u<<(slot%4))<<16;
            m.user[slot]=(slot%3==0?2u:0u)<<16;
        }
        unsigned cu=0,seCount=0,rbMask=0;
        for(unsigned se=0;se<6;++se) {
            bool active=false;
            for(unsigned sa=0;sa<2;++sa) {
                const unsigned slot=se*2+sa;if(!(enabled&(1u<<slot)))continue;
                active=true;rbMask|=3u<<(slot*2);
                for(unsigned w=0;w<4;++w)if(!(((m.hw[slot]|m.user[slot])>>16)&(1u<<w)))cu+=2;
            }
            seCount+=unsigned(active);
        }
        Snapshot s{};Trace t{};const auto status=m.run(s,t);
        if(!enabled){CHECK(status==Status::NoActiveUnits);continue;}
        CHECK(status==Status::Ok);CHECK(s.facts.activeCu==cu && s.facts.activeSe==seCount);
        CHECK(s.facts.activeRbMask==rbMask && s.facts.activeSaMask==enabled);
        CHECK(s.facts.minGoodCuPerSa==(cu/(seCount*4))*2);
    }
    for(unsigned bit=0;bit<24;++bit) {
        Model m;m.rbHw=1u<<(bit+4);Snapshot s{};Trace t{};
        CHECK(m.run(s,t)==Status::Ok);CHECK(s.facts.activeRbMask==(0xffffffu&~(1u<<bit)));
    }
}
void metadata(const char* path) {
    std::ifstream file(path,std::ios::binary);std::vector<uint8_t> b((std::istreambuf_iterator<char>(file)),{});
    CHECK(file.good() || file.eof());CHECK(b.size()==1756);if(b.size()!=1756)return;
    Metadata m{};Layout l{};CHECK(build(b.data(),b.size(),0x80000,m,l)==Status::Ok);
    CHECK(m.maxSe==6 && m.maxSaPerSe==2 && m.maxRbPerSe==4 && m.maxWgpPerSa==4);
    for(size_t n=0;n<b.size();++n){CHECK(build(b.data(),n,0x80000,m,l)!=Status::Ok);CHECK(!m.maxSe && !l.barBytes);}
    // The complete real binary checksum and table schema both matter.
    for(size_t n=1344;n<1476;++n){auto bad=b;bad[n]^=1;CHECK(build(bad.data(),bad.size(),0x80000,m,l)!=Status::Ok);}
    const auto put16=[](std::vector<uint8_t>& v,size_t p,uint16_t value){v[p]=uint8_t(value);v[p+1]=uint8_t(value>>8);};
    const auto put32=[&](std::vector<uint8_t>& v,size_t p,uint32_t value){put16(v,p,uint16_t(value));put16(v,p+2,uint16_t(value>>16));};
    const auto fix=[&](std::vector<uint8_t>& v){uint16_t sum=0;for(size_t p=1344;p<1476;++p)sum=uint16_t(sum+v[p]);
        put16(v,22,sum);sum=0;for(size_t p=10;p<v.size();++p)sum=uint16_t(sum+v[p]);put16(v,8,sum);};
    // Repair real checksums to reach the schema/arithmetic checks, rather
    // than allowing corruption detection to hide an invalid metadata field.
    for(unsigned field:{0u,4u,8u,12u,16u,20u,24u,76u})for(uint32_t value:{0u,UINT32_MAX}) {
        if((field==16 && value==0) || (field==20 && value==0))continue;
        auto bad=b;put32(bad,1344+field,value);fix(bad);
        CHECK(build(bad.data(),bad.size(),0x80000,m,l)==Status::UnsupportedMetadata);CHECK(!m.maxSe && !l.barBytes);
    }
    for(const auto pair: {std::pair<unsigned,uint32_t>{12,9},{76,3},{24,1},{16,17},{16,16}}) {
        auto bad=b;put32(bad,1344+pair.first,pair.second);
        if(pair.first==16 && pair.second==16)put32(bad,1344+20,1);
        fix(bad);CHECK(build(bad.data(),bad.size(),0x80000,m,l)==Status::UnsupportedMetadata);
    }
    CHECK(build(b.data(),b.size(),0x100,m,l)==Status::InvalidLayout);CHECK(!m.maxSe && !l.barBytes);
}
void preflight() {
    Snapshot s{};Trace t{};Model model;const Io io={&model,Model::read,Model::write};
    for(unsigned field=0;field<4;++field)for(uint32_t value:{0u,UINT32_MAX}) {
        Metadata m=declaration;if(field==0)m.maxSe=value;else if(field==1)m.maxSaPerSe=value;
        else if(field==2)m.maxRbPerSe=value;else m.maxWgpPerSa=value;
        CHECK(capture(m,locations,io,s,t)==Status::UnsupportedMetadata);CHECK(zero(s));CHECK(model.calls==0);
    }
    for(unsigned field=0;field<7;++field)for(uint32_t value:{0u,3u,4u,0x18u,0x7f000u,UINT32_MAX}) {
        Layout l=locations;uint32_t* fields[]={&l.selector,&l.saHardware,&l.saUser,&l.rbHardware,&l.rbUser,&l.wgpHardware,&l.wgpUser};
        *fields[field]=value;CHECK(capture(declaration,l,io,s,t)==Status::InvalidLayout);CHECK(model.calls==0);
    }
    Layout l=locations;l.saHardware=l.selector;
    CHECK(capture(declaration,l,io,s,t)==Status::InvalidLayout);CHECK(model.calls==0);
    CHECK(capture(declaration,locations,{nullptr,nullptr,nullptr},s,t)==Status::InvalidArgument);
}
}
int main(int argc,char** argv){preflight();protocol();harvesting();if(argc==2)metadata(argv[1]);
    printf("gfx11_graphics_topology: %u checks, %u failures; real metadata %s\n",checks,failures,argc==2?"checked":"not supplied");return failures?1:0;}
