#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual AIR/IR/metallib compilation and ACO with explicit tools; no GPU.

Parent starts an isolated child with deliberately unusable environment tool
overrides/PATH before any threads start. No fake executables are generated.
"""
import concurrent.futures
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

if '--child' not in sys.argv:
    env = os.environ.copy()
    env.update(METAL2VULKAN_LLVM_DIS='missing-environment-air-dis',
               METAL2VULKAN_SPIRV_VAL='missing-environment-spirv-val',
               PATH='missing-environment-search-path')
    raise SystemExit(subprocess.run([sys.executable, str(Path(__file__).resolve()),
                                     *sys.argv[1:], '--child'], env=env, timeout=170).returncode)

binary, dis, val, aco, output = [Path(p).resolve() for p in sys.argv[1:6]]
if output.exists():
    raise RuntimeError('Use a fresh evidence path')
lib = c.CDLL(str(binary))
p, n, u = c.c_void_p, c.c_size_t, c.c_uint32
class Tools(c.Structure):
    _fields_ = [('struct_size', u), ('version', u), ('air_dis_path', p), ('spirv_val_path', p)]
class Info(c.Structure):
    _fields_ = [(name, u) for name in ('variants','regions','resources','table','push','set','stride','gfx')]
class Variant(c.Structure):
    _fields_ = [('local', u * 3)] + [(name, u) for name in ('code','executable','alignment')] + [('rsrc',u*3)] + [
        (name,u) for name in ('dispatch','sgprs','vgprs','bindings','reads','writes','table','push','wave')]
triplet = u * 3
shader_args = [p,n,u,u,c.c_char_p,u,u,u,u,u]
for name,args,result in [
    ('hn_shader_compile',shader_args,p), ('hn_shader_compile_with_tools',[p,*shader_args],p),
    ('hn_shader_result_error',[p],c.c_char_p), ('hn_shader_result_reflection',[p],c.c_char_p),
    ('hn_shader_result_words',[p,c.POINTER(n)],p), ('hn_shader_result_free',[p],None),
    ('hn_pipeline_build',[p,u,p,p,c.c_char_p],p), ('hn_pipeline_error',[p],c.c_char_p),
    ('hn_pipeline_info',[p,p,n],c.c_int), ('hn_pipeline_variant',[p,u,p,n],c.c_int),
    ('hn_pipeline_blob_size',[p,u,u],n), ('hn_pipeline_copy_blob',[p,u,u,p,n],c.c_int),
    ('hn_pipeline_free',[p],None)]:
    fn=getattr(lib,name);fn.argtypes=args;fn.restype=result
checks=0
report={'success':False,'gpu_executed':False,'metal_objects_created':False,'checks':0,
        'tools':{name:{'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
                 for name,path in [('compiler',binary),('air_dis',dis),('spirv_val',val),('aco',aco)]},
        'environment_tool_overrides_invalid':True,'records':[]}
def check(value,message):
    global checks
    checks+=1
    if not value:raise AssertionError(message)
def save():
    report['checks']=checks;output.write_text(json.dumps(report,indent=2)+'\n')
def tools(air=dis,validator=val):
    a=c.create_string_buffer(os.fsencode(air));v=c.create_string_buffer(os.fsencode(validator))
    return Tools(c.sizeof(Tools),1,c.cast(a,p),c.cast(v,p)),a,v
def compile_(raw,fmt,name,descriptor):
    buf=c.create_string_buffer(raw)
    return lib.hn_shader_compile_with_tools(c.byref(descriptor) if descriptor is not None else None,
        buf,len(raw),fmt,0,name,1,1,1,1,0),buf
def shader_error(shader):
    check(bool(shader),'owned error result exists')
    error=lib.hn_shader_result_error(shader);check(bool(error),'invalid tools/input fail explicitly')
    count=n(77);words=lib.hn_shader_result_words(shader,c.byref(count))
    check(not words and count.value==0 and not lib.hn_shader_result_reflection(shader),'failure publishes no code/reflection')
    return error.decode()
def shader_bytes(shader):
    check(bool(shader) and lib.hn_shader_result_error(shader)==b'','actual explicit compile succeeds')
    count=n();words=lib.hn_shader_result_words(shader,c.byref(count))
    check(bool(words) and count.value>=5,'actual SPIR-V words exist')
    return c.string_at(words,count.value*4)

try:
    check(c.sizeof(Tools)==24 and c.sizeof(Info)==32 and c.sizeof(Variant)==72,'actual x86_64 C layouts')
    check(os.environ['PATH']=='missing-environment-search-path','no executable PATH available')
    fixtures=Path(__file__).parent/'fixtures'
    cases=[('AIR',fixtures/'apple/add_one.air',0,b'add_one'),
           ('LLVM',fixtures/'affine_xor.ll',1,b'affine_xor'),
           ('metallib',fixtures/'metallib/apple-32023-macos15-add-one.metallib',2,b'add_one')]
    baseline={}
    for label,path,fmt,name in cases:
        raw=path.read_bytes();descriptor,a,v=tools();shader,buf=compile_(raw,fmt,name,descriptor)
        try:
            spirv=shader_bytes(shader);baseline[label]=hashlib.sha256(spirv).hexdigest()
            # Destroy all caller-owned sources/path strings and the descriptor
            # before actual later validation/ACO. Successful output must own it.
            c.memset(buf,0xff,len(raw));c.memset(a,0,len(a));c.memset(v,0,len(v));c.memset(c.byref(descriptor),0,c.sizeof(descriptor))
            product=lib.hn_pipeline_build(shader,1,triplet(4099,1,1),triplet(64,1,1),os.fsencode(aco))
            try:
                check(bool(product) and lib.hn_pipeline_error(product)==b'','retained explicit validator produces actual ACO product')
                info=Info();check(lib.hn_pipeline_info(product,c.byref(info),c.sizeof(info))==1,'actual product metadata')
                check((info.variants,info.regions,info.resources,info.gfx)==(2,2,2,1100),'full and tail actual variants')
                records=[]
                for index,local in enumerate((64,3)):
                    variant=Variant();check(lib.hn_pipeline_variant(product,index,c.byref(variant),c.sizeof(variant))==1,'actual variant metadata')
                    check(list(variant.local)==[local,1,1] and variant.wave==32,'real local specialization')
                    size=lib.hn_pipeline_blob_size(product,0,index);code=c.create_string_buffer(size)
                    check(size==variant.code and size>0 and lib.hn_pipeline_copy_blob(product,0,index,code,size)==1,'owned real machine code')
                    records.append({'local':local,'code_sha256':hashlib.sha256(code.raw).hexdigest()})
                check(records[0]['code_sha256']!=records[1]['code_sha256'],'tail has actual different code')
                report['records'].append({'label':label,'fixture_sha256':hashlib.sha256(raw).hexdigest(),
                                          'spirv_sha256':baseline[label],'variants':records})
            finally:
                lib.hn_pipeline_free(product)
        finally:
            lib.hn_shader_result_free(shader)
    raw=cases[0][1].read_bytes()
    # The legacy entry still consults its configured environment, proving the
    # explicit success above did not secretly rewrite that shared environment.
    memory=c.create_string_buffer(raw);legacy=lib.hn_shader_compile(memory,len(raw),0,0,b'add_one',1,1,1,1,0)
    try:report['legacy_invalid_environment_error']=shader_error(legacy)
    finally:lib.hn_shader_result_free(legacy)
    invalid=[]
    for label,air,validator in [('relative','relative-tool',val),('missing',dis.parent/'missing-tool',val),
                                ('directory',dis.parent,val),('wrong-validator',dis,dis),('wrong-disassembler',val,val)]:
        descriptor,a,v=tools(air,validator);invalid.append((label,descriptor,a,v))
    for field,value in [('struct_size',0),('struct_size',23),('struct_size',25),('version',0),('version',2),
                        ('air_dis_path',None),('spirv_val_path',None)]:
        descriptor,a,v=tools();setattr(descriptor,field,value);invalid.append((field+str(value),descriptor,a,v))
    for value in (b'',b'x'*4097,b'\xff'):
        descriptor,a,v=tools();a=c.create_string_buffer(value);descriptor.air_dis_path=c.cast(a,p)
        invalid.append(('bad-path-bytes',descriptor,a,v))
    invalid.append(('null',None,None,None))
    for label,descriptor,a,v in invalid:
        shader,_=compile_(raw,0,b'add_one',descriptor)
        try:report.setdefault('expected_errors',[]).append({'label':label,'error':shader_error(shader)})
        finally:lib.hn_shader_result_free(shader)
    # Calls overlap on separate host threads, all using the same loaded actual
    # compiler. Different per-call contexts must not override one another.
    def concurrent_case(index):
        label,path,fmt,name=cases[index%3];descriptor,a,v=tools(validator=dis if index==3 else val)
        shader,_=compile_(path.read_bytes(),fmt,name,descriptor)
        try:
            error=lib.hn_shader_result_error(shader)
            count=n();words=lib.hn_shader_result_words(shader,c.byref(count))
            return index,error.decode() if error else '',hashlib.sha256(c.string_at(words,count.value*4)).hexdigest() if words else None
        finally:lib.hn_shader_result_free(shader)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        results=list(executor.map(concurrent_case,range(4)))
    for index,error,digest in results:
        check(bool(error) and digest is None if index==3 else not error and digest==baseline[cases[index%3][0]],'concurrent contexts remain independent')
    report['concurrent_results']=[{'case':i,'error':e,'spirv_sha256':h} for i,e,h in results]
    check(os.environ['METAL2VULKAN_LLVM_DIS']=='missing-environment-air-dis' and
          os.environ['METAL2VULKAN_SPIRV_VAL']=='missing-environment-spirv-val','compiler leaves process environment untouched')
    report['success']=True
finally:
    save()
print(f'explicit tools: {checks} checks passed; real translator/validator/ACO, no GPU')
