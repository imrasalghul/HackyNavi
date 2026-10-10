// SPDX-License-Identifier: MIT
// Host MMIO protocol tests only. No GPU execution, measured clocks or modeset.
#include "hackynavi/display_bandwidth.hpp"
#include <iostream>
#include <map>
#include <vector>
namespace h=hackynavi::displaybandwidth;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}} while(0)
void put16(std::vector<uint8_t>& b,size_t p,uint16_t v){b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
void put32(std::vector<uint8_t>& b,size_t p,uint32_t v){put16(b,p,uint16_t(v));put16(b,p+2,uint16_t(v>>16));}
uint16_t sum(const std::vector<uint8_t>& b,size_t p){uint16_t s=0;for(;p<b.size();++p)s=uint16_t(s+b[p]);return s;}
void checksums(std::vector<uint8_t>& b){put16(b,14,sum(b,60));put16(b,8,sum(b,10));}
std::vector<uint8_t> discovery(){
    std::vector<uint8_t> b(164,0);put32(b,0,0x28211407);put16(b,4,1);put16(b,6,4);put16(b,10,164);
    put16(b,12,60);put16(b,16,104);put32(b,60,0x53445049);put16(b,64,3);put16(b,66,104);
    put16(b,72,1);put16(b,76,140);put16(b,142,1);put16(b,144,271);b[147]=3;b[148]=3;b[149]=2;
    put32(b,160,0x34c0);checksums(b);return b;
}
uint32_t& field(h::Input& in,h::Field f){return in.fields[size_t(f)];}
h::Input input(){
    h::Input in={};in.profile=h::Profile::SingleLinearRgb32DirectVram;in.identity={1,2,3,4};
    in.fieldCount=h::kFieldCount;in.detKbytes=1152;in.compbufKbytes=128;
    in.width=1920;in.height=1080;in.htotal=2200;in.vtotal=1125;in.vblankEnd=41;
    in.vstartupStart=44;in.vreadyOffset=328;in.vupdateWidth=247;in.vupdateOffset=550;
    in.dccgReferenceKHz=100000;in.dchubReferenceKHz=50000;in.timerDivider=2;
    field(in,h::Field::dlg_dlg_vblank_end)=41;
    field(in,h::Field::dlg_refcyc_per_htotal)=189629;
    field(in,h::Field::dlg_ref_freq_to_pix_freq)=176527;
    field(in,h::Field::dlg_min_dst_y_next_start)=4488;
    field(in,h::Field::dlg_vratio_prefetch)=599186;
    field(in,h::Field::dlg_refcyc_per_line_delivery_l)=646;
    field(in,h::Field::ttu_refcyc_per_req_delivery_l)=21504;
    field(in,h::Field::ttu_min_ttu_vblank)=29282;
    field(in,h::Field::ttu_qos_level_high_wm)=2962;
    // Protocol fixture: distinct explicit WM policy records, not target data.
    for(uint32_t n=0;n<4;++n)in.watermarks[n]={100+n,5615+n*100,123+n,234+n,4000+n*100,
        10000+n*100,9000+n*100,50000+n*100,60000+n*100,4000+n*100};
    return in;
}
const h::Poll poll={3,10};
struct Event { bool write;uint32_t offset,value; };
struct Device {
    h::Layout layout={};h::Controller controller;h::Input in=input();h::Plan plan;
    std::map<uint32_t,uint32_t> regs;std::vector<Event> events;
    unsigned enters=0,leaves=0,writes=0,calls=0,delays=0,failWrite=0,dropWrite=0,failRead=0,revokeWrite=0;
    unsigned changeRead=0;uint8_t changePipe=0;bool changeConfig=false;
    bool entered=false,admit=true,live=true,reflect=true,configError=false;
    explicit Device(uint8_t pipe=0){
        auto d=discovery();CHECK(h::buildLayout(d.data(),d.size(),0x100000,layout)==h::Status::Ok);
        in.pipe=pipe;CHECK(h::plan(in,plan)==h::Status::Ok);
        // Entire exact DCN block is backed by a host map; accesses are logged.
        for(uint32_t off=0x80;off<0x1d90;++off)regs[off]=0;
        for(uint8_t p=0;p<4;++p){
            regs[0x80+p*2]=p==pipe?0:0x100;regs[0x81+p*2]=p==pipe?0:0x90000000;
            regs[0x5f3+p*0xdc]=0xb;regs[0x5f4+p*0xdc]=1;
            regs[0x642+p*0xdc]=0x19;regs[0x5e5+p*0xdc]=8;
            regs[0x5ea+p*0xdc]=in.width|(in.height<<16);
        }
        regs[0x525]=0x1002;
        regs[0x4ba]=0x404; // previous compbuf 4 + all four DETs 4 =20 segments
        for(unsigned n=0;n<4;++n)regs[0x4bb+n]=0x404;
    }
    h::Io io(){return {this,enter,leave,valid,read,write,delay};}
    static bool enter(void* c,const h::Plan& plan){auto& d=*static_cast<Device*>(c);++d.enters;CHECK(!d.entered);
        if(!d.admit)return false;d.entered=true;CHECK(plan.identity().calculationId==4);return true;}
    static void leave(void* c){auto& d=*static_cast<Device*>(c);CHECK(d.entered);d.entered=false;++d.leaves;}
    static bool valid(void* c,const h::Identity&){auto& d=*static_cast<Device*>(c);CHECK(d.entered);return d.live;}
    static bool read(void* c,uint32_t a,uint32_t& v){
        auto& d=*static_cast<Device*>(c);CHECK(d.entered);++d.calls;
        CHECK(a>=d.layout.baseDwords*4&&!(a&3));const uint32_t off=a/4-d.layout.baseDwords;
        CHECK(d.regs.count(off)==1);
        if(d.calls==d.changeRead)d.regs[(d.changeConfig?0x80:0x81)+d.changePipe*2]^=d.changeConfig?1u:0x10000000u;
        for(unsigned p=0;p<4;++p)if(d.regs[0x81+p*2]&0x80000000)
            CHECK(!(off>=0x5e5+p*0xdc&&off<=0x6c0+p*0xdc));
        v=d.regs[off];d.events.push_back({false,off,v});return d.calls!=d.failRead;
    }
    static bool write(void* c,uint32_t a,uint32_t v){
        auto& d=*static_cast<Device*>(c);CHECK(d.entered);++d.writes;
        const uint32_t off=a/4-d.layout.baseDwords;CHECK(d.regs.count(off)==1);
        d.events.push_back({true,off,v});
        if(d.writes!=d.dropWrite){
            const uint32_t old=d.regs[off];d.regs[off]=v;
            if(off>=0x4ba&&off<=0x4be){
                // SIZE_CURRENT is read-only and responds separately from SIZE.
                d.regs[off]=(v&~0x1f00u)|(d.reflect?(v&31)<<8:old&0x1f00);
                if(off==0x4ba){CHECK(!(v&0x80080000));if(d.configError)d.regs[off]|=0x80000000;}
                unsigned total=0;for(uint32_t r=0x4ba;r<=0x4be;++r)total+=(d.regs[r]>>8)&31;
                CHECK(total<=20);
            }
        }
        if(d.writes==d.revokeWrite)d.live=false;
        return d.writes!=d.failWrite; // ambiguous failure delivers the write
    }
    static void delay(void* c,uint32_t us){auto& d=*static_cast<Device*>(c);CHECK(d.entered);CHECK(us==10);++d.delays;}
    h::Status configure(h::Result& r){return controller.configure(layout,io(),plan,poll,r);}
};
void validation(){
    h::Plan p;auto in=input();CHECK(h::plan(in,p)==h::Status::Ok);CHECK(p.valid()&&p.watermarkSource(3)==103);
    auto bad=in;bad.fieldCount--;CHECK(h::plan(bad,p)==h::Status::InvalidPlan&&!p.valid());
    bad=in;bad.profile=h::Profile(2);CHECK(h::plan(bad,p)==h::Status::InvalidPlan);
    bad=in;bad.detKbytes=1153;CHECK(h::plan(bad,p)==h::Status::InvalidPlan);
    bad=in;bad.compbufKbytes=192;CHECK(h::plan(bad,p)==h::Status::InvalidPlan);
    bad=in;bad.watermarks[1].sourceId=0;CHECK(h::plan(bad,p)==h::Status::InvalidPlan);
    bad=in;bad.watermarks[1].sourceId=bad.watermarks[0].sourceId;CHECK(h::plan(bad,p)==h::Status::InvalidPlan);
    bad=in;bad.watermarks[0].urgentNs=UINT32_MAX;CHECK(h::plan(bad,p)==h::Status::FieldOverflow);
    bad=in;bad.watermarks[3].fracUrgentNomPerThousand=1001;CHECK(h::plan(bad,p)==h::Status::InvalidPlan);
    bad=in;bad.dchubReferenceKHz=100000;CHECK(h::plan(bad,p)==h::Status::InvalidPlan);
    bad=in;bad.timerDivider=1;CHECK(h::plan(bad,p)==h::Status::InvalidPlan);
    bad=in;bad.vstartupStart=1;bad.vreadyOffset=32767;CHECK(h::plan(bad,p)==h::Status::InvalidPlan);
    bad=in;field(bad,h::Field::rq_rq_regs_l_dpte_group_size)=4294966267u;
    CHECK(h::plan(bad,p)==h::Status::FieldOverflow); // actual disabled-path DML overflow found during review
    bad=in;field(bad,h::Field::rq_plane1_base_address)=1;CHECK(h::plan(bad,p)==h::Status::InvalidPlan);
    auto bytes=discovery();h::Layout l={};CHECK(h::buildLayout(bytes.data(),bytes.size(),4,l)==h::Status::OutsideBar);
    bytes[149]=1;checksums(bytes);CHECK(h::buildLayout(bytes.data(),bytes.size(),0x100000,l)==h::Status::UnsupportedDmu);
}
void success(){
    for(uint8_t pipe=0;pipe<4;++pipe){
        Device d(pipe);h::Result r={};const uint32_t p=pipe*0xdc;
        d.regs[0x66c+p]=0xa5a50000; // preserve crossbar and all non-plane-base fields
        d.regs[0x4fe]=0x40000000;d.regs[0x4c4]=0xf000f000;d.regs[0x5f8+p]=0x40000000;
        CHECK(d.configure(r)==h::Status::Ok&&r.writeAttempted&&r.retentionRequired);
        CHECK(d.controller.phase()==h::Phase::Configured&&d.enters==d.leaves&&!d.entered);
        CHECK(d.regs[0x4ba]==0x202&&d.regs[0x4bb+pipe]==0x1212);
        for(uint32_t q=0;q<4;++q)if(q!=pipe)CHECK(d.regs[0x4bb+q]==0);
        CHECK(d.regs[0x4fe]==(0x40000000u|280u)); // trunc(5615*50/1000)
        CHECK((d.regs[0x507]&0x3fff)==285); // independently supplied B
        CHECK((d.regs[0x510]&0x3fff)==290);CHECK((d.regs[0x519]&0x3fff)==295);
        CHECK(d.regs[0x4c4]==0xf040f100);CHECK(d.regs[0x5f8+p]==0x40000100);
        CHECK((d.regs[0x66c+p]&~0x7ff0u)==(0xa5a50000u&~0x7ff0u));
        CHECK((d.regs[0x5f3+p]&0xf)==0xb); // remains blank/drained; never enables OTG
        const auto writes=d.writes;CHECK(d.controller.retire(r)==h::Status::Ok&&!r.retentionRequired);
        CHECK(d.writes==writes&&d.controller.phase()==h::Phase::Retired);
    }
}
void powerStates(){
    for(uint8_t pipe=0;pipe<4;++pipe)for(uint32_t on:{0u,0x10000000u})for(uint32_t config:{0u,1u})
        for(uint32_t off:{0x80000000u,0x90000000u}){
            Device d(pipe);for(uint8_t p=0;p<4;++p)d.regs[0x81+p*2]=p==pipe?on:off;
            d.regs[0x80+pipe*2]=config;h::Result r={};
            CHECK(d.configure(r)==h::Status::Ok&&r.retentionRequired);CHECK(d.controller.retire(r)==h::Status::Ok);
        }
    // Desired-bit changes on OFF domains still reject, without reading that
    // domain's gated HUBP window. CONFIG changes remain exact, not masked.
    for(uint8_t changed=0;changed<4;++changed)for(bool config:{false,true}){
        Device d;h::Result r={};CHECK(d.configure(r)==h::Status::Ok);const auto writes=d.writes;
        d.regs[(config?0x80:0x81)+changed*2]^=config?1u:0x10000000u;
        CHECK(d.controller.retire(r)==h::Status::PowerUnavailable&&r.retentionRequired&&d.writes==writes);
    }
    for(bool config:{false,true})for(unsigned at:{3u,4u,5u,8u,40u,100u,200u}){
        Device d;d.changeRead=at;d.changeConfig=config;h::Result r={};
        CHECK(d.configure(r)==h::Status::PowerUnavailable);CHECK(r.retentionRequired==(d.writes!=0));
    }
    for(unsigned bit=0;bit<32;++bit)if(bit!=28){Device d;d.regs[0x81]=1u<<bit;h::Result r={};
        CHECK(d.configure(r)==h::Status::PowerUnavailable&&!r.writeAttempted);}
    for(unsigned bit=1;bit<32;++bit){Device d;d.regs[0x80]=1u<<bit;h::Result r={};
        CHECK(d.configure(r)==h::Status::PowerUnavailable&&!r.writeAttempted);}
}
void failuresAndOwnership(){
    h::Result r={};Device denied;denied.admit=false;
    CHECK(denied.configure(r)==h::Status::OwnershipRejected&&denied.events.empty()&&!r.retentionRequired);
    Device live;live.regs[0x1b41]=1;CHECK(live.configure(r)==h::Status::ActivePipe&&!r.writeAttempted);
    Device gated;gated.regs[0x80]=0x100;gated.regs[0x81]=0x90000000;
    CHECK(gated.configure(r)==h::Status::PowerUnavailable&&!r.writeAttempted);
    Device transitional;transitional.regs[0x81]=0x40000000;
    CHECK(transitional.configure(r)==h::Status::PowerUnavailable&&!r.writeAttempted);
    Device timer;timer.regs[0x525]=2;CHECK(timer.configure(r)==h::Status::ClockUnavailable&&!r.writeAttempted);
    Device foreign;foreign.regs[0x1b8a]=1;CHECK(foreign.configure(r)==h::Status::ForeignControl&&!r.writeAttempted);
    Device dimensions;dimensions.regs[0x5ea]=1;CHECK(dimensions.configure(r)==h::Status::ForeignControl&&!r.writeAttempted);
    Device state;state.regs[0x4ba]=0x403;CHECK(state.configure(r)==h::Status::BufferState&&!r.writeAttempted);
    Device fault;fault.regs[0x4ba]|=0x80000000;CHECK(fault.configure(r)==h::Status::HardwareFault&&!r.writeAttempted);
    Device timeout;timeout.reflect=false;CHECK(timeout.configure(r)==h::Status::Timeout&&r.retentionRequired);
    CHECK(timeout.delays==2&&timeout.controller.phase()==h::Phase::Faulted);
    CHECK(timeout.controller.retire(r)==h::Status::BufferState&&r.retentionRequired);
    Device mismatch;mismatch.dropWrite=1;CHECK(mismatch.configure(r)==h::Status::VerifyFailed&&r.retentionRequired);
    Device removed;removed.regs[0x525]=UINT32_MAX;CHECK(removed.configure(r)==h::Status::DeviceUnavailable&&!r.writeAttempted);
    Device baseline;CHECK(baseline.configure(r)==h::Status::Ok);const auto count=baseline.writes;
    for(unsigned n=1;n<=count;++n){
        Device d;d.failWrite=n;CHECK(d.configure(r)==h::Status::TransportError&&r.retentionRequired);
        CHECK(d.writes==n&&d.enters==d.leaves&&d.controller.requiresHardwareRetirement());
    }
    Device revoked;revoked.revokeWrite=1;CHECK(revoked.configure(r)==h::Status::Revoked&&r.retentionRequired);
    CHECK(revoked.writes==1&&revoked.enters==revoked.leaves);
    Device failedRead;failedRead.failRead=1;CHECK(failedRead.configure(r)==h::Status::TransportError&&!r.retentionRequired);
    Device afterWrite;afterWrite.failRead=baseline.calls-1;
    CHECK(afterWrite.configure(r)==h::Status::TransportError&&r.retentionRequired);
}
}
int main(){powerStates();validation();success();failuresAndOwnership();std::cout<<checks<<" checks, "<<failures<<" failures; host protocol only\n";return failures?1:0;}
