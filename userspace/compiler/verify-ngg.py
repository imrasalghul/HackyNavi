#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode actual words in the bounded NGG fixture profile, without execution.

This is an offline machine-format/control-flow verifier, not a kernel admission
boundary, an ISA execution model or a rendering backend. Encodings come from
pinned Mesa aco_assembler.cpp (SOP/VOP/EXP emitters) and aco_opcodes.py.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def verify(code, metadata):
    expected = {'abi_version':1, 'profile':'gfx1100-position-only-ngg-v1', 'target':'gfx1100',
                'mesa_commit':'35b085c4c067f198ce0b6437d94678e47b9c645f', 'stage':'vertex',
                'hardware_stage':'next_gen_geometry', 'code_alignment':256, 'wave_size':32,
                'input_vgprs':6, 'wgp_mode':False, 'spi_shader_pgm_rsrc2_gs':2,
                'ngg_passthrough':True, 'primgen_passthru_no_msg':True, 'early_primitive_export':False,
                'vgt_shader_stages_en':(2<<3)|(1<<13)|(2<<15)|(1<<22)|(1<<25)|(1<<26), 'spi_shader_pos_format':4,
                'max_workgroup_size':256, 'vertices_per_primitive':3, 'position_exports':1,
                'parameter_exports':0, 'resource_count':0, 'user_sgprs':1, 'base_vertex_sgpr':8,
                'merged_wave_info_sgpr':3, 'packed_primitive_vgpr':0, 'vertex_id_vgpr':5,
                'gs_vgpr_comp_cnt':0, 'es_vgpr_comp_cnt':0, 'scratch_bytes_per_wave':0,
                'lds_bytes':0, 'attribute_ring_writes':0, 'constants_bytes':0, 'relocations':[],
                'gpu_executed':False, 'draw_state_complete':False}
    for key, value in expected.items():
        if type(metadata.get(key)) is not type(value) or metadata[key] != value:
            raise ValueError('Incorrect NGG metadata: '+key)
    for key in ('code_bytes','executable_bytes','num_sgprs','num_vgprs','float_mode','spi_shader_pgm_rsrc1_gs'):
        if type(metadata.get(key)) is not int:
            raise ValueError('Noninteger NGG metadata: '+key)
    if type(metadata.get('mem_ordered')) is not bool:
        raise ValueError('Invalid memory ordering metadata')
    if not code or len(code)>1024*1024 or len(code)%4 or metadata['code_bytes']!=len(code):
        raise ValueError('Invalid code extent')
    executable=metadata['executable_bytes']; vgprs=metadata['num_vgprs']
    if not 0<executable<=len(code) or executable%4 or not 6<=vgprs<=256 or not 9<=metadata['num_sgprs']<=128:
        raise ValueError('Invalid executable/allocation extent')
    if not 0<=metadata['float_mode']<=255:
        raise ValueError('Invalid float mode')
    rsrc1=(vgprs-1)//8 | 1<<21 | metadata['float_mode']<<12 | int(metadata['mem_ordered'])<<30
    if metadata['spi_shader_pgm_rsrc1_gs']!=rsrc1:
        raise ValueError('GS resource register disagrees with allocation')
    words=list(struct.unpack('<'+str(len(code)//4)+'I',code)); end=executable//4
    if words[end:] != [0xbf9f0000]*5:
        raise ValueError('Noncanonical ACO five-word padding')
    scalar={3,8}; vector={0,5}; listing=[]; branches=[]; exports=[]; waits=[]
    at=0

    def source(value,literal=None):
        if value<128:
            if value not in scalar: raise ValueError('Undefined SGPR source')
            return 's'+str(value)
        if 128<=value<=208 or 240<=value<=248: return 'inline:'+str(value)
        if value==255:
            if literal is None: raise ValueError('Missing literal source')
            return '0x%08x'%literal
        if 256<=value<=511:
            value-=256
            if value not in vector or value>=vgprs: raise ValueError('Undefined VGPR source')
            return 'v'+str(value)
        raise ValueError('Unsupported source selector')

    def write_v(value):
        if value>=vgprs or value==0: raise ValueError('Invalid destination or clobbered packed primitive')
        vector.add(value)

    def write_s(value,count=1):
        if value+count>metadata['num_sgprs'] and value not in (106,126):
            raise ValueError('SGPR destination exceeds allocation')
        scalar.update(range(value,value+count))

    def pair(position):
        if position+1>=end: raise ValueError('Truncated two-word instruction')
        return words[position+1]

    def literal_for(values,width):
        if 255 not in values: return None,width
        if at+width>=end: raise ValueError('Truncated literal')
        return words[at+width],width+1

    while at<end:
        word=words[at]; width=1; item={'offset':at*4}
        if word&0xff800000==0xbf800000: # SOPP
            op=(word>>16)&0x7f; imm=word&0xffff
            names={0x35:'s_setprio',0x25:'s_cbranch_execz',0x07:'s_delay_alu',0x09:'s_waitcnt',0x30:'s_endpgm'}
            if op not in names: raise ValueError('Unsupported SOPP opcode')
            item.update(op=names[op],immediate=imm)
            if op==0x35 and (at!=0 or imm!=3): raise ValueError('Unexpected priority control')
            if op==0x25:
                if imm&0x8000 or not listing or listing[-1].get('destination')!='s126':
                    raise ValueError('Unexpected NGG branch/mask structure')
                target=at+1+imm
                if target>=end: raise ValueError('Branch leaves executable code')
                item['target']=target*4;branches.append((at,target))
            if op==0x09:
                if imm!=0xfff0: raise ValueError('Unexpected export wait')
                waits.append(at)
            if op==0x30 and (imm or at+1!=end): raise ValueError('Nonterminal program end')
        elif word&0xff800000==0xbe800000: # SOP1
            if (word>>8)&0xff: raise ValueError('Unsupported SOP1 opcode')
            dst=(word>>16)&127; src=word&255
            literal,width=literal_for([src],width)
            item.update(op='s_mov_b32',destination='s'+str(dst),sources=[source(src,literal)])
            write_s(dst)
        elif word&0xc0000000==0x80000000: # SOP2
            op=(word>>23)&127; dst=(word>>16)&127; src=[word&255,(word>>8)&255]
            names={8:'s_lshl_b32',0x2b:'s_bfm_b64',0x26:'s_bfe_u32'}
            if op not in names: raise ValueError('Unsupported SOP2 opcode')
            literal,width=literal_for(src,width)
            item.update(op=names[op],destination='s'+str(dst),sources=[source(x,literal) for x in src])
            write_s(dst,2 if op==0x2b else 1)
        elif word&0xfe000000==0x7e000000: # VOP1
            if (word>>9)&255 != 1: raise ValueError('Unsupported VOP1 opcode')
            dst=(word>>17)&255; src=word&511; literal,width=literal_for([src],width)
            item.update(op='v_mov_b32',destination='v'+str(dst),sources=[source(src,literal)])
            write_v(dst)
        elif word&0xfe000000==0x7c000000: # VOPC, implicit VCC_LO
            op=(word>>17)&255
            if op not in (0x42,0x45): raise ValueError('Unsupported VOPC comparison')
            src=[word&511,256+((word>>9)&255)]; literal,width=literal_for(src,width)
            item.update(op='v_cmp_eq_i32' if op==0x42 else 'v_cmp_lg_i32',destination='s106',sources=[source(x,literal) for x in src])
            write_s(106)
        elif word&0xfc000000==0xd4000000: # VOP3
            second=pair(at); width=2; op=(word>>16)&0x3ff; dst=word&255
            if word&0xff00 or second&0x98000000 or (op!=0x101 and second&0x60000000):
                raise ValueError('Unexpected VOP3 modifiers')
            if op not in (0x42,0x45,0x101,0x181): raise ValueError('Unsupported VOP3 opcode')
            src=[(second>>(9*i))&511 for i in range(3)]; literal,width=literal_for(src,width)
            if op in (0x42,0x45) and src[2]!=128: raise ValueError('Noncanonical unused comparison source')
            if op==0x181 and src[1:]!=[128,128]: raise ValueError('Noncanonical unused MOV source')
            count=3 if op==0x101 else 1 if op==0x181 else 2
            values=[source(x,literal) for x in src[:count]]
            for i in range(min(count,2)):
                if second&(1<<(29+i)): values[i]='neg('+values[i]+')'
            item.update(op={0x42:'v_cmp_eq_i32',0x45:'v_cmp_lg_i32',0x101:'v_cndmask_b32',0x181:'v_mov_b32'}[op],
                        destination=('s' if op in (0x42,0x45) else 'v')+str(dst),sources=values)
            if op in (0x42,0x45): write_s(dst)
            else: write_v(dst)
        elif word&0xfc000000==0xc8000000: # VOPD, both operands read before destinations
            second=pair(at); width=2; ops=[(word>>22)&15,(word>>17)&31]
            if any(op not in (8,9) for op in ops): raise ValueError('Unsupported VOPD opcode')
            dx=(second>>24)&255; dy=((second>>17)&127)*2+1-(dx&1)
            sources=[[word&511,256+((word>>9)&255)], [second&511,256+((second>>9)&255)]]
            literal,width=literal_for([s[0] for s in sources],width)
            pairs=[]
            for op,dst,src in zip(ops,[dx,dy],sources):
                if op==8 and src[1]!=256: raise ValueError('Noncanonical VOPD MOV unused source')
                values=[source(src[0],literal)]
                if op==9: values.extend([source(src[1]),source(106)])
                pairs.append({'op':'v_dual_mov_b32' if op==8 else 'v_dual_cndmask_b32','destination':'v'+str(dst),'sources':values})
            item.update(op='VOPD',paired=pairs);write_v(dx);write_v(dy)
        elif word&0x80000000==0: # VOP2
            op=word>>25; dst=(word>>17)&255; src=[word&511,256+((word>>9)&255)]
            if op not in (0x25,1): raise ValueError('Unsupported VOP2 opcode')
            literal,width=literal_for(src,width)
            values=[source(x,literal) for x in src]
            if op==1: values.append(source(106))
            item.update(op='v_add_u32' if op==0x25 else 'v_cndmask_b32',destination='v'+str(dst),sources=values)
            write_v(dst)
        elif word in (0xf80008cf,0xf8000941): # actual full POS0 or single-channel PRIM, DONE
            second=pair(at); width=2; ispos=word==0xf80008cf
            src=[(second>>(8*i))&255 for i in range(4)]
            if not ispos and src!=[0,128,128,128]: raise ValueError('Primitive export does not preserve packed v0')
            item.update(op='exp',target='pos0' if ispos else 'prim',done=True,
                        sources=[source(256+x) for x in (src if ispos else src[:1])])
            exports.append((at,ispos))
        else:
            raise ValueError('Unsupported instruction word 0x%08x at %u'%(word,at*4))
        item['words']=['%08x'%x for x in words[at:at+width]]
        listing.append(item);at+=width
    if [kind for _,kind in exports]!=[True,False] or len(branches)!=2 or len(waits)!=1:
        raise ValueError('Unexpected NGG export/control-flow counts')
    pos,prim=exports[0][0],exports[1][0]
    if not (branches[0][0]<pos and branches[0][1]==pos+2==waits[0] and
            waits[0]<branches[1][0]<prim and branches[1][1]==prim+2==end-1):
        raise ValueError('NGG guards/export wait targets disagree')
    if listing[-1]['op']!='s_endpgm': raise ValueError('Missing terminal program end')
    return {'success':True,'code_sha256':hashlib.sha256(code).hexdigest(),'instructions':listing,
            'position_export_offset':pos*4,'primitive_export_offset':prim*4,
            'memory_instructions':0,'executable_bytes':executable,'gpu_executed':False,
            'scope':'Offline format/defined-register/export-guard checks; no instruction or GPU execution.'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('product',type=Path);parser.add_argument('report',type=Path)
    args=parser.parse_args()
    if args.report.exists(): parser.error('Report must be new')
    result=verify((args.product/'shader.bin').read_bytes(),json.loads((args.product/'shader.json').read_text()))
    args.report.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'success':True,'instructions':len(result['instructions']),'sha256':result['code_sha256']}))


if __name__=='__main__': main()
