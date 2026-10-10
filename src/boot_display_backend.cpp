// SPDX-License-Identifier: MIT
// Register fields and topology: Linux 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0,
// dcn_3_2_0_offset.h/sh_mask.h, dcn32_optc.c, dcn10_mpc.c,
// dcn32_dio_stream_encoder.c and dcn10_link_encoder.c. See the source-pinned
// design document. No inherited controller is reconstructed and no write occurs.
#include "hackynavi/boot_display_backend.hpp"
namespace hackynavi { namespace bootbackend { namespace {
struct Dmu { unsigned count;bool valid;uint64_t pipe,mpc; };
void collect(const DiscoveryIp& ip,void* context) {
    if(ip.hardwareId!=271)return;
    auto& d=*static_cast<Dmu*>(context);++d.count;
    d.valid=!ip.dieId&&!ip.instance&&ip.major==3&&ip.minor==2&&!ip.revision&&
        !ip.subRevision&&!ip.variant&&!ip.harvest&&ip.baseAddressCount>=4;
    if(d.valid){d.pipe=ip.baseAddresses[2];d.mpc=ip.baseAddresses[3];}
}
const uint16_t linkOffsets[kLinkRegisters]={0x1f04,0x20b1,0x20b2};
const uint16_t frontendOffsets[kFrontendRegisters]={0x208b,0x2091,0x210c,0x2094,0x2098,0x209e};
const uint16_t timingOffsets[kTimingRegisters]={0x1b41,0x1b85,0x1b8a,0x1b91,0x1b8b,
    0x1b54,0x1b82,0x1b44,0x1aca,0x1acb,0x1acc,0x1ad0};
const uint16_t combinerOffsets[kCombinerRegisters]={0,1,2,5,0xe,0x3d8};
uint64_t address(const Dmu& d,Bank b,size_t n,size_t r) {
    if(b==Bank::Link)return(d.pipe+linkOffsets[r]+n*(r?0x100u:1u))*4;
    if(b==Bank::Frontend)return(d.pipe+frontendOffsets[r]+n*0x100u)*4;
    if(b==Bank::Timing)return(d.pipe+timingOffsets[r]+n*(r<8?0x80u:0x10u))*4;
    return(d.mpc+combinerOffsets[r]+n*(r==size_t(CombinerRegister::Output)?4u:0x15u))*4;
}
size_t instances(Bank b){return b==Bank::Link||b==Bank::Frontend?kLinks:kPipes;}
size_t registers(Bank b){return b==Bank::Link?kLinkRegisters:b==Bank::Frontend?kFrontendRegisters:
    b==Bank::Timing?kTimingRegisters:kCombinerRegisters;}
Status finish(Status s,Result& r){r.status=s;return s;}
Status validate(const Dmu& d,uint64_t bar) {
    if(bar<4||d.pipe>UINT32_MAX/4||d.mpc>UINT32_MAX/4)return Status::OutsideBar;
    // Distinct physical controls may not alias through a forged discovery base.
    for(unsigned b=0;b<4;++b)for(size_t n=0;n<instances(Bank(b));++n)for(size_t r=0;r<registers(Bank(b));++r) {
        const auto a=address(d,Bank(b),n,r);if(a>UINT32_MAX||a>bar-4)return Status::OutsideBar;
        for(unsigned c=0;c<=b;++c)for(size_t p=0;p<instances(Bank(c));++p)for(size_t q=0;q<registers(Bank(c));++q) {
            if(c==b&&(p>n||(p==n&&q>=r)))continue;
            if(a==address(d,Bank(c),p,q))return Status::AliasedRegisters;
        }
    }
    return Status::Ok;
}
Status read(const Dmu& d,const Io& io,Bank b,size_t n,size_t r,uint32_t& saved,uint32_t& mask,Result& out) {
    out.bank=b;out.instance=uint8_t(n);out.reg=uint8_t(r);out.lastRegister=uint32_t(address(d,b,n,r));
    out.lastValue=0;out.lastReadSucceeded=false;
    if(!io.valid(io.context)||!io.canRead(io.context,b,uint8_t(n),out.lastRegister)||!io.valid(io.context))return Status::AccessDenied;
    ++out.reads;uint32_t value=0;
    if(!io.read32(io.context,out.lastRegister,value))return Status::ReadFailed;
    out.lastReadSucceeded=true;out.lastValue=value;
    if(!io.valid(io.context)||!io.canRead(io.context,b,uint8_t(n),out.lastRegister)||!io.valid(io.context))return Status::AccessDenied;
    if(value==UINT32_MAX)return Status::DeviceUnavailable;
    if(out.pass){if(saved!=value)return Status::Changed;}
    else {saved=value;mask|=1u<<r;}
    return Status::Ok;
}
Status scan(const Dmu& d,const Io& io,Capture& c,Result& out) {
    // Establish every encoder mux first; never continue into legacy FE state
    // after observing an HPO/FRL/DP2 route or an unknown mux encoding.
    for(size_t n=0;n<kLinks;++n) {
        const auto s=read(d,io,Bank::Link,n,0,c.link[n][0],c.linkReadMask[n],out);if(s!=Status::Ok)return s;
        if(c.link[n][0]!=0)return Status::UnsupportedMux;
    }
    for(size_t n=0;n<kLinks;++n)for(size_t r=1;r<kLinkRegisters;++r) {
        const auto s=read(d,io,Bank::Link,n,r,c.link[n][r],c.linkReadMask[n],out);if(s!=Status::Ok)return s;
    }
    for(size_t n=0;n<kLinks;++n)for(size_t r=0;r<kFrontendRegisters;++r) {
        const auto s=read(d,io,Bank::Frontend,n,r,c.frontend[n][r],c.frontendReadMask[n],out);if(s!=Status::Ok)return s;
    }
    for(size_t n=0;n<kPipes;++n)for(size_t r=0;r<kTimingRegisters;++r) {
        const auto s=read(d,io,Bank::Timing,n,r,c.timing[n][r],c.timingReadMask[n],out);if(s!=Status::Ok)return s;
    }
    for(size_t n=0;n<kPipes;++n)for(size_t r=0;r<kCombinerRegisters;++r) {
        const auto s=read(d,io,Bank::Combiner,n,r,c.combiner[n][r],c.combinerReadMask[n],out);if(s!=Status::Ok)return s;
    }
    return Status::Ok;
}
Status classify(Capture& c) {
    unsigned enabled=0;uint8_t be=0;
    for(uint8_t n=0;n<kLinks;++n) {
        const auto control=c.link[n][1],enable=c.link[n][2];
        if(enable&~0x101u)return Status::UnsupportedSignal;
        if(enable&1u){if(enable!=0x101)return Status::Unsettled;++enabled;be=n;}
        else if(enable||control&0x7f00u)return Status::SharedRoute;
    }
    if(!enabled)return Status::Inactive;
    if(enabled!=1)return Status::SharedRoute;
    const auto control=c.link[be][1];
    if((control&~0x70077f00u)||(control&0x70000u)!=0x30000u)return Status::UnsupportedSignal;
    const auto selected=(control>>8)&0x7f;
    if(!selected||(selected&(selected-1))||selected>0x10)return Status::SharedRoute;
    uint8_t fe=0;while((1u<<fe)!=selected)++fe;
    const auto* f=c.frontend[fe];
    // Legacy RGB, one pixel per cycle, no split/bypass/stereo/Dolby route.
    if((f[0]&~0x01000007u)||!(f[0]&0x01000000u)||(f[0]&7u)>=kPipes)return Status::UnsupportedSignal;
    if((f[1]&~0x7du)||!(f[1]&1u)||f[2])return Status::UnsupportedSignal;
    if((f[3]&~0x313f031fu)||(f[3]&0x19u)!=0x19u)return Status::UnsupportedSignal;
    const uint8_t depth=uint8_t((f[3]>>28)&3u);const bool deep=(f[3]&0x01000000u)!=0;
    if(deep!=(depth!=0)||((f[3]&4u)&&!(f[3]&2u)))return Status::UnsupportedSignal;
    if((f[4]&~0x1031u)||(f[4]&0x31u)!=0x31u||(f[5]&~1u))return Status::UnsupportedSignal;
    for(uint8_t n=0;n<kLinks;++n)if(n!=fe) {
        // Do not call a second FE harmless merely because no backend selected it.
        if(c.frontend[n][0]&0x01000000u||c.frontend[n][1]&1u||c.frontend[n][2]&0x10001u)return Status::SharedRoute;
    }
    const uint8_t tg=uint8_t(f[0]&7u);unsigned active=0;
    for(uint8_t n=0;n<kPipes;++n) {
        const auto* t=c.timing[n];
        if(t[0]&~0x00017301u)return Status::UnsupportedTiming; // rejects OUT_MUX/HPO/reserved
        const auto master=t[0]&0x10001u;
        if(master&&master!=0x10001u)return Status::Unsettled;
        if(master){++active;if(n!=tg)return Status::SharedRoute;}
        if(t[1]&~0x10113u||t[1]&0x10u)return Status::UnsupportedTiming;
        // The initial profile has none of these optional timing/lock modes.
        // Reject unclassified reserved encodings instead of masking them away.
        if(t[2]||t[3]||t[4]||t[5]||t[6]||t[7]||t[8]||t[10]||t[11])return Status::UnsupportedTiming;
        if(n!=tg&&t[9]!=0xffff0000u)return Status::SharedRoute;
    }
    if(active!=1)return Status::Inactive;
    const auto* t=c.timing[tg];
    if(!(t[1]&0x100u)||(t[0]&0x7300u)>0x200u)return Status::UnsupportedTiming;
    const uint8_t opp=uint8_t((t[9]>>16)&15u);
    if(opp>=kPipes||t[9]!=(0xfff00000u|(uint32_t(opp)<<16)))return Status::UnsupportedTiming;
    const uint8_t mpcc=uint8_t(c.combiner[opp][5]&15u);
    if(mpcc>=kPipes)return Status::UnsupportedCombiner;
    for(uint8_t n=0;n<kPipes;++n) {
        const auto* m=c.combiner[n];
        if(m[0]&~15u||m[1]&~15u||m[2]&~15u||m[3]&~0x7fu||m[4]&~7u||m[5]&~0x7fffafu||m[5]&0x20u)
            return Status::UnsupportedCombiner;
        if(n!=opp&&(m[5]&15u)!=15u)return Status::SharedRoute;
        if(n==mpcc) {
            if(m[0]>=kPipes||m[1]!=15u||m[2]!=opp||m[3]!=opp)return Status::UnsupportedCombiner;
            // Current MPCC status is retained, not mistaken for a stop proof.
            if(m[4]!=1u&&m[4]!=2u)return Status::Unsettled;
        } else if(m[0]!=15u||m[1]!=15u||m[2]!=15u||m[3]!=15u||(m[4]&3u)!=1u)
            return Status::SharedRoute;
    }
    c.backend=be;c.frontendIndex=fe;c.timingIndex=tg;c.opp=opp;c.mpcc=mpcc;c.dpp=uint8_t(c.combiner[mpcc][0]);
    c.scrambled=(f[3]&2u)!=0;c.quarterRate=(f[3]&4u)!=0;
    c.bitsPerComponent=depth==0?8:depth==1?10:depth==2?12:16;
    return Status::Ok;
}
}
Status capture(const uint8_t* discovery,size_t discoveryBytes,uint64_t bar,
    const uint8_t* rom,size_t romBytes,size_t imageOffset,const Io& io,Capture& out,Result& r) {
    out={};r={};out.path=out.backend=out.frontendIndex=out.timingIndex=out.opp=out.mpcc=out.dpp=0xff;
    if(!discovery||!rom||!io.valid||!io.canRead||!io.read32)return finish(Status::InvalidArgument,r);
    DiscoveryInfo info={};Dmu d={};
    if(parseDiscovery(discovery,discoveryBytes,info,collect,&d)!=DiscoveryStatus::Ok)return finish(Status::DiscoveryRejected,r);
    if(d.count!=1||!d.valid||info.baseAddresses64Bit||info.dieCount!=1||info.dies[0].id)return finish(Status::UnsupportedDmu,r);
    auto s=validate(d,bar);if(s!=Status::Ok)return finish(s,r);
    ImageInfo image={};DisplayInfo display={};dmcub::Layout mailbox={};
    if(parseImage(rom,romBytes,imageOffset,image)!=hackynavi::Status::Ok||!image.checksumValid||image.codeType||
        image.vendorId!=0x1002||image.deviceId!=0x744c||image.size>1024*1024||
        parseDisplay(rom,romBytes,image,display)!=hackynavi::Status::Ok||!display.pathCount)
        return finish(Status::RomRejected,r);
    if(dmcub::buildLayout(discovery,discoveryBytes,bar,mailbox)!=dmcub::Status::Ok)return finish(Status::OutsideBar,r);
    // Snapshot parsed routes before MMIO; do not re-read caller ROM after the
    // hardware inventory. These are decoded from the checked image, not caller
    // assertions. The caller owns immutable ROM/discovery bytes for this call.
    dmcub::Route routes[kMaxDisplayPaths]={};
    for(uint8_t p=0;p<display.pathCount;++p) {
        if(dmcub::parseRoute(rom,romBytes,image,mailbox,p,routes[p])!=dmcub::Status::Ok)
            return finish(Status::UnsupportedRoute,r);
    }
    for(uint8_t pass=0;pass<2;++pass){r.pass=pass;s=scan(d,io,out,r);if(s!=Status::Ok)return finish(s,r);}
    s=classify(out);if(s!=Status::Ok)return finish(s,r);
    unsigned matches=0;
    for(uint8_t p=0;p<display.pathCount;++p) {
        const auto& route=routes[p];
        if(route.phy==out.backend) {
            ++matches;out.route=route;out.path=p;
            if((route.connectorObject&255u)!=0x0c||uint32_t(route.hpd)!=(out.link[out.backend][1]>>28))
                return finish(Status::UnsupportedRoute,r);
        }
    }
    if(matches!=1)return finish(Status::UnsupportedRoute,r);
    if(!io.valid(io.context))return finish(Status::AccessDenied,r);
    out.valid=true;return finish(Status::Ok,r);
}
const char* statusString(Status s) {
    switch(s) {
#define NAME(x) case Status::x:return #x
        NAME(Ok);NAME(InvalidArgument);NAME(DiscoveryRejected);NAME(UnsupportedDmu);NAME(OutsideBar);
        NAME(AliasedRegisters);NAME(RomRejected);NAME(UnsupportedRoute);NAME(AccessDenied);NAME(ReadFailed);
        NAME(DeviceUnavailable);NAME(Changed);NAME(UnsupportedMux);NAME(UnsupportedSignal);NAME(SharedRoute);
        NAME(Inactive);NAME(Unsettled);NAME(UnsupportedTiming);NAME(UnsupportedCombiner);
#undef NAME
    }return "Unknown";
}
} }
