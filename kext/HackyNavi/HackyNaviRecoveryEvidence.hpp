// SPDX-License-Identifier: MIT
#ifndef HACKYNAVI_RECOVERY_EVIDENCE_HPP
#define HACKYNAVI_RECOVERY_EVIDENCE_HPP
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

// CPU-only serialization. These dictionaries never carry a device lease,
// allocation permission, reset proof, or a substitute for a live admission.
namespace hackynavi { namespace recoveryevidence {
inline bool number(OSDictionary* out,const char* key,uint64_t value) {
    auto* item=OSNumber::withNumber(value,64);if(!item)return false;
    const bool ok=out->setObject(key,item);item->release();return ok;
}
inline bool flag(OSDictionary* out,const char* key,bool value) {
    return out->setObject(key,value?kOSBooleanTrue:kOSBooleanFalse);
}
inline bool text(OSDictionary* out,const char* key,const char* value) {
    auto* item=OSString::withCString(value);if(!item)return false;
    const bool ok=out->setObject(key,item);item->release();return ok;
}
inline bool historical(OSDictionary* out) {
    return flag(out,"HistoricalEvidenceOnly",true)&&flag(out,"CurrentHardwareAuthority",false);
}
inline bool words(OSDictionary* out,const char* key,const uint32_t* values,unsigned count) {
    auto* array=OSArray::withCapacity(count);if(!array)return false;
    bool ok=true;
    for(unsigned i=0;i<count&&ok;++i) {
        auto* item=OSNumber::withNumber(values[i],32);
        ok=item&&array->setObject(item);if(item)item->release();
    }
    ok=ok&&out->setObject(key,array);array->release();return ok;
}
} }
#endif
