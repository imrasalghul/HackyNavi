// SPDX-License-Identifier: MIT
#include "hackynavi/edid.hpp"
#include <cstring>
#include <iostream>
#include <vector>
namespace e=hackynavi::edid;
namespace {
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<__LINE__<<": " #x "\n";}}while(0)
void checksum(std::vector<uint8_t>& b,size_t block) {
    unsigned s=0;for(size_t i=0;i<127;++i)s+=b[block*128+i];b[block*128+127]=uint8_t(0-s);
}
void fix(std::vector<uint8_t>& b) { for(size_t i=0;i<b.size()/128;++i)checksum(b,i); }
std::vector<uint8_t> fixture() {
    std::vector<uint8_t> b(256,0);for(size_t i=1;i<7;++i)b[i]=255;
    b[8]=0x4c;b[9]=0x2d;b[10]=0x34;b[11]=0x12;b[12]=0x78;b[13]=0x56;
    b[18]=1;b[19]=4;b[20]=0xa2;b[24]=2;b[126]=1;
    for(size_t i=38;i<54;++i)b[i]=1; // Unused standard timing slots.
    // 1920x1080 148.5MHz, totals2200x1125, positive separate sync.
    const uint8_t dtd[18]={2,0x3a,0x80,0x18,0x71,0x38,0x2d,0x40,0x58,0x2c,0x45,0,0,0,0,0,0,0x1e};
    std::memcpy(b.data()+54,dtd,18);
    b[128]=2;b[129]=3;b[131]=0x20;
    // Native1080p60,720p60,2160p60 and a deliberately unsupported VIC.
    const uint8_t dbc[]={0x44,0x90,4,97,1,
        0x67,3,12,0,0x10,0,0x78,60,
        0x67,0xd8,0x5d,0xc4,1,120,0x88,0x60};
    std::memcpy(b.data()+132,dbc,sizeof(dbc));b[130]=uint8_t(4+sizeof(dbc));fix(b);return b;
}
void rejected(const std::vector<uint8_t>& b,e::Status wanted,size_t bytes=0) {
    e::Monitor m={};m.serial=0xa1234567;const e::Monitor before=m;
    const auto actual=e::parse(b.data(),bytes?bytes:b.size(),m);
    if(actual!=wanted)std::cerr<<"wanted "<<e::statusString(wanted)<<", got "<<e::statusString(actual)<<", bytes="<<bytes<<"\n";
    CHECK(actual==wanted);
    CHECK(std::memcmp(&m,&before,sizeof(m))==0);
}
void basic() {
    for(unsigned flags=0;flags<256;++flags){
        auto bytes=fixture();const size_t at=128+bytes[130];
        bytes[at]=0xe2;bytes[at+1]=0;bytes[at+2]=uint8_t(flags);bytes[130]+=3;fix(bytes);
        e::Monitor parsed={};CHECK(e::parse(bytes.data(),bytes.size(),parsed)==e::Status::Ok);
        CHECK(parsed.rgbQuantizationSelectable==bool(flags&0x40));CHECK(parsed.yccQuantizationSelectable==bool(flags&0x80));
        bytes[at+3]=0xe2;bytes[at+4]=0;bytes[at+5]=uint8_t(flags^0x40);bytes[130]+=3;fix(bytes);
        rejected(bytes,e::Status::ConflictingCapabilities);
    }
    auto b=fixture();e::Monitor m={};CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok);
    CHECK(m.manufacturer==0x4c2d && m.product==0x1234 && m.serial==0x5678);
    CHECK(m.hdmi && m.scdc && m.scrambleLowRates && m.ycbcr444 && m.deepColor444 && m.frlCapable);
    CHECK(m.deepColorMask==7 && m.maxTmdsCharacterClockKHz==600000);
    CHECK(m.modeCount==3 && m.skippedTimings==1);
    const auto& t=m.modes[0];CHECK(t.preferred && t.native && t.vic==16);
    CHECK(t.width==1920 && t.height==1080 && t.pixelClockKHz==148500);
    CHECK(t.hTotal==2200 && t.hSyncStart==2008 && t.hSyncEnd==2052);
    CHECK(t.vTotal==1125 && t.vSyncStart==1084 && t.vSyncEnd==1089 && t.hPositive && t.vPositive);
    CHECK(m.modes[1].width==1280 && m.modes[1].hSyncStart==1390);
    CHECK(m.modes[2].width==3840 && m.modes[2].pixelClockKHz==594000 && m.modes[2].hTotal==4400);
    // Block order does not silently discard the HF-VSDB 600MHz limit.
    for(size_t i=0;i<8;++i)std::swap(b[137+i],b[145+i]);fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.maxTmdsCharacterClockKHz==600000);
    // Base-only DVI/DP EDID carries no invented HDMI capabilities.
    b=fixture();b.resize(128);b[126]=0;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.modeCount==1 && !m.hdmi);
    CHECK(!m.scdc && !m.deepColorMask && !m.maxTmdsCharacterClockKHz);
    b[19]=3;b[24]=0;fix(b);CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && !m.modes[0].preferred);
}
void faults() {
    const auto original=fixture();e::Monitor m={};CHECK(e::parse(nullptr,0,m)==e::Status::InvalidArgument);
    for(size_t n=1;n<original.size();++n)rejected(original,e::Status::Truncated,n);
    auto b=original;b.push_back(0);rejected(b,e::Status::InvalidArgument);
    for(size_t i=0;i<original.size();++i) {
        b=original;b[i]^=1;
        if(i<8)rejected(b,e::Status::BadHeader);
        else rejected(b,e::Status::BadChecksum);
    }
    b=original;b[126]=8;fix(b);rejected(b,e::Status::TooManyBlocks);
    b=original;b[18]=2;fix(b);rejected(b,e::Status::UnsupportedVersion);
    b=original;b[19]=2;fix(b);rejected(b,e::Status::UnsupportedVersion);
    b=original;b[20]&=0x7f;fix(b);rejected(b,e::Status::AnalogDisplay);
    b=original;b[130]=3;fix(b);rejected(b,e::Status::BadExtension);
    b=original;b[130]=128;fix(b);rejected(b,e::Status::BadExtension);
    b=original;b[132]=0x5f;fix(b);rejected(b,e::Status::BadExtension);
    b=original;b[54+9]=0;fix(b);rejected(b,e::Status::BadTiming);
    b=original;b[54+8]=255;fix(b);rejected(b,e::Status::BadTiming);
    b=original;b[54+17]|=0x80;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.skippedTimings==2 && !m.modes[0].preferred);
    b=original;b[151]&=0x7f;fix(b);rejected(b,e::Status::BadExtension); // 600MHz without SCDC.
    b=original;b[152]=0x70;fix(b);rejected(b,e::Status::BadExtension); // Reserved FRL code.
    b=original;b[149]=2;fix(b);rejected(b,e::Status::BadExtension); // Unknown HF version.
    b=original;b[131]|=1;fix(b);rejected(b,e::Status::BadExtension); // Native DTD count but no DTD.
    b=original;b[254]=1;fix(b);rejected(b,e::Status::BadExtension); // Nonzero trailing padding.
    b=original;b[130]=0;fix(b);rejected(b,e::Status::BadExtension);
    b=original;std::memset(b.data()+132,0,123);b[130]=0;b[131]=0;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && !m.hdmi);
}
void extensions() {
    auto b=fixture();e::Monitor m={};
    // HDMI Forum SCDB uses the same SCDS positions after the extended tag.
    b[145]=0xe7;b[146]=0x79;b[147]=b[148]=0;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.scdc && m.maxTmdsCharacterClockKHz==600000);
    // Duplicate conflicting capability blocks must not depend on parse order.
    b=fixture();std::memcpy(b.data()+153,b.data()+145,8);b[158]=100;b[130]+=8;fix(b);
    rejected(b,e::Status::ConflictingCapabilities);
    b[158]=120;fix(b);CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok);
    b=fixture();b[128]=0x70;fix(b);CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.skippedExtensions==1 && !m.hdmi);
    b=fixture();b[129]=2;fix(b);CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.skippedExtensions==1);
    // Only the RGB SVD advertises97. A420-only listing cannot create an RGB mode.
    b=fixture();b[132]=0xe4;b[133]=0x0e;b[134]=97;b[135]=96;b[136]=95;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.modeCount==1 && m.skippedTimings==3);
    b=fixture();b[153]=0xe2;b[154]=0x0e;b[155]=97;b[130]+=3;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.modeCount==2 && m.modes[1].vic==4);
    b[155]=16;fix(b); // Also removes a preferred matching base DTD.
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.modeCount==2 && m.modes[0].vic==4);
    b=fixture();b[144]=30;b[150]=60;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.maxTmdsCharacterClockKHz==150000);
    b=fixture();b[132]=0xe4;b[133]=0x78;fix(b);rejected(b,e::Status::BadExtension);
    b=fixture();b[133]=193;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && !m.modes[0].native && m.skippedTimings==2);
    // Supported CTA timings have exact positive porch/pulse widths.
    const uint8_t vics[]={4,19,16,31,32,33,34,93,94,95,96,97};
    b=fixture();std::memset(b.data()+132,0,123);b[132]=uint8_t(0x40+sizeof(vics));
    std::memcpy(b.data()+133,vics,sizeof(vics));b[130]=uint8_t(5+sizeof(vics));fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.modeCount==12);
    for(size_t i=0;i<m.modeCount;++i) {
        const auto& t=m.modes[i];CHECK(t.width<t.hSyncStart && t.hSyncStart<t.hSyncEnd && t.hSyncEnd<t.hTotal);
        CHECK(t.height<t.vSyncStart && t.vSyncStart<t.vSyncEnd && t.vSyncEnd<t.vTotal);
    }
    // CTA detailed timing, including native designation and DTD/SVD dedup.
    b=fixture();std::memcpy(b.data()+153,b.data()+54,18);b[131]|=1;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.modeCount==3 && m.modes[0].native);
}
void counts() {
    auto b=fixture();size_t count=17;bool final=true;e::Monitor m={};
    CHECK(e::requiredBlocks(b.data(),128,count,final)==e::Status::Ok && count==2 && !final);
    CHECK(e::requiredBlocks(b.data(),256,count,final)==e::Status::Ok && count==2 && final);
    // HF override must occupy bytes4..6 of block1; base255 may override down.
    b[126]=255;std::memmove(b.data()+135,b.data()+132,21);
    b[132]=0xe2;b[133]=0x78;b[134]=1;b[130]+=3;fix(b);
    CHECK(e::requiredBlocks(b.data(),128,count,final)==e::Status::Ok && !final);
    CHECK(e::requiredBlocks(b.data(),256,count,final)==e::Status::Ok && count==2 && final);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.blocks==2 && m.modeCount==3);
    b[126]=1;b[134]=2;fix(b);
    CHECK(e::requiredBlocks(b.data(),256,count,final)==e::Status::Ok && count==3 && final);
    rejected(b,e::Status::Truncated);
    b.resize(384);b[256]=0x70;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.blocks==3 && m.skippedExtensions==1);
    // 420-only override in a later CTA reconciles earlier RGB modes too.
    b[256]=2;b[257]=3;b[258]=7;b[260]=0xe2;b[261]=0x0e;b[262]=97;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.modeCount==2);
    b[261]=0x78;b[262]=2;fix(b);rejected(b,e::Status::BadExtension);
    b=fixture();b[35]=3;b[36]=1;b[37]=0x80;b[38]=0x81;b[39]=0x80;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.skippedTimings==6);
    b=fixture();b[150]=60;b[151]=0;fix(b);rejected(b,e::Status::BadExtension); // FRL without SCDC.
    count=17;final=false;b=fixture();b[255]^=1;
    CHECK(e::requiredBlocks(b.data(),256,count,final)==e::Status::BadChecksum && count==17 && !final);
    b=fixture();b[126]=0;fix(b);
    CHECK(e::requiredBlocks(b.data(),128,count,final)==e::Status::Ok && count==1 && final);
}
std::vector<uint8_t> forumFixture(size_t len=13) {
    auto b=fixture();std::memset(b.data()+145,0,110);
    // Exact G90XF capability bytes, without the EDID's identifying serial.
    const uint8_t forum[]={0x6d,0xd8,0x5d,0xc4,1,120,0x80,0x6b,0,0x30,0xa5,0xc3,0x64,0x3f};
    std::memcpy(b.data()+145,forum,len+1);b[145]=uint8_t(0x60|len);
    b[130]=uint8_t(18+len);fix(b);return b;
}
void forumDsc() {
    e::Monitor m={};auto b=forumFixture();
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok);
    CHECK(m.forumVersion==1 && m.forumPayloadBytes==13);
    CHECK(m.frl.code==6 && m.frl.maxLanes==4 && m.frl.maxGbpsPerLane==12);
    const auto& d=m.hdmiDsc;
    CHECK(d.present && d.supported1p2 && d.limitsPresent && d.native420 && !d.allBpp);
    CHECK(d.bpcMask==3 && d.maxSlicesCode==4 && d.maxSlices==8 && d.sliceLimitsDecoded);
    CHECK(d.sliceClockMHzBeforeKSliceAdjust==340 && d.totalChunkCode==63 && d.maxTotalChunkBytes==65536);
    CHECK(d.frl.code==6 && d.frl.maxLanes==4 && d.frl.maxGbpsPerLane==12);
    // Equivalent HF-SCDB carries the same SCDS; source header/OUI differ.
    const size_t at=159;std::memcpy(b.data()+at,b.data()+145,14);
    b[at]=0xed;b[at+1]=0x79;b[at+2]=b[at+3]=0;b[130]+=14;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok);
    for(size_t i=4;i<=13;++i) {
        auto c=b;c[at+i]^=i==7?1:2;fix(c);
        // Version/DSC reserved encodings can be malformed before conflict.
        const auto s=e::parse(c.data(),c.size(),m);
        CHECK(s==e::Status::ConflictingCapabilities || s==e::Status::BadExtension);
    }
    const uint8_t lanes[]={0,3,3,4,4,4,4},rates[]={0,3,6,6,8,10,12};
    for(uint8_t i=0;i<16;++i) {
        b=forumFixture(7);b[152]=uint8_t(i<<4);fix(b);
        if(i>6){rejected(b,e::Status::BadExtension);continue;}
        CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok);
        CHECK(m.frl.code==i && m.frl.maxLanes==lanes[i] && m.frl.maxGbpsPerLane==rates[i]);
        CHECK(m.frlCapable==bool(i) && !m.hdmiDsc.present && !m.hdmiDsc.supported1p2);
    }
    const uint8_t slices[]={0,1,2,4,8,8,12,0};
    for(uint8_t i=0;i<16;++i) {
        b=forumFixture();b[157]=uint8_t(0x60|i);fix(b);
        if(i>7){rejected(b,e::Status::BadExtension);continue;}
        CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok);
        CHECK(m.hdmiDsc.maxSlicesCode==i && m.hdmiDsc.maxSlices==slices[i]);
        CHECK(m.hdmiDsc.sliceLimitsDecoded==(i>0 && i<7));
    }
    for(uint8_t i=0;i<64;++i) {
        b=forumFixture();b[158]=i;fix(b);
        CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok);
        CHECK(m.hdmiDsc.totalChunkCode==i && m.hdmiDsc.maxTotalChunkBytes==(i?1024u*(1u+i):0));
    }
    for(size_t len=7;len<=13;++len) {
        b=forumFixture(len);
        if(len==11 || len==12){rejected(b,e::Status::BadExtension);continue;}
        CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok);
        CHECK(m.hdmiDsc.supported1p2==(len==13));
    }
    b=forumFixture();b[157]=0x74;fix(b);rejected(b,e::Status::BadExtension);
    b=forumFixture();b[158]|=0x80;fix(b);rejected(b,e::Status::BadExtension);
    b=forumFixture();b[152]=0;fix(b);rejected(b,e::Status::BadExtension);
    b=forumFixture();b[156]=0x8f;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.hdmiDsc.bpcMask==7 && m.hdmiDsc.allBpp);
}
std::vector<uint8_t> typeVIIFixture() {
    auto b=forumFixture();
    // Exact G90XF 165Hz CTA Type VII descriptor, no identifying bytes.
    const uint8_t timing[]={0xf6,0x22,0x02,0x41,0xdc,0x18,0x04,0xff,0x0e,0x7f,0x01,
        0x9f,0x80,0x1f,0x00,0x6f,0x08,0xb1,0x00,0x02,0x00,0x09,0x00};
    std::memcpy(b.data()+159,timing,sizeof(timing));b[130]+=sizeof(timing);fix(b);return b;
}
void typeVII() {
    e::Monitor m={};auto b=typeVIIFixture();
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.modeCount==4);
    const auto& t=m.modes[3];
    CHECK(t.sources==e::CtaTypeVII && !t.vic && !t.preferred && !t.native);
    CHECK(t.pixelClockKHz==1629250 && t.width==3840 && t.height==2160);
    CHECK(t.hTotal==4224 && t.hSyncStart==4000 && t.hSyncEnd==4032 && t.hPositive);
    CHECK(t.vTotal==2338 && t.vSyncStart==2163 && t.vSyncEnd==2173 && !t.vPositive);
    CHECK(m.modes[0].sources==(e::BaseDtd|e::CtaVic));
    for(unsigned bit=4;bit<8;++bit) {
        b=typeVIIFixture();b[165]|=uint8_t(1u<<bit);fix(b);
        CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.modeCount==3 && m.skippedTimings==2);
    }
    b=typeVIIFixture();b[161]=3;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.modeCount==3 && m.skippedTimings==2);
    for(unsigned len=1;len<22;++len) {
        b=typeVIIFixture();b[159]=uint8_t(0xe0|len);b[130]=uint8_t(32+len);
        std::memset(b.data()+160+len,0,95-len);fix(b);rejected(b,e::Status::BadExtension);
    }
    b=typeVIIFixture();b[165]=9;fix(b);rejected(b,e::Status::BadTiming);
    // 16-bit wire dimensions +1 and total sums must never wrap to a mode.
    for(size_t offset: {size_t(166),size_t(168),size_t(172),size_t(174),size_t(176),size_t(180)}) {
        b=typeVIIFixture();b[offset]=b[offset+1]=255;fix(b);rejected(b,e::Status::BadTiming);
    }
    b=typeVIIFixture();b[162]=b[163]=b[164]=255;fix(b);
    CHECK(e::parse(b.data(),b.size(),m)==e::Status::Ok && m.modes[3].pixelClockKHz==16777216);
    // A structurally valid sink timing is not permission to program that clock.
}
}
int main(){basic();faults();extensions();counts();forumDsc();typeVII();std::cout<<checks<<" EDID checks, "<<failures<<" failures\n";return failures?1:0;}
