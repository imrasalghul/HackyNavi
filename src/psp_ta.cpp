// SPDX-License-Identifier: MIT
// Original bounded admission for Linux3857c2fe TA v2.0 metadata. See
// amdgpu_ucode.h enum ta_fw_type/ta_firmware_header_v2_0 and
// amdgpu_psp.c psp_init_ta_microcode, psp_ta_init_shared_buf.
#include "hackynavi/psp_ta.hpp"
namespace hackynavi { namespace pspta {
Status parse(const uint8_t* data,size_t bytes,Image& out) {
    const uintptr_t a=reinterpret_cast<uintptr_t>(data),b=reinterpret_cast<uintptr_t>(&out);
    if(data && bytes && bytes-1<=UINTPTR_MAX-a &&
       (a<=b ? b-a<bytes : a-b<sizeof(out))) return Status::InvalidArgument;
    out=Image{};
    if(!data || !bytes || bytes-1>UINTPTR_MAX-a) return Status::InvalidArgument;
    PspFirmwareV2 parsed={};
    if(parsePspFirmwareV2(data,bytes,parsed)!=hackynavi::Status::Ok)
        return Status::InvalidContainer;
    const auto& h=parsed.header;
    if(h.ipMajor!=13 || h.ipMinor || h.headerSizeBytes!=36 || h.payloadOffset!=256 ||
       parsed.descriptorCount>8 || uint64_t(h.payloadOffset)+h.payloadSize!=bytes)
        return Status::UnsupportedProfile;
    uint32_t seen=0; uint64_t total=0;
    Application hdcp={},dtm={};
    for(uint32_t i=0;i<parsed.descriptorCount;++i) {
        const auto& e=parsed.descriptors[i];
        if(!e.type || e.type>8 || !e.version || !e.image.size ||
           (e.image.offset&3) || (e.image.size&3)) return Status::InvalidEntry;
        const uint32_t bit=1u<<e.type;
        if(seen&bit) return Status::DuplicateEntry;
        seen|=bit; total+=e.image.size;
        for(uint32_t j=0;j<i;++j) {
            const auto& p=parsed.descriptors[j].image;
            if(e.image.offset<=p.offset ? p.offset-e.image.offset<e.image.size :
                                         e.image.offset-p.offset<p.size) return Status::Overlap;
        }
        if(e.type==4) hdcp={e.version,e.image};
        if(e.type==5) dtm={e.version,e.image};
    }
    if(!hdcp.image.size || !dtm.image.size) return Status::MissingApplication;
    if(total!=h.payloadSize) return Status::InvalidEntry;
    out={h,hdcp,dtm}; return Status::Ok;
}
const char* statusString(Status s) {
    switch(s) {
#define N(x) case Status::x:return #x
    N(Ok);N(InvalidArgument);N(InvalidContainer);N(UnsupportedProfile);N(InvalidEntry);
    N(DuplicateEntry);N(Overlap);N(MissingApplication);
#undef N
    }
    return "Unknown";
}
} }
