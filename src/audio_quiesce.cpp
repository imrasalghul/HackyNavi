// SPDX-License-Identifier: MIT
// Independently implemented from register/sequence facts in Intel HDA1.0a,
// https://www.intel.com/content/dam/www/public/us/en/documents/product-specifications/high-definition-audio-specification.pdf
// sections3.3.1,3.3.7,3.3.8,3.3.13,3.3.23,3.3.29,3.3.33,3.3.35 and4.5.5;
// cross-checked against Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0:
// include/sound/hda_register.h; sound/hda/core/controller.c
// (hdac_wait_for_cmd_dmas, snd_hdac_bus_stop_cmd_io/enter_link_reset/stop_chip)
// and sound/hda/core/stream.c (snd_hdac_stream_clear). No Linux code copied.
// controller.c uses100ms ring/reset waits. Stream waits here share that bounded
// software policy, not a specification guarantee of DMA retirement by100ms.
#include "hackynavi/audio_quiesce.hpp"
namespace hackynavi { namespace audioquiesce { namespace {
constexpr uint32_t GCAP=0,VMIN=2,VMAJ=3,GCTL=8,WAKEEN=0xc,INTCTL=0x20;
constexpr uint32_t CORBCTL=0x4c,RIRBCTL=0x5c,DPLBASE=0x70;
constexpr uint32_t kPollUs=10,kPhaseUs=100000,kResetHoldUs=100;
uint32_t streamOffset(uint8_t stream){return 0x80+uint32_t(stream)*0x20;}
struct Run {
    const Io& io;Result& r;uint64_t bytes,start,last;uint32_t budget;
    bool fail(Status s){r.status=s;return false;}
    bool check() {
        if(!io.valid(io.context))return fail(Status::Revoked);
        uint64_t now=0;if(!io.nowUs(io.context,now)||now<last)return fail(Status::ClockFailure);
        last=now;r.elapsedUs=now-start;
        return r.elapsedUs<budget||fail(Status::Timeout);
    }
    bool read(uint32_t offset,uint8_t width,uint32_t& value) {
        if(!check())return false;
        if(offset>bytes||width>bytes-offset)return fail(Status::OutsideBar);
        r.lastOffset=offset;r.lastWidth=width;r.lastValue=0;r.lastReadSucceeded=false;++r.reads;
        uint8_t b=0;uint16_t w=0;uint32_t d=0;bool ok=false;
        if(width==1){ok=io.read8(io.context,offset,b);d=b;}
        else if(width==2){ok=io.read16(io.context,offset,w);d=w;}
        else ok=io.read32(io.context,offset,d);
        if(!ok)return fail(Status::ReadFailed);
        value=d;r.lastValue=d;r.lastReadSucceeded=true;
        if(d==(width==1?0xffU:width==2?0xffffU:UINT32_MAX))return fail(Status::DeviceUnavailable);
        return check();
    }
    bool write(uint32_t offset,uint8_t width,uint32_t value) {
        if(!check())return false;
        if(offset>bytes||width>bytes-offset)return fail(Status::OutsideBar);
        r.lastOffset=offset;r.lastWidth=width;r.lastValue=value;r.lastReadSucceeded=false;
        r.touched=true;++r.writes;
        const bool ok=width==1?io.write8(io.context,offset,uint8_t(value)):
            width==2?io.write16(io.context,offset,uint16_t(value)):io.write32(io.context,offset,value);
        if(!ok)return fail(Status::WriteFailed);
        return check();
    }
    bool delay(uint32_t us) {
        if(!check())return false;
        if(!io.delayUs(io.context,us))return fail(Status::ClockFailure);
        return check();
    }
    bool zero(uint32_t offset,uint8_t width,uint32_t mask) {
        const uint64_t began=last;
        // Finite iteration bound also rejects a stalled time source.
        for(unsigned n=0;n<=kPhaseUs/kPollUs;++n) {
            uint32_t value=0;if(!read(offset,width,value))return false;
            if(last-began>=kPhaseUs)return fail(Status::Timeout);
            if(!(value&mask))return true;
            if(!delay(kPollUs))return false;
        }
        return fail(Status::ClockFailure);
    }
    bool clear(uint32_t offset,uint8_t width,uint32_t mask) {
        uint32_t value=0;if(!read(offset,width,value))return false;
        if((value&mask)&&!write(offset,width,value&~mask))return false;
        return zero(offset,width,mask);
    }
    bool holdReset() {
        r.stage=Stage::ResetHold;const uint64_t began=last;
        for(unsigned n=0;n<=kResetHoldUs/kPollUs;++n) {
            uint32_t control=0;
            if(!read(GCTL,4,control))return false;
            if(control&0x103)return fail(Status::StateChanged);
            if(last-began>=kResetHoldUs)return true;
            if(!delay(kPollUs))return false;
        }
        return fail(Status::ClockFailure);
    }
    bool stopped() {
        for(uint8_t i=0;i<r.streamCount;++i) {
            r.stream=i;uint32_t ctl=0;if(!read(streamOffset(i),1,ctl))return false;
            if(ctl&0x1e)return fail(Status::StateChanged);
        }
        r.stream=0xff;
        const uint32_t offsets[]={CORBCTL,RIRBCTL,DPLBASE,INTCTL,WAKEEN};
        const uint8_t widths[]={1,1,4,4,2};
        const uint32_t masks[]={3,7,1,((uint32_t(1)<<r.streamCount)-1)|0xc0000000,0x7fff};
        for(unsigned i=0;i<5;++i){uint32_t value=0;
            if(!read(offsets[i],widths[i],value))return false;
            if(value&masks[i])return fail(Status::StateChanged);
        }
        uint32_t gcap=0,control=0;
        if(!read(GCAP,2,gcap)||!read(GCTL,4,control))return false;
        if(gcap!=r.gcap||(control&0x103)!=1)return fail(Status::StateChanged);
        return true;
    }
    bool execute() {
        uint32_t control=0;if(!read(GCTL,4,control))return false;
        if(control&2)return fail(Status::FlushInProgress); // Never replay FCNTRL1.
        if(!(control&1)) {
            r.initiallyInReset=true;r.stage=Stage::Interrupts;
            return clear(WAKEEN,2,0x7fff)&&holdReset();
        }
        r.stage=Stage::Capabilities;uint32_t major=0,minor=0,gcap=0;
        if(!read(VMAJ,1,major)||!read(VMIN,1,minor)||!read(GCAP,2,gcap))return false;
        if(major!=1||minor!=0)return fail(Status::UnsupportedVersion);
        r.gcap=uint16_t(gcap);r.inputStreams=uint8_t((gcap>>8)&15);
        r.outputStreams=uint8_t((gcap>>12)&15);r.bidirectionalStreams=uint8_t((gcap>>3)&31);
        const unsigned count=r.inputStreams+r.outputStreams+r.bidirectionalStreams;
        if(!count||count>kMaxStreams)return fail(Status::UnsupportedCapabilities);
        r.streamCount=uint8_t(count);r.streamCountsKnown=true;
        if(bytes<streamOffset(r.streamCount))return fail(Status::OutsideBar);
        r.stage=Stage::Interrupts;
        const uint32_t streamMask=(uint32_t(1)<<r.streamCount)-1;
        if(!clear(INTCTL,4,streamMask|0xc0000000)||!clear(WAKEEN,2,0x7fff))return false;
        r.stage=Stage::Streams;
        for(uint8_t i=0;i<r.streamCount;++i) {
            r.stream=i;
            if(!clear(streamOffset(i),1,0x1e))return false;
            r.stoppedStreamMask|=uint32_t(1)<<i;
        }
        r.stream=0xff;r.stage=Stage::CommandDma;
        if(!clear(RIRBCTL,1,7)||!clear(CORBCTL,1,3))return false;
        // Clear UNSOL only while CRST1 and FCNTRL0 are still observed. Clearing
        // bit8 does not replay a flush command or release controller reset.
        if(!read(GCTL,4,control))return false;
        if((control&3)!=1)return fail(Status::StateChanged);
        if((control&0x100)&&!write(GCTL,4,control&~0x100U))return false;
        if(!zero(GCTL,4,0x100))return false;
        r.stage=Stage::PositionDma;if(!clear(DPLBASE,4,1))return false;
        r.stage=Stage::VerifyStopped;if(!stopped())return false;
        r.stage=Stage::ControllerReset;
        // stopped()'s final read is GCTL; preserve reserved fields and do not
        // rewrite any address, status or command-pointer register.
        if(!write(GCTL,4,r.lastValue&~1U)||!zero(GCTL,4,1))return false;
        return holdReset();
    }
};
} // namespace
Status stop(const Io& io,uint64_t barBytes,uint32_t timeout,Result& out) {
    out={};out.status=Status::InvalidArgument;out.stage=Stage::Preflight;out.stream=0xff;
    if(!io.valid||!io.read8||!io.read16||!io.read32||!io.write8||!io.write16||
       !io.write32||!io.nowUs||!io.delayUs||!timeout||timeout>1000000)return out.status;
    if(barBytes<0x80){out.status=Status::OutsideBar;return out.status;}
    if(!io.valid(io.context)){out.status=Status::Revoked;return out.status;}
    uint64_t start=0;if(!io.nowUs(io.context,start)){out.status=Status::ClockFailure;return out.status;}
    Run run{io,out,barBytes,start,start,timeout};
    if(run.execute()&&run.check()){out.status=Status::Ok;out.complete=true;out.stage=Stage::Complete;}
    return out.status;
}
const char* statusString(Status s) {
    switch(s) {
    case Status::Ok:return "HDA reset held; PCI/GPU retirement unproved";
    case Status::InvalidArgument:return "invalid HDA quiesce argument";
    case Status::Revoked:return "audio ownership or power revoked";
    case Status::ReadFailed:return "audio register read failed";
    case Status::WriteFailed:return "audio register write failed; may have landed";
    case Status::DeviceUnavailable:return "audio register returned all ones";
    case Status::UnsupportedVersion:return "unsupported HDA version";
    case Status::UnsupportedCapabilities:return "unsupported HDA stream counts";
    case Status::OutsideBar:return "HDA register outside audio BAR0";
    case Status::Timeout:return "HDA stop deadline expired";
    case Status::ClockFailure:return "HDA monotonic clock or delay failed";
    case Status::StateChanged:return "HDA state changed during quiescence";
    case Status::FlushInProgress:return "inherited HDA flush request remains asserted";
    }return "unknown HDA quiesce status";
}
} }
