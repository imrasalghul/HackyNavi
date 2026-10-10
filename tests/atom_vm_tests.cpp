// SPDX-License-Identifier: MIT
// Offline interpreter and I/O fault tests. The register model is test-only and
// does not establish successful ASIC POST on physical hardware.
#include "hackynavi/atom_vm.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <vector>
namespace a=hackynavi::atomvm;
namespace {
size_t checks=0,failures=0;
#define CHECK(x) do { ++checks;if(!(x)) { ++failures;std::cerr<<__LINE__<<": " #x "\n"; } } while(0)
void p16(std::vector<uint8_t>& b,size_t p,uint16_t v) { b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8); }
void p32(std::vector<uint8_t>& b,size_t p,uint32_t v) { for(unsigned i=0;i<4;++i)b[p+i]=uint8_t(v>>(i*8)); }
uint32_t u32(const std::vector<uint8_t>& b,size_t p) { uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(b[p+i])<<(i*8);return v; }
void imm(std::vector<uint8_t>& b,uint32_t v) { for(unsigned i=0;i<4;++i)b.push_back(uint8_t(v>>(i*8))); }
void mov(std::vector<uint8_t>& code,uint8_t kind,uint8_t index,uint32_t value) { code.push_back(uint8_t(kind+1));code.push_back(5);code.push_back(index);if(kind==0)code.push_back(0);imm(code,value); }
struct Fixture {
    std::vector<uint8_t> image=std::vector<uint8_t>(4096,0);
    std::vector<uint8_t> params=std::vector<uint8_t>(128,0),scratch=std::vector<uint8_t>(128,0);
    std::unique_ptr<a::Workspace> work{new a::Workspace};
    a::Program program={};a::Result result={};a::Limits limits={100000,1000000,32};
    struct Operation { a::Space space;bool write;uint32_t index,value; };
    std::vector<Operation> operations;std::map<uint64_t,uint32_t> registers;
    uint64_t delayed=0;size_t failAt=0;bool failDelay=false;unsigned events=0;uint8_t lastEvent=0;
    Fixture() {
        image[0]=0x55;image[1]=0xaa;p16(image,0x48,0x80);p16(image,0x80,36);
        image[0x84]='A';image[0x85]='T';image[0x86]='O';image[0x87]='M';
        p16(image,0x9e,0x100);p16(image,0xa0,0x180);
        p16(image,0x100,36);p16(image,0x180,68);
        p16(image,0x18c,0x240);p16(image,0x240,32);image[0x242]=3;image[0x243]=5;
        p32(image,0x248,100000);p32(image,0x24c,96000);
        command(0,{91});
    }
    void command(uint8_t index,const std::vector<uint8_t>& code,uint8_t ws=16,uint8_t ps=0) {
        const size_t base=0x400+size_t(index)*128;
        CHECK(base+6+code.size()<=image.size());CHECK(code.size()+6<=128);
        p16(image,0x104+size_t(index)*2,uint16_t(base));p16(image,base,uint16_t(code.size()+6));
        image[base+2]=2;image[base+3]=1;image[base+4]=ws;image[base+5]=ps;
        std::copy(code.begin(),code.end(),image.begin()+static_cast<std::ptrdiff_t>(base+6));
    }
    static uint64_t key(a::Space s,uint32_t idx) { return uint64_t(static_cast<unsigned>(s))<<32|idx; }
    static bool read(void* ctx,a::Space s,uint32_t idx,uint32_t& value) {
        auto& f=*static_cast<Fixture*>(ctx);value=f.registers[key(s,idx)];
        f.operations.push_back({s,false,idx,value});return f.operations.size()!=f.failAt;
    }
    static bool write(void* ctx,a::Space s,uint32_t idx,uint32_t value) {
        auto& f=*static_cast<Fixture*>(ctx);f.operations.push_back({s,true,idx,value});
        f.registers[key(s,idx)]=value;return f.operations.size()!=f.failAt; // landed ambiguous write
    }
    static bool delay(void* ctx,uint32_t us) { auto& f=*static_cast<Fixture*>(ctx);f.delayed+=us;return !f.failDelay; }
    static void event(void* ctx,a::Event,uint8_t value) { auto& f=*static_cast<Fixture*>(ctx);++f.events;f.lastEvent=value; }
    a::Io io() { return {this,read,write,delay,event}; }
    a::Status run(uint8_t index=0) {
        const auto s=a::open(image.data(),image.size(),program);if(s!=a::Status::Ok)return s;
        return a::execute(program,index,params.data(),params.size(),scratch.data(),scratch.size(),io(),limits,*work,result);
    }
};
void arithmetic() {
    Fixture f;std::vector<uint8_t> code;
    mov(code,1,0,0x11223344); // PS0 full move avoids an initial read.
    code.insert(code.end(),{2,0x8d,0,0xcd,0xab}); // PS0.word16 = abcd
    code.insert(code.end(),{8,0x05,0});imm(code,0xffff00ff); // AND
    code.insert(code.end(),{14,0x05,0});imm(code,0x5500); // OR
    code.insert(code.end(),{104,0x05,0});imm(code,0x10000); // XOR
    code.insert(code.end(),{44,0x05,0});imm(code,3); // ADD
    code.insert(code.end(),{50,0x05,0});imm(code,1); // SUB
    code.push_back(91);f.command(0,code);
    CHECK(f.run()==a::Status::Ok);CHECK(u32(f.params,0)==0xabcc5546);CHECK(f.operations.empty());
    // Partial move must preserve the unselected bytes of a real register.
    f.command(0,{1,0x65,0x20,0,0x77,91});f.registers[0x20]=0x12345678;
    CHECK(f.run()==a::Status::Ok);CHECK(f.registers[0x20]==0x12347778);CHECK(f.operations.size()==2);
    CHECK(f.result.writeAttempted);
    // Legacy MMIO index 0 is expressed as a DWORD offset by ATOM, but the
    // hardware selector stores byte offsets: AMD's interpreter shifts by two.
    code.clear();mov(code,0,0,0x123);code.push_back(91);f.command(0,code);f.operations.clear();
    CHECK(f.run()==a::Status::Ok);CHECK(f.registers[0]==0x48c);CHECK(f.operations.size()==1);
    // Full-DWORD shift and field shift differ: SHL operates on the complete
    // saved DWORD before extracting the destination byte.
    f.command(0,{110,0x65,0,4,91});p32(f.params,0,0x12345678);
    CHECK(f.run()==a::Status::Ok);CHECK(u32(f.params,0)==0x12346778);
    f.command(0,{20,0x28,0,4,91});p32(f.params,0,0x12345678);
    CHECK(f.run()==a::Status::Ok);CHECK(u32(f.params,0)==0x12346078);
    f.command(0,{20,0,0,32,91});CHECK(f.run()==a::Status::BadOperand);
    // Division places quotient/remainder in special WS40/41. MUL32/DIV32
    // retain the high half in WS41 (DIV32 is quotient-high, not remainder).
    code.clear();mov(code,1,0,25);code.insert(code.end(),{38,5,0});imm(code,7);
    code.insert(code.end(),{2,2,1,0x40,2,2,2,0x41,91});f.command(0,code);
    CHECK(f.run()==a::Status::Ok);CHECK(u32(f.params,4)==3);CHECK(u32(f.params,8)==4);
    code.clear();mov(code,1,0,0xffffffffu);code.insert(code.end(),{123,5,0});imm(code,2);
    code.insert(code.end(),{2,2,1,0x40,2,2,2,0x41,125,5,0});imm(code,3);
    code.insert(code.end(),{2,2,3,0x40,2,2,4,0x41,91});f.command(0,code);
    CHECK(f.run()==a::Status::Ok);CHECK(u32(f.params,4)==0xfffffffeu);CHECK(u32(f.params,8)==1);
    CHECK(u32(f.params,12)==0xaaaaaaaau);CHECK(u32(f.params,16)==0);
}
void spacesAndParameters() {
    Fixture f;std::vector<uint8_t> code;
    mov(code,2,0x48,0x100);mov(code,0,0x20,0xabcdef);code.push_back(91);f.command(0,code,0);
    CHECK(f.run()==a::Status::Ok);CHECK(f.registers[0x120]==0xabcdef);
    // Source ID operands are relative to SETDATABLOCK. FB accesses CPU scratch.
    p16(f.image,0x180+4+9*2,0x280);p16(f.image,0x280,16);p32(f.image,0x284,0x55443322);
    f.command(0,{102,9,2,4,0,4,0,59,5,8,0,0,0,4,1,3,0,2,3,1,3,91});
    CHECK(f.run()==a::Status::Ok);CHECK(u32(f.params,0)==0x55443322);CHECK(u32(f.scratch,20)==0x55443322);CHECK(u32(f.params,4)==0x55443322);
    // CALLTABLE shifts parameter base by caller header PS bytes, shares global
    // VM registers, but uses a fresh independent zeroed workspace.
    code.clear();mov(code,2,0,0xffffffff);mov(code,2,0x40,99);code.insert(code.end(),{82,1,2,2,0,0,91});f.command(0,code,1,8);
    f.command(1,{2,2,0,0,2,2,1,0x40,91},1);
    CHECK(f.run()==a::Status::Ok);CHECK(f.result.maximumDepth==2);CHECK(u32(f.params,0)==0xffffffff);
    CHECK(u32(f.params,8)==0);CHECK(u32(f.params,12)==99);
    // Missing optional CALLTABLE is a defined no-op; missing entry root fails.
    f.command(0,{82,3,91});CHECK(f.run()==a::Status::Ok);CHECK(f.run(3)==a::Status::MissingTable);
    f.command(0,{82,1,91},0,124);f.command(1,{2,5,2,0,0,0,0,91});
    CHECK(f.run()==a::Status::ParameterBounds);
    f.command(0,{2,2,0,0,91},0);CHECK(f.run()==a::Status::WorkspaceBounds);
    f.command(0,{2,1,32,0,91});CHECK(f.run()==a::Status::ParameterBounds);
    f.command(0,{59,5,128,0,0,0,2,3,0,0,91});CHECK(f.run()==a::Status::ScratchBounds);
    // Actual typed MC/PLL callbacks, and explicit rejection of upstream's
    // unimplemented PCI/SYSIO register paths.
    f.command(0,{5,5,2,0x78,0x56,0x34,0x12,6,6,3,2,91});f.operations.clear();
    CHECK(f.run()==a::Status::Ok);CHECK(f.registers[Fixture::key(a::Space::MemoryController,3)]==0x12345678);
    for(uint8_t port: {uint8_t(56),uint8_t(57)}) {
        f.command(0,{port,0,1,5,1,0,0,0,0,0,91});f.operations.clear();
        CHECK(f.run()==a::Status::UnsupportedIo);CHECK(f.operations.empty());
    }
}
void controlAndLimits() {
    Fixture f;
    for(uint32_t value=1;value<=3;++value)for(unsigned condition=0;condition<7;++condition) {
        p32(f.params,0,value);
        f.command(0,{61,5,0,2,0,0,0,uint8_t(67+condition),19,0,98,0,91,98,1,91});
        const bool expected=condition==0 || (condition==1 && value==2) || (condition==2 && value<2) ||
            (condition==3 && value>2) || (condition==4 && value<=2) || (condition==5 && value>=2) || (condition==6 && value!=2);
        CHECK(f.run()==a::Status::Ok);CHECK(f.lastEvent==unsigned(expected));
    }
    f.events=0;
    // Two decrement iterations with a conditional backward branch.
    f.command(0,{3,5,0,2,0,0,0,51,5,0,1,0,0,0,62,5,0,0,0,0,0,73,13,0,91},1);
    CHECK(f.run()==a::Status::Ok);CHECK(f.result.instructions==8);
    f.command(0,{67,6,0});f.limits.instructions=17;
    CHECK(f.run()==a::Status::InstructionLimit);CHECK(f.result.instructions==17);
    f.command(0,{82,0,91});f.limits.nesting=3;
    CHECK(f.run()==a::Status::NestingLimit);CHECK(f.result.maximumDepth==3);
    f.command(0,{80,2,81,9,91});f.limits.delayMicroseconds=2008;f.delayed=0;
    CHECK(f.run()==a::Status::DelayLimit);CHECK(f.delayed==2000);CHECK(f.result.delayedMicroseconds==2000);
    f.limits.delayMicroseconds=2009;f.delayed=0;CHECK(f.run()==a::Status::Ok);CHECK(f.delayed==2009);
    f.failDelay=true;CHECK(f.run()==a::Status::IoFailed);
    // Switch with two cases, a default, and explicit table-relative targets.
    f.failDelay=false;f.limits.instructions=1000;
    f.command(0,{66,0x25,2,0x63,1,22,0,0x63,2,25,0,0x5a,0x5a,98,3,91,98,1,91,98,2,91});
    CHECK(f.run()==a::Status::Ok);CHECK(f.events==1);CHECK(f.result.instructionOffset==0x400+27);
    // Preflight must validate untaken paths BEFORE any hardware write.
    f.command(0,{1,5,1,0,5,0,0,0,68,18,0,91,83});f.operations.clear();
    CHECK(f.run()==a::Status::UnsupportedOpcode);CHECK(f.operations.empty());CHECK(!f.result.writeAttempted);
    f.command(0,{67,7,0,91});CHECK(f.run()==a::Status::BadBranch);
    f.command(0,{67,0,0,91});CHECK(f.run()==a::Status::BadBranch);
    f.command(0,{0});CHECK(f.run()==a::Status::UnsupportedOpcode);
    f.command(0,{90});CHECK(f.run()==a::Status::BadBranch); // no implicit success at table end
    f.command(0,{122,3,0,0,0xff,83,91});CHECK(f.run()==a::Status::Ok); // PROCESSDS is embedded data
    f.command(0,{122,255,0,91});CHECK(f.run()==a::Status::Truncated);
}
void indirectAndFailures() {
    Fixture f;
    // Read method 1: index -> MMIO selector, then read MMIO data. Write method
    // 0x81: same selector followed by DATA->MMIO. No synthetic I/O return path.
    std::vector<uint8_t> iio={1,1,4,32,0,6,16,0,0,3,0x30,0,2,0x31,0,9,0,0,
        1,0x81,4,32,0,6,16,0,0,3,0x30,0,8,32,0,0,3,0x31,0,9,0,0};
    p16(f.image,0x180+4+23*2,0x300);p16(f.image,0x300,uint16_t(4+iio.size()));
    std::copy(iio.begin(),iio.end(),f.image.begin()+0x304);
    f.command(0,{55,1,0,2,0,0,0x21,0,1,1,0x22,0,0,91});f.registers[0x31]=0xdeadbeef;
    CHECK(f.run()==a::Status::Ok);CHECK(u32(f.params,0)==0xdeadbeef);CHECK(f.registers[0x30]==0x22);CHECK(f.registers[0x31]==0xdeadbeef);
    CHECK(f.operations.size()==4);const size_t n=f.operations.size();
    for(size_t failure=1;failure<=n;++failure) {
        f.operations.clear();f.failAt=failure;
        CHECK(f.run()==a::Status::IoFailed);CHECK(f.operations.size()==failure);CHECK(f.result.writeAttempted);
    }
    f.failAt=0;f.operations.clear();f.limits.instructions=4;
    CHECK(f.run()==a::Status::InstructionLimit);CHECK(f.operations.empty());
    // Malformed IIO bit fields would make the original C shift undefined.
    for(uint8_t width: {uint8_t(0),uint8_t(33),uint8_t(255)}) {
        f.image[0x307]=width;CHECK(f.run()==a::Status::BadIndirectIo);
    }
    f.image[0x307]=32;f.image[0x308]=1;CHECK(f.run()==a::Status::BadIndirectIo);
    // Callback fails after the hardware accepted a write: result retains the
    // attempted mutation so the owner cannot discard hardware references.
    Fixture g;g.command(0,{1,5,0x20,0,1,2,3,4,91});g.failAt=1;
    CHECK(g.run()==a::Status::IoFailed);CHECK(g.result.writeAttempted);CHECK(g.registers[0x20]==0x04030201);
    g.command(0,{2,0,0,0x20,0,91});g.operations.clear();
    CHECK(g.run()==a::Status::IoFailed);CHECK(!g.result.writeAttempted);
}
void boundariesAndInitialization() {
    Fixture f;uint8_t params[a::kAsicInitParameterBytes]={};
    CHECK(a::open(f.image.data(),f.image.size(),f.program)==a::Status::Ok);
    CHECK(a::prepareAsicInit(f.program,true,params)==a::Status::Ok);
    CHECK(params[0]==0xa0 && params[1]==0x86 && params[2]==1 && params[3]==0);
    CHECK(params[4]==0 && params[5]==0x77 && params[6]==1 && params[7]==0);
    CHECK(a::prepareAsicInit(f.program,false,params)==a::Status::Ok);CHECK(params[7]==0x20);
    CHECK(std::all_of(params+8,params+72,[](uint8_t v){return v==0;}));
    f.image[0x402]=1;CHECK(a::prepareAsicInit(f.program,true,params)==a::Status::UnsupportedVersion);
    f.image[0x402]=2;f.image[0x242]=2;CHECK(a::prepareAsicInit(f.program,true,params)==a::Status::UnsupportedVersion);
    Fixture b;const auto original=b.image;
    for(size_t size=0;size<0x407;++size) {
        a::Program p={};const auto s=a::open(b.image.data(),size,p);
        if(s==a::Status::Ok)CHECK(a::analyze(p,0,*b.work,b.result)!=a::Status::Ok);
        else CHECK(p.image==nullptr);
    }
    // Truncations of each operand width cannot execute an earlier valid write.
    for(size_t length=1;length<8;++length) {
        b.command(0,{1,5,1,0,1,2,3,4,91});p16(b.image,0x400,uint16_t(6+length));b.operations.clear();
        CHECK(b.run()!=a::Status::Ok);CHECK(b.operations.empty());
    }
    b.image=original;b.limits.nesting=33;CHECK(b.run()==a::Status::InvalidArgument);
    b.limits.nesting=32;b.limits.instructions=0;CHECK(b.run()==a::Status::InvalidArgument);
    // An IIO method may end exactly at the 16-bit address-space boundary; its
    // exclusive end must not wrap to zero and become an apparent empty method.
    Fixture end;end.image.resize(a::kAddressSpace);
    p16(end.image,0x180+4+23*2,65524);p16(end.image,65524,12);
    const uint8_t method[]={1,1,2,0x20,0,9,0,0};
    std::copy(method,method+sizeof(method),end.image.begin()+65528);
    end.command(0,{55,1,0,2,0,0,0x40,0,91});end.registers[0x20]=0x11223344;
    const auto endStatus=end.run();
    if(endStatus!=a::Status::Ok)std::cerr<<"IIO boundary: "<<a::statusName(endStatus)
        <<" offset="<<end.result.instructionOffset<<" opcode="<<unsigned(end.result.opcode)
        <<" method="<<end.program.indirectStart[1]<<".."<<end.program.indirectEnd[1]<<"\n";
    CHECK(endStatus==a::Status::Ok);CHECK(u32(end.params,0)==0x11223344);
    // Full-word bounds apply to scratch reads/writes, not just their starting
    // byte. Last valid DWORD works; an overlapping end is rejected.
    Fixture edge;edge.command(0,{4,5,31,0x78,0x56,0x34,0x12,2,3,0,31,91});
    CHECK(edge.run()==a::Status::Ok);CHECK(u32(edge.params,0)==0x12345678);
    edge.scratch.resize(127);CHECK(edge.run()==a::Status::ScratchBounds);
    std::vector<uint8_t> shift;mov(shift,2,0x43,31);shift.insert(shift.end(),{2,2,0,0x44,2,2,1,0x45,91});edge.command(0,shift);
    CHECK(edge.run()==a::Status::Ok);CHECK(u32(edge.params,0)==0x80000000);CHECK(u32(edge.params,4)==0x7fffffff);
    shift.clear();mov(shift,2,0x43,32);shift.insert(shift.end(),{2,2,0,0x44,91});edge.command(0,shift);
    CHECK(edge.run()==a::Status::BadOperand);
}
void suppliedRom(const char* path) {
    std::ifstream in(path,std::ios::binary);std::vector<uint8_t> full((std::istreambuf_iterator<char>(in)),{});
    CHECK(full.size()>=0x40000+57856);if(full.size()<0x40000+57856)return;
    Fixture f;f.image.assign(full.begin()+0x40000,full.begin()+0x40000+57856);
    CHECK(a::open(f.image.data(),f.image.size(),f.program)==a::Status::Ok);
    CHECK(a::analyze(f.program,0,*f.work,f.result)==a::Status::Ok);
    CHECK(f.result.analyzedTables==1);CHECK(f.result.analyzedInstructions==29);CHECK(f.result.calledTables[0]==1);
    std::vector<uint16_t> offsets;
    for(unsigned off=0;off<a::kAddressSpace;++off)if(f.work->byteKind[off]==2)offsets.push_back(uint16_t(off));
    CHECK(offsets.size()==29);
    const unsigned expected[]={1,3,13,27,45,51,55,60,62,68,69,73,75,76,81,86,91,102};
    for(unsigned op=0;op<128;++op) {
        const bool present=std::find(std::begin(expected),std::end(expected),op)!=std::end(expected);
        CHECK(((f.result.opcodes[op/32]>>(op%32))&1)==unsigned(present));
    }
    uint8_t p[a::kAsicInitParameterBytes]={};CHECK(a::prepareAsicInit(f.program,true,p)==a::Status::Ok);
    std::copy(p,p+sizeof(p),f.params.begin());
    // The test device reports completion through the same indexed MMIO data
    // register polled by the real image. This checks bytecode behavior only.
    f.registers[0x0d]=0x80000000;f.registers[0x0de3]=2049;
    CHECK(f.run()==a::Status::Ok);CHECK(f.result.writeAttempted);CHECK(f.delayed==0);
    CHECK(f.registers[0x0c]==0x58184);CHECK(f.registers[6]==1);CHECK((f.registers[0x53]&0x200)==0x200);
    const auto operations=f.operations;CHECK(!operations.empty());
    for(size_t failure=1;failure<=operations.size();++failure) {
        f.failAt=failure;f.operations.clear();
        CHECK(f.run()==a::Status::IoFailed);CHECK(f.operations.size()==failure);
    }
    // A non-responding ASIC never becomes a successful POST due to the test
    // transport: the global instruction budget ends the polling loop.
    f.failAt=0;f.operations.clear();f.registers[0x0d]=0;f.limits.instructions=100;f.limits.delayMicroseconds=1000000;
    CHECK(f.run()==a::Status::InstructionLimit);CHECK(f.result.writeAttempted);CHECK(f.delayed>0);
    // This ROM's own counter expiry then falls through to setting SCRATCH7.
    // A native recovery owner MUST additionally require the observed hardware
    // wait completion, rather than treating VM success/that marker as POST.
    f.operations.clear();f.delayed=0;f.registers[0x53]=0;
    f.limits.instructions=2000000;f.limits.delayMicroseconds=5000000;
    CHECK(f.run()==a::Status::Ok);CHECK(f.delayed==4000000);CHECK((f.registers[0x53]&0x200)!=0);
    CHECK(std::none_of(f.operations.begin(),f.operations.end(),[](const Fixture::Operation& op) {
        return !op.write && op.index==0x0d && op.value==0x80000000;
    }));
    // Corrupt every reachable opcode individually, including untaken paths.
    // Unsupported bytecode must fail in preflight with zero I/O.
    const auto valid=f.image;
    for(uint16_t off:offsets) {
        f.image=valid;f.image[off]=0;f.operations.clear();
        CHECK(f.run()!=a::Status::Ok);CHECK(f.operations.empty());
    }
}
}
int main(int argc,char** argv) {
    arithmetic();spacesAndParameters();controlAndLimits();indirectAndFailures();boundariesAndInitialization();
    if(argc>1)suppliedRom(argv[1]);
    std::cout<<checks<<" checks, "<<failures<<" failures\n";
    return failures?1:0;
}
