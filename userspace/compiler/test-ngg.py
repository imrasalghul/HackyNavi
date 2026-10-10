#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual SPIRV-Tools -> NIR/ACO NGG products and machine-format checks."""
import argparse
import copy
import ctypes as c
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT=Path(__file__).resolve().parents[2]; HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('verify_ngg',HERE/'verify-ngg.py')
decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args(); output=args.output.resolve();compiler=args.compiler.resolve()
    if output.exists(): parser.error('Evidence directory must be new')
    build=json.loads((compiler.parent/'report.json').read_text())
    if not build['success'] or build['artifact_sha256']!=sha(compiler): raise RuntimeError('Compiler build mismatch')
    for name,value in build['source_files'].items():
        if sha(ROOT/name)!=value: raise RuntimeError('Compiler source changed since build: '+name)
    output.mkdir(parents=True)
    assembler=ROOT/'.tools/userspace/build-spirv-tools/tools/spirv-as.exe'
    validator=ROOT/'.tools/userspace/build-spirv-tools/tools/spirv-val.exe'
    tools=json.loads((ROOT/'build/metal-explicit-tools-local/explicit-tools.json').read_text())['tools']
    for name in ('compiler','air_dis','spirv_val'):
        if sha(Path(tools[name]['path']))!=tools[name]['sha256']: raise RuntimeError('Explicit tools changed: '+name)
    report={'success':False,'gpu_executed':False,'metal_objects_created':False,'checks':0,
            'build_report_sha256':sha(compiler.parent/'report.json'),
            'tools':{name:{'path':str(p),'sha256':sha(p)} for name,p in [('assembler',assembler),('validator',validator),('ngg_compiler',compiler)]},
            'sources':{p.relative_to(ROOT).as_posix():sha(p) for p in [HERE/n for n in
                       ('ngg.c','hackynavi_gfx11_ngg.h','build-ngg.py','verify-ngg.py','test-ngg.py','fixtures/ngg/triangle.spvasm')]},
            'commands':[],'positive':[],'negative':[],'machine_rejections':[]}
    def check(condition,message):
        report['checks']+=1
        if not condition: raise AssertionError(message)
    def run(command,success=True):
        result=subprocess.run([str(x) for x in command],capture_output=True,text=True,timeout=30)
        report['commands'].append({'args':[str(x) for x in command],'returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
        check((result.returncode==0)==success,str(command)+result.stderr);return result
    template=(HERE/'fixtures/ngg/triangle.spvasm').read_text()
    def compile_text(name,text,error=None,entry='main'):
        directory=output/name;directory.mkdir();source=directory/'source.spvasm';source.write_text(text)
        spirv=directory/'shader.spv';product=directory/'product';product.mkdir()
        run([assembler,'--target-env','vulkan1.2',source,'-o',spirv]);run([validator,'--target-env','vulkan1.2',spirv])
        result=run([compiler,spirv,entry,product],error is None)
        if error is not None:
            check(error in result.stderr,'Precise rejection: '+name+result.stderr)
            check(not list(product.iterdir()),'Rejected shader published output')
            report['negative'].append({'name':name,'spirv_sha256':sha(spirv),'diagnostic':result.stderr.strip()});return
        code=(product/'shader.bin').read_bytes();metadata=json.loads((product/'shader.json').read_text())
        decoded=decoder.verify(code,metadata);(directory/'disassembly.json').write_text(json.dumps(decoded,indent=2)+'\n')
        check('not supported in the current configuration' in (product/'aco-diagnostics.txt').read_text(),'ACO fallback explicitly identified')
        check('HW (NEXT_GEN_GEOMETRY_SHADER)' in (product/'aco.txt').read_text(),'Actual ACO NGG hardware stage')
        report['positive'].append({'name':name,'spirv_sha256':sha(spirv),'code_sha256':sha(product/'shader.bin'),'metadata':metadata,'decoded':decoded})
        return code,metadata
    try:
        invalid=output/'invalid.spv';invalid.write_bytes(b'not SPIR-V');run([validator,'--target-env','vulkan1.2',invalid],False)
        baseline=compile_text('triangle',template)
        for name,minus,one in [('half',-0.5,0.5),('literal',-0.25,0.25),('reverse',1,-1)]:
            source=template.replace('%minus = OpConstant %float -1',f'%minus = OpConstant %float {minus}').replace('%one = OpConstant %float 1',f'%one = OpConstant %float {one}')
            code,_=compile_text(name,source);check(code!=baseline[0],'Actual source position changes machine code')
        compile_text('location_output',template.replace('BuiltIn Position','Location 0'),'Position output')
        compile_text('attribute_input',template.replace('BuiltIn VertexIndex','Location 0'),'vertex attribute inputs')
        compile_text('instance_input',template.replace('BuiltIn VertexIndex','BuiltIn InstanceIndex'),'VertexIndex system input')
        compile_text('no_output',template.replace('OpStore %position %selected',''),'missing full Position store')
        compile_text('specialization',template.replace('%i0 = OpConstant','%i0 = OpSpecConstant'),'specialization')
        compile_text('conversion',template.replace('OpStore %position %selected','%value = OpConvertSToF %float %id\n%sum = OpCompositeConstruct %vec4 %value %zero %zero %one\nOpStore %position %sum'),'position selection')
        compile_text('wrong_entry',template,'entrypoint',entry='missing')
        # Actual pinned SDL vertex AIR -> real SPIR-V, preserving its inputs.
        lib=c.CDLL(tools['compiler']['path']);ptr=c.c_void_p;size=c.c_size_t;u32=c.c_uint32
        class Tools(c.Structure):
            _fields_=[('struct_size',u32),('version',u32),('air_dis_path',ptr),('spirv_val_path',ptr)]
        lib.hn_shader_compile_with_tools.argtypes=[ptr,ptr,size,u32,u32,c.c_char_p,u32,u32,u32,u32,u32]
        lib.hn_shader_compile_with_tools.restype=ptr
        for name,arguments,result in [('error',[ptr],c.c_char_p),('words',[ptr,c.POINTER(size)],ptr),('spirv_entry_point',[ptr],c.c_char_p),('reflection',[ptr],c.c_char_p)]:
            fn=getattr(lib,'hn_shader_result_'+name);fn.argtypes=arguments;fn.restype=result
        lib.hn_shader_result_free.argtypes=[ptr];lib.hn_shader_result_free.restype=None
        air=c.create_string_buffer(os.fsencode(tools['air_dis']['path']));val=c.create_string_buffer(os.fsencode(tools['spirv_val']['path']))
        config=Tools(c.sizeof(Tools),1,c.cast(air,ptr),c.cast(val,ptr));fixture=ROOT/'userspace/metal/tests/fixtures/metallib/sdl-2.30.11-macos.metallib'
        raw=fixture.read_bytes();memory=c.create_string_buffer(raw)
        shader=lib.hn_shader_compile_with_tools(c.byref(config),memory,len(raw),2,1,b'SDL_Solid_vertex',1,1,1,0,1)
        try:
            check(bool(shader) and lib.hn_shader_result_error(shader)==b'','Actual SDL vertex AIR translates')
            count=size();words=lib.hn_shader_result_words(shader,c.byref(count));check(bool(words) and count.value>5,'Actual vertex SPIR-V present')
            entry=lib.hn_shader_result_spirv_entry_point(shader).decode();directory=output/'sdl_vertex';directory.mkdir()
            spirv=directory/'shader.spv';spirv.write_bytes(c.string_at(words,count.value*4));(directory/'reflection.json').write_bytes(lib.hn_shader_result_reflection(shader))
            run([validator,'--target-env','vulkan1.2',spirv]);product=directory/'product';product.mkdir()
            result=run([compiler,spirv,entry,product],False)
            check('vertex attribute inputs are unsupported' in result.stderr,'Actual SDL vertex input rejected precisely')
            check(not list(product.iterdir()),'SDL rejection publishes no replacement vertex program')
            report['negative'].append({'name':'actual SDL_Solid_vertex','fixture_sha256':sha(fixture),'spirv_sha256':sha(spirv),'diagnostic':result.stderr.strip()});report['sdl_tools']=tools
        finally:
            if shader: lib.hn_shader_result_free(shader)
        code,metadata=baseline
        def rejects(name,candidate=code,meta=metadata):
            try: decoder.verify(candidate,meta)
            except ValueError as error:
                report['machine_rejections'].append({'name':name,'diagnostic':str(error)});check(True,name);return
            raise AssertionError('Accepted malformed product: '+name)
        for count in range(len(code)):rejects('truncation '+str(count),code[:count])
        proof=decoder.verify(code,metadata);pos=proof['position_export_offset'];prim=proof['primitive_export_offset']
        for offset,value,name in [(pos,0xf80000cf,'position missing DONE'),(pos,0xf80008ce,'position missing component'),
                                  (pos+4,0xffffffff,'undefined position source'),(prim+4,0x80808001,'changed packed primitive'),
                                  (pos+8,0xbf89ffff,'missing export completion wait'),(20,0xbfa5ffff,'backward branch'),
                                  (20,0xbfa5fffe,'out of code branch'),(metadata['executable_bytes']-4,0xbf800000,'missing end'),
                                  (metadata['executable_bytes'],0,'invalid padding')]:
            changed=bytearray(code);struct.pack_into('<I',changed,offset,value);rejects(name,bytes(changed))
        for key,value in [('wave_size',64),('lds_bytes',1),('attribute_ring_writes',1),('wgp_mode',True),
                          ('primgen_passthru_no_msg',False),('base_vertex_sgpr',0),('vertex_id_vgpr',0),
                          ('spi_shader_pgm_rsrc1_gs',0),('spi_shader_pgm_rsrc2_gs',0),('vgt_shader_stages_en',0),
                          ('spi_shader_pos_format',0),('resource_count',True)]:
            changed=copy.deepcopy(metadata);changed[key]=value;rejects('metadata '+key,meta=changed)
        report['success']=True
    finally:
        (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':True,'checks':report['checks'],'positive':len(report['positive']),'negative':len(report['negative']),
                      'machine_rejections':len(report['machine_rejections']),'report_sha256':sha(output/'report.json')}))


if __name__=='__main__':main()
