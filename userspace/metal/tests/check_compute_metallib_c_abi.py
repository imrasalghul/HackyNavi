#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the unchanged, actually Apple-built macOS15 compute library and AIR."""
import ctypes as c
import hashlib
import json
from pathlib import Path
import sys

here = Path(__file__).resolve().parent
binary, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
if output.exists():
    raise RuntimeError('Use a fresh report path')
lib = c.CDLL(str(binary))
p, n, u = c.c_void_p, c.c_size_t, c.c_uint32
class Info(c.Structure):
    _fields_ = [('struct_bytes', u), ('stage', u), ('name_bytes', u), ('air_bytes', u),
                ('air_version', c.c_uint16 * 2), ('language_version', c.c_uint16 * 2),
                ('wrapped_air_sha256', c.c_uint8 * 32), ('reserved', u * 2)]
for name, args, result in [
    ('hn_metallib_parse', [p,n], p), ('hn_metallib_error', [p], c.c_char_p),
    ('hn_metallib_function_count', [p], u), ('hn_metallib_function_info', [p,u,p,n], c.c_int),
    ('hn_metallib_copy_name', [p,u,p,n], c.c_int), ('hn_metallib_copy_air', [p,u,p,n], c.c_int),
    ('hn_metallib_free', [p], None),
    ('hn_shader_compile', [p,n,u,u,c.c_char_p,u,u,u,u,u], p),
    ('hn_shader_result_error', [p], c.c_char_p), ('hn_shader_result_reflection', [p], c.c_char_p),
    ('hn_shader_result_words', [p,c.POINTER(n)], c.POINTER(u)), ('hn_shader_result_free', [p], None)]:
    fn = getattr(lib,name); fn.argtypes=args; fn.restype=result
checks = 0
report = {'success':False,'checks':0,'gpu_execution':False,'metal_object_instantiation':False,
          'compiler_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'compiles':[]}
def save():
    report['checks']=checks
    output.write_text(json.dumps(report,indent=2)+'\n')
def check(value, message):
    global checks
    checks += 1
    if not value: raise AssertionError(message)
def compile_shader(data, fmt, label, entry=b'add_one', stage=0, success=True):
    memory=c.create_string_buffer(data)
    result=lib.hn_shader_compile(memory,len(data),fmt,stage,entry,1,1,1,1,0)
    try:
        check(bool(result),'actual compile returned a result')
        error=lib.hn_shader_result_error(result)
        count=n(); words=lib.hn_shader_result_words(result,c.byref(count))
        reflection=lib.hn_shader_result_reflection(result)
        if not success:
            check(bool(error),'invalid library selection fails')
            check(not words and not count.value and not reflection,'failed compile has no success payload')
            report['compiles'].append({'label':label,'expected_failure':error.decode()}); save(); return None
        check(error==b'',error.decode() if error else 'missing error status')
        check(bool(words) and count.value>=5 and words[0]==0x07230203,'actual validated SPIR-V')
        check(bool(reflection),'actual reflection exists')
        parsed=json.loads(reflection)
        check(parsed['entry_point']=='add_one' and parsed['stage']=='Kernel','real compute reflection')
        check(parsed['max_work_group_size'] is None,'actual authored fixture has no AIR ceiling')
        code=c.string_at(words,count.value*4)
        report['compiles'].append({'label':label,'spirv_bytes':len(code),'spirv_sha256':hashlib.sha256(code).hexdigest(),
                                  'reflection_sha256':hashlib.sha256(reflection).hexdigest()}); save()
        return code, reflection
    finally:
        if result: lib.hn_shader_result_free(result)

save()
data=(here/'fixtures/metallib/apple-32023-macos15-add-one.metallib').read_bytes()
check(hashlib.sha256(data).hexdigest()=='ed77099cd3e56ac404331fa68dd9401a7b326a7b7ea80d942b29b8b1fe0af865','actual fixture hash')
memory=c.create_string_buffer(data)
owned=lib.hn_metallib_parse(memory,len(data))
try:
    check(bool(owned) and lib.hn_metallib_error(owned)==b'','parse actual modern compute library')
    c.memset(memory,0xff,len(data)); del memory
    check(lib.hn_metallib_function_count(owned)==1,'one exact compute function')
    info=Info()
    check(lib.hn_metallib_function_info(owned,0,c.byref(info),c.sizeof(info))==1,'owned function metadata')
    check((info.struct_bytes,info.stage,info.name_bytes,info.air_bytes)==(64,0,8,3332),'exact metadata')
    check(list(info.air_version)==[2,7] and list(info.language_version)==[2,4],'actual AIR/MSL versions')
    check(list(info.reserved)==[0,0],'reserved ABI fields zero')
    for index,size in [(1,64),(0,63),(0,65)]:
        before=c.string_at(c.byref(info),64)
        check(lib.hn_metallib_function_info(owned,index,c.byref(info),size)==0,'invalid info rejected')
        check(c.string_at(c.byref(info),64)==before,'invalid info leaves output unchanged')
    name=c.create_string_buffer(8); air=c.create_string_buffer(3332)
    check(lib.hn_metallib_copy_name(owned,0,name,8)==1 and name.raw==b'add_one\0','exact owned name')
    check(lib.hn_metallib_copy_air(owned,0,air,3332)==1,'exact owned AIR copy')
    check(hashlib.sha256(air.raw).hexdigest()=='8d5c89281870b3d1f8f6868ad56f9396fdd8804acc697c59297c9f338889a13f','raw AIR hash')
    for size in [0,3331,3333]:
        before=air.raw
        check(lib.hn_metallib_copy_air(owned,0,air,size)==0,'invalid AIR destination size rejected')
        check(air.raw==before,'rejected AIR copy unchanged')
finally:
    if owned: lib.hn_metallib_free(owned)
# The copied actual AIR remains valid after input mutation/free and parse free.
copied=compile_shader(air.raw,0,'owned raw AIR after library/input free')
container=compile_shader(data,2,'actual compute metallib')
check(copied==container,'container and copied actual AIR have identical validated code/reflection')
prelink=(here/'fixtures/metallib/apple-32023-macos15-add-one.air').read_bytes()
check(hashlib.sha256(prelink).hexdigest()=='40b3308114c0fd8bf39e0fef68b2389bc45a5a6bbfa8d898c54840150469ef1d','actual prelink AIR hash')
compile_shader(prelink,0,'actual prelink Apple AIR')
compile_shader(data,2,'wrong case-sensitive entry',entry=b'Add_one',success=False)
compile_shader(data,2,'wrong requested stage',stage=1,success=False)
report['success']=True; save()
print(f'{checks} actual compute metallib ABI/compiler checks, 0 failures')
