// SPDX-License-Identifier: MIT
// Tests validate actual ISA admission, not GPU execution or hardware isolation.
#include "hackynavi/gfx11_compute_product.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace hackynavi::gfx11product;
namespace {
size_t checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); } } while(false)
struct Fixture { const char* name; const char* hex; size_t executable; unsigned local; };
// Captured through the real owned pipeline C ABI, independently identical to
// standalone ACO. Source products.json records provenance and exact hashes.
const Fixture fixtures[] = {
    // SHA256 ac333acf99387a4063fae75a89dd4e6968907f6b78702d26442dd8a3575f7dda
    {"add_one-64",
     "900085be020085bf000208f4000000f8000308f40000000a810100f40c0000f8000046d6040c0104"
     "07fc89bf010087bf000047d606000a02000050e000014280f70389bf8102024a000068e000014380"
     "0000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf"
     ,84,64},
    // SHA256 98b773c473a79de6d64f7ed6f9e559fd88d5a87058e67dfab484bd1264a5c93f
    {"add_one-3",
     "900085be020085bf000208f4000000f8000308f40000000a810100f40c0000f80483049607fc89bf"
     "990087bf000055d60608000482000030000050e000014280f70389bf8102024a000068e000014380"
     "0000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf"
     ,84,3},
    // SHA256 66bc66e2cb814db18d426cc38eb52bc4c843b3a1619952de20945d9a018ce0f4
    {"affine_xor-64",
     "900085be020085bf000208f4000000f8000308f40000000a810100f40c0000f8000046d6040c0104"
     "07fc89bf910087bf0600004a82000230000050e001024280f70389bf02002cd787040202010087bf"
     "020045d6ff0402045a5a5a5a000068e0010243800000b0bf00009fbf00009fbf00009fbf00009fbf"
     "00009fbf"
     ,104,64},
    // SHA256 0a9ad10b394dce951c53a04726ff40a2f24dee3574758a905382ed7f79b0a723
    {"affine_xor-3",
     "900085be020085bf000208f4000000f8000308f40000000a810100f40c0000f80483049607fc89bf"
     "990087bf000055d60608000482000230000050e001024280f70389bf02002cd787040202010087bf"
     "020045d6ff0402045a5a5a5a000068e0010243800000b0bf00009fbf00009fbf00009fbf00009fbf"
     "00009fbf"
     ,104,3},
    // Actual 1x1x1 products: build/compute-prototype-isa, byte-identical to
    // standalone ACO. These are uniform MUBUF accesses, not scalar SMEM.
    // SHA256 ce6d630314a3aeeb90343813ad70146644851a7ca13532dd5fe0247e0d46cb69
    {"add_one-1",
     "900085be020085bf000208f4000000f8000308f40000000a810100f40c0000f807fc89bf04060480"
     "d90487bf04820484000050e080000204f70389bf00050e7e078107800702027e000068e080010304"
     "0000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf",84,1},
    // SHA256 6c1aec25b2e75152f0bacf0171fee370420c432e19b5ce344d4d8a9064b913b5
    {"affine_xor-1",
     "900085be020085bf000208f4000000f8000308f40000000a810100f40c0000f807fc89bf04060480"
     "d90487bf04820784000050e080000207f70389bf0005107e0887089608ff088d5a5a5a5a990487bf"
     "080408800802027e000068e0800103070000b0bf00009fbf00009fbf00009fbf00009fbf00009fbf",100,1},
};
using Bytes = std::vector<uint8_t>;
void word(Bytes& b,uint32_t w) { for(unsigned i=0;i<4;++i)b.push_back(uint8_t(w>>(i*8))); }
uint32_t get(const Bytes& b,size_t o) { return uint32_t(b[o])|(uint32_t(b[o+1])<<8)|(uint32_t(b[o+2])<<16)|(uint32_t(b[o+3])<<24); }
void put(Bytes& b,size_t o,uint32_t w) { for(unsigned i=0;i<4;++i)b[o+i]=uint8_t(w>>(i*8)); }
unsigned nibble(char c) { return c<='9'?unsigned(c-'0'):unsigned(c-'a'+10); }
Bytes bytes(const Fixture& f) {
    Bytes b;for(size_t i=0;f.hex[i];i+=2)b.push_back(uint8_t(nibble(f.hex[i])*16+nibble(f.hex[i+1])));return b;
}
Request request(const Bytes& b,size_t executable=0,unsigned local=64) {
    Request r={b.data(),b.size(),executable?executable:b.size(),{local,1,1},32,108,24};return r;
}
void cleared(const Report& r) {
    CHECK(!r.instructionCount&&!r.requiredSgprs&&!r.requiredVgprs&&!r.readMask&&!r.writeMask);
    CHECK(!r.rsrc1&&!r.rsrc2&&!r.rsrc3);
}
Report check(const Request& r,Status expected) {
    Report out;std::memset(&out,0xa5,sizeof(out));
    const Status status=validate(r,out);
    if(status!=expected) std::fprintf(stderr,"expected %s, got %s at %u word %08x\n",statusString(expected),statusString(status),out.byteOffset,out.instructionWord);
    CHECK(status==expected);CHECK(out.status==status);
    if(status!=Status::Ok)cleared(out);
    return out;
}
Report check(const Bytes& b,Status expected=Status::Ok) {return check(request(b),expected);}
void end(Bytes& b) {word(b,0xbfb00000);}
void mov(Bytes& b,unsigned d,unsigned s) {word(b,0xbe800000|(d<<16)|s);}
void sop2(Bytes& b,unsigned op,unsigned d,unsigned a,unsigned c) {word(b,0x80000000|(op<<23)|(d<<16)|(c<<8)|a);}
void vop1(Bytes& b,unsigned op,unsigned d,unsigned s) {word(b,0x7e000000|(d<<17)|(op<<9)|s);}
void scalar(Bytes& b,unsigned dest,unsigned base,unsigned offset=0,unsigned offsetReg=124,bool four=true) {
    word(b,(four?0xf4080000:0xf4000000)|(dest<<6)|(base/2));word(b,(offsetReg<<25)|offset);
}
void lgkm(Bytes& b) {word(b,0xbf89fc07);}
void vm(Bytes& b) {word(b,0xbf8903f7);}
void shift(Bytes& b,unsigned dest=1,unsigned source=0,unsigned amount=2) {word(b,0x30000000|(dest<<17)|(source<<9)|(128+amount));}
void memory(Bytes& b,bool load,unsigned descriptor=8,unsigned data=2,unsigned address=1,unsigned offset=128) {
    word(b,load?0xe0500000:0xe0680000);word(b,(offset<<24)|0x00400000|((descriptor/4)<<16)|(data<<8)|address);
}
void uniformMemory(Bytes& b,bool load,unsigned descriptor=8,unsigned data=2,unsigned offset=4) {
    word(b,load?0xe0500000:0xe0680000);word(b,(offset<<24)|((descriptor/4)<<16)|(data<<8)|128);
}
void vop3(Bytes& b,unsigned op,unsigned dest,unsigned a,unsigned c,unsigned d) {
    word(b,0xd4000000|(op<<16)|dest);word(b,a|(c<<9)|(d<<18));
}
Bytes ready(unsigned binding=0) {
    Bytes b;scalar(b,8,0,binding*16);lgkm(b);shift(b);return b;
}
void genuineProducts() {
    for(const auto& f:fixtures) {
        Bytes b=bytes(f);const auto r=request(b,f.executable,f.local);
        Report out=check(r,Status::Ok);
        CHECK(out.readMask==1&&out.writeMask==2);
        CHECK(out.requiredSgprs==16&&out.requiredVgprs==((f.executable==84||f.local==1)?2u:3u));
        CHECK(out.instructionCount==(f.local==1?(f.executable==100?19u:16u):f.executable==104?17u:f.local==64?14u:15u));
        CHECK(out.rsrc1==0x202c0002&&out.rsrc2==0x788&&!out.rsrc3);
        // Allocate exactly the actual operands, independently of ACO's larger
        // scheduling allocation. RSRC1 is regenerated, never trusted metadata.
        auto exact=r;exact.numSgprs=out.requiredSgprs;exact.numVgprs=out.requiredVgprs;
        CHECK(check(exact,Status::Ok).rsrc1==0x202c0000);
        exact.numSgprs--;check(exact,Status::RegisterLimit);
        exact=r;exact.numVgprs=out.requiredVgprs-1;check(exact,Status::RegisterLimit);
        // Changing real ALU literals is admitted from instructions, not hashes.
        if(f.executable==104) {
            for(uint32_t literal:{0u,0xffffffffu,0xbfb00000u,0xbf89fc07u,0x12345678u}) {
                Bytes altered=b;put(altered,88,literal);
                check(request(altered,f.executable,f.local),Status::Ok);
            }
        } else if(f.local!=1) {
            Bytes altered=b;put(altered,68,(get(altered,68)&~511u)|130);
            check(request(altered,f.executable,f.local),Status::Ok);
        }
    }
}
void packedLocalProduct() {
    // Actual affine_xor [64,2,2] product, independently standalone-identical.
    // SHA256 8fd078c856b560864c6e0178a941c86457ed0378bcd97e39594757160b9e7851
    const Fixture f={"affine_xor-64x2x2",
        "900085be020085bf000208f4000000f8000308f40000000a810100f40c0000f8bf000036a10087bf"
        "000046d6040c010407fc89bf0600004ac10087bf82000230000050e001024280f70389bf02002cd7"
        "87040202020045d6ff0402045a5a5a5a000068e0010243800000b0bf00009fbf00009fbf00009fbf"
        "00009fbf00009fbf",108,64};
    const Bytes b=bytes(f);auto r=request(b,f.executable,64);r.local[1]=2;r.local[2]=2;
    const Report out=check(r,Status::Ok);
    CHECK(out.readMask==1&&out.writeMask==2&&out.requiredSgprs==16&&out.requiredVgprs==3);
    CHECK(out.instructionCount==18&&out.rsrc1==0x202c0002&&out.rsrc2==0x1788&&!out.rsrc3);
}
void boundsAndLaunch() {
    Bytes b;end(b);auto r=request(b);
    check(r,Status::Ok);
    auto x=r;x.code=nullptr;check(x,Status::InvalidArgument);
    x=r;x.codeBytes=0;check(x,Status::InvalidArgument);
    for(size_t n:{size_t(1),size_t(3),kMaxCodeBytes+4}) {x=r;x.codeBytes=n;check(x,Status::InvalidArgument);}
    for(size_t n:{size_t(0),size_t(1),size_t(8),size_t(-1)}) {x=r;x.executableBytes=n;check(x,Status::InvalidArgument);}
    x=r;x.code=reinterpret_cast<const uint8_t*>(UINTPTR_MAX-1);check(x,Status::InvalidArgument);
    for(unsigned n=0;n<3;++n)for(uint32_t v:{0u,1025u,0xffffffffu}) {x=r;x.local[n]=v;check(x,Status::InvalidLaunch);}
    for(uint32_t v:{0u,16u,64u,0xffffffffu}) {x=r;x.waveSize=v;check(x,Status::InvalidLaunch);}
    for(uint32_t v:{0u,7u,129u,0xffffffffu}) {x=r;x.numSgprs=v;check(x,Status::InvalidLaunch);}
    for(uint32_t v:{0u,257u,0xffffffffu}) {x=r;x.numVgprs=v;check(x,Status::InvalidLaunch);}
    x=r;x.local[0]=1024;x.local[1]=2;check(x,Status::InvalidLaunch);
    x=r;x.local[0]=1024;check(x,Status::Ok);
    x=r;x.local[0]=1;x.local[1]=32;x.local[2]=32;CHECK(check(x,Status::Ok).rsrc2==0x1788);
    x=r;x.local[0]=1;x.local[1]=32;CHECK(check(x,Status::Ok).rsrc2==0xf88);
    // The pure decoder performs byte reads and accepts unaligned immutable input.
    Bytes unaligned(1,0xff);unaligned.insert(unaligned.end(),b.begin(),b.end());
    x=r;x.code=unaligned.data()+1;check(x,Status::Ok);
    Bytes large;for(unsigned n=0;n<4095;++n)mov(large,8,128);end(large);
    CHECK(large.size()==kMaxCodeBytes);check(large,Status::Ok);
    word(large,0xbf9f0000);check(large,Status::InvalidArgument);
}
void registerResidencyAdmission() {
    // Use the actual captured ACO instruction bytes, with independently
    // declared launch-register reservations. These are admission-envelope
    // tests, not claims that these fixtures were recompiled for every shape
    // or executed on hardware. The explicit capacity tiers come from the
    // pinned Navi31 physical register geometry, not the helper under test.
    struct Boundary { uint32_t vgprs, threads; };
    const Boundary boundaries[] = {
        {24,1024},{192,1024},{193,896},{216,896},{217,768},{240,768},
        {241,640},{248,640},{249,640},{256,640},
    };
    for (const auto& fixture : fixtures) {
        const Bytes code=bytes(fixture);
        for (const auto& boundary : boundaries) {
            for (unsigned axis=0;axis<3;++axis) {
                auto r=request(code,fixture.executable,1);
                r.numVgprs=boundary.vgprs;
                // The last resident thread and the first nonresident thread
                // must give the same answer regardless of dimension axis.
                r.local[axis]=boundary.threads;
                const Report accepted=check(r,Status::Ok);
                CHECK(accepted.readMask==1&&accepted.writeMask==2);
                CHECK(accepted.rsrc1==(0x202c0000|((boundary.vgprs-1)/8)));
                r.local[axis]=boundary.threads+1;
                const Report rejected=check(r,Status::InvalidLaunch);
                CHECK(!rejected.byteOffset&&!rejected.instructionWord);
                r.local[axis]=boundary.threads-1;
                check(r,Status::Ok);
            }
            // All tier boundaries are divisible by 32: these exercise the
            // checked product, not just a bound on a single local dimension.
            auto r=request(code,fixture.executable,32);
            r.numVgprs=boundary.vgprs;r.local[1]=boundary.threads/32;
            check(r,Status::Ok);
            ++r.local[1];check(r,Status::InvalidLaunch);
            r.local[0]=8;r.local[1]=4;r.local[2]=boundary.threads/32;
            check(r,Status::Ok);
            ++r.local[2];check(r,Status::InvalidLaunch);
        }
        // Same 1024-thread declaration crosses the first real occupancy tier
        // without changing ISA bytes, SGPRs, EXEC mode or architectural bound.
        auto r=request(code,fixture.executable,1024);
        r.numVgprs=192;check(r,Status::Ok);
        r.numVgprs=193;check(r,Status::InvalidLaunch);
        // Restoring the resident shape permits a fresh report; a previous
        // rejected envelope must not leak RSRCs or poison later validation.
        r.local[0]=640;r.numVgprs=256;check(r,Status::Ok);
    }
}
void structural() {
    Bytes b;mov(b,8,255);check(b,Status::Truncated);
    word(b,0xbfb00000);check(b,Status::MissingEnd); // literal is not an end instruction
    end(b);check(b,Status::Ok);
    b.clear();word(b,0xf4080200);check(b,Status::Truncated);
    b.clear();word(b,0xd6450000);word(b,0x020204ff);check(b,Status::Truncated);
    for(uint32_t bad:{0xbfb00001u,0xbfb0ffffu}) {b.clear();word(b,bad);check(b,Status::ReservedBits);}
    b.clear();end(b);mov(b,8,128);check(b,Status::MissingEnd);
    b.clear();mov(b,8,128);check(b,Status::MissingEnd);
    b.clear();end(b);word(b,0xbf9f0000);check(request(b,4),Status::Ok);
    for(unsigned bit=0;bit<32;++bit) {
        Bytes c=b;put(c,4,get(c,4)^(1u<<bit));check(request(c,4),Status::InvalidPadding);
    }
    b.clear();word(b,0xbf9f0000);end(b);check(b,Status::UnsupportedInstruction);
    for(uint32_t forbidden:{0xbe804800u,0xbe804900u,0xbe804a00u,0xbe804c00u,0xbe804d00u,
        0xb8880000u,0xb9080000u,0xb9880000u,0xbf900000u,0xbfb60000u,0xbfb70000u,0xbfb10000u,
        0xbfa00000u,0xbfa10000u,0xbf820000u,0xbf830000u,0xbfbd0000u,0xf0000000u,
        0xdc000000u,0xd8000000u,0xf8000000u,0xcc000000u,0x7e000000u,0x7c000000u,0xffffffffu}) {
        b.clear();word(b,forbidden);word(b,0);end(b);check(b,Status::UnsupportedInstruction);
    }
    for(unsigned op=1;op<256;++op) {
        b.clear();word(b,0xbe880080|(op<<8));end(b);check(b,Status::UnsupportedInstruction);
    }
    for(unsigned op=0;op<128;++op)if(op!=0&&op!=8&&op!=26&&op!=44) {
        b.clear();word(b,0x80088080|(op<<23));end(b);Report report{};CHECK(validate(request(b),report)!=Status::Ok);cleared(report);
    }
    for(unsigned op=0;op<1024;++op)if(op!=581&&op!=582&&op!=583&&op!=597&&op!=812) {
        b.clear();vop3(b,op,0,128,128,128);end(b);check(b,Status::UnsupportedInstruction);
    }
    for(unsigned bit=8;bit<16;++bit) {
        b.clear();vop3(b,597,0,128,128,128);put(b,0,get(b,0)|(1u<<bit));end(b);check(b,Status::ReservedBits);
    }
    for(unsigned bit=27;bit<32;++bit) {
        b.clear();vop3(b,597,0,128,128,128);put(b,4,get(b,4)|(1u<<bit));end(b);check(b,Status::ReservedBits);
    }
    b.clear();vop3(b,812,0,128,128,129);end(b);check(b,Status::ReservedBits);
    b.clear();vop3(b,597,0,4,5,6);end(b);check(b,Status::InvalidOperand);
    b.clear();vop3(b,597,0,4,4,5);end(b);check(b,Status::Ok);
    b.clear();vop3(b,597,0,4,5,255);word(b,7);end(b);check(b,Status::InvalidOperand);
    b.clear();vop3(b,597,0,255,255,255);word(b,7);end(b);check(b,Status::Ok);
}
void initializedRegisters() {
    for(unsigned s=0;s<256;++s) {
        Bytes b;mov(b,8,s);if(s==255)word(b,0xdecafbad);end(b);
        Status status=Status::InvalidOperand;
        if(s<8||(s>=128&&s<=208)||s==255)status=Status::Ok;
        else if(s<106)status=Status::UninitializedRegister;
        check(b,status);
    }
    for(unsigned d=0;d<128;++d) {
        Bytes b;mov(b,d,128);end(b);check(b,d<106?Status::Ok:Status::InvalidOperand);
    }
    for(unsigned s=0;s<512;++s) {
        Bytes b;vop3(b,597,0,s,128,128);if(s==255)word(b,1);end(b);
        Status status=Status::InvalidOperand;
        if(s<8||(s>=128&&s<=208)||s==255||s==256)status=Status::Ok;
        else if(s<106||(s>256&&s<280))status=Status::UninitializedRegister;
        else if(s>=280)status=Status::RegisterLimit;
        check(b,status);
    }
    // Scalar ADD/LSHL/XOR update SCC but no admitted operation reads it;
    // special selectors and changes to VCC/EXEC remain excluded.
    for(unsigned s:{106u,107u,124u,125u,126u,127u,251u,252u,253u,254u,249u,250u}) {
        Bytes b;word(b,0x4a000000|s);end(b);check(b,Status::InvalidOperand);
    }
}
void uniformAccessAndTransfers() {
    // Every source selector is checked for the new VOP1 transfer; there is no
    // implicit access to inactive, unknown, special or out-of-allocation data.
    for(unsigned source=0;source<512;++source) {
        Bytes b;vop1(b,1,1,source);if(source==255)word(b,0x12345678);end(b);
        Status expected=Status::InvalidOperand;
        if(source<8||(source>=128&&source<=208)||source==255||source==256)expected=Status::Ok;
        else if(source<106||(source>256&&source<280))expected=Status::UninitializedRegister;
        else if(source>=280)expected=Status::RegisterLimit;
        check(b,expected);
        b.clear();vop1(b,2,8,source);end(b);
        expected=source<256?Status::InvalidOperand:source==256?Status::Ok:
            source<280?Status::UninitializedRegister:Status::RegisterLimit;
        check(b,expected);
    }
    for(unsigned destination=0;destination<256;++destination) {
        Bytes b;vop1(b,2,destination,256);end(b);
        check(b,destination<106?Status::Ok:Status::InvalidOperand);
    }
    for(unsigned op=0;op<256;++op)if(op!=1&&op!=2) {
        Bytes b;vop1(b,op,0,256);end(b);check(b,Status::UnsupportedInstruction);
    }
    for(unsigned binding=0;binding<32;++binding) {
        Bytes b=ready(binding);sop2(b,8,4,4,130);uniformMemory(b,true);vm(b);
        vop1(b,2,16,258);sop2(b,0,16,16,129);vop1(b,1,2,16);
        uniformMemory(b,false);end(b);
        const Report r=check(b);CHECK(r.readMask==(1u<<binding)&&r.writeMask==(1u<<binding));
    }
    // Scalar alignment propagates through arithmetic, moves and both transfer
    // directions. Modulo-32 shifts and integer overflow never overstate it.
    for(unsigned shiftAmount=0;shiftAmount<32;++shiftAmount) {
        Bytes b=ready();sop2(b,8,4,4,128+shiftAmount);uniformMemory(b,false,8,0);end(b);
        check(b,shiftAmount>=2?Status::Ok:Status::InvalidProvenance);
    }
    for(unsigned op:{0u,8u,26u,44u})for(unsigned source:{106u,124u,126u,251u,254u}) {
        Bytes b;sop2(b,op,4,source,130);end(b);check(b,Status::InvalidOperand);
    }
    Bytes b=ready();sop2(b,8,4,4,130);mov(b,5,4);vop1(b,1,1,5);vop1(b,2,6,257);
    uniformMemory(b,false,8,0,6);end(b);check(b);
    b=ready();sop2(b,8,4,4,130);sop2(b,0,4,4,132);uniformMemory(b,false,8,0);end(b);check(b);
    b=ready();sop2(b,8,4,4,130);sop2(b,26,4,4,132);uniformMemory(b,false,8,0);end(b);check(b);
    b=ready();sop2(b,44,4,4,132);uniformMemory(b,false,8,0);end(b);check(b);
    for(unsigned op:{0u,26u}) {
        b=ready();sop2(b,8,4,4,130);sop2(b,op,4,4,129);uniformMemory(b,false,8,0);end(b);check(b,Status::InvalidProvenance);
    }
    b=ready();sop2(b,8,4,4,130);sop2(b,8,4,4,5);uniformMemory(b,false,8,0);end(b);check(b);
    b=ready();sop2(b,8,4,4,5);uniformMemory(b,false,8,0);end(b);check(b,Status::InvalidProvenance);
    b=ready();mov(b,4,255);word(b,0xfffffffcu);sop2(b,0,4,4,132);uniformMemory(b,false,8,0);end(b);check(b);
    b=ready();sop2(b,8,4,4,255);word(b,32);uniformMemory(b,false,8,0);end(b);check(b,Status::InvalidProvenance);
    // In OFFEN=0 the canonical unused VADDR must not be read as VGPR128.
    // Every other field, pending counter and tuple rule still applies.
    b=ready();uniformMemory(b,false,8,0,128);end(b);check(b);
    for(unsigned offset:{4u,9u,10u,11u,129u,193u}) {
        b=ready();uniformMemory(b,false,8,0,offset);end(b);check(b,Status::InvalidProvenance);
    }
    for(unsigned offset:{106u,124u,125u,126u,127u,255u}) {
        b=ready();uniformMemory(b,false,8,0,offset);end(b);check(b,Status::InvalidOperand);
    }
    b=ready();uniformMemory(b,false,8,0,16);end(b);check(b,Status::UninitializedRegister);
    b=ready();memory(b,true);vop1(b,2,16,258);vm(b);end(b);check(b,Status::PendingLoad);
    b.clear();scalar(b,8,0);vop1(b,2,8,256);lgkm(b);end(b);check(b,Status::PendingLoad);
    b=ready();vop1(b,2,8,256);uniformMemory(b,false,8,0,128);end(b);check(b,Status::InvalidProvenance);
    b=ready();vop1(b,1,1,8);vop1(b,2,16,257);uniformMemory(b,false,16,0,128);end(b);check(b,Status::InvalidProvenance);
    // Breaking a tuple never treats its component number as data alignment.
    b=ready();mov(b,4,11);sop2(b,0,4,4,128);uniformMemory(b,false,8,0);end(b);check(b,Status::InvalidProvenance);
    b=ready();word(b,0xbf850000);vop1(b,2,16,256);end(b);check(b,Status::InvalidClause);
    // VOP2 AND is needed to unpack the local-X bits of multidimensional
    // workgroups. Either operand can prove the masked result aligned.
    for(unsigned zeros=0;zeros<=32;++zeros) {
        b=ready();word(b,0x360200ff);word(b,zeros==32?0u:UINT32_MAX<<zeros);
        memory(b,false,8,0);end(b);check(b,zeros>=2?Status::Ok:Status::InvalidProvenance);
    }
    b=ready();word(b,0x360202bf);memory(b,false,8,0);end(b);check(b); // 63 & aligned v1
    b=ready();word(b,0x360200bf);memory(b,false,8,0);end(b);check(b,Status::InvalidProvenance);
    b=ready();memory(b,true);word(b,0x360204bf);vm(b);end(b);check(b,Status::PendingLoad);
    b.clear();word(b,0x360000ff);check(b,Status::Truncated);
}
void provenanceAndBounds() {
    for(unsigned binding=0;binding<32;++binding) {
        Bytes b=ready(binding);memory(b,true);vm(b);memory(b,false);end(b);
        const Report out=check(b);CHECK(out.readMask==(1u<<binding)&&out.writeMask==(1u<<binding));
    }
    Bytes b=ready();mov(b,8,8);memory(b,true);vm(b);end(b);check(b,Status::InvalidProvenance);
    for(unsigned component=0;component<4;++component) {
        b=ready();mov(b,8+component,128);memory(b,true);vm(b);end(b);check(b,Status::InvalidProvenance);
    }
    b.clear();for(unsigned n=8;n<12;++n)mov(b,n,128);shift(b);memory(b,true);vm(b);end(b);check(b,Status::InvalidProvenance);
    b.clear();scalar(b,8,2,0);lgkm(b);shift(b);memory(b,true);vm(b);end(b);check(b,Status::InvalidProvenance);
    b.clear();mov(b,16,0);mov(b,17,1);scalar(b,8,16);lgkm(b);end(b);check(b,Status::InvalidProvenance);
    b.clear();mov(b,1,1);scalar(b,8,0);lgkm(b);end(b);check(b,Status::InvalidProvenance);
    b.clear();scalar(b,0,0);lgkm(b);end(b);check(b,Status::InvalidProvenance);
    b.clear();scalar(b,4,0,0,5);lgkm(b);end(b);check(b,Status::InvalidProvenance);
    b.clear();mov(b,5,144);scalar(b,8,0,0,5);lgkm(b);shift(b);memory(b,false,8,0);end(b);
    CHECK(check(b).writeMask==2);
    b.clear();mov(b,5,255);word(b,496);scalar(b,8,0,0,5);lgkm(b);shift(b);memory(b,false,8,0);end(b);
    CHECK(check(b).writeMask==0x80000000u);
    b.clear();mov(b,5,255);word(b,0xfffffff0);scalar(b,8,0,32,5);lgkm(b);end(b);check(b,Status::OutOfBounds);
    for(unsigned off:{1u,4u,8u,12u,512u,0xfffffu}) {
        b.clear();scalar(b,8,0,off);lgkm(b);end(b);
        check(b,off<16&&!(off&3)?Status::InvalidProvenance:Status::OutOfBounds);
    }
    b.clear();scalar(b,8,0,0,124,false);lgkm(b);end(b);check(b,Status::InvalidProvenance);
    b.clear();scalar(b,9,0);lgkm(b);end(b);check(b,Status::InvalidOperand);
    for(unsigned off:{0u,4u,44u,48u,3u}) {
        b.clear();scalar(b,8,2,off,124,false);lgkm(b);end(b);
        check(b,off<=44&&!(off&3)?Status::Ok:Status::OutOfBounds);
    }
    for(unsigned off:{0u,16u,32u,36u}) {
        b.clear();scalar(b,8,2,off);lgkm(b);end(b);check(b,off<=32?Status::Ok:Status::OutOfBounds);
    }
    for(unsigned offReg:{106u,123u,125u,126u,127u}) {
        b.clear();scalar(b,8,0,0,offReg);lgkm(b);end(b);check(b,Status::InvalidOperand);
    }
    for(unsigned bit=13;bit<18;++bit) {
        b.clear();scalar(b,8,0);put(b,0,get(b,0)|(1u<<bit));lgkm(b);end(b);check(b,Status::ReservedBits);
    }
    for(unsigned bit=20;bit<25;++bit) {
        b.clear();scalar(b,8,0);put(b,4,get(b,4)|(1u<<bit));lgkm(b);end(b);check(b,Status::ReservedBits);
    }
    for(unsigned offset:{129u,192u,193u,5u,0u,255u,124u}) {
        b=ready();memory(b,true,8,2,1,offset);vm(b);end(b);
        check(b,offset==255||offset==124?Status::InvalidOperand:Status::InvalidProvenance);
    }
    b=ready();mov(b,5,128);memory(b,false,8,0,1,5);end(b);check(b);
    b=ready();memory(b,false,8,0,0);end(b);check(b,Status::InvalidProvenance); // unaligned v0
    for(unsigned bit=0;bit<18;++bit) {
        b=ready();const size_t at=b.size();memory(b,true);put(b,at,get(b,at)|(1u<<bit));vm(b);end(b);check(b,Status::ReservedBits);
    }
    for(unsigned bit=21;bit<24;++bit) {
        b=ready();const size_t at=b.size();memory(b,true);put(b,at+4,get(b,at+4)^(1u<<bit));vm(b);end(b);check(b,Status::ReservedBits);
    }
    // Valid arbitrary integer ALU recipes retain a proof of aligned addresses.
    b=ready();vop3(b,583,1,256,128,130);memory(b,false,8,0);end(b);check(b);
    b=ready();vop3(b,582,1,256,130,128);memory(b,false,8,0);end(b);check(b);
    b=ready();vop3(b,812,1,132,256,128);memory(b,false,8,0);end(b);check(b);
    b=ready();vop3(b,583,1,256,128,129);memory(b,false,8,0);end(b);check(b,Status::InvalidProvenance);
}
void pendingAndClauses() {
    Bytes b;scalar(b,8,0);shift(b);memory(b,true);vm(b);end(b);check(b,Status::PendingLoad);
    b.clear();scalar(b,8,0);mov(b,8,128);lgkm(b);end(b);check(b,Status::PendingLoad);
    b.clear();scalar(b,8,0);word(b,0xbf89fc17);shift(b);memory(b,true);vm(b);end(b);check(b,Status::PendingLoad);
    b.clear();scalar(b,8,0);vm(b);shift(b);memory(b,true);vm(b);end(b);check(b,Status::PendingLoad);
    b.clear();scalar(b,8,0);word(b,0xbf870001);shift(b);memory(b,true);vm(b);end(b);check(b,Status::PendingLoad);
    b.clear();scalar(b,8,0);end(b);check(b,Status::PendingLoad);
    b=ready();memory(b,true);word(b,0x4a040481);end(b);check(b,Status::PendingLoad); // read pending v2
    b=ready();memory(b,true);word(b,0x4a040081);end(b);check(b,Status::PendingLoad); // overwrite pending v2
    b=ready();memory(b,true);word(b,0xbf8907f7);memory(b,false);end(b);check(b,Status::PendingLoad);
    b=ready();memory(b,true);lgkm(b);memory(b,false);end(b);check(b,Status::PendingLoad);
    b=ready();memory(b,true);word(b,0xbf870001);memory(b,false);end(b);check(b,Status::PendingLoad);
    b=ready();memory(b,true);end(b);check(b,Status::PendingLoad);
    b=ready();memory(b,true);vm(b);memory(b,false);end(b);check(b);
    for(uint32_t wait:{0xbf890008u,0xbf89ffffu}) {b.clear();word(b,wait);end(b);check(b,Status::InvalidWait);}
    for(uint32_t delay:{0xbf870800u,0xbf870008u,0xbf87000cu,0xbf870400u,0xbf870600u,0xbf870010u,0xbf8700e1u}) {
        b.clear();word(b,delay);end(b);check(b,Status::ReservedBits);
    }
    for(uint32_t delay:{0xbf870000u,0xbf870001u,0xbf870091u,0xbf870099u,0xbf87000bu,0xbf870587u}) {
        b.clear();word(b,delay);end(b);check(b);
    }
    b.clear();word(b,0xbf850000);scalar(b,8,0);lgkm(b);end(b);check(b);
    b.clear();word(b,0xbf850001);scalar(b,8,0);scalar(b,12,0,16);lgkm(b);end(b);check(b);
    b.clear();word(b,0xbf850020);scalar(b,8,0);lgkm(b);end(b);check(b,Status::InvalidClause);
    b.clear();word(b,0xbf850001);scalar(b,8,0);lgkm(b);end(b);check(b,Status::InvalidClause);
    b.clear();word(b,0xbf850001);word(b,0xbf850000);scalar(b,8,0);lgkm(b);end(b);check(b,Status::InvalidClause);
    b=ready();word(b,0xbf850000);memory(b,true);vm(b);end(b);check(b,Status::InvalidClause);
    b.clear();mov(b,5,128);word(b,0xbf850001);scalar(b,8,2,0,5,false);scalar(b,5,2,4,124,false);lgkm(b);end(b);check(b,Status::InvalidClause); // replay WAR
    b.clear();word(b,0xbf850001);scalar(b,5,2,0,124,false);scalar(b,8,2,0,5,false);lgkm(b);end(b);check(b,Status::PendingLoad); // RAW
    b.clear();word(b,0xbf850001);scalar(b,8,2,0,124,false);scalar(b,8,2,4,124,false);lgkm(b);end(b);check(b,Status::PendingLoad); // WAW
    // No counter wrap or fabricated completion. A zero wait starts a new epoch.
    for(unsigned count:{63u,64u}) {
        b.clear();for(unsigned n=0;n<count;++n)scalar(b,8+n,2,0,124,false);lgkm(b);end(b);
        check(b,count==63?Status::Ok:Status::InvalidWait);
        b=ready();for(unsigned n=0;n<count;++n)memory(b,true,8,2+n);vm(b);end(b);
        auto r=request(b);r.numVgprs=128;check(r,count==63?Status::Ok:Status::InvalidWait);
        b=ready();for(unsigned n=0;n<count;++n)memory(b,false,8,0);end(b);check(b,count==63?Status::Ok:Status::InvalidWait);
    }
    b.clear();for(unsigned epoch=0;epoch<3;++epoch) {for(unsigned n=0;n<63;++n)scalar(b,8+n,2,0,124,false);lgkm(b);}end(b);check(b);
}
void mutationAndTruncation() {
    for(const auto& f:fixtures) {
        const Bytes original=bytes(f);
        for(size_t size=4;size<original.size();size+=4) {
            Bytes truncated(original.begin(),original.begin()+size);
            Report report{};auto r=request(truncated,size,f.local);
            const auto status=validate(r,report);
            if(size==f.executable)CHECK(status==Status::Ok);
            if(size<f.executable) {CHECK(status!=Status::Ok);cleared(report);}
        }
        for(size_t offset=0;offset<original.size();++offset)for(unsigned bit=0;bit<8;++bit) {
            Bytes altered=original;altered[offset]^=uint8_t(1u<<bit);
            auto r=request(altered,f.executable,f.local);Report report{};
            const Status status=validate(r,report);CHECK(status==report.status);
            if(status!=Status::Ok) {cleared(report);CHECK(report.byteOffset<=r.codeBytes);}
            else {
                CHECK(report.requiredSgprs<=r.numSgprs&&report.requiredVgprs<=r.numVgprs);
                CHECK(report.rsrc1==0x202c0002&&report.rsrc2==0x788&&!report.rsrc3);
            }
        }
    }
    uint32_t random=0x713fd9ab;
    for(unsigned n=0;n<20000;++n) {
        Bytes b;const unsigned count=1+(n%40);
        for(unsigned k=0;k<count;++k) {random^=random<<13;random^=random>>17;random^=random<<5;word(b,random);}
        end(b);Report r{};const Status s=validate(request(b),r);CHECK(s==r.status);if(s!=Status::Ok)cleared(r);
    }
}
} // namespace
int main() {
    genuineProducts();packedLocalProduct();boundsAndLaunch();registerResidencyAdmission();structural();initializedRegisters();uniformAccessAndTransfers();provenanceAndBounds();pendingAndClauses();mutationAndTruncation();
    std::printf("gfx11_compute_product: %zu checks, %zu failures\n",checks,failures);
    return failures?1:0;
}
