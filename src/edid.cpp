// SPDX-License-Identifier: MIT
// EDID/CTA wire fields and timing definitions referenced against Linux
// 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0 drivers/gpu/drm/drm_edid.c.
// Independent bounded parser; no Linux connector/quirk implementation copied.
#include "hackynavi/edid.hpp"
namespace hackynavi { namespace edid { namespace {
uint16_t le16(const uint8_t* p) { return uint16_t(p[0]|uint16_t(p[1])<<8); }
bool same(const Timing& a,const Timing& b) {
    return a.pixelClockKHz==b.pixelClockKHz && a.width==b.width && a.height==b.height &&
        a.hTotal==b.hTotal && a.hSyncStart==b.hSyncStart && a.hSyncEnd==b.hSyncEnd &&
        a.vTotal==b.vTotal && a.vSyncStart==b.vSyncStart && a.vSyncEnd==b.vSyncEnd &&
        a.hPositive==b.hPositive && a.vPositive==b.vPositive;
}
Status add(Monitor& m,const Timing& t) {
    for(size_t i=0;i<m.modeCount;++i)if(same(m.modes[i],t)) {
        m.modes[i].preferred=m.modes[i].preferred||t.preferred;
        m.modes[i].native=m.modes[i].native||t.native;
        if(t.vic)m.modes[i].vic=t.vic;
        return Status::Ok;
    }
    if(m.modeCount==kMaxModes)return Status::TooManyModes;
    m.modes[m.modeCount++]=t;return Status::Ok;
}
Status detailed(const uint8_t* p,Monitor& m,bool preferred,bool native) {
    if(!le16(p))return Status::Ok; // Monitor descriptor or unused slot.
    // Interlace, stereo, analog/composite sync need distinct programming.
    if((p[17]&0xe1) || (p[17]&0x18)!=0x18) { ++m.skippedTimings;return Status::Ok; }
    const uint16_t ha=uint16_t(p[2]|uint16_t(p[4]&0xf0)<<4);
    const uint16_t hb=uint16_t(p[3]|uint16_t(p[4]&15)<<8);
    const uint16_t va=uint16_t(p[5]|uint16_t(p[7]&0xf0)<<4);
    const uint16_t vb=uint16_t(p[6]|uint16_t(p[7]&15)<<8);
    const uint16_t ho=uint16_t(p[8]|uint16_t(p[11]&0xc0)<<2);
    const uint16_t hw=uint16_t(p[9]|uint16_t(p[11]&0x30)<<4);
    const uint16_t vo=uint16_t((p[10]>>4)|uint16_t(p[11]&12)<<2);
    const uint16_t vw=uint16_t((p[10]&15)|uint16_t(p[11]&3)<<4);
    if(ha<64 || va<64 || !hb || !vb || !ho || !vo || !hw || !vw || ho+hw>hb || vo+vw>vb)
        return Status::BadTiming;
    Timing t={uint32_t(le16(p))*10,ha,va,uint16_t(ha+hb),uint16_t(ha+ho),uint16_t(ha+ho+hw),
        uint16_t(va+vb),uint16_t(va+vo),uint16_t(va+vo+vw),bool(p[17]&2),bool(p[17]&4),preferred,native,0};
    return add(m,t);
}
bool vicTiming(uint8_t vic,Timing& t) {
    t={};t.vic=vic;t.hPositive=t.vPositive=true;
    if(vic==4 || vic==19) {
        t.pixelClockKHz=74250;t.width=1280;t.height=720;t.hTotal=vic==4?1650:1980;
        t.hSyncStart=vic==4?1390:1720;t.hSyncEnd=uint16_t(t.hSyncStart+40);
        t.vTotal=750;t.vSyncStart=725;t.vSyncEnd=730;
    } else if(vic==16 || (vic>=31 && vic<=34)) {
        t.pixelClockKHz=(vic==16 || vic==31)?148500:74250;t.width=1920;t.height=1080;
        t.hTotal=(vic==31 || vic==33)?2640:(vic==32?2750:2200);
        t.hSyncStart=uint16_t(t.hTotal-192);t.hSyncEnd=uint16_t(t.hSyncStart+44);
        t.vTotal=1125;t.vSyncStart=1084;t.vSyncEnd=1089;
    } else if(vic>=93 && vic<=97) {
        t.pixelClockKHz=vic>=96?594000:297000;t.width=3840;t.height=2160;
        t.hTotal=vic==93?5500:((vic==94 || vic==96)?5280:4400);
        t.hSyncStart=uint16_t(t.hTotal-384);t.hSyncEnd=uint16_t(t.hSyncStart+88);
        t.vTotal=2250;t.vSyncStart=2168;t.vSyncEnd=2178;
    } else return false;
    return true;
}
struct Caps {
    bool legacy,forum,video;
    uint8_t videoFlags;
    uint32_t legacyClock,forumClock;
    uint8_t legacyFlags,forumFlags,frl;
    uint32_t only420[8];
};
Status capabilities(const uint8_t* p,size_t len,bool forum,Monitor& m,Caps& c) {
    if(forum) {
        if(len<7 || p[4]!=1)return Status::BadExtension;
        const uint32_t clock=uint32_t(p[5])*5000;
        const uint8_t flags=p[6]&0x88,frl=p[7]>>4;
        if(frl>6 || (clock>340000 && !(flags&0x80)) || ((frl || (flags&8)) && !(flags&0x80)))
            return Status::BadExtension;
        if(c.forum && (c.forumClock!=clock || c.forumFlags!=flags || c.frl!=frl))
            return Status::ConflictingCapabilities;
        c.forum=true;c.forumClock=clock;c.forumFlags=flags;c.frl=frl;
        m.scdc=bool(flags&0x80);m.scrambleLowRates=bool(flags&8);m.frlCapable=frl!=0;
    } else {
        if(len<5)return Status::BadExtension;
        const uint8_t flags=len>=6?uint8_t(p[6]&0x78):0;
        const uint32_t clock=len>=7?uint32_t(p[7])*5000:0;
        if(c.legacy && (c.legacyClock!=clock || c.legacyFlags!=flags))
            return Status::ConflictingCapabilities;
        c.legacy=true;c.legacyClock=clock;c.legacyFlags=flags;
        m.deepColorMask=(flags>>4)&7;m.deepColor444=bool(flags&8);
    }
    m.hdmi=true;return Status::Ok;
}
Status cta(const uint8_t* p,Monitor& m,Caps& caps,size_t block) {
    if(p[1]<3) { ++m.skippedExtensions;return Status::Ok; }
    const size_t end=p[2];
    if(end==0) {
        // With no data collection or DTDs, bytes4..126 must be padding.
        for(size_t i=4;i<127;++i)if(p[i])return Status::BadExtension;
        return (p[3]&15)?Status::BadExtension:Status::Ok;
    }
    if(end<4 || end>127)return Status::BadExtension;
    m.ycbcr444=m.ycbcr444||bool(p[3]&0x20);
    for(size_t i=4;i<end;) {
        const size_t len=p[i]&31;const unsigned tag=p[i]>>5;
        if(len>end-i-1)return Status::BadExtension;
        const uint8_t* db=p+i;
        if(tag==2)for(size_t j=1;j<=len;++j) {
            // 129..192 mark native VIC1..64. 193..253 are distinct VICs.
            const uint8_t raw=db[j],vic=(raw>=129 && raw<=192)?uint8_t(raw&127):raw;
            Timing t={};
            if(vicTiming(vic,t)) {
                t.native=raw>=129 && raw<=192;const auto s=add(m,t);if(s!=Status::Ok)return s;
            } else ++m.skippedTimings;
        }
        if(tag==3 && len>=3) {
            const uint32_t oui=uint32_t(db[1])|uint32_t(db[2])<<8|uint32_t(db[3])<<16;
            if(oui==0x000c03 || oui==0xc45dd8) {
                const auto s=capabilities(db,len,oui==0xc45dd8,m,caps);if(s!=Status::Ok)return s;
            }
        } else if(tag==7 && len>=1) {
            if(db[1]==0x00) {
                if(len!=2)return Status::BadExtension;
                if(caps.video&&caps.videoFlags!=db[2])return Status::ConflictingCapabilities;
                caps.video=true;caps.videoFlags=db[2];
                m.rgbQuantizationSelectable=(db[2]&0x40)!=0;
                m.yccQuantizationSelectable=(db[2]&0x80)!=0;
            } else if(db[1]==0x79) {
                const auto s=capabilities(db,len,true,m,caps);if(s!=Status::Ok)return s;
            } else if(db[1]==0x78) {
                if(block!=1 || i!=4 || len!=2 || !db[2])return Status::BadExtension;
            } else if(db[1]==0x0e) {
                m.skippedTimings+=uint16_t(len-1);
                for(size_t j=2;j<=len;++j)caps.only420[db[j]/32]|=uint32_t(1)<<(db[j]%32);
            }
        }
        i+=len+1;
    }
    size_t n=0,offset=end;bool padding=false;
    for(;offset+18<=127;offset+=18) {
        if(!le16(p+offset)) { padding=true;for(size_t j=0;j<18;++j)if(p[offset+j])return Status::BadExtension;continue; }
        if(padding)return Status::BadExtension;
        const auto s=detailed(p+offset,m,false,n<(p[3]&15));if(s!=Status::Ok)return s;
        ++n;
    }
    for(;offset<127;++offset)if(p[offset])return Status::BadExtension;
    return n<(p[3]&15)?Status::BadExtension:Status::Ok;
}
} // namespace
Status requiredBlocks(const uint8_t* data,size_t bytes,size_t& result,bool& final) {
    if(!data)return Status::InvalidArgument;
    if(bytes<kBlockBytes)return Status::Truncated;
    if(bytes%kBlockBytes || bytes>kMaxBlocks*kBlockBytes)return Status::InvalidArgument;
    static const uint8_t header[8]={0,255,255,255,255,255,255,0};
    for(size_t i=0;i<8;++i)if(data[i]!=header[i])return Status::BadHeader;
    for(size_t b=0;b<bytes/128;++b) { unsigned sum=0;for(size_t i=0;i<128;++i)sum+=data[b*128+i];if(sum&255)return Status::BadChecksum; }
    if(data[18]!=1 || (data[19]!=3 && data[19]!=4))return Status::UnsupportedVersion;
    if(!(data[20]&0x80))return Status::AnalogDisplay;
    if(data[126] && bytes==128) {result=2;final=false;return Status::Ok;}
    size_t blocks=size_t(data[126])+1;
    if(data[126] && bytes>=256) {
        const auto* cta=data+128;
        if(cta[0]==2 && cta[1]>=3 && cta[2]>=7 && cta[2]<=127 && cta[4]==0xe2 && cta[5]==0x78) {
            if(!cta[6])return Status::BadExtension;
            blocks=size_t(cta[6])+1;
        }
    }
    if(blocks>kMaxBlocks)return Status::TooManyBlocks;
    result=blocks;final=true;return Status::Ok;
}
Status parse(const uint8_t* data,size_t bytes,Monitor& output) {
    if(!data)return Status::InvalidArgument;
    if(bytes<128)return Status::Truncated;
    // Partial final block is incomplete input, not a valid prefix for the
    // acquisition helper. Validate only whole blocks before reporting length.
    size_t blocks=0;bool final=false;
    const auto count=requiredBlocks(data,bytes-(bytes%128),blocks,final);
    if(count!=Status::Ok)return count;
    if(!final || bytes<blocks*128)return Status::Truncated;
    if(bytes!=blocks*128)return Status::InvalidArgument;
    Monitor m={};Caps c={};m.blocks=uint8_t(blocks);m.version=data[18];m.revision=data[19];
    m.manufacturer=uint16_t(uint16_t(data[8])<<8|data[9]);m.product=le16(data+10);
    m.serial=uint32_t(le16(data+12))|uint32_t(le16(data+14))<<16;
    for(unsigned bits=unsigned(data[35])|unsigned(data[36])<<8|unsigned(data[37]&0x80)<<9;bits;bits>>=1)
        if(bits&1)++m.skippedTimings;
    for(size_t i=38;i<54;i+=2)if(data[i]!=1 || data[i+1]!=1)++m.skippedTimings;
    for(size_t i=0;i<4;++i) {
        const auto s=detailed(data+54+18*i,m,i==0 && (data[19]>=4 || (data[24]&2)),false);
        if(s!=Status::Ok)return s;
    }
    for(size_t b=1;b<blocks;++b) {
        if(data[b*128]==2) { const auto s=cta(data+b*128,m,c,b);if(s!=Status::Ok)return s; }
        else ++m.skippedExtensions;
    }
    // HF-VSDB extends legacy TMDS above340MHz. A lower field cannot increase
    // the HDMI VSDB limit or manufacture one when the legacy limit is absent.
    m.maxTmdsCharacterClockKHz=c.forumClock>340000?c.forumClock:c.legacyClock;
    for(size_t i=0;i<m.modeCount;) {
        bool only420=false;
        for(unsigned vic=1;vic<256 && !only420;++vic)if(c.only420[vic/32]&(uint32_t(1)<<(vic%32))) {
            Timing t={};only420=vicTiming(uint8_t(vic),t) && same(t,m.modes[i]);
        }
        if(!only420) {++i;continue;}
        // Reconcile across all extensions and DTD/SVD representations: a
        // 420-only declaration never authorizes an RGB/444 scanout path.
        for(size_t j=i+1;j<m.modeCount;++j)m.modes[j-1]=m.modes[j];
        --m.modeCount;++m.skippedTimings;
    }
    output=m;return Status::Ok;
}
const char* statusString(Status s) { switch(s) {
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(Truncated);NAME(BadHeader);NAME(BadChecksum);
    NAME(UnsupportedVersion);NAME(AnalogDisplay);NAME(TooManyBlocks);NAME(BadExtension);
    NAME(BadTiming);NAME(ConflictingCapabilities);NAME(TooManyModes);
#undef NAME
    }return "Unknown";
}
} }
