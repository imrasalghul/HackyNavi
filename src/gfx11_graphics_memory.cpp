// SPDX-License-Identifier: MIT
#include "hackynavi/gfx11_graphics_memory.hpp"
namespace hackynavi { namespace graphicsmemory { namespace {
bool fit(uint64_t lower,uint64_t upper,uint64_t attributes,uint64_t ib,Plan& plan) {
    const uint64_t size=attributes+kCodeBytes+ib;
    if(upper<lower || size>upper-lower) return false;
    uint64_t start=(upper-size)&~(kAttributeAlignment-1);
    if(start<lower) return false;
    if(start/kWindowBytes!=(start+size-1)/kWindowBytes) {
        // The top partial window cannot contain this aligned group. Since the
        // complete group is smaller than one window, one boundary descent is
        // sufficient; never scan 32768 windows or probe unmapped addresses.
        upper=(upper-1)&~(kWindowBytes-1);
        if(upper<lower || size>upper-lower) return false;
        start=(upper-size)&~(kAttributeAlignment-1);
        if(start<lower) return false;
    }
    plan={start+attributes,start+attributes+kCodeBytes,start,ib,attributes};return true;
}
}
Status place(const Mapping* mappings,size_t count,uint64_t attributeBytes,uint64_t indirectBytes,Plan& out) {
    if(count>kMaxMappings || (count && (!mappings || reinterpret_cast<uintptr_t>(mappings)%alignof(Mapping) ||
        count*sizeof(Mapping)-1>UINTPTR_MAX-reinterpret_cast<uintptr_t>(mappings))) ||
        attributeBytes<kAttributeAlignment || attributeBytes>16u*1024u*1024u ||
        (attributeBytes&(kAttributeAlignment-1)) || (indirectBytes!=4096 && indirectBytes!=8192))
        return Status::InvalidArgument;
    uint8_t order[kMaxMappings]={};
    for(size_t n=0;n<count;++n) {
        const auto& m=mappings[n];
        if(!m.bytes || ((m.va|m.bytes)&4095) || m.va<kVaBegin || m.va>=kVaEnd || m.bytes>kVaEnd-m.va)
            return Status::InvalidMapping;
        size_t insert=n;
        while(insert && mappings[order[insert-1]].va>m.va){order[insert]=order[insert-1];--insert;}
        order[insert]=static_cast<uint8_t>(n);
    }
    for(size_t n=1;n<count;++n) {
        const auto& previous=mappings[order[n-1]];
        if(previous.va+previous.bytes>mappings[order[n]].va) return Status::InvalidMapping;
    }
    Plan candidate{};uint64_t upper=kVaEnd;
    for(size_t n=count;n;--n) {
        const auto& m=mappings[order[n-1]];
        if(fit(m.va+m.bytes,upper,attributeBytes,indirectBytes,candidate)){out=candidate;return Status::Ok;}
        upper=m.va;
    }
    if(!fit(kVaBegin,upper,attributeBytes,indirectBytes,candidate)) return Status::NoSpace;
    out=candidate;return Status::Ok;
}
} }
