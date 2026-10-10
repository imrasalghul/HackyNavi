#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual SDL metallib -> C ABI/CLI -> real AIR translation/validation; no GPU."""
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
library_path = Path(sys.argv[1]).resolve()
cli = None if sys.argv[2] == '-' else Path(sys.argv[2]).resolve()
report_path = Path(sys.argv[3]).resolve()
report_path.write_text(json.dumps({'success': False, 'complete': False}) + '\n', encoding='utf-8')
library = c.CDLL(str(library_path))
handle = c.c_void_p
u32 = c.c_uint32
library.hn_shader_abi_version.restype = u32
library.hn_shader_compile.argtypes = [handle, c.c_size_t, u32, u32, c.c_char_p] + [u32] * 5
library.hn_shader_compile.restype = handle
for name in ('error', 'reflection', 'spirv_entry_point'):
    function = getattr(library, 'hn_shader_result_' + name)
    function.argtypes = [handle]
    function.restype = c.c_char_p
library.hn_shader_result_words.argtypes = [handle, c.POINTER(c.c_size_t)]
library.hn_shader_result_words.restype = c.POINTER(u32)
library.hn_shader_result_free.argtypes = [handle]
library.hn_shader_result_free.restype = None
assert library.hn_shader_abi_version() == 1


def compile_shader(data, name, stage, fmt=2, expected_error=None):
    memory = c.create_string_buffer(bytes(data))
    result = library.hn_shader_compile(memory, len(data), fmt, stage, name.encode(), 64, 1, 1, 1, 0)
    assert result, 'compiler returned no owned success/error result'
    # The implementation cannot retain caller input after the call.
    c.memset(memory, 0, len(data))
    try:
        error = library.hn_shader_result_error(result).decode()
        count = c.c_size_t(0xdeadbeef)
        pointer = library.hn_shader_result_words(result, c.byref(count))
        reflection = library.hn_shader_result_reflection(result)
        entry = library.hn_shader_result_spirv_entry_point(result)
        if expected_error is not None:
            assert error and expected_error in error, error
            assert not pointer and count.value == 0 and not reflection and not entry
            assert 'panicked' not in error, error
            return error
        assert not error, error
        assert pointer and count.value >= 5 and reflection and entry
        words = c.string_at(pointer, count.value * 4)
        reflected = json.loads(reflection)
        assert reflected['entry_point'] == name
        assert reflected['stage'] == ['Kernel', 'Vertex', 'Fragment'][stage]
        assert words[:4] == b'\x03\x02\x23\x07'
        return words, reflected, entry.decode()
    finally:
        library.hn_shader_result_free(result)


fixture = HERE / 'fixtures/metallib/sdl-2.30.11-macos.metallib'
data = fixture.read_bytes()
assert hashlib.sha256(data).hexdigest() == '7c898daf138f3d72385140b81f99578628fb6972a8dde31cc3382a0c40e2bc26'
# Independent fixture-table reading is only the test oracle for the exact AIR
# comparison. Production callers go through the Rust parser.
function_count = struct.unpack_from('<I', data, 88)[0]
cursor = 92
records = []
for _ in range(function_count):
    size = struct.unpack_from('<I', data, cursor)[0]
    tags = {}
    pos = cursor + 4
    while data[pos:pos + 4] != b'ENDT':
        tag = data[pos:pos + 4]
        length = struct.unpack_from('<H', data, pos + 4)[0]
        tags[tag] = (pos + 6, data[pos + 6:pos + 6 + length])
        pos += 6 + length
    records.append(tags)
    cursor += size

report = {'scope': 'actual SDL metallib C ABI and CLI translation', 'hardware_executed': False,
          'metal_device_registered': False, 'fixture_sha256': hashlib.sha256(data).hexdigest(),
          'library_sha256': hashlib.sha256(library_path.read_bytes()).hexdigest(),
          'cli_sha256': hashlib.sha256(cli.read_bytes()).hexdigest() if cli else None,
          'cli_tested': cli is not None, 'functions': [], 'failures': []}
with tempfile.TemporaryDirectory(prefix='hn-metallib-abi-') as directory:
    directory = Path(directory)
    for index, tags in enumerate(records):
        name = tags[b'NAME'][1][:-1].decode()
        stage = tags[b'TYPE'][1][0] + 1
        actual = compile_shader(data, name, stage)
        bitcode_start = struct.unpack_from('<Q', data, 72)[0] + struct.unpack_from('<Q', tags[b'OFFT'][1], 16)[0]
        raw_offset, raw_bytes = struct.unpack_from('<II', data, bitcode_start + 8)
        raw = data[bitcode_start + raw_offset:bitcode_start + raw_offset + raw_bytes]
        direct = compile_shader(raw, name, stage, fmt=0)
        assert actual == direct, 'container route differs from exact raw AIR translation'
        words, reflection, entry = actual
        module = directory / f'{index}.spv'
        module.write_bytes(words)
        subprocess.run([os.environ['METAL2VULKAN_SPIRV_VAL'], '--target-env', 'vulkan1.2', str(module)],
                       check=True, capture_output=True, timeout=60)
        for wrong_stage in set(range(3)) - {stage}:
            message = compile_shader(data, name, wrong_stage, expected_error='metallib stage mismatch')
            report['failures'].append({'name': name, 'stage': wrong_stage, 'error': message})
        report['functions'].append({'name': name, 'stage': reflection['stage'], 'words': len(words) // 4,
                                    'spirv_sha256': hashlib.sha256(words).hexdigest(),
                                    'spirv_entry': entry, 'raw_air_equivalent': True,
                                    'independent_validator_exit': 0})

    for name in ('add_one', 'sdl_solid_vertex', 'SDL_Solid_vertex ', ''):
        message = compile_shader(data, name, 1, expected_error='entry point' if not name else 'FunctionNotFound')
        report['failures'].append({'name': name, 'error': message})
    for fmt in (3, 0xffffffff):
        message = compile_shader(data, 'SDL_Solid_vertex', 1, fmt=fmt, expected_error='unknown input format')
        report['failures'].append({'format': fmt, 'error': message})
    invalid = bytearray(data)
    invalid[6] = 3
    report['failures'].append({'case': 'unsupported-version', 'error':
        compile_shader(invalid, 'SDL_Solid_vertex', 1, expected_error='UnsupportedVersion')})
    invalid = bytearray(data)
    invalid[records[0][b'HASH'][0]] ^= 1
    report['failures'].append({'case': 'wrong-air-hash', 'error':
        compile_shader(invalid, 'SDL_Solid_vertex', 1, expected_error='HashMismatch')})
    report['failures'].append({'case': 'truncated', 'error':
        compile_shader(data[:-1], 'SDL_Solid_vertex', 1, expected_error='FileSizeMismatch')})

    # Container metadata is not executable authority. Renaming the table cannot
    # rename the real AIR entry; changing its stage cannot change the AIR stage.
    invalid = bytearray(data)
    invalid[records[0][b'NAME'][0]] = ord('X')
    report['failures'].append({'case': 'table-name-disagrees-with-air', 'error':
        compile_shader(invalid, 'XDL_Solid_vertex', 1, expected_error='entry point mismatch')})
    invalid = bytearray(data)
    invalid[records[0][b'TYPE'][0]] = 1
    report['failures'].append({'case': 'table-stage-disagrees-with-air', 'error':
        compile_shader(invalid, 'SDL_Solid_vertex', 2, expected_error='entry point mismatch')})

    # Both existing input values remain functional through the same exported ABI.
    old_air = compile_shader((HERE / 'fixtures/apple/add_one.air').read_bytes(), 'add_one', 0, fmt=0)
    old_text = compile_shader((HERE / 'fixtures/affine_xor.ll').read_bytes(), 'affine_xor', 0, fmt=1)
    assert old_air[0] != old_text[0]
    report['existing_formats'] = {'air_words': len(old_air[0]) // 4, 'llvm_words': len(old_text[0]) // 4}

    if cli is not None:
        output = directory / 'cli-success'
        result = subprocess.run([str(cli), str(fixture), 'SDL_Solid_fragment', 'fragment', str(output)],
                                text=True, capture_output=True, timeout=120)
        assert result.returncode == 0, result.stderr
        direct = compile_shader(data, 'SDL_Solid_fragment', 2)
        assert (output / 'shader.spv').read_bytes() == direct[0]
        assert json.loads((output / 'reflection.json').read_text()) == direct[1]
        assert json.loads((output / 'module.json').read_text())['spirv_entry_point'] == direct[2]
        for index, (name, stage) in enumerate([('SDL_Solid_fragment', 'vertex'), ('missing', 'compute')]):
            rejected_output = directory / f'cli-failure-{index}'
            result = subprocess.run([str(cli), str(fixture), name, stage, str(rejected_output)],
                                    text=True, capture_output=True, timeout=120)
            assert result.returncode != 0 and not rejected_output.exists()
        report['cli_success_matches_c_abi'] = True
        report['cli_failures_publish_no_directory'] = True

report['success'] = True
report['complete'] = True
report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(f"{len(records)} actual SDL functions translated, raw-AIR equal and validated; "
      f"{len(report['failures'])} rejected C ABI cases; old formats passed; "
      f"CLI {'passed' if cli else 'NOT RUN (explicit skip)'}; GPU not executed")
