// SPDX-License-Identifier: MIT
// Protocol from AMD Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0
// display/dc/link/protocols/link_ddc.c:write_scdc_data/read_scdc_data,
// link_dpms.c:enable_link_hdmi/disable_link, link_hdmi_frl.c (FRL admission).
// Register programming is in dio_link.cpp, with its AMD copyright notice.
// The 1ms SCDC ratio settle requirement is documented in that revision's
// drivers/gpu/drm/display/drm_scdc_helper.c. No sink-lock proof is inferred.
#include "hackynavi/hdmi_link.hpp"
namespace hackynavi { namespace hdmitmds {
namespace {
bool valid(const Io& io) {return io.readScdc && io.writeScdc && io.sourceConfigure &&
    io.sourceEnable && io.sourceDisable && io.ddcHealthy && io.sourceHealthy && io.delayUs;}
bool valid(const dmcub::PollOptions& p) {return p.maxPolls && p.maxPolls<=2000000 &&
    p.delayUs && p.delayUs<=100 && uint64_t(p.maxPolls)*p.delayUs<=2000000;}
bool limit(const Limits& c) {return c.maxCharacterClockKHz>=10 &&
    c.maxCharacterClockKHz<=600000 && !(c.deepColorMask&~7U);}
class Sequence {
    const Io& io;Result& out;
    bool rd(uint8_t at,uint8_t& value) {
        ++out.scdcReads;out.lastScdcOffset=at;
        if(!io.readScdc(io.context,at,value))return false;
        out.lastScdcValue=value;return true;
    }
    bool wr(uint8_t at,uint8_t value) {
        ++out.scdcWrites;out.lastScdcOffset=at;out.lastScdcValue=value;
        return io.writeScdc(io.context,at,value);
    }
    Status configureSink(uint8_t bits) {
        uint8_t value=0;
        if(!rd(0x20,value))return Status::ScdcFailed;
        // Preserve reserved bits. Ownership starts before a possibly landed write.
        out.sinkMayBeChanged=true;
        if(!wr(0x20,uint8_t((value&~3U)|bits)))return Status::ScdcFailed;
        if(!rd(0x20,value))return Status::ScdcFailed;
        if((value&3)!=bits)return Status::VerifyFailed;
        io.delayUs(io.context,1000);out.delayUs+=1000;return Status::Ok;
    }
    Status finish(Status status) {out.status=status;return status;}
    Status failure(Status status) {
        if(out.sourceConfigureAttempted || out.sinkMayBeChanged) {
            out.cleanupAttempted=true;
            if(io.sourceHealthy(io.context)) {
                ++out.sourceCommands;out.sourceDisabled=io.sourceDisable(io.context);
            }
            // Never change receiver ratio underneath a possibly running source.
            if(out.sourceDisabled && out.sinkMayBeChanged && io.ddcHealthy(io.context))
                out.sinkReset=configureSink(0)==Status::Ok;
            out.recoveryRequired=!out.sourceDisabled ||
                (out.sinkMayBeChanged && !out.sinkReset) || !io.ddcHealthy(io.context);
        }
        // A timeout during even the first SCDC read can leave the shared
        // I2C controller/pins owned, independently of whether a PHY was touched.
        out.recoveryRequired=out.recoveryRequired || !io.ddcHealthy(io.context);
        return finish(status);
    }
public:
    Sequence(const Io& i,Result& r):io(i),out(r) {}
    Status activate(const Config& cfg) {
        out.stage=Stage::Capabilities;
        if(cfg.sink.scdc) {
            if(!rd(1,out.sinkVersion))return failure(Status::ScdcFailed);
            if(!out.sinkVersion || out.sinkVersion==255)return failure(Status::UnsupportedReceiver);
            if(cfg.sink.frlCapable) {
                uint8_t frl=0;if(!rd(0x31,frl))return failure(Status::ScdcFailed);
                if(frl&15)return failure(Status::UnsupportedReceiver);
            }
        }
        out.stage=Stage::SourceConfigure;out.sourceConfigureAttempted=true;++out.sourceCommands;
        if(!io.sourceConfigure(io.context,out.configuration.source))return failure(Status::SourceFailed);
        out.sourceConfigured=true;
        if(cfg.sink.scdc) {
            out.stage=Stage::SinkConfigure;uint8_t sourceVersion=0;
            if(!wr(2,1) || !rd(2,sourceVersion))return failure(Status::ScdcFailed);
            if(sourceVersion!=1)return failure(Status::VerifyFailed);
            const auto status=configureSink(out.configuration.sinkConfig);
            if(status!=Status::Ok)return failure(status);
        }
        out.stage=Stage::SourceEnable;out.sourceEnableAttempted=true;++out.sourceCommands;
        if(!io.sourceEnable(io.context,out.configuration.characterClockKHz))return failure(Status::SourceFailed);
        out.stage=Stage::Verify;
        if(cfg.sink.scdc) {
            uint8_t value=0;if(!rd(0x20,value))return failure(Status::ScdcFailed);
            if((value&3)!=out.configuration.sinkConfig)return failure(Status::VerifyFailed);
        }
        out.activated=true;out.stage=Stage::Complete;return finish(Status::Ok);
    }
    Status deactivate(bool scdc) {
        out.stage=Stage::SourceDisable;++out.sourceCommands;
        if(!io.sourceHealthy(io.context) || !io.sourceDisable(io.context)) {
            out.recoveryRequired=true;return finish(Status::SourceFailed);
        }
        out.sourceDisabled=true;
        if(scdc) {
            out.stage=Stage::SinkDisable;
            if(!io.ddcHealthy(io.context)) {out.recoveryRequired=true;return finish(Status::ScdcFailed);}
            const auto status=configureSink(0);
            out.sinkReset=status==Status::Ok;
            // Sink config uncertainty alone is not local GPU memory retention.
            out.recoveryRequired=!io.ddcHealthy(io.context);
            if(status!=Status::Ok)return finish(status);
        }
        out.stage=Stage::Complete;return finish(Status::Ok);
    }
};
struct Hardware {
    hdmiddc::Engine& ddc;dio::Link& source;dmcub::Inbox& inbox;
    const dmcub::PollOptions& polls;const Delay& delay;HardwareResult& out;
    static bool read(void* c,uint8_t at,uint8_t& value) {
        auto& h=*static_cast<Hardware*>(c);h.out.ddcStatus=h.ddc.readScdc(at,1,h.out.ddc);
        if(h.out.ddcStatus!=hdmiddc::Status::Ok || h.out.ddc.bytes!=1)return false;
        value=h.out.ddc.data[0];return true;
    }
    static bool write(void* c,uint8_t at,uint8_t value) {
        auto& h=*static_cast<Hardware*>(c);h.out.ddcStatus=h.ddc.writeScdc(at,&value,1,h.out.ddc);
        return h.out.ddcStatus==hdmiddc::Status::Ok;
    }
    static bool configure(void* c,const dio::HdmiConfig& cfg) {
        auto& h=*static_cast<Hardware*>(c);
        h.out.sourceStatus=h.source.configureHdmi(h.inbox,cfg,h.polls,h.out.source);
        return h.out.sourceStatus==dio::Status::Ok;
    }
    static bool enable(void* c,uint32_t clock) {
        auto& h=*static_cast<Hardware*>(c);
        h.out.sourceStatus=h.source.enable(h.inbox,clock,h.polls,h.out.source);
        return h.out.sourceStatus==dio::Status::Ok;
    }
    static bool disable(void* c) {
        auto& h=*static_cast<Hardware*>(c);
        h.out.sourceStatus=h.source.disable(h.inbox,h.polls,h.out.source);
        return h.out.sourceStatus==dio::Status::Ok;
    }
    static bool ddcHealthy(void* c) {return static_cast<Hardware*>(c)->ddc.isOpen();}
    static bool sourceHealthy(void* c) {auto& h=*static_cast<Hardware*>(c);
        return !h.source.needsRecovery() && h.inbox.isOpen();}
    static void wait(void* c,uint32_t us) {auto& h=*static_cast<Hardware*>(c);h.delay.delayUs(h.delay.context,us);}
    Io io() {return {this,read,write,configure,enable,disable,ddcHealthy,sourceHealthy,wait};}
};
bool routed(const hdmiddc::Engine& ddc,const dio::Link& source,const dmcub::Inbox& inbox) {
    return ddc.isOpen() && inbox.isOpen() && !source.needsRecovery() &&
        (source.route().connectorObject&255)==0x0c && source.laneCount()==4 &&
        ddc.portIndex()==source.route().ddc;
}
} // namespace
Status plan(const Config& cfg,Plan& out) {
    out=Plan{};
    if(!limit(cfg.source) || !limit(cfg.sink.limits))return Status::InvalidArgument;
    if(cfg.encoding!=Encoding::Rgb && cfg.encoding!=Encoding::YCbCr444)return Status::UnsupportedFormat;
    if((cfg.encoding==Encoding::YCbCr444 && !cfg.sink.ycbcr444) ||
        (cfg.bitsPerComponent>8 && cfg.encoding==Encoding::YCbCr444 && !cfg.sink.deepColor444))return Status::LinkLimits;
    // Calculate using scramble=true first so high-rate requirements can be
    // checked against receiver capabilities, not rejected as malformed config.
    dio::HdmiConfig source={cfg.pixelClockKHz,cfg.bitsPerComponent,true};uint32_t clock=0;
    if(dio::hdmiCharacterClock(source,clock)!=dio::Status::Ok)return Status::InvalidArgument;
    const uint8_t depth=cfg.bitsPerComponent==8?0:cfg.bitsPerComponent==10?1:cfg.bitsPerComponent==12?2:4;
    if(clock>cfg.source.maxCharacterClockKHz || clock>cfg.sink.limits.maxCharacterClockKHz ||
        (depth && (!(cfg.source.deepColorMask&depth) || !(cfg.sink.limits.deepColorMask&depth))))return Status::LinkLimits;
    const bool high=clock>340000;
    if((high && !cfg.sink.scdc) || (cfg.scrambleLowRates &&
        (!cfg.sink.scdc || !cfg.sink.scrambleLowRates)) || (cfg.sink.frlCapable && !cfg.sink.scdc))return Status::LinkLimits;
    source.scramble=high || cfg.scrambleLowRates;
    out.source=source;out.characterClockKHz=clock;out.sinkConfig=uint8_t((source.scramble?1:0)|(high?2:0));
    return Status::Ok;
}
Status activate(const Io& io,const Config& cfg,Result& out) {
    out=Result{};
    if(!valid(io)) {out.status=Status::InvalidArgument;return out.status;}
    out.status=plan(cfg,out.configuration);if(out.status!=Status::Ok)return out.status;
    if(!io.sourceHealthy(io.context) || (cfg.sink.scdc && !io.ddcHealthy(io.context))) {
        out.status=Status::InvalidState;return out.status;
    }
    return Sequence(io,out).activate(cfg);
}
Status deactivate(const Io& io,bool scdc,Result& out) {
    out=Result{};
    if(!valid(io)) {out.status=Status::InvalidArgument;return out.status;}
    return Sequence(io,out).deactivate(scdc);
}
Status activate(hdmiddc::Engine& ddc,dio::Link& source,dmcub::Inbox& inbox,
    const Config& cfg,const dmcub::PollOptions& polls,const Delay& delay,HardwareResult& out) {
    out=HardwareResult{};
    if(!valid(polls) || !delay.delayUs) {out.link.status=Status::InvalidArgument;return out.link.status;}
    if(!routed(ddc,source,inbox) || source.phase()!=dio::Phase::Prepared) {
        out.link.status=Status::InvalidState;return out.link.status;
    }
    Hardware hw={ddc,source,inbox,polls,delay,out};return activate(hw.io(),cfg,out.link);
}
Status deactivate(hdmiddc::Engine& ddc,dio::Link& source,dmcub::Inbox& inbox,
    bool scdc,const dmcub::PollOptions& polls,const Delay& delay,HardwareResult& out) {
    out=HardwareResult{};
    if(!valid(polls) || !delay.delayUs) {out.link.status=Status::InvalidArgument;return out.link.status;}
    // A closed/lost DDC does not prevent observing and stopping the LOCAL PHY.
    if(!inbox.isOpen() || source.needsRecovery() || source.phase()==dio::Phase::Closed ||
        (source.route().connectorObject&255)!=0x0c || source.laneCount()!=4 ||
        (ddc.isOpen() && ddc.portIndex()!=source.route().ddc)) {
        out.link.status=Status::InvalidState;return out.link.status;
    }
    Hardware hw={ddc,source,inbox,polls,delay,out};return deactivate(hw.io(),scdc,out.link);
}
const char* statusString(Status s) {
    switch(s) {
#define S(x) case Status::x:return #x
    S(Ok);S(InvalidArgument);S(InvalidState);S(UnsupportedFormat);S(UnsupportedReceiver);
    S(LinkLimits);S(ScdcFailed);S(SourceFailed);S(VerifyFailed);
#undef S
    }return "Unknown";
}
} }
