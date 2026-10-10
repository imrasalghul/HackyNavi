#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile actual captured Apple vertex/fragment AIR and metallib through ACO.

Uses real owned compiler C ABI, SPIRV-Tools, and the separate frozen NGG/PS
adapters. No substitute input, Metal object, GPU dispatch, or drawing occurs.
"""
import argparse
import ctypes as c
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT=Path(__file__).resolve().parents[2]; HERE=Path(__file__).resolve().parent


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value);return value


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture',type=Path,required=True)
    parser.add_argument('--ngg-compiler',type=Path,required=True)
    parser.add_argument('--fragment-compiler',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();capture=args.capture.resolve();output=args.output.resolve()
    if output.exists():parser.error('Evidence directory must be new')
    prior=json.loads((capture/'report.json').read_text())
    if not prior['success'] or not prior['stage_removed'] or not prior['export_hashes_verified']:
        parser.error('Requires a completed verified Apple capture')
    for name,row in prior['artifacts'].items():
        if name not in ('graphics.metal','graphics.air','graphics.metallib') or sha(capture/name)!=row['sha256']:
            parser.error('Captured Apple fixture changed')
    if (capture/'graphics.metal').read_bytes()!=(HERE/'fixtures/apple-graphics/triangle.metal').read_bytes():
        parser.error('Capture does not use the authored source')
    compilers={'vertex':args.ngg_compiler.resolve(),'fragment':args.fragment_compiler.resolve()}
    compiler_reports={}
    for stage,path in compilers.items():
        data=json.loads((path.parent/'report.json').read_text())
        if not data['success'] or sha(path)!=data['artifact_sha256']:parser.error('Stage adapter build mismatch')
        for name,value in data['source_files'].items():
            if sha(ROOT/name)!=value:parser.error('Stage adapter source changed since build')
        compiler_reports[stage]=sha(path.parent/'report.json')
    tools=json.loads((ROOT/'build/metal-explicit-tools-local/explicit-tools.json').read_text())['tools']
    for name in ('compiler','air_dis','spirv_val'):
        if sha(Path(tools[name]['path']))!=tools[name]['sha256']:parser.error('Frozen compiler/tool changed')
    output.mkdir(parents=True)
    report={'success':False,'checks':0,'capture_report_sha256':sha(capture/'report.json'),
            'adapter_build_reports':compiler_reports,'tools':tools,'products':[],'commands':[],
            'source_sha256':sha(HERE/'fixtures/apple-graphics/triangle.metal'),
            'runner_sha256':sha(Path(__file__)),'verifiers':{name:sha(HERE/name) for name in ('verify-ngg.py','verify-fragment.py')},
            'gpu_executed':False,'metal_objects_created':False,'draw_submitted':False}
    def check(condition,message):
        report['checks']+=1
        if not condition:raise AssertionError(message)
    def run(argv):
        result=subprocess.run([str(x) for x in argv],capture_output=True,text=True,timeout=30)
        report['commands'].append({'args':[str(x) for x in argv],'returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr})
        check(result.returncode==0,result.stderr);return result
    ngg=module('verify_apple_ngg',HERE/'verify-ngg.py');fragment=module('verify_apple_fragment',HERE/'verify-fragment.py')
    lib=c.CDLL(tools['compiler']['path']);ptr=c.c_void_p;size=c.c_size_t;u32=c.c_uint32
    class Tools(c.Structure):
        _fields_=[('struct_size',u32),('version',u32),('air_dis_path',ptr),('spirv_val_path',ptr)]
    lib.hn_shader_compile_with_tools.argtypes=[ptr,ptr,size,u32,u32,c.c_char_p,u32,u32,u32,u32,u32]
    lib.hn_shader_compile_with_tools.restype=ptr
    for name,arguments,result in [('error',[ptr],c.c_char_p),('words',[ptr,c.POINTER(size)],ptr),('spirv_entry_point',[ptr],c.c_char_p),('reflection',[ptr],c.c_char_p)]:
        fn=getattr(lib,'hn_shader_result_'+name);fn.argtypes=arguments;fn.restype=result
    lib.hn_shader_result_free.argtypes=[ptr];lib.hn_shader_result_free.restype=None
    air=c.create_string_buffer(os.fsencode(tools['air_dis']['path']));val=c.create_string_buffer(os.fsencode(tools['spirv_val']['path']))
    config=Tools(c.sizeof(Tools),1,c.cast(air,ptr),c.cast(val,ptr));identities={}
    try:
        for extension,fmt in [('air',0),('metallib',2)]:
            raw=(capture/('graphics.'+extension)).read_bytes();memory=c.create_string_buffer(raw)
            for stage,stage_id,entry in [('vertex',1,'hn_triangle_vertex'),('fragment',2,'hn_constant_fragment')]:
                directory=output/(extension+'-'+stage);directory.mkdir();product=directory/'product';product.mkdir()
                shader=lib.hn_shader_compile_with_tools(c.byref(config),memory,len(raw),fmt,stage_id,entry.encode(),1,1,1,0,1)
                try:
                    check(bool(shader),'Actual owned shader result exists')
                    error=lib.hn_shader_result_error(shader).decode()
                    check(not error,'Actual Apple shader translation: '+error)
                    count=size();words=lib.hn_shader_result_words(shader,c.byref(count))
                    check(bool(words) and 5<count.value<=16*1024*1024,'Real bounded SPIR-V result')
                    spirv=directory/'shader.spv';spirv.write_bytes(c.string_at(words,count.value*4))
                    reflection_raw=lib.hn_shader_result_reflection(shader);reflection=json.loads(reflection_raw)
                    (directory/'reflection.json').write_bytes(reflection_raw)
                    actual_entry=lib.hn_shader_result_spirv_entry_point(shader).decode()
                    check(reflection['entry_point']==entry and reflection['stage'].lower()==stage,'Actual AIR stage/entry remains exact')
                    check(reflection['bindings']==[],'Actual shader has no resource bindings')
                    if stage=='vertex':
                        check(reflection['vertex_attributes']==[] and reflection['varyings']==[],'Position-only vertex interface')
                        check(reflection['vertex_builtins']['uses_vertex_index'] and reflection['vertex_builtins']['writes_position'],
                              'Actual VertexIndex and Position metadata')
                    run([tools['spirv_val']['path'],'--target-env','vulkan1.2',spirv])
                    run([compilers[stage],spirv,actual_entry,product])
                    code=(product/'shader.bin').read_bytes();metadata=json.loads((product/'shader.json').read_text())
                    decoded=(ngg if stage=='vertex' else fragment).verify(code,metadata)
                    (directory/'disassembly.json').write_text(json.dumps(decoded,indent=2)+'\n')
                    if stage=='fragment':
                        expected=[struct.unpack('<I',struct.pack('<f',value))[0] for value in (.25,.5,.75,1)]
                        check(metadata['color_bits']==expected,'Actual Apple constant color survives translation and ACO')
                    identity=(code,metadata)
                    if extension=='air':identities[stage]=identity
                    else:check(identity==identities[stage],'Standalone AIR and owned metallib produce the same actual '+stage+' code/metadata')
                    report['products'].append({'input_format':extension,'stage':stage,'entry':entry,'spirv_entry':actual_entry,
                                               'input_sha256':sha(capture/('graphics.'+extension)),'spirv_sha256':sha(spirv),
                                               'reflection_sha256':sha(directory/'reflection.json'),'code_sha256':sha(product/'shader.bin'),
                                               'metadata':metadata,'decoded':decoded})
                finally:
                    if shader:lib.hn_shader_result_free(shader)
        report['success']=True
    finally:
        (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'success':True,'checks':report['checks'],'products':len(report['products']),'report_sha256':sha(output/'report.json')}))


if __name__=='__main__':main()
