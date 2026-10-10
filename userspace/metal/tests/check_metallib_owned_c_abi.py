#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the actual owned metallib DLL/dylib ABI and compile copied real AIR."""
import ctypes as c
import hashlib
import json
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
library_path = Path(sys.argv[1]).resolve()
report_path = Path(sys.argv[2]).resolve()
if report_path.exists():
    raise RuntimeError('Use a fresh report path to preserve prior evidence')
library = c.CDLL(str(library_path))
handle, size, u32 = c.c_void_p, c.c_size_t, c.c_uint32

class Info(c.Structure):
    _fields_ = [('struct_bytes', u32), ('stage', u32), ('name_bytes', u32), ('air_bytes', u32),
                ('air_version', c.c_uint16 * 2), ('language_version', c.c_uint16 * 2),
                ('wrapped_air_sha256', c.c_uint8 * 32), ('reserved', u32 * 2)]

library.hn_metallib_abi_version.restype = u32
library.hn_metallib_parse.argtypes = [handle, size]
library.hn_metallib_parse.restype = handle
library.hn_metallib_error.argtypes = [handle]
library.hn_metallib_error.restype = c.c_char_p
library.hn_metallib_function_count.argtypes = [handle]
library.hn_metallib_function_count.restype = u32
library.hn_metallib_function_info.argtypes = [handle, u32, handle, size]
library.hn_metallib_function_info.restype = c.c_int32
for name in ('copy_name', 'copy_air'):
    function = getattr(library, 'hn_metallib_' + name)
    function.argtypes = [handle, u32, handle, size]
    function.restype = c.c_int32
library.hn_metallib_free.argtypes = [handle]
library.hn_metallib_free.restype = None
library.hn_shader_abi_version.restype = u32
library.hn_pipeline_abi_version.restype = u32
library.hn_shader_compile.argtypes = [handle, size, u32, u32, c.c_char_p] + [u32] * 5
library.hn_shader_compile.restype = handle
library.hn_shader_result_error.argtypes = [handle]
library.hn_shader_result_error.restype = c.c_char_p
library.hn_shader_result_reflection.argtypes = [handle]
library.hn_shader_result_reflection.restype = c.c_char_p
library.hn_shader_result_words.argtypes = [handle, c.POINTER(size)]
library.hn_shader_result_words.restype = c.POINTER(u32)
library.hn_shader_result_free.argtypes = [handle]
library.hn_shader_result_free.restype = None

checks = 0
def check(value, message='owned metallib ABI assertion failed'):
    global checks
    checks += 1
    if not value:
        raise AssertionError(message)

report = {'scope': 'owned real metallib C ABI, exact copies and post-free AIR compilation',
          'success': False, 'complete': False, 'hardware_executed': False, 'metal_device_registered': False,
          'library_sha256': hashlib.sha256(library_path.read_bytes()).hexdigest(), 'functions': []}
report_path.write_text(json.dumps(report, indent=2) + '\n')

def sentinel():
    value = Info()
    c.memset(c.byref(value), 0xa5, c.sizeof(value))
    return value

data = (HERE / 'fixtures/metallib/sdl-2.30.11-macos.metallib').read_bytes()
report['fixture_sha256'] = hashlib.sha256(data).hexdigest()
check(report['fixture_sha256'] == '7c898daf138f3d72385140b81f99578628fb6972a8dde31cc3382a0c40e2bc26')
check(c.sizeof(Info) == 64 and Info.wrapped_air_sha256.offset == 24)
check(library.hn_metallib_abi_version() == library.hn_shader_abi_version() == library.hn_pipeline_abi_version() == 1)

# This independent table read is only a fixture oracle, never production code.
expected_names = ['SDL_Solid_vertex', 'SDL_Copy_vertex', 'SDL_Solid_fragment', 'SDL_Copy_fragment',
                  'SDL_YUV_fragment', 'SDL_NV12_fragment', 'SDL_NV21_fragment']
records = []
cursor = 92
for index in range(struct.unpack_from('<I', data, 88)[0]):
    group_bytes = struct.unpack_from('<I', data, cursor)[0]
    position, tags = cursor + 4, {}
    while data[position:position + 4] != b'ENDT':
        tag = data[position:position + 4]
        count = struct.unpack_from('<H', data, position + 4)[0]
        tags[tag] = data[position + 6:position + 6 + count]
        position += 6 + count
    start = struct.unpack_from('<Q', data, 72)[0] + struct.unpack_from('<Q', tags[b'OFFT'], 16)[0]
    offset, length = struct.unpack_from('<II', data, start + 8)
    name = tags[b'NAME']
    check(name == expected_names[index].encode() + b'\0')
    records.append((name, [1, 2, 0][tags[b'TYPE'][0]], data[start + offset:start + offset + length], tags[b'HASH']))
    cursor += group_bytes
check(len(records) == 7)

input_memory = c.create_string_buffer(data)
parsed = library.hn_metallib_parse(input_memory, len(data))
second = library.hn_metallib_parse(input_memory, len(data))
check(bool(parsed) and bool(second) and parsed != second)
c.memset(input_memory, 0xff, len(data))
del input_memory
check(library.hn_metallib_error(parsed) == b'')
check(library.hn_metallib_function_count(parsed) == 7)
owned = []
try:
    for index, (name, stage, raw, digest) in enumerate(records):
        info = sentinel()
        for wrong_size in (0, 1, 63, 65, size(-1).value):
            check(library.hn_metallib_function_info(parsed, index, c.byref(info), wrong_size) == 0)
            check(bytes(info) == b'\xa5' * 64)
        check(library.hn_metallib_function_info(parsed, index, None, 64) == 0)
        check(library.hn_metallib_function_info(parsed, index, c.byref(info), 64) == 1)
        check((info.struct_bytes, info.stage, info.name_bytes, info.air_bytes) == (64, stage, len(name), len(raw)))
        check(list(info.air_version) == [1, 8] and list(info.language_version) == [1, 1])
        check(bytes(info.wrapped_air_sha256) == digest and list(info.reserved) == [0, 0])
        copied = []
        for suffix, expected in [('name', name), ('air', raw)]:
            operation = getattr(library, 'hn_metallib_copy_' + suffix)
            buffer = c.create_string_buffer(b'\xcd' * (len(expected) + 2), len(expected) + 2)
            for wrong_size in (0, 1, len(expected) - 1, len(expected) + 1, size(-1).value):
                check(operation(parsed, index, c.byref(buffer, 1), wrong_size) == 0)
                check(buffer.raw == b'\xcd' * len(buffer))
            check(operation(parsed, index, None, len(expected)) == 0)
            check(operation(parsed, index, handle(size(-1).value), len(expected)) == 0)
            check(operation(parsed, index, c.byref(buffer, 1), len(expected)) == 1)
            check(buffer.raw == b'\xcd' + expected + b'\xcd')
            copied.append(buffer.raw[1:-1])
        owned.append((info.stage, *copied))

    for index in (7, 8, 0xffffffff):
        info = sentinel()
        check(library.hn_metallib_function_info(parsed, index, c.byref(info), 64) == 0)
        check(bytes(info) == b'\xa5' * 64)
        for operation, count in [(library.hn_metallib_copy_name, 17), (library.hn_metallib_copy_air, 2980)]:
            buffer = c.create_string_buffer(b'\xcc' * count, count)
            check(operation(parsed, index, buffer, count) == 0 and buffer.raw == b'\xcc' * count)
    misaligned = (Info * 2)()
    c.memset(c.byref(misaligned), 0xab, c.sizeof(misaligned))
    check(library.hn_metallib_function_info(parsed, 0, c.byref(misaligned, 1), 64) == 0)
    check(bytes(misaligned) == b'\xab' * 128)
    check(library.hn_metallib_function_info(parsed, 0, handle(size(-1).value - 3), 64) == 0)
finally:
    library.hn_metallib_free(parsed)
check(library.hn_metallib_function_count(second) == 7)
info = sentinel()
check(library.hn_metallib_function_info(second, 6, c.byref(info), 64) == 1 and info.air_bytes == 3180)
library.hn_metallib_free(second)

# All actual names/AIR below are caller-owned copies after both parse owners and
# their original input allocation have gone. The actual compiler validates AIR.
for stage, name, raw in owned:
    memory = c.create_string_buffer(raw)
    result = library.hn_shader_compile(memory, len(raw), 0, stage, name, 64, 1, 1, 1, 0)
    check(bool(result))
    try:
        check(library.hn_shader_result_error(result) == b'')
        words = size(0)
        pointer = library.hn_shader_result_words(result, c.byref(words))
        check(bool(pointer) and words.value >= 5)
        reflection = json.loads(library.hn_shader_result_reflection(result))
        check(reflection['entry_point'] == name[:-1].decode())
        check(reflection['stage'] == ['Kernel', 'Vertex', 'Fragment'][stage])
        report['functions'].append({'name': name[:-1].decode(), 'stage': stage, 'air_bytes': len(raw),
            'air_sha256': hashlib.sha256(raw).hexdigest(), 'spirv_words': words.value,
            'spirv_sha256': hashlib.sha256(c.string_at(pointer, words.value * 4)).hexdigest(),
            'compiled_after_library_free': True})
    finally:
        library.hn_shader_result_free(result)

check(library.hn_metallib_error(None) is None)
check(library.hn_metallib_function_count(None) == 0)
library.hn_metallib_free(None)
memory = c.create_string_buffer(data)
bad_version = bytearray(data); bad_version[6] = 3
bad_memory = c.create_string_buffer(bytes(bad_version))
cases = [(None, len(data)), (memory, 0), (memory, 64 * 1024 * 1024 + 1),
         (memory, size(-1).value), (handle(size(-1).value), 88),
         (memory, len(data) - 1), (bad_memory, len(data))]
for pointer, count in cases:
    failed = library.hn_metallib_parse(pointer, count)
    check(bool(failed))
    try:
        message = library.hn_metallib_error(failed)
        check(bool(message) and b'panicked' not in message)
        check(library.hn_metallib_function_count(failed) == 0)
        for rejected in (failed, None):
            info = sentinel()
            check(library.hn_metallib_function_info(rejected, 0, c.byref(info), 64) == 0)
            check(bytes(info) == b'\xa5' * 64)
            output = c.create_string_buffer(b'\xac' * 2980, 2980)
            check(library.hn_metallib_copy_name(rejected, 0, output, 17) == 0)
            check(library.hn_metallib_copy_air(rejected, 0, output, 2980) == 0)
            check(output.raw == b'\xac' * len(output))
    finally:
        library.hn_metallib_free(failed)

report.update(success=True, complete=True, checks=checks, owned_air_compiled_functions=len(owned))
report_path.write_text(json.dumps(report, indent=2) + '\n')
print(f'{checks} owned ABI checks; {len(owned)} real functions compiled after free; no GPU execution')
