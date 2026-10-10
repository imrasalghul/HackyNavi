#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real translator/planner/ACO orchestration, copied owned products; no GPU."""
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

library=c.CDLL(str(Path(sys.argv[1]).resolve()))
compiler=Path(sys.argv[2]).resolve()
handle=c.c_void_p;u32=c.c_uint32;triplet=u32*3
class Dispatch(c.Structure):
    _fields_=[(n,triplet) for n in ('local','groups','thread_base','group_base','spec_ids')]+[
        ('push_offset',u32),('push_size',u32),('push',u32*12)]
class Info(c.Structure):
    _fields_=[(n,u32) for n in ('variants','regions','resources','table','push','set','stride','gfx')]
class Variant(c.Structure):
    _fields_=[('local',triplet),('code_bytes',u32),('exec_bytes',u32),('alignment',u32),('rsrc',triplet)]+[
        (n,u32) for n in ('dispatch','sgprs','vgprs','bindings','reads','writes','table','push','wave')]
class Region(c.Structure):_fields_=[('variant',u32),('dispatch',Dispatch)]
class Resource(c.Structure):
    _fields_=[(n,u32) for n in ('set','binding','metal','access','declared','reflected')]
assert [c.sizeof(t) for t in (Info,Variant,Region,Resource)]==[32,72,120,24]
library.hn_shader_compile.argtypes=[c.c_void_p,c.c_size_t,u32,u32,c.c_char_p]+[u32]*5
library.hn_shader_compile.restype=handle
for name in ('hn_shader_result_error','hn_pipeline_error'):
    f=getattr(library,name);f.argtypes=[handle];f.restype=c.c_char_p
for name in ('hn_shader_result_free','hn_pipeline_free'):
    f=getattr(library,name);f.argtypes=[handle];f.restype=None
library.hn_pipeline_abi_version.restype=u32
library.hn_pipeline_build.argtypes=[handle,u32,c.POINTER(u32),c.POINTER(u32),c.c_char_p]
library.hn_pipeline_build.restype=handle
library.hn_pipeline_info.argtypes=[handle,c.POINTER(Info),c.c_size_t];library.hn_pipeline_info.restype=c.c_int
for name,kind in [('variant',Variant),('region',Region),('resource',Resource)]:
    f=getattr(library,'hn_pipeline_'+name);f.argtypes=[handle,u32,c.POINTER(kind),c.c_size_t];f.restype=c.c_int
library.hn_pipeline_blob_size.argtypes=[handle,u32,u32];library.hn_pipeline_blob_size.restype=c.c_size_t
library.hn_pipeline_copy_blob.argtypes=[handle,u32,u32,c.c_void_p,c.c_size_t];library.hn_pipeline_copy_blob.restype=c.c_int
assert library.hn_pipeline_abi_version()==1
def blob(product,kind,index=0):
    size=library.hn_pipeline_blob_size(product,kind,index);assert size
    buf=c.create_string_buffer(size);assert library.hn_pipeline_copy_blob(product,kind,index,buf,size)==1
    return buf.raw
def compile_source(data,name,fmt=1,mode=1):
    memory=c.create_string_buffer(data)
    shader=library.hn_shader_compile(memory,len(data),fmt,0,name.encode(),64,1,1,mode,0)
    assert shader and not library.hn_shader_result_error(shader),library.hn_shader_result_error(shader)
    return shader
def build(shader,grid=(4099,1,1),local=(64,1,1),mode=1,tool=compiler):
    return library.hn_pipeline_build(shader,mode,triplet(*grid),triplet(*local),str(tool).encode())
def assert_failure(product):
    try:
        assert product and library.hn_pipeline_error(product)
        info=Info();c.memset(c.byref(info),0xa5,c.sizeof(info));before=bytes(info)
        assert library.hn_pipeline_info(product,c.byref(info),c.sizeof(info))==0
        assert bytes(info)==before
        for kind in range(5):assert library.hn_pipeline_blob_size(product,kind,0)==0
    finally:library.hn_pipeline_free(product)

records=[]
fixtures=Path(__file__).parent/'fixtures'
for name,data,fmt in [('add_one',(fixtures/'apple/add_one.air').read_bytes(),0),
                      ('affine_xor',(fixtures/'affine_xor.ll').read_bytes(),1)]:
    shader=compile_source(data,name,fmt)
    try:
        product=build(shader)
        assert product and not library.hn_pipeline_error(product),library.hn_pipeline_error(product)
        for grid,local,mode in [((0,1,1),(64,1,1),1),((4099,1,1),(0,1,1),1),((1,1,1),(1025,1,1),1),((1,1,1),(64,1,1),0)]:
            assert_failure(build(shader,grid,local,mode))
        assert_failure(build(shader,tool='relative-compiler'))
        assert_failure(build(shader,tool=compiler.parent/'missing-aco-compiler'))
    finally:library.hn_shader_result_free(shader)
    # All access below occurs AFTER the original successful shader was freed.
    try:
        info=Info();assert library.hn_pipeline_info(product,c.byref(info),c.sizeof(info))==1
        assert tuple(getattr(info,n) for n in ('variants','regions','resources','table','push','stride','gfx'))==(2,2,2,32,48,16,1100)
        assert json.loads(blob(product,3))['entry_point']==name
        entry=blob(product,4).decode();assert entry
        source_spv=blob(product,2)
        codes=[]
        with tempfile.TemporaryDirectory(prefix='hn-pipeline-oracle-') as temp:
            temp=Path(temp);module=temp/'input.spv';module.write_bytes(source_spv)
            for index,local in enumerate([(64,1,1),(3,1,1)]):
                region=Region();variant=Variant()
                assert library.hn_pipeline_region(product,index,c.byref(region),c.sizeof(region))==1
                assert region.variant==index
                assert list(region.dispatch.local)==list(local)
                assert list(region.dispatch.spec_ids)==[0,1,2]
                assert list(region.dispatch.push)==[4099,1,1,index*4096,0,0,index*64,0,0,65,1,1]
                assert library.hn_pipeline_variant(product,index,c.byref(variant),c.sizeof(variant))==1
                assert list(variant.local)==list(local) and variant.wave==32
                assert (variant.bindings,variant.reads,variant.writes)==(3,1,2)
                code=blob(product,0,index);metadata=json.loads(blob(product,1,index))
                assert len(code)==variant.code_bytes and metadata['workgroup_size']==list(local)
                standalone=temp/f'variant-{index}';standalone.mkdir()
                subprocess.run([str(compiler),str(module),entry,*map(str,local),str(standalone)],check=True,capture_output=True,timeout=120)
                assert code==(standalone/'shader.bin').read_bytes()
                assert metadata==json.loads((standalone/'shader.json').read_text())
                codes.append(code)
                records.append({'kernel':name,'local':local,'code_bytes':len(code),'code_sha256':hashlib.sha256(code).hexdigest(),
                    'spirv_sha256':hashlib.sha256(source_spv).hexdigest(),'rsrc':list(variant.rsrc),'standalone_equal':True})
        assert codes[0]!=codes[1], 'real tail specialization must differ for this kernel'
        for index,access in enumerate((1,2)):
            resource=Resource();assert library.hn_pipeline_resource(product,index,c.byref(resource),c.sizeof(resource))==1
            assert (resource.binding,resource.metal,resource.access,resource.reflected)==(index,index,access,access)
        # No borrowed data: caller mutation cannot alter retained code.
        code=bytearray(blob(product,0));code[0]^=255;assert bytes(code)!=blob(product,0)
        bad=Variant();c.memset(c.byref(bad),0xa5,c.sizeof(bad));before=bytes(bad)
        assert library.hn_pipeline_variant(product,2,c.byref(bad),c.sizeof(bad))==0 and bytes(bad)==before
        assert library.hn_pipeline_variant(product,0,c.byref(bad),c.sizeof(bad)-1)==0 and bytes(bad)==before
        out=c.create_string_buffer(b'unchanged');before=out.raw
        assert library.hn_pipeline_copy_blob(product,0,0,out,len(out))==0 and out.raw==before
        assert library.hn_pipeline_blob_size(product,99,0)==0
    finally:library.hn_pipeline_free(product)
assert records[0]['code_sha256']!=records[2]['code_sha256']
# Renaming the authored function does not select an implementation by name.
data=(fixtures/'affine_xor.ll').read_bytes().replace(b'affine_xor',b'opaque_kernel_42')
shader=compile_source(data,'opaque_kernel_42')
try:
    product=build(shader)
    try:
        assert product and not library.hn_pipeline_error(product),library.hn_pipeline_error(product)
        assert hashlib.sha256(blob(product,0)).hexdigest()==records[2]['code_sha256']
    finally:library.hn_pipeline_free(product)
finally:library.hn_shader_result_free(shader)
assert_failure(build(None))
# Real subprocess failure must not publish code. Python is deliberately the
# wrong compiler here; it reads the binary SPIR-V as a script and exits in error.
shader=compile_source((fixtures/'affine_xor.ll').read_bytes(),'affine_xor')
try:assert_failure(build(shader,tool=Path(sys.executable)))
finally:library.hn_shader_result_free(shader)
# On macOS, a test-only wrapper invokes the REAL compiler for variant0 and then
# deliberately fails variant1. It emits no substitute code or success result.
# The marker proves one genuine partial product existed before total rejection.
partial_failure_test=False
if os.name!='nt':
    with tempfile.TemporaryDirectory(prefix='hn-pipeline-fault-') as temp:
        temp=Path(temp);wrapper=temp/'fail-tail';marker=temp/'compiled-first.sha256'
        wrapper.write_text('#!'+sys.executable+'\n'+
            'import hashlib,pathlib,subprocess,sys\n'+
            'if sys.argv[3]=="3": sys.exit(73)\n'+
            'r=subprocess.run('+repr([str(compiler)])+'+sys.argv[1:])\n'+
            'if r.returncode: sys.exit(r.returncode)\n'+
            'pathlib.Path('+repr(str(marker))+').write_text(hashlib.sha256((pathlib.Path(sys.argv[6])/"shader.bin").read_bytes()).hexdigest())\n')
        wrapper.chmod(0o755)
        shader=compile_source((fixtures/'apple/add_one.air').read_bytes(),'add_one',0)
        try:
            product=build(shader,tool=wrapper)
            assert b'73' in library.hn_pipeline_error(product),library.hn_pipeline_error(product)
            assert_failure(product)
            assert marker.read_text()==records[0]['code_sha256']
            partial_failure_test=True
        finally:library.hn_shader_result_free(shader)
library.hn_pipeline_free(None)
print(json.dumps({'success':True,'hardware_executed':False,'metal_device_registered':False,
    'real_first_variant_then_injected_tail_failure':partial_failure_test,
    'claim':'real AIR/SPIR-V plus two kernels and local64/local3 ACO products; independent lifetime and exact standalone comparison',
    'variants':records},indent=2))
