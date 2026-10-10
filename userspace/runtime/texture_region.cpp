// SPDX-License-Identifier: MIT
#include "texture_region.hpp"
#include <cstring>
namespace hackynavi { namespace texture { namespace {
bool span(const void* p, size_t bytes) {
    return p && bytes && bytes - 1 <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(p);
}
bool overlap(const void* a, size_t an, const void* b, size_t bn) {
    const uintptr_t av = reinterpret_cast<uintptr_t>(a), bv = reinterpret_cast<uintptr_t>(b);
    return av <= bv ? bv-av < an : av-bv < bn;
}
bool valid(const Transfer& t) {
    return t.rowBytes() && t.rows() && t.rowBytes() <= t.texturePitch() &&
        t.rowBytes() <= t.hostPitch() && t.rows() <= SIZE_MAX/t.rowBytes() &&
        t.packedBytes() == t.rows()*t.rowBytes() &&
        t.rows()-1 <= (SIZE_MAX-t.rowBytes())/t.hostPitch() &&
        t.hostSpan() == (t.rows()-1)*t.hostPitch()+t.rowBytes() &&
        t.textureOffset() < t.textureBytes() &&
        t.rowBytes() <= t.textureBytes()-t.textureOffset() &&
        t.rows()-1 <= (t.textureBytes()-t.textureOffset()-t.rowBytes())/t.texturePitch();
}
bool copy(const Transfer& t, const void* source, size_t sourceSize, size_t sourceOffset,
          size_t sourcePitch, void* destination, size_t destinationSize,
          size_t destinationOffset, size_t destinationPitch) {
    if (!valid(t) || !span(source,sourceSize) || !span(destination,destinationSize) ||
        overlap(source,sourceSize,destination,destinationSize) ||
        overlap(destination,destinationSize,&t,sizeof(t)) ||
        sourceOffset > sourceSize || t.rowBytes() > sourceSize-sourceOffset ||
        destinationOffset > destinationSize || t.rowBytes() > destinationSize-destinationOffset ||
        !sourcePitch || !destinationPitch ||
        t.rows()-1 > (sourceSize-sourceOffset-t.rowBytes())/sourcePitch ||
        t.rows()-1 > (destinationSize-destinationOffset-t.rowBytes())/destinationPitch) return false;
    const auto* src = static_cast<const uint8_t*>(source)+sourceOffset;
    auto* dst = static_cast<uint8_t*>(destination)+destinationOffset;
    for (size_t row=0;row<t.rows();++row)
        std::memcpy(dst+row*destinationPitch,src+row*sourcePitch,t.rowBytes());
    return true;
}
} // namespace
bool plan(const HNColorLayout& layout, Region r, uint64_t pitch, Transfer& out) {
    // Check only the linear CPU geometry here. Exact CB fields and topology
    // are validated by hn_color_target_validate at texture construction.
    if (!pitch && r.height==1 && r.width<=16384) pitch=r.width*4;
    if (!layout.width || layout.width>16384 || !layout.height || layout.height>16384 ||
        layout.pitch_pixels != ((layout.width+63u)&~63u) ||
        layout.pitch_bytes != uint64_t(layout.pitch_pixels)*4 ||
        layout.total_bytes != uint64_t(layout.pitch_bytes)*layout.height ||
        layout.slice_bytes != layout.total_bytes || layout.total_bytes>SIZE_MAX ||
        r.z || r.depth!=1 || !r.width || !r.height || r.x>=layout.width || r.y>=layout.height ||
        r.width>layout.width-r.x || r.height>layout.height-r.y ||
        pitch<r.width*4 || pitch>SIZE_MAX || (pitch&3)) return false;
    const uint64_t row=r.width*4, offset=r.y*layout.pitch_bytes+r.x*4;
    if (r.height-1>(SIZE_MAX-row)/pitch || r.height>SIZE_MAX/row) return false;
    Transfer candidate;
    candidate.offset_=offset;candidate.row_=size_t(row);candidate.rows_=size_t(r.height);
    candidate.texturePitch_=layout.pitch_bytes;candidate.hostPitch_=size_t(pitch);
    candidate.hostSpan_=size_t((r.height-1)*pitch+row);candidate.packed_=size_t(r.height*row);
    candidate.textureBytes_=size_t(layout.total_bytes);
    if (!valid(candidate)) return false;
    out=candidate;return true;
}
bool planRead(const HNColorLayout& layout,Region r,uint64_t pitch,uint64_t image,uint64_t level,uint64_t slice,Transfer& out) {
    return !image&&!level&&!slice&&pitch&&pitch<uint64_t(INT32_MAX)*4&&plan(layout,r,pitch,out);
}
bool planWrite(const HNColorLayout& layout,Region r,uint64_t pitch,uint64_t image,uint64_t level,uint64_t slice,Transfer& out) {
    return !image&&!level&&!slice&&pitch<32767u*4u&&plan(layout,r,pitch,out);
}
bool pack(const Transfer& t,const void* host,size_t hostBytes,void* packed,size_t packedBytes) {
    if (hostBytes<t.hostSpan() || packedBytes<t.packedBytes()) return false;
    return copy(t,host,t.hostSpan(),0,t.hostPitch(),packed,t.packedBytes(),0,t.rowBytes());
}
bool commit(const Transfer& t,const void* packed,size_t packedBytes,void* texture,size_t textureBytes) {
    if (packedBytes<t.packedBytes() || textureBytes<t.textureBytes()) return false;
    return copy(t,packed,t.packedBytes(),0,t.rowBytes(),texture,t.textureBytes(),size_t(t.textureOffset()),t.texturePitch());
}
bool read(const Transfer& t,const void* texture,size_t textureBytes,void* host,size_t hostBytes) {
    if (textureBytes<t.textureBytes() || hostBytes<t.hostSpan()) return false;
    return copy(t,texture,t.textureBytes(),size_t(t.textureOffset()),t.texturePitch(),host,t.hostSpan(),0,t.hostPitch());
}
} }
