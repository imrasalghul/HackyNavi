// SPDX-License-Identifier: MIT
#include "hackynavi/edid_acquire.hpp"
namespace hackynavi { namespace edidacq {
namespace {
Status fail(Result& out,Status status) {out.status=status;return status;}
bool read(const Io& io,uint8_t block,uint8_t* dst,Result& out) {
    out.lastBlock=block;++out.readCalls;return io.readBlock(io.context,block,dst);
}
struct AuxContext {dpaux::Engine& engine;HardwareResult& result;};
bool auxStop(AuxContext& context,uint8_t address) {
    auto& out=context.result;
    if(!context.engine.isOpen())return false;
    const dpaux::Request stop={dpaux::Operation::I2cWrite,address,0,false,nullptr};
    out.stopAttempted=true;out.stopCompleted=false;
    out.stopStatus=context.engine.transferWithRetries(stop,out.stop);
    out.packets+=out.stop.attempts;
    out.stopCompleted=out.stopStatus==dpaux::Status::Ok;
    return out.stopCompleted;
}
bool auxTransfer(AuxContext& context,const dpaux::Request& request) {
    auto& out=context.result;
    out.auxStatus=context.engine.transferWithRetries(request,out.aux);
    out.packets+=out.aux.attempts;
    if(out.auxStatus==dpaux::Status::Ok)return true;
    // An ACK short I2C read has consumed these bytes at the sink. Accept the
    // progress and request only the remainder, never replay the accepted read.
    return request.operation==dpaux::Operation::I2cRead &&
        out.auxStatus==dpaux::Status::ShortReply && out.aux.length &&
        out.aux.length<=request.length;
}
bool auxRead(void* opaque,uint8_t block,uint8_t* bytes) {
    auto& context=*static_cast<AuxContext*>(opaque);
    uint8_t address=0x50;
    if(block>=2) {
        address=0x30;const uint8_t segment=uint8_t(block>>1);
        const dpaux::Request request={dpaux::Operation::I2cWrite,address,1,true,&segment};
        if(!auxTransfer(context,request)) { (void)auxStop(context,address);return false; }
    }
    address=0x50;const uint8_t offset=uint8_t((block&1)*128);
    const dpaux::Request pointer={dpaux::Operation::I2cWrite,address,1,true,&offset};
    if(!auxTransfer(context,pointer)) { (void)auxStop(context,address);return false; }
    for(size_t done=0;done<edid::kBlockBytes;) {
        const size_t remaining=edid::kBlockBytes-done;
        const uint8_t length=uint8_t(remaining<dpaux::kMaxPayload?remaining:dpaux::kMaxPayload);
        const dpaux::Request request={dpaux::Operation::I2cRead,address,length,true,nullptr};
        if(!auxTransfer(context,request) || !context.result.aux.length || context.result.aux.length>length) {
            (void)auxStop(context,address);return false;
        }
        for(size_t i=0;i<context.result.aux.length;++i)bytes[done+i]=context.result.aux.data[i];
        done+=context.result.aux.length;
    }
    return auxStop(context,address);
}
struct DdcContext {hdmiddc::Engine& engine;HardwareResult& result;};
bool ddcRead(void* opaque,uint8_t block,uint8_t* bytes) {
    auto& context=*static_cast<DdcContext*>(opaque);auto& out=context.result;
    out.ddcStatus=context.engine.readEdidBlock(block,out.ddc);++out.packets;
    if(out.ddcStatus!=hdmiddc::Status::Ok || out.ddc.bytes!=edid::kBlockBytes)return false;
    for(size_t i=0;i<edid::kBlockBytes;++i)bytes[i]=out.ddc.data[i];
    return true;
}
} // namespace
Status capture(const Io& io,uint8_t* buffer,size_t capacity,edid::Monitor& monitor,Result& out) {
    out=Result{};
    if(!io.readBlock || !buffer)return fail(out,Status::InvalidArgument);
    if(capacity<edid::kBlockBytes)return fail(out,Status::NoSpace);
    out.stage=Stage::Read;size_t needed=1;bool final=false;
    do {
        if(needed>capacity/edid::kBlockBytes)return fail(out,Status::NoSpace);
        while(out.blocksRead<needed) {
            if(!read(io,uint8_t(out.blocksRead),buffer+out.blocksRead*edid::kBlockBytes,out))
                return fail(out,Status::TransportFailed);
            ++out.blocksRead;
        }
        out.edidStatus=edid::requiredBlocks(buffer,out.blocksRead*edid::kBlockBytes,needed,final);
        if(out.edidStatus!=edid::Status::Ok)return fail(out,Status::EdidRejected);
        out.expectedBlocks=needed;
    } while(!final || out.blocksRead<needed);
    // A stable base alone cannot detect an extension changing mid-read.
    // Re-read all effective blocks; comparison catches changes even if both
    // versions independently have correct checksums and identical counts.
    out.stage=Stage::Verify;uint8_t verify[edid::kBlockBytes];
    for(size_t b=0;b<needed;++b) {
        if(!read(io,uint8_t(b),verify,out))return fail(out,Status::TransportFailed);
        for(size_t i=0;i<edid::kBlockBytes;++i)
            if(verify[i]!=buffer[b*edid::kBlockBytes+i])return fail(out,Status::ChangedWhileReading);
        ++out.verifiedBlocks;
    }
    out.stage=Stage::Parse;
    out.edidStatus=edid::parse(buffer,needed*edid::kBlockBytes,monitor);
    if(out.edidStatus!=edid::Status::Ok)return fail(out,Status::EdidRejected);
    out.bytes=needed*edid::kBlockBytes;out.complete=true;out.stage=Stage::Complete;
    return fail(out,Status::Ok);
}
Status capture(dpaux::Engine& engine,uint8_t* buffer,size_t capacity,edid::Monitor& monitor,HardwareResult& out) {
    out=HardwareResult{};
    if(!engine.isOpen())return fail(out.capture,Status::InvalidState);
    AuxContext context={engine,out};return capture({&context,auxRead},buffer,capacity,monitor,out.capture);
}
Status capture(hdmiddc::Engine& engine,uint8_t* buffer,size_t capacity,edid::Monitor& monitor,HardwareResult& out) {
    out=HardwareResult{};
    if(!engine.isOpen())return fail(out.capture,Status::InvalidState);
    DdcContext context={engine,out};return capture({&context,ddcRead},buffer,capacity,monitor,out.capture);
}
const char* statusString(Status s) {switch(s) {
#define NAME(x) case Status::x:return #x
    NAME(Ok);NAME(InvalidArgument);NAME(InvalidState);NAME(NoSpace);NAME(TransportFailed);
    NAME(EdidRejected);NAME(ChangedWhileReading);
#undef NAME
    }return "Unknown";
}
} }
