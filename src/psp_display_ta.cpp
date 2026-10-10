// SPDX-License-Identifier: MIT
// Wire ABI: Linux3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0
// drivers/gpu/drm/amd/display/modules/hdcp/{hdcp_psp.h,hdcp_psp.c}.
// Lease, token and fail-stop lifetime handling are HackyNavi policy.
#include "hackynavi/psp_display_ta.hpp"
namespace hackynavi { namespace pspdisplayta {
namespace {
constexpr uint32_t kDtmUpdate=4,kHdcp1Create=1,kHdcp1Destroy=2,kHdcp1Query=7;
constexpr uint32_t kHdcp2Create=14,kHdcp2Destroy=9,kHdcp2Query=12;
constexpr size_t kHdcpOutput=5136;
bool overlap(const void* a,size_t an,const void* b,size_t bn) {
    const uintptr_t x=reinterpret_cast<uintptr_t>(a),y=reinterpret_cast<uintptr_t>(b);
    return x<=y ? y-x<an : x-y<bn;
}
void zero(uint8_t* p,size_t n) { for(size_t i=0;i<n;++i)p[i]=0; }
void put(uint8_t* p,uint32_t v) {for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
uint32_t get(const uint8_t* p) {
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
bool pollValid(const pspring::PollOptions& p) {
    return p.maxPolls && p.maxPolls<=1000000 && p.delayUs && p.delayUs<=1000 &&
        uint64_t(p.maxPolls)*p.delayUs<=10000000;
}
struct Request {
    pspta::Kind kind; uint32_t command,input[15]; size_t outputBytes;
};
size_t inputOffset(const Request& q) { return q.kind==pspta::Kind::Dtm?16:12; }
size_t statusOffset(const Request& q) { return q.kind==pspta::Kind::Dtm?8:4; }
size_t outputOffset(const Request& q) { return q.kind==pspta::Kind::Dtm?76:kHdcpOutput; }
uint8_t expected(const Request& q,size_t at) {
    if(at<4)return uint8_t(q.command>>(8*at));
    if(at==statusOffset(q))return 1; // GenericFailure sentinel, never zero-before-invoke.
    const size_t start=inputOffset(q);
    if(at>=start && at<start+sizeof(q.input))
        return uint8_t(q.input[(at-start)/4]>>(8*((at-start)%4)));
    return 0;
}
bool responseMatches(const Request& q,const uint8_t* bytes) {
    const size_t st=statusOffset(q),out=outputOffset(q);
    for(size_t i=0;i<pspta::kSharedBytes;++i) {
        if((i>=st&&i<st+4) || (i>=out&&i<out+q.outputBytes) ||
           (q.kind==pspta::Kind::Dtm&&i>=4&&i<8))continue;
        if(bytes[i]!=expected(q,i))return false;
    }
    return true;
}
} // namespace
State::State():phase_(Phase::Fresh),ring_(nullptr),io_{},nextSerial_(1),guardLost_(false),records_{} {}
struct Access {
    static Status finish(State& s,Result& r,Status status) {
        r.retentionRequired=s.retentionRequired();r.status=status;return status;
    }
    static Status fault(State& s,Result& r,Status status) {
        const bool first=s.phase_!=Phase::FaultedRetained;
        s.phase_=Phase::FaultedRetained;
        if(s.ring_) {
            s.ring_->phase_=pspring::Phase::Faulted;
            for(unsigned i=0;i<2;++i)if(s.ring_->ta_[i].info.retained)
                s.ring_->ta_[i].info.phase=pspring::TaPhase::FaultedRetained;
        }
        if(first&&s.io_.fault)s.io_.fault(s.io_.ring.context);
        return finish(s,r,status);
    }
    static bool valid(State& s) {
        if(s.guardLost_ || !s.io_.valid || !s.io_.valid(s.io_.ring.context)) {
            s.guardLost_=true;return false;
        }
        return true;
    }
    static bool read(void* c,uint32_t a,uint32_t& v) {
        auto& s=*static_cast<State*>(c);
        return valid(s)&&s.io_.ring.read32(s.io_.ring.context,a,v)&&valid(s);
    }
    static bool write(void* c,uint32_t a,uint32_t v) {
        auto& s=*static_cast<State*>(c);
        return valid(s)&&s.io_.ring.write32(s.io_.ring.context,a,v)&&valid(s);
    }
    static bool flush(void* c) {
        auto& s=*static_cast<State*>(c);
        return valid(s)&&s.io_.ring.flushHdp(s.io_.ring.context)&&valid(s);
    }
    static bool acquire(void* c) {
        auto& s=*static_cast<State*>(c);
        return valid(s)&&s.io_.ring.beforeReadback(s.io_.ring.context)&&valid(s);
    }
    static void delayUs(void* c,uint32_t us) {
        auto& s=*static_cast<State*>(c);
        if(valid(s)){s.io_.ring.delayUs(s.io_.ring.context,us);(void)valid(s);}
    }
    static void delayMs(void* c,uint32_t ms) {
        auto& s=*static_cast<State*>(c);
        if(valid(s)){s.io_.ring.delayMs(s.io_.ring.context,ms);(void)valid(s);}
    }
    static pspring::Io io(State& s) {return {&s,read,write,flush,acquire,delayUs,delayMs};}
    static bool aliases(const State& s,const void* p,size_t n) {
        if(overlap(&s,sizeof(s),p,n))return true;
        if(!s.ring_)return false;
        const auto& x=*s.ring_;
        return overlap(&x,sizeof(x),p,n)||overlap(x.allocator_,sizeof(*x.allocator_),p,n)||
            overlap(x.buffers_.scratch,x.buffers_.scratchBytes,p,n);
    }
    static Status begin(State& s,Result& r,const pspring::PollOptions* p=nullptr) {
        // Result aliases cannot safely receive an error without corrupting owner state.
        if(aliases(s,&r,sizeof(r)))return Status::AliasedStorage;
        r=Result{};
        if(s.phase_!=Phase::Bound||!s.ring_)return finish(s,r,Status::InvalidState);
        if(p&&!pollValid(*p))return finish(s,r,Status::InvalidArgument);
        auto& x=*s.ring_;
        if(x.phase_!=pspring::Phase::Running)return fault(s,r,Status::InvalidState);
        if(x.pending_)return finish(s,r,Status::Busy);
        if(!x.tmrReady_||!x.displayBindingsValid())return fault(s,r,Status::InvalidAllocation);
        for(unsigned i=0;i<2;++i) {
            const auto& t=x.ta_[i];
            if(t.displayOwner!=&s||t.info.phase!=pspring::TaPhase::Loaded||
               !t.info.retained||!t.info.sessionValid||!t.info.loadAcknowledged)
                return fault(s,r,Status::InvalidState);
        }
        if(!valid(s))return fault(s,r,Status::GuardFailed);
        return Status::Ok;
    }
    static Status bind(pspring::State& x,const Io& io,State& s,Result& r) {
        if(overlap(&s,sizeof(s),&r,sizeof(r))||overlap(&x,sizeof(x),&r,sizeof(r))||
           overlap(&s,sizeof(s),&x,sizeof(x))||overlap(&io,sizeof(io),&r,sizeof(r))||
           overlap(&s,sizeof(s),&io,sizeof(io))||overlap(&x,sizeof(x),&io,sizeof(io))||
           (x.allocator_&&(overlap(x.allocator_,sizeof(*x.allocator_),&s,sizeof(s))||
             overlap(x.allocator_,sizeof(*x.allocator_),&r,sizeof(r))||
             overlap(x.allocator_,sizeof(*x.allocator_),&io,sizeof(io))))||
           (x.buffers_.scratch&&(overlap(x.buffers_.scratch,x.buffers_.scratchBytes,&s,sizeof(s))||
             overlap(x.buffers_.scratch,x.buffers_.scratchBytes,&r,sizeof(r))||
             overlap(x.buffers_.scratch,x.buffers_.scratchBytes,&io,sizeof(io)))))return Status::AliasedStorage;
        r=Result{};
        if(s.phase_!=Phase::Fresh||x.phase_!=pspring::Phase::Running||x.pending_||!x.tmrReady_)
            return finish(s,r,Status::InvalidState);
        if(!io.valid||!io.fault||!io.ring.read32||!io.ring.write32||!io.ring.flushHdp||
           !io.ring.beforeReadback||!io.ring.delayUs||!io.ring.delayMs||
           !x.buffers_.scratch||x.buffers_.scratchBytes<pspta::kSharedBytes)
            return finish(s,r,Status::InvalidArgument);
        if(!x.displayBindingsValid())return finish(s,r,Status::InvalidAllocation);
        for(unsigned i=0;i<2;++i) {
            const auto& t=x.ta_[i];
            if(t.displayOwner||t.info.phase!=pspring::TaPhase::Loaded||!t.info.retained||
               !t.info.sessionValid||!t.info.loadAcknowledged||t.info.invocations)
                return finish(s,r,Status::InvalidState);
        }
        if(!io.valid(io.ring.context))return finish(s,r,Status::GuardFailed);
        s.ring_=&x;s.io_=io;s.phase_=Phase::Bound;
        for(unsigned i=0;i<2;++i)x.ta_[i].displayOwner=&s;
        return finish(s,r,Status::Ok);
    }
    static Status invoke(State& s,const Request& q,const pspring::PollOptions& p,Result& r) {
        auto& x=*s.ring_;uint8_t* bytes=x.buffers_.scratch;
        const auto shared=x.ta_[static_cast<unsigned>(q.kind)].shared;
        zero(bytes,pspta::kSharedBytes);put(bytes,q.command);put(bytes+statusOffset(q),1);
        for(unsigned i=0;i<15;++i)put(bytes+inputOffset(q)+4*i,q.input[i]);
        r.command=q.command;r.applicationStatus=1;
        const VramTransferIo transport={&s,x.layout_.barBytes,read,write};
        r.sharedWriteAttempted=true;
        if(uploadVram(*x.allocator_,shared,0,bytes,pspta::kSharedBytes,transport,r.transfer)!=VramTransferStatus::Ok)
            return fault(s,r,s.guardLost_?Status::GuardFailed:Status::TransferFailed);
        if(!flush(&s))return fault(s,r,s.guardLost_?Status::GuardFailed:Status::VisibilityFailed);
        const auto wrapped=io(s);r.invokeAttempted=true;
        if(x.submitDisplayInvoke(wrapped,q.kind,q.command,s,r.psp)!=pspring::Status::Ok||
           pspring::wait(wrapped,x,p,r.psp)!=pspring::Status::Ok)
            return fault(s,r,s.guardLost_?Status::GuardFailed:Status::PspFailed);
        r.pspAcknowledged=true;
        if(!acquire(&s))return fault(s,r,s.guardLost_?Status::GuardFailed:Status::VisibilityFailed);
        if(readVram(*x.allocator_,shared,0,bytes,pspta::kSharedBytes,transport,r.transfer)!=VramTransferStatus::Ok)
            return fault(s,r,s.guardLost_?Status::GuardFailed:Status::TransferFailed);
        r.applicationStatus=get(bytes+statusOffset(q));
        if(q.kind==pspta::Kind::Dtm)r.responseId=get(bytes+4); // Linux does not define response-id matching.
        if(!responseMatches(q,bytes))return fault(s,r,Status::ResponseCorrupt);
        if(r.applicationStatus)return fault(s,r,Status::ApplicationRejected);
        // The native deadline/generation can expire while validating the full
        // shared image. Never publish a new owned object after that guard lost.
        if(!valid(s))return fault(s,r,Status::GuardFailed);
        r.applicationAcknowledged=true;return finish(s,r,Status::Ok);
    }
    static bool display(const State& s,const Display& token) {
        return token.owner_==&s&&token.slot_<kDisplays&&token.serial_&&
            s.records_[token.slot_].displaySerial==token.serial_&&s.records_[token.slot_].topologyActive;
    }
    static bool auth(const State& s,const Authentication& token) {
        return token.owner_==&s&&token.slot_<kDisplays&&token.serial_&&
            s.records_[token.slot_].authSerial==token.serial_&&s.records_[token.slot_].sessionValid;
    }
    static Status add(State& s,const Topology& top,const pspring::PollOptions& p,Display& out,Result& r) {
        if(aliases(s,&out,sizeof(out))||aliases(s,&top,sizeof(top))||aliases(s,&p,sizeof(p))||
           overlap(&out,sizeof(out),&top,sizeof(top))||overlap(&out,sizeof(out),&p,sizeof(p))||
           overlap(&out,sizeof(out),&r,sizeof(r))||
           overlap(&top,sizeof(top),&r,sizeof(r))||overlap(&p,sizeof(p),&r,sizeof(r)))return Status::AliasedStorage;
        const Status started=begin(s,r,&p);if(started!=Status::Ok)return started;
        if(display(s,out))return finish(s,r,Status::Busy);
        if(top.otg>=4||top.ddc>=5||top.streamEncoder>=5||top.transmitter>=5)return finish(s,r,Status::InvalidArgument);
        unsigned slot=kDisplays;
        for(unsigned i=0;i<kDisplays;++i) {
            const auto& rec=s.records_[i];
            if(!rec.topologyMayExist){if(slot==kDisplays)slot=i;continue;}
            if(rec.topology.otg==top.otg||rec.topology.ddc==top.ddc||
               rec.topology.streamEncoder==top.streamEncoder||rec.topology.transmitter==top.transmitter)
                return finish(s,r,Status::Busy);
        }
        if(slot==kDisplays||s.nextSerial_==UINT32_MAX)return finish(s,r,Status::Exhausted);
        Request q={pspta::Kind::Dtm,kDtmUpdate,{},4};
        q.input[0]=slot;q.input[1]=1;q.input[3]=uint32_t(top.otg)+1;q.input[4]=uint32_t(top.ddc)+1;
        q.input[5]=top.transmitter;q.input[6]=top.streamEncoder;q.input[9]=23;
        q.input[10]=0x10;q.input[11]=top.transmitter;q.input[12]=top.hdcpCapable?1:0;
        q.input[13]=1;q.input[14]=top.transmitter;
        auto& rec=s.records_[slot];rec.topology=top;rec.displaySerial=s.nextSerial_++;rec.topologyMayExist=true;
        const Status status=invoke(s,q,p,r);if(status!=Status::Ok)return status;
        rec.topologyActive=true;out.owner_=&s;out.slot_=uint8_t(slot);out.serial_=rec.displaySerial;
        return finish(s,r,Status::Ok);
    }
    static Status remove(State& s,const Display& token,const pspring::PollOptions& p,Result& r) {
        if(aliases(s,&token,sizeof(token))||aliases(s,&p,sizeof(p))||
           overlap(&token,sizeof(token),&r,sizeof(r))||overlap(&p,sizeof(p),&r,sizeof(r)))return Status::AliasedStorage;
        const Status started=begin(s,r,&p);if(started!=Status::Ok)return started;
        if(!display(s,token))return finish(s,r,Status::StaleToken);
        auto& rec=s.records_[token.slot_];if(rec.sessionMayExist)return finish(s,r,Status::Busy);
        Request q={pspta::Kind::Dtm,kDtmUpdate,{},4};q.input[0]=token.slot_;
        const Status status=invoke(s,q,p,r);if(status!=Status::Ok)return status;
        rec.topologyMayExist=false;rec.topologyActive=false;return finish(s,r,Status::Ok);
    }
    static Status create(State& s,const Display& token,Protocol protocol,Content content,
        const pspring::PollOptions& p,Authentication& out,Result& r) {
        if(aliases(s,&out,sizeof(out))||aliases(s,&token,sizeof(token))||aliases(s,&p,sizeof(p))||
           overlap(&out,sizeof(out),&token,sizeof(token))||overlap(&out,sizeof(out),&p,sizeof(p))||
           overlap(&out,sizeof(out),&r,sizeof(r))||
           overlap(&token,sizeof(token),&r,sizeof(r))||overlap(&p,sizeof(p),&r,sizeof(r)))return Status::AliasedStorage;
        const Status started=begin(s,r,&p);if(started!=Status::Ok)return started;
        if(auth(s,out))return finish(s,r,Status::Busy);
        if(!display(s,token))return finish(s,r,Status::StaleToken);
        if((protocol!=Protocol::Hdcp1&&protocol!=Protocol::Hdcp2)||
           (content!=Content::Type0&&content!=Content::Type1&&content!=Content::Maximum)||
           (protocol==Protocol::Hdcp1&&content!=Content::Type0))return finish(s,r,Status::InvalidArgument);
        auto& rec=s.records_[token.slot_];if(rec.sessionMayExist)return finish(s,r,Status::Busy);
        if(!rec.topology.hdcpCapable)return finish(s,r,Status::InvalidArgument);
        if(s.nextSerial_==UINT32_MAX)return finish(s,r,Status::Exhausted);
        Request q={pspta::Kind::Hdcp,protocol==Protocol::Hdcp1?kHdcp1Create:kHdcp2Create,{},protocol==Protocol::Hdcp1?32u:4u};
        q.input[0]=token.slot_;if(protocol==Protocol::Hdcp2)q.input[1]=static_cast<uint32_t>(content);
        rec.protocol=protocol;rec.sessionMayExist=true;rec.authSerial=s.nextSerial_++;
        const Status status=invoke(s,q,p,r);if(status!=Status::Ok)return status;
        const auto* bytes=s.ring_->buffers_.scratch+kHdcpOutput;r.sessionId=get(bytes);
        if(r.sessionId==0xffff)return fault(s,r,Status::ResponseCorrupt);
        for(unsigned i=0;i<kDisplays;++i)if(i!=token.slot_&&s.records_[i].sessionValid&&s.records_[i].session==r.sessionId)
            return fault(s,r,Status::ResponseCorrupt);
        rec.session=r.sessionId;rec.sessionValid=true;r.sessionValid=true;
        if(protocol==Protocol::Hdcp1) {
            for(unsigned i=0;i<8;++i)r.an[i]=bytes[4+i];
            for(unsigned i=0;i<5;++i)r.aksv[i]=bytes[12+i];r.ainfo=bytes[17];
        }
        out.owner_=&s;out.slot_=token.slot_;out.serial_=rec.authSerial;return finish(s,r,Status::Ok);
    }
    static Status session(State& s,const Authentication& token,const pspring::PollOptions& p,bool destroy,Result& r) {
        if(aliases(s,&token,sizeof(token))||aliases(s,&p,sizeof(p))||
           overlap(&token,sizeof(token),&r,sizeof(r))||overlap(&p,sizeof(p),&r,sizeof(r)))return Status::AliasedStorage;
        const Status started=begin(s,r,&p);if(started!=Status::Ok)return started;
        if(!auth(s,token))return finish(s,r,Status::StaleToken);
        auto& rec=s.records_[token.slot_];const bool one=rec.protocol==Protocol::Hdcp1;
        const uint32_t cmd=destroy?(one?kHdcp1Destroy:kHdcp2Destroy):(one?kHdcp1Query:kHdcp2Query);
        Request q={pspta::Kind::Hdcp,cmd,{},destroy?0u:(one?4u:8u)};q.input[0]=rec.session;
        const Status status=invoke(s,q,p,r);if(status!=Status::Ok)return status;
        r.sessionId=rec.session;
        if(destroy){rec.sessionMayExist=false;rec.sessionValid=false;rec.session=0;}
        else {
            const auto* bytes=s.ring_->buffers_.scratch+kHdcpOutput;
            r.contentType=one?0:get(bytes);r.protectionLevel=get(bytes+(one?0:4));
            if(r.protectionLevel>1||(!one&&(r.contentType>2||(r.protectionLevel&&r.contentType==0))))
                return fault(s,r,Status::ResponseCorrupt);
            r.sessionValid=true;r.protectionValid=true;
        }
        return finish(s,r,Status::Ok);
    }
    static Status retire(State& s,Result& r) {
        const Status started=begin(s,r);if(started!=Status::Ok)return started;
        for(unsigned i=0;i<kDisplays;++i)if(s.records_[i].topologyMayExist||s.records_[i].sessionMayExist)
            return finish(s,r,Status::Busy);
        for(unsigned i=0;i<2;++i)s.ring_->ta_[i].displayOwner=nullptr;
        s.phase_=Phase::Retired;return finish(s,r,Status::Ok);
    }
};
Status bind(pspring::State& x,const Io& io,State& s,Result& r){return Access::bind(x,io,s,r);}
Status addDisplay(State& s,const Topology& t,const pspring::PollOptions& p,Display& out,Result& r){return Access::add(s,t,p,out,r);}
Status removeDisplay(State& s,const Display& t,const pspring::PollOptions& p,Result& r){return Access::remove(s,t,p,r);}
Status createAuthentication(State& s,const Display& t,Protocol v,Content c,const pspring::PollOptions& p,Authentication& out,Result& r){return Access::create(s,t,v,c,p,out,r);}
Status queryAuthentication(State& s,const Authentication& t,const pspring::PollOptions& p,Result& r){return Access::session(s,t,p,false,r);}
Status destroyAuthentication(State& s,const Authentication& t,const pspring::PollOptions& p,Result& r){return Access::session(s,t,p,true,r);}
Status retire(State& s,Result& r){return Access::retire(s,r);}
const char* statusString(Status s) {
    switch(s) {
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(InvalidState);NAME(InvalidAllocation);NAME(AliasedStorage);
    NAME(StaleToken);NAME(Busy);NAME(Exhausted);NAME(GuardFailed);NAME(TransferFailed);NAME(VisibilityFailed);
    NAME(PspFailed);NAME(ApplicationRejected);NAME(ResponseCorrupt);
#undef NAME
    }return "Unknown";
}
} }
