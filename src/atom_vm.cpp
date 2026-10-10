// SPDX-License-Identifier: MIT
/*
 * Adapted from drivers/gpu/drm/amd/amdgpu/atom.c and atom.h, Linux commit
 * 3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0.
 * Copyright 2008 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 * Original author: Stanislaw Skowronek.
 *
 * The port adds bounds, control-flow validation, bounded iterative call frames,
 * explicit error propagation and injected I/O. It does not replace failed
 * hardware reads with fabricated values. Linux's unimplemented PCI/SYSIO and
 * REPEAT/SAVEREG/RESTOREREG instructions are explicit errors here.
 */
#include "hackynavi/atom_vm.hpp"
namespace hackynavi { namespace atomvm { namespace {
bool range(size_t p,size_t n,size_t total) { return p<=total && n<=total-p; }
uint16_t u16(const uint8_t* b,size_t p) { return uint16_t(uint16_t(b[p]) | uint16_t(uint16_t(b[p+1])<<8)); }
uint32_t u32(const uint8_t* b,size_t p) { return uint32_t(u16(b,p)) | uint32_t(u16(b,p+2))<<16; }
void put32(uint8_t* b,size_t p,uint32_t v) { for(unsigned i=0;i<4;++i)b[p+i]=uint8_t(v>>(i*8)); }
template<class T> void zero(T& v) { uint8_t* p=reinterpret_cast<uint8_t*>(&v); for(size_t i=0;i<sizeof(T);++i)p[i]=0; }
const uint32_t masks[8]={0xffffffffu,0xffffu,0xffff00u,0xffff0000u,0xffu,0xff00u,0xff0000u,0xff000000u};
const uint8_t shifts[8]={0,0,8,16,0,8,16,24};
const uint8_t destinations[8][4]={{0,0,0,0},{1,2,3,0},{1,2,3,0},{1,2,3,0},{4,5,6,7},{4,5,6,7},{4,5,6,7},{4,5,6,7}};
const uint8_t defaultDestination[8]={0,0,1,2,0,1,2,3};
const uint8_t destinationKind[6]={0,1,2,3,6,7};
const uint8_t iioLengths[10]={1,2,3,3,3,3,4,4,4,3};
Status table(const Program& p,bool command,unsigned index,uint32_t& base,uint32_t& end) {
    const unsigned count=command?p.commandCount:p.dataCount;
    if(index>=count)return Status::BadTable;
    base=u16(p.image,(command?p.commands:p.data)+4u+index*2u);
    if(!base) { end=0; return Status::MissingTable; }
    if(!range(base,command?6u:4u,p.imageBytes))return Status::Truncated;
    const uint32_t n=u16(p.image,base);
    if(n<(command?6u:4u))return Status::BadTable;
    if(!range(base,n,p.imageBytes) || base+n>kAddressSpace)return Status::Truncated;
    end=base+n; return Status::Ok;
}
enum class Action : uint8_t { Move,And,Or,ShiftLeft,ShiftRight,Mul,Div,Add,Sub,
    SetPort,SetReg,SetFb,Compare,Switch,Jump,Test,Delay,Call,Clear,Nop,End,
    Mask,Post,Beep,SetData,Xor,Shl,Shr,Debug,ProcessData,Mul32,Div32 };
struct Operand { uint8_t kind,alignment; uint32_t value; };
struct Instruction {
    Action action; uint8_t opcode,attribute,argument;
    Operand destination,source; uint32_t extra,next,cases;
};
bool take(const Program& p,uint32_t& pc,uint32_t end,unsigned n,uint32_t& value) {
    if(!range(pc,n,end))return false;
    value=0; for(unsigned i=0;i<n;++i)value|=uint32_t(p.image[pc+i])<<(i*8);
    pc+=n; return true;
}
unsigned immediateBytes(unsigned alignment) { return alignment==0?4u:alignment<4?2u:1u; }
bool operand(const Program& p,uint32_t& pc,uint32_t end,uint8_t kind,uint8_t align,Operand& out) {
    out.kind=kind; out.alignment=align;
    const unsigned n=(kind==0 || kind==4)?2u:kind==5?immediateBytes(align):1u;
    return take(p,pc,end,n,out.value);
}
Status decode(const Program& p,uint32_t pc,uint32_t end,Instruction& i) {
    zero(i); uint32_t v=0;
    if(!take(p,pc,end,1,v))return Status::Truncated;
    i.opcode=uint8_t(v);
    bool binary=false,unary=false;
    if(v>=1 && v<=54) {
        static const Action actions[9]={Action::Move,Action::And,Action::Or,Action::ShiftLeft,Action::ShiftRight,Action::Mul,Action::Div,Action::Add,Action::Sub};
        i.action=actions[(v-1)/6]; i.argument=destinationKind[(v-1)%6];
        unary=v>=19 && v<=30; binary=!unary;
    } else if(v>=60 && v<=65) { i.action=Action::Compare; i.argument=destinationKind[v-60]; binary=true; }
    else if(v>=74 && v<=79) { i.action=Action::Test; i.argument=destinationKind[v-74]; binary=true; }
    else if(v>=84 && v<=89) { i.action=Action::Clear; i.argument=destinationKind[v-84]; unary=true; }
    else if(v>=92 && v<=97) { i.action=Action::Mask; i.argument=destinationKind[v-92]; binary=true; }
    else if(v>=103 && v<=120) {
        static const Action actions[3]={Action::Xor,Action::Shl,Action::Shr};
        i.action=actions[(v-103)/6]; i.argument=destinationKind[(v-103)%6]; binary=true;
    } else if(v>=123 && v<=126) { i.action=v<125?Action::Mul32:Action::Div32; i.argument=uint8_t(1+(v-123)%2); binary=true; }
    else switch(v) {
    case 55: case 56: case 57: i.action=Action::SetPort; i.argument=uint8_t(v-55); break;
    case 58: i.action=Action::SetReg; break; case 59:i.action=Action::SetFb;break;
    case 66:i.action=Action::Switch;break;
    case 67:case 68:case 69:case 70:case 71:case 72:case 73:i.action=Action::Jump;i.argument=uint8_t(v-67);break;
    case 80:case 81:i.action=Action::Delay;i.argument=uint8_t(v==80?1:0);break;
    case 82:i.action=Action::Call;break; case 90:i.action=Action::Nop;break;
    case 91:i.action=Action::End;break;case 98:i.action=Action::Post;break;
    case 99:i.action=Action::Beep;break;case 102:i.action=Action::SetData;break;
    case 121:i.action=Action::Debug;break;case 122:i.action=Action::ProcessData;break;
    default:return Status::UnsupportedOpcode;
    }
    if(binary || unary) {
        if(!take(p,pc,end,1,v))return Status::Truncated;
        i.attribute=uint8_t(v);
        if(unary)i.attribute=uint8_t((v&0x38u) | uint32_t(defaultDestination[(v>>3)&7])<<6);
        const uint8_t align=destinations[(i.attribute>>3)&7][i.attribute>>6];
        if(!operand(p,pc,end,i.argument,align,i.destination))return Status::Truncated;
        if(i.action==Action::Mask && !take(p,pc,end,immediateBytes((i.attribute>>3)&7),i.extra))return Status::Truncated;
        if(binary && !operand(p,pc,end,i.attribute&7,uint8_t((i.attribute>>3)&7),i.source))return Status::Truncated;
        if(unary && i.action!=Action::Clear && !take(p,pc,end,1,i.extra))return Status::Truncated;
    } else if(i.action==Action::SetFb || i.action==Action::Switch) {
        if(!take(p,pc,end,1,v))return Status::Truncated;
        i.attribute=uint8_t(v);
        if(!operand(p,pc,end,i.attribute&7,uint8_t((i.attribute>>3)&7),i.source))return Status::Truncated;
        if(i.action==Action::Switch) {
            i.cases=pc;
            while(true) {
                if(!range(pc,2,end))return Status::Truncated;
                if(u16(p.image,pc)==0x5a5a) { pc+=2;break; }
                if(p.image[pc++]!=0x63)return Status::BadOperand;
                if(!take(p,pc,end,immediateBytes((i.attribute>>3)&7),v) || !take(p,pc,end,2,v))return Status::Truncated;
            }
        }
    } else {
        unsigned n=0;
        switch(i.action) {
        case Action::SetPort:n=i.argument==0?2u:1u;break;
        case Action::SetReg:case Action::Jump:case Action::ProcessData:n=2;break;
        case Action::Delay:case Action::Call:case Action::SetData:case Action::Post:case Action::Debug:n=1;break;
        default:break;
        }
        if(n && !take(p,pc,end,n,i.extra))return Status::Truncated;
        if(i.action==Action::ProcessData) {
            if(!range(pc,i.extra,end))return Status::Truncated;
            pc+=i.extra;
        }
    }
    i.next=pc;return Status::Ok;
}
Status enqueue(Workspace& w,size_t& count,uint32_t pc,uint32_t start,uint32_t end) {
    if(pc<start+6 || pc>=end)return Status::BadBranch;
    if(w.byteKind[pc]==3)return Status::BadBranch;
    if(w.byteKind[pc])return Status::Ok;
    if(count>=kPendingInstructions)return Status::InstructionLimit;
    w.byteKind[pc]=1; w.pending[count++]=uint16_t(pc);return Status::Ok;
}
Status analyzeOpened(const Program& p,uint8_t root,Workspace& w,Result& r) {
    for(size_t i=0;i<256;++i)w.tables[i]=0;
    w.tables[root]=1;
    for(unsigned iteration=0;iteration<256;++iteration) {
        unsigned index=0;while(index<256 && w.tables[index]!=1)++index;
        if(index==256)return Status::Ok;
        w.tables[index]=2;uint32_t base=0,end=0;
        Status s=table(p,true,index,base,end);
        r.tableIndex=uint8_t(index);r.tableOffset=uint16_t(base);
        if(s==Status::MissingTable && index!=root)continue;
        if(s!=Status::Ok)return s;
        ++r.analyzedTables;r.calledTables[index/32]|=uint32_t(1)<<(index%32);
        for(size_t k=0;k<kAddressSpace;++k)w.byteKind[k]=0;
        size_t count=0;s=enqueue(w,count,base+6,base,end);if(s!=Status::Ok)return s;
        while(count) {
            const uint32_t pc=w.pending[--count];
            if(w.byteKind[pc]==3)return Status::BadBranch;
            if(w.byteKind[pc]==2)continue;
            r.instructionOffset=uint16_t(pc);r.opcode=p.image[pc];
            Instruction i={};s=decode(p,pc,end,i);if(s!=Status::Ok)return s;
            w.byteKind[pc]=2;
            for(uint32_t k=pc+1;k<i.next;++k) { if(w.byteKind[k])return Status::BadBranch;w.byteKind[k]=3; }
            ++r.analyzedInstructions;r.opcodes[i.opcode/32]|=uint32_t(1)<<(i.opcode%32);
            if(i.action==Action::Call) {
                if(i.extra>=p.commandCount)return Status::BadTable;
                if(!w.tables[i.extra])w.tables[i.extra]=1;
            }
            if(i.action==Action::Jump) { s=enqueue(w,count,base+i.extra,base,end);if(s!=Status::Ok)return s; }
            if(i.action==Action::Switch) {
                uint32_t cursor=i.cases;
                while(u16(p.image,cursor)!=0x5a5a) {
                    cursor+=1+immediateBytes((i.attribute>>3)&7);
                    s=enqueue(w,count,base+u16(p.image,cursor),base,end);if(s!=Status::Ok)return s;cursor+=2;
                }
            }
            if(i.action!=Action::End && !(i.action==Action::Jump && i.argument==0)) {
                s=enqueue(w,count,i.next,base,end);if(s!=Status::Ok)return s;
            }
        }
    }
    return Status::Ok;
}
struct Runtime {
    const Program& p; const Io& io; const Limits& limits; Result& r;
    uint8_t* parameters;size_t parameterBytes;uint8_t* scratch;size_t scratchBytes;
    uint32_t divmul[2],fbBase;uint16_t dataBlock,regBlock,ioAttribute,ioMode;
    uint8_t shift;bool equal,above;
    Status tick() {
        if(r.instructions>=limits.instructions)return Status::InstructionLimit;
        ++r.instructions;return Status::Ok;
    }
    Status read(Space space,uint32_t index,uint32_t& value) {
        ++r.ioOperations;return io.read(io.context,space,index,value)?Status::Ok:Status::IoFailed;
    }
    Status write(Space space,uint32_t index,uint32_t value) {
        ++r.ioOperations;r.writeAttempted=true;
        return io.write(io.context,space,index,value)?Status::Ok:Status::IoFailed;
    }
    Status indirect(unsigned method,uint32_t index,uint32_t data,uint32_t& value) {
        if(method>=256 || !p.indirectStart[method])return Status::BadIndirectIo;
        uint32_t pc=p.indirectStart[method],end=p.indirectEnd[method];
        // This accumulator initialization is defined by AMD's interpreter.
        // An actual READ opcode always obtains data through the real callback.
        value=0xcdcdcdcdu;
        while(pc<end) {
            Status s=tick();if(s!=Status::Ok)return s;
            const uint8_t op=p.image[pc];
            const uint8_t n=iioLengths[op];
            if(!range(pc,n,end))return Status::BadIndirectIo;
            if(op==9)return Status::Ok;
            if(op==2) { s=read(Space::Mmio,u16(p.image,pc+1),value);if(s!=Status::Ok)return s; }
            else if(op==3) { s=write(Space::Mmio,u16(p.image,pc+1),value);if(s!=Status::Ok)return s; }
            else if(op>=4 && op<=8) {
                const unsigned width=p.image[pc+1],source=p.image[pc+2];
                const unsigned dest=op>=6?p.image[pc+3]:source;
                const uint32_t mask=width==32?0xffffffffu:((uint32_t(1)<<width)-1);
                if(op==4)value&=~(mask<<dest);
                else if(op==5)value|=mask<<dest;
                else {
                    const uint32_t v=op==6?index:op==7?ioAttribute:data;
                    value=(value&~(mask<<dest))|(((v>>source)&mask)<<dest);
                }
            } else if(op!=0)return Status::BadIndirectIo;
            pc+=n;
        }
        return Status::BadIndirectIo;
    }
    Status registerRead(uint32_t index,uint32_t& value) {
        index+=regBlock;
        if(ioMode==0)return read(Space::Mmio,index,value);
        if(ioMode==1 || ioMode==2 || !(ioMode&0x80))return Status::UnsupportedIo;
        return indirect(ioMode&0x7f,index,0,value);
    }
    Status registerWrite(uint32_t index,uint32_t value) {
        index+=regBlock;
        if(ioMode==0)return write(Space::Mmio,index,index==0?value<<2:value);
        if(ioMode==1 || ioMode==2 || !(ioMode&0x80))return Status::UnsupportedIo;
        uint32_t ignored=0;return indirect(ioMode&0xff,index,value,ignored);
    }
    Status workspaceRead(Frame& f,uint32_t index,uint32_t& value) {
        switch(index) {
        case 0x40:case 0x41:value=divmul[index-0x40];break;
        case 0x42:value=dataBlock;break;case 0x43:value=shift;break;
        case 0x44:case 0x45:
            if(shift>=32)return Status::BadOperand;
            value=uint32_t(1)<<shift;if(index==0x45)value=~value;break;
        case 0x46:value=fbBase;break;case 0x47:value=ioAttribute;break;case 0x48:value=regBlock;break;
        default:if(index>=f.workspaceWords)return Status::WorkspaceBounds;value=f.words[index];break;
        }
        return Status::Ok;
    }
    Status workspaceWrite(Frame& f,uint32_t index,uint32_t value) {
        switch(index) {
        case 0x40:case 0x41:divmul[index-0x40]=value;break;
        case 0x42:dataBlock=uint16_t(value);break;case 0x43:shift=uint8_t(value);break;
        case 0x44:case 0x45:break; // These architectural derived registers are read-only.
        case 0x46:fbBase=value;break;case 0x47:ioAttribute=uint16_t(value);break;case 0x48:regBlock=uint16_t(value);break;
        default:if(index>=f.workspaceWords)return Status::WorkspaceBounds;f.words[index]=value;break;
        }
        return Status::Ok;
    }
    Status get(Frame& f,const Operand& op,uint32_t& value,uint32_t& saved) {
        uint32_t full=0;Status s=Status::Ok;size_t offset=0;
        switch(op.kind) {
        case 0:s=registerRead(op.value,full);break;
        case 1:
            offset=f.parameterBase+size_t(op.value)*4;
            if(!range(offset,4,parameterBytes))return Status::ParameterBounds;
            full=u32(parameters,offset);break;
        case 2:s=workspaceRead(f,op.value,full);break;
        case 3:
            // Linux uses fb_base / 4, including integer truncation.
            offset=(size_t(fbBase)/4+op.value)*4;
            if(!range(offset,4,scratchBytes))return Status::ScratchBounds;
            full=u32(scratch,offset);break;
        case 4:
            offset=size_t(dataBlock)+op.value;
            if(!range(offset,4,p.imageBytes))return Status::Truncated;
            full=u32(p.image,offset);break;
        case 5:saved=op.value;value=op.value;return Status::Ok;
        case 6:s=read(Space::Pll,op.value,full);break;
        case 7:s=read(Space::MemoryController,op.value,full);break;
        default:return Status::BadOperand;
        }
        if(s!=Status::Ok)return s;
        saved=full;value=(full&masks[op.alignment])>>shifts[op.alignment];return Status::Ok;
    }
    Status put(Frame& f,const Operand& op,uint32_t value,uint32_t saved) {
        value=((value<<shifts[op.alignment])&masks[op.alignment])|(saved&~masks[op.alignment]);
        size_t offset=0;
        switch(op.kind) {
        case 0:return registerWrite(op.value,value);
        case 1:offset=f.parameterBase+size_t(op.value)*4;if(!range(offset,4,parameterBytes))return Status::ParameterBounds;put32(parameters,offset,value);return Status::Ok;
        case 2:return workspaceWrite(f,op.value,value);
        case 3:offset=(size_t(fbBase)/4+op.value)*4;if(!range(offset,4,scratchBytes))return Status::ScratchBounds;put32(scratch,offset,value);return Status::Ok;
        case 6:return write(Space::Pll,op.value,value);
        case 7:return write(Space::MemoryController,op.value,value);
        default:return Status::BadOperand;
        }
    }
};
Status push(const Program& p,uint8_t index,size_t params,Workspace& w,unsigned& depth,const Limits& limits) {
    uint32_t base=0,end=0;Status s=table(p,true,index,base,end);if(s!=Status::Ok)return s;
    if(depth>=limits.nesting)return Status::NestingLimit;
    Frame& f=w.frames[depth++];zero(f);
    f.start=base;f.end=end;f.pc=base+6;f.index=index;f.parameterBase=params;
    f.workspaceWords=p.image[base+4];f.parameterShift=uint8_t((p.image[base+5]&0x7f)/4);
    return Status::Ok;
}
Status run(Runtime& rt,uint8_t root,Workspace& w) {
    unsigned depth=0;Status s=push(rt.p,root,0,w,depth,rt.limits);if(s!=Status::Ok)return s;
    while(depth) {
        Frame& f=w.frames[depth-1];Result& r=rt.r;
        r.depth=uint8_t(depth);if(depth>r.maximumDepth)r.maximumDepth=uint8_t(depth);
        r.tableIndex=f.index;r.tableOffset=uint16_t(f.start);r.instructionOffset=uint16_t(f.pc);
        s=rt.tick();if(s!=Status::Ok)return s;
        Instruction i={};s=decode(rt.p,f.pc,f.end,i);if(s!=Status::Ok)return s;
        r.opcode=i.opcode;f.pc=i.next;
        uint32_t dst=0,src=0,saved=0,ignored=0;
        const bool binary=(i.opcode>=1 && i.opcode<=54) || (i.opcode>=60 && i.opcode<=65) ||
            (i.opcode>=74 && i.opcode<=79) || (i.opcode>=84 && i.opcode<=89) ||
            (i.opcode>=92 && i.opcode<=97) || (i.opcode>=103 && i.opcode<=120) || i.opcode>=123;
        if(binary) {
            if(i.action!=Action::Move || ((i.attribute>>3)&7)!=0) {
                s=rt.get(f,i.destination,dst,saved);if(s!=Status::Ok)return s;
            }
            if(i.action!=Action::Clear && i.action!=Action::ShiftLeft && i.action!=Action::ShiftRight) {
                s=rt.get(f,i.source,src,ignored);if(s!=Status::Ok)return s;
            }
            bool write=true;
            switch(i.action) {
            case Action::Move:dst=src;break;case Action::And:dst&=src;break;case Action::Or:dst|=src;break;
            case Action::Xor:dst^=src;break;case Action::Add:dst+=src;break;case Action::Sub:dst-=src;break;
            case Action::Clear:dst=0;break;case Action::Mask:dst=(dst&i.extra)|src;break;
            case Action::ShiftLeft:case Action::ShiftRight:
                if(i.extra>=32)return Status::BadOperand;
                dst=i.action==Action::ShiftLeft?dst<<i.extra:dst>>i.extra;break;
            case Action::Shl:case Action::Shr:
                // Linux narrows the source to uint8_t before applying this ISA shift.
                src=uint8_t(src);if(src>=32)return Status::BadOperand;
                dst=i.action==Action::Shl?saved<<src:saved>>src;
                dst=(dst&masks[i.destination.alignment])>>shifts[i.destination.alignment];break;
            case Action::Compare:rt.equal=dst==src;rt.above=dst>src;write=false;break;
            case Action::Test:rt.equal=(dst&src)==0;write=false;break;
            case Action::Mul:rt.divmul[0]=dst*src;write=false;break;
            case Action::Div:rt.divmul[0]=src?dst/src:0;rt.divmul[1]=src?dst%src:0;write=false;break;
            case Action::Mul32:case Action::Div32: {
                const uint64_t v=i.action==Action::Mul32?uint64_t(dst)*src:
                    src?((uint64_t(rt.divmul[1])<<32)|dst)/src:0;
                rt.divmul[0]=uint32_t(v);rt.divmul[1]=uint32_t(v>>32);write=false;break;
            }
            default:return Status::UnsupportedOpcode;
            }
            if(write) { s=rt.put(f,i.destination,dst,saved);if(s!=Status::Ok)return s; }
            continue;
        }
        switch(i.action) {
        case Action::End:--depth;break;case Action::Nop:case Action::ProcessData:break;
        case Action::SetPort:
            if(i.argument==0)rt.ioMode=i.extra?uint16_t(0x80u|i.extra):0;
            else rt.ioMode=i.argument;break;
        case Action::SetReg:rt.regBlock=uint16_t(i.extra);break;
        case Action::SetFb:s=rt.get(f,i.source,src,ignored);if(s!=Status::Ok)return s;rt.fbBase=src;break;
        case Action::SetData:
            if(i.extra==0)rt.dataBlock=0;
            else if(i.extra==255)rt.dataBlock=uint16_t(f.start);
            else {
                if(i.extra>=rt.p.dataCount)return Status::BadTable;
                rt.dataBlock=u16(rt.p.image,rt.p.data+4+i.extra*2);
            }
            break;
        case Action::Jump: {
            const bool yes=i.argument==0 || (i.argument==1 && rt.equal) || (i.argument==2 && !(rt.above||rt.equal)) ||
                (i.argument==3 && rt.above) || (i.argument==4 && !rt.above) || (i.argument==5 && (rt.above||rt.equal)) || (i.argument==6 && !rt.equal);
            if(yes)f.pc=f.start+i.extra;break;
        }
        case Action::Switch: {
            s=rt.get(f,i.source,src,ignored);if(s!=Status::Ok)return s;
            uint32_t cursor=i.cases;
            while(u16(rt.p.image,cursor)!=0x5a5a) {
                ++cursor;uint32_t val=0,target=0;
                if(!take(rt.p,cursor,i.next,immediateBytes((i.attribute>>3)&7),val) || !take(rt.p,cursor,i.next,2,target))return Status::Truncated;
                if(val==src) { f.pc=f.start+target;break; }
            }
            break;
        }
        case Action::Delay:
            src=i.extra*(i.argument?1000u:1u);
            if(src>rt.limits.delayMicroseconds-r.delayedMicroseconds)return Status::DelayLimit;
            r.delayedMicroseconds+=src;
            if(!rt.io.delayMicroseconds(rt.io.context,src))return Status::IoFailed;
            break;
        case Action::Call: {
            uint32_t base=0,end=0;s=table(rt.p,true,i.extra,base,end);
            if(s==Status::MissingTable)break;
            if(s!=Status::Ok)return s;
            const size_t nextBase=f.parameterBase+size_t(f.parameterShift)*4;
            if(nextBase>rt.parameterBytes)return Status::ParameterBounds;
            s=push(rt.p,uint8_t(i.extra),nextBase,w,depth,rt.limits);if(s!=Status::Ok)return s;break;
        }
        case Action::Post:case Action::Debug:case Action::Beep:
            if(rt.io.event)rt.io.event(rt.io.context,i.action==Action::Post?Event::PostCard:i.action==Action::Debug?Event::Debug:Event::Beep,uint8_t(i.extra));break;
        default:return Status::UnsupportedOpcode;
        }
    }
    return Status::Ok;
}
} // namespace
Status open(const void* image,size_t bytes,Program& out) {
    zero(out);
    if(!image || bytes<0x4a || bytes>kAddressSpace)return Status::InvalidArgument;
    const uint8_t* b=static_cast<const uint8_t*>(image);
    if(b[0]!=0x55 || b[1]!=0xaa)return Status::BadImage;
    const uint32_t header=u16(b,0x48);
    if(!range(header,34,bytes) || u16(b,header)<34 || !range(header,u16(b,header),bytes))return Status::BadImage;
    if(b[header+4]!='A' || b[header+5]!='T' || b[header+6]!='O' || b[header+7]!='M')return Status::BadImage;
    Program p={};p.image=b;p.imageBytes=bytes;p.commands=u16(b,header+30);p.data=u16(b,header+32);
    if(!p.commands || !p.data || !range(p.commands,4,bytes) || !range(p.data,4,bytes))return Status::BadTable;
    const uint16_t cs=u16(b,p.commands),ds=u16(b,p.data);
    if(cs<4 || ds<4 || (cs&1) || (ds&1) || !range(p.commands,cs,bytes) || !range(p.data,ds,bytes))return Status::BadTable;
    p.commandCount=uint16_t((cs-4)/2);p.dataCount=uint16_t((ds-4)/2);
    if(p.commandCount>256 || p.dataCount>256)return Status::BadTable;
    if(p.dataCount>23) {
        uint32_t start=0,end=0;Status s=table(p,false,23,start,end);
        if(s!=Status::Ok && s!=Status::MissingTable)return s;
        if(s==Status::Ok) {
            uint32_t pc=start+4;
            while(pc<end && b[pc]==1) {
                if(!range(pc,2,end))return Status::BadIndirectIo;
                const unsigned index=b[pc+1];pc+=2;const uint32_t method=pc;
                if(p.indirectStart[index])return Status::BadIndirectIo;
                bool terminated=false;
                while(pc<end) {
                    const unsigned op=b[pc];
                    if(op>=10 || op==1 || !range(pc,iioLengths[op],end))return Status::BadIndirectIo;
                    if(op>=4 && op<=8) {
                        const unsigned width=b[pc+1],source=b[pc+2],dest=op>=6?b[pc+3]:source;
                        if(width==0 || width>32 || dest>=32 || width>32-dest || (op>=6 && (source>=32 || width>32-source)))return Status::BadIndirectIo;
                    }
                    pc+=iioLengths[op];if(op==9) { terminated=true;break; }
                }
                if(!terminated)return Status::BadIndirectIo;
                p.indirectStart[index]=uint16_t(method);p.indirectEnd[index]=pc;
            }
        }
    }
    out=p;return Status::Ok;
}
Status analyze(const Program& program,uint8_t index,Workspace& workspace,Result& result) {
    zero(result);Program p={};Status s=open(program.image,program.imageBytes,p);
    if(s==Status::Ok)s=analyzeOpened(p,index,workspace,result);
    result.status=s;return s;
}
Status execute(const Program& program,uint8_t index,void* parameters,size_t parameterBytes,
               void* scratch,size_t scratchBytes,const Io& io,const Limits& limits,Workspace& workspace,Result& result) {
    zero(result);
    if((parameterBytes && !parameters) || (scratchBytes && !scratch) || !io.read || !io.write || !io.delayMicroseconds ||
       !limits.instructions || !limits.nesting || limits.nesting>kMaxDepth) { result.status=Status::InvalidArgument;return result.status; }
    Program p={};Status s=open(program.image,program.imageBytes,p);
    if(s==Status::Ok)s=analyzeOpened(p,index,workspace,result);
    if(s==Status::Ok) {
        Runtime runtime={p,io,limits,result,static_cast<uint8_t*>(parameters),parameterBytes,
            static_cast<uint8_t*>(scratch),scratchBytes,{0,0},0,0,0,0,0,0,false,false};
        s=run(runtime,index,workspace);
    }
    result.status=s;return s;
}
Status prepareAsicInit(const Program& program,bool fbReset,uint8_t (&parameters)[kAsicInitParameterBytes]) {
    zero(parameters);Program p={};Status s=open(program.image,program.imageBytes,p);if(s!=Status::Ok)return s;
    uint32_t start=0,end=0;s=table(p,true,0,start,end);if(s!=Status::Ok)return s;
    if(p.image[start+2]!=2 || p.image[start+3]<1)return Status::UnsupportedVersion;
    s=table(p,false,4,start,end);if(s!=Status::Ok)return s;
    if(p.image[start+2]!=3 || p.image[start+3]<1)return Status::UnsupportedVersion;
    if(!range(start,16,end))return Status::Truncated;
    put32(parameters,0,u32(p.image,start+8)&0xffffffu);
    put32(parameters,4,(u32(p.image,start+12)&0xffffffu)|(fbReset?0u:0x20000000u));
    return Status::Ok;
}
const char* statusName(Status s) {
    switch(s) {
    case Status::Ok:return "ok";case Status::InvalidArgument:return "invalid-argument";case Status::BadImage:return "bad-image";
    case Status::BadTable:return "bad-table";case Status::Truncated:return "truncated";case Status::UnsupportedVersion:return "unsupported-version";
    case Status::UnsupportedOpcode:return "unsupported-opcode";case Status::UnsupportedIo:return "unsupported-io";
    case Status::BadOperand:return "bad-operand";case Status::BadBranch:return "bad-branch";case Status::BadIndirectIo:return "bad-indirect-io";
    case Status::MissingTable:return "missing-table";case Status::ParameterBounds:return "parameter-bounds";
    case Status::WorkspaceBounds:return "workspace-bounds";case Status::ScratchBounds:return "scratch-bounds";
    case Status::InstructionLimit:return "instruction-limit";case Status::NestingLimit:return "nesting-limit";
    case Status::DelayLimit:return "delay-limit";case Status::IoFailed:return "io-failed";
    }
    return "unknown-status";
}
} }
