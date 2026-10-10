// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_NATIVE_READ_CAPTURE_HPP
#define HACKYNAVI_NATIVE_READ_CAPTURE_HPP
#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include "hackynavi/read_capture.hpp"

// CPU-only temporary diagnostic storage. Always allocate the large trace off
// stack; publication makes immutable OSNumber copies and retains no trace,
// callback, mapping, Operation or hardware ownership token.
template<class Trace> class HackyNaviReadCapture {
public:
    HackyNaviReadCapture() : data_(static_cast<Trace*>(IOMalloc(sizeof(Trace)))) {
        if(data_) *data_=Trace{};
    }
    ~HackyNaviReadCapture() { if(data_) IOFree(data_,sizeof(Trace)); }
    Trace* get() { return data_; }
    bool publish(OSDictionary* parent,const char* key) const {
        if(!parent)return false;
        auto* report=OSDictionary::withCapacity(12);
        if(!report)return false;
        auto* status=OSString::withCString(data_?"Recorded":"NoMemory");
        bool ok=status&&report->setObject("Status",status)&&
            report->setObject("HistoricalEvidenceOnly",kOSBooleanTrue)&&
            report->setObject("CurrentHardwareAuthority",kOSBooleanFalse);
        if(status)status->release();
        if(data_) {
            auto* offsets=OSArray::withCapacity(data_->recorded);
            auto* values=OSArray::withCapacity(data_->recorded);
            auto* callback=OSArray::withCapacity(Trace::maskWords);
            auto* nonAllOnes=OSArray::withCapacity(Trace::maskWords);
            ok=ok&&offsets&&values&&callback&&nonAllOnes;
            for(uint32_t i=0;ok&&i<data_->recorded;++i)
                ok=append(offsets,data_->entries[i].byteOffset)&&append(values,data_->entries[i].returnedValue);
            for(size_t i=0;ok&&i<(data_->recorded+31)/32;++i)
                ok=append(callback,data_->callbackSucceeded[i])&&append(nonAllOnes,data_->nonAllOnes[i]);
            ok=ok&&number(report,"FormatVersion",1)&&number(report,"Capacity",Trace::capacity)&&
                number(report,"ReadsAttempted",data_->attempted)&&number(report,"ReadsRecorded",data_->recorded)&&
                report->setObject("Truncated",data_->truncated?kOSBooleanTrue:kOSBooleanFalse)&&
                report->setObject("RegisterOffsets",offsets)&&report->setObject("ReturnedValues",values)&&
                report->setObject("CallbackSucceededMask",callback)&&report->setObject("NonAllOnesMask",nonAllOnes);
            if(offsets)offsets->release();if(values)values->release();
            if(callback)callback->release();if(nonAllOnes)nonAllOnes->release();
        }
        // Incomplete child allocation must never masquerade as a full trace.
        if(ok)ok=parent->setObject(key,report);
        report->release();return ok;
    }
    HackyNaviReadCapture(const HackyNaviReadCapture&)=delete;
    HackyNaviReadCapture& operator=(const HackyNaviReadCapture&)=delete;
private:
    static bool append(OSArray* out,uint32_t value) {
        auto* n=OSNumber::withNumber(value,32);if(!n)return false;
        const bool ok=out->setObject(n);n->release();return ok;
    }
    static bool number(OSDictionary* out,const char* key,uint32_t value) {
        auto* n=OSNumber::withNumber(value,32);if(!n)return false;
        const bool ok=out->setObject(key,n);n->release();return ok;
    }
    Trace* data_;
};
#endif
