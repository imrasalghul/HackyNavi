// SPDX-License-Identifier: MIT
// Offline protocol fault injection only; no hardware or successful GPU output.
#include "hackynavi/bootstrap_quiesce.hpp"
#include <iostream>
#include <map>
#include <vector>
using namespace hackynavi;
using namespace hackynavi::bootstrap;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do { ++checks; if (!(x)) {++failures;std::cerr<<__LINE__<<": " #x "\n";} } while(0)
Navi31IpLayout layout() {
    Navi31IpLayout l={};l.valid=true;const uint16_t ids[]={11,34,255,1,42,43};
    const uint8_t majors[]={11,3,13,13,6,6};
    for(unsigned i=0;i<6;++i)l.blocks[i]={ids[i],0,0,majors[i],0,0,2,{0x1260,0xa000}};
    l.blocks[1].basesDwords[0]=0x1a000;l.blocks[2].basesDwords[0]=0x16000;return l;
}
std::vector<uint32_t> addresses() {
    std::vector<uint32_t> out;
    const uint32_t controls[]={0x91,0x803,0x802,0x2904,0x2903,0x2a00,0x2807,0xf40,0xda4};
    for(unsigned i=0;i<9;++i)out.push_back(((i==0?0x16000:i<=6?0xa000:0x1260)+controls[i])*4);
    for(unsigned i=0;i<16;++i)out.push_back((0x1260+0x1688+i)*4);
    for(unsigned i=0;i<16;++i)out.push_back((0x1a000+0x740+i)*4);
    for(unsigned engine=0;engine<2;++engine){
        for(unsigned queue=0;queue<8;++queue){
            out.push_back((0x1260+engine*0x600+0x80+queue*0x58)*4);
            out.push_back((0x1260+engine*0x600+0x8a+queue*0x58)*4);
        }
        out.push_back((0xa000+0x589a+engine*0x20)*4);
        out.push_back((0x1260+0x25+engine*0x600)*4);
    }return out;
}
struct Harness {
    std::map<uint32_t,uint32_t> values;
    std::vector<uint32_t> visited;
    unsigned reads=0,validCalls=0,failRead=0,allOnesRead=0,revokeAt=0;
    bool finalSos=false;
    Harness(){auto a=addresses();for(auto v:a)values[v]=0;values[a[6]]=0x40000000;}
    static bool valid(void* p){auto& h=*static_cast<Harness*>(p);return ++h.validCalls!=h.revokeAt;}
    static bool read(void* p,uint32_t r,uint32_t& v){
        auto& h=*static_cast<Harness*>(p);++h.reads;h.visited.push_back(r);
        v=h.values[r];if(h.reads==h.allOnesRead)v=UINT32_MAX;
        if(h.finalSos&&h.reads==78)v=1;
        if(h.reads==h.failRead){v=0xa1234567;return false;}return true;
    }
    GuardInventoryIo io(){return {this,valid,read};}
};
bool bit(const uint32_t* words,unsigned i){return (words[i/32]&(1U<<(i%32)))!=0;}
unsigned bits(const uint32_t* words){unsigned n=0;for(unsigned i=0;i<96;++i)n+=bit(words,i);return n;}
GuardInventoryStatus run(Harness& h,GuardInventory& out){return captureGuardInventory(layout(),0x80000,h.io(),out);}
void golden(){
    Harness h;GuardInventory r;CHECK(run(h,r)==GuardInventoryStatus::Ok);
    CHECK(r.complete&&r.layoutStatus==Status::Ok&&r.reads==78&&h.reads==78&&h.validCalls==156);
    CHECK(r.architectureCompared&&r.architectureMatch&&!r.rs64);
    CHECK(r.sosAfterAttempted&&r.sosAfterReadSucceeded&&r.sosAfterValid&&r.sosAfter==0);
    CHECK(bits(r.attempted)==77&&bits(r.readSucceeded)==77&&bits(r.valid)==77);
    CHECK(bits(r.guardCompared)==62&&bits(r.guardMismatch)==0);
    auto a=addresses();CHECK(a.size()==77&&h.visited.size()==78);
    for(unsigned i=0;i<77;++i){CHECK(r.offsets[i]==a[i]);CHECK(h.visited[i]==a[i]);CHECK(r.values[i]==h.values[a[i]]);}
    CHECK(h.visited[77]==a[0]);CHECK(r.lastIndex==77&&r.lastOffset==a[0]&&r.lastReadSucceeded);
    h=Harness{};h.values[a[4]]=h.values[a[5]]=1;CHECK(run(h,r)==GuardInventoryStatus::Ok&&r.rs64&&r.architectureMatch);
}
void faults(){
    for(unsigned n=1;n<=78;++n)for(unsigned type=0;type<2;++type){
        Harness h;GuardInventory r;
        if(type)h.allOnesRead=n;else h.failRead=n;
        CHECK(run(h,r)==(type?GuardInventoryStatus::DeviceUnavailable:GuardInventoryStatus::ReadFailed));
        CHECK(!r.complete&&h.reads==n&&r.reads==n&&r.lastIndex==n-1);
        CHECK(bits(r.attempted)==(n<=77?n:77));
        CHECK(bits(r.valid)==n-1);
        CHECK(bits(r.readSucceeded)==(n<=77?n-1+type:77));
        if(n<=77){CHECK(r.values[n-1]==(type?UINT32_MAX:0));CHECK(!bit(r.valid,n-1));}
        else CHECK(r.sosAfterAttempted&&!r.sosAfterValid&&r.sosAfterReadSucceeded==bool(type));
        CHECK(r.lastReadSucceeded==bool(type));
        CHECK(r.lastValue==(type?UINT32_MAX:0));
    }
    for(unsigned n=1;n<=156;++n){
        Harness h;h.revokeAt=n;GuardInventory r;
        CHECK(run(h,r)==GuardInventoryStatus::AccessDenied);
        CHECK(!r.complete&&h.validCalls==n&&h.reads==n/2&&r.reads==n/2);
        CHECK(bits(r.valid)==((n-1)/2<77?(n-1)/2:77));
        CHECK(bits(r.attempted)==(n/2<77?n/2:77));
        CHECK(bits(r.readSucceeded)==(n/2<77?n/2:77));
        CHECK(r.lastReadSucceeded==(n%2==0));
    }
}
void semantics(){
    Harness base;GuardInventory wanted;CHECK(run(base,wanted)==GuardInventoryStatus::Ok);
    const auto a=addresses();
    for(unsigned i=1;i<77;++i)if(wanted.guardMasks[i]){
        for(unsigned b=0;b<32;++b)if(wanted.guardMasks[i]&(1U<<b)){
            Harness h;GuardInventory r;h.values[a[i]]^=1U<<b;
            CHECK(run(h,r)==GuardInventoryStatus::Ok&&r.complete&&r.reads==78);
            CHECK(bits(r.guardMismatch)==1&&bit(r.guardMismatch,i));
            CHECK(bits(r.guardCompared)==62); // Semantic failure does not hide later readable guards.
        }
    }
    Harness h;GuardInventory r;h.values[a[0]]=1;
    CHECK(run(h,r)==GuardInventoryStatus::PriorDriverRequiresReset&&h.reads==1&&!r.complete);
    CHECK(bit(r.valid,0)&&bit(r.guardMismatch,0)&&!r.sosAfterAttempted);
    h=Harness{};h.finalSos=true;CHECK(run(h,r)==GuardInventoryStatus::PriorDriverRequiresReset&&h.reads==78&&!r.complete);
    CHECK(r.sosAfterValid&&r.sosAfter==1&&bits(r.valid)==77);
    for(unsigned index=4;index<=5;++index){h=Harness{};h.values[a[index]]=1;
        CHECK(run(h,r)==GuardInventoryStatus::Ok&&r.complete&&r.architectureCompared&&!r.architectureMatch);}
    h=Harness{};for(unsigned i=1;i<77;++i)h.values[a[i]]|=0x80000000U;
    CHECK(run(h,r)==GuardInventoryStatus::Ok&&r.complete&&bits(r.guardMismatch)==0);
}
void arguments(){
    Harness h;GuardInventory r;auto l=layout();l.valid=false;
    CHECK(captureGuardInventory(l,0x80000,h.io(),r)==GuardInventoryStatus::LayoutRejected&&h.reads==0&&h.validCalls==0);
    CHECK(r.layoutStatus==Status::UnsupportedLayout&&bits(r.attempted)==0);
    CHECK(captureGuardInventory(layout(),4,h.io(),r)==GuardInventoryStatus::LayoutRejected&&r.layoutStatus==Status::OutsideBar);
    auto io=h.io();io.valid=nullptr;CHECK(captureGuardInventory(layout(),0x80000,io,r)==GuardInventoryStatus::InvalidArgument);
    io=h.io();io.read32=nullptr;CHECK(captureGuardInventory(layout(),0x80000,io,r)==GuardInventoryStatus::InvalidArgument);
    CHECK(h.reads==0&&h.validCalls==0);
}
}
int main(){golden();faults();semantics();arguments();std::cout<<checks<<" bootstrap inventory assertions, "<<failures<<" failures\n";return failures?1:0;}
