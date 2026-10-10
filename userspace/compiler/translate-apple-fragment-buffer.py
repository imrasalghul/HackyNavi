#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Translate the genuine captured Apple buffer PS, without any GPU operation."""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    capture, output = args.capture.resolve(), args.output.resolve()
    if output.exists():
        parser.error('Evidence output must be new')
    prior = json.loads((capture / 'report.json').read_text())
    if not all(prior.get(k) for k in ('success', 'source_unchanged', 'export_hashes_verified', 'stage_removed')):
        parser.error('Requires verified actual Apple compiler capture')
    expected = {'fragment-buffer.metal', 'fragment-buffer.air', 'fragment-buffer.metallib'}
    if set(prior['artifacts']) != expected:
        parser.error('Unexpected capture artifacts')
    for name, record in prior['artifacts'].items():
        if sha(capture / name) != record['sha256'] or (capture / name).stat().st_size != record['bytes']:
            parser.error('Capture artifact changed')
    if (capture / 'fragment-buffer.metal').read_bytes() != (HERE / 'fixtures/apple-fragment-buffer/fragment-buffer.metal').read_bytes():
        parser.error('Captured input differs from authored source')
    tools = json.loads((ROOT / 'build/metal-explicit-tools-local/explicit-tools.json').read_text())['tools']
    for name in ('compiler', 'air_dis', 'spirv_val'):
        if sha(Path(tools[name]['path'])) != tools[name]['sha256']:
            parser.error('Actual frozen compiler/tool changed')
    output.mkdir(parents=True)
    report = {'success': False, 'capture_report_sha256': sha(capture / 'report.json'),
              'runner_sha256': sha(Path(__file__)), 'tools': tools, 'products': [],
              'commands': [], 'gpu_executed': False, 'metal_objects_created': False}
    lib = c.CDLL(tools['compiler']['path'])
    ptr, size, u32 = c.c_void_p, c.c_size_t, c.c_uint32
    class Tools(c.Structure):
        _fields_ = [('struct_size', u32), ('version', u32), ('air_dis_path', ptr), ('spirv_val_path', ptr)]
    lib.hn_shader_compile_with_tools.argtypes = [ptr, ptr, size, u32, u32, c.c_char_p, u32, u32, u32, u32, u32]
    lib.hn_shader_compile_with_tools.restype = ptr
    for name, arguments, result in [('error', [ptr], c.c_char_p), ('words', [ptr, c.POINTER(size)], ptr),
                                   ('spirv_entry_point', [ptr], c.c_char_p), ('reflection', [ptr], c.c_char_p)]:
        fn = getattr(lib, 'hn_shader_result_' + name)
        fn.argtypes, fn.restype = arguments, result
    lib.hn_shader_result_free.argtypes, lib.hn_shader_result_free.restype = [ptr], None
    air = c.create_string_buffer(os.fsencode(tools['air_dis']['path']))
    val = c.create_string_buffer(os.fsencode(tools['spirv_val']['path']))
    config = Tools(c.sizeof(Tools), 1, c.cast(air, ptr), c.cast(val, ptr))
    identities = []
    try:
        for extension, fmt in [('air', 0), ('metallib', 2)]:
            directory = output / extension
            directory.mkdir()
            raw = (capture / ('fragment-buffer.' + extension)).read_bytes()
            memory = c.create_string_buffer(raw)
            shader = lib.hn_shader_compile_with_tools(c.byref(config), memory, len(raw), fmt, 2,
                                                      b'hn_buffer_fragment', 1, 1, 1, 0, 1)
            try:
                if not shader:
                    raise RuntimeError('Actual shader result allocation failed')
                error = lib.hn_shader_result_error(shader).decode()
                if error:
                    raise RuntimeError(error)
                count = size()
                words = lib.hn_shader_result_words(shader, c.byref(count))
                if not words or not 5 < count.value <= 16 * 1024 * 1024:
                    raise RuntimeError('Invalid actual SPIR-V span')
                spirv = directory / 'shader.spv'
                spirv.write_bytes(c.string_at(words, count.value * 4))
                reflection = lib.hn_shader_result_reflection(shader)
                (directory / 'reflection.json').write_bytes(reflection)
                entry = lib.hn_shader_result_spirv_entry_point(shader).decode()
                for argv in ([tools['spirv_val']['path'], '--target-env', 'vulkan1.2', str(spirv)],
                             [str(Path(tools['spirv_val']['path']).with_name('spirv-dis.exe')), str(spirv), '-o', str(directory / 'shader.spvasm')]):
                    run = subprocess.run(argv, capture_output=True, text=True, timeout=30)
                    report['commands'].append({'argv': argv, 'exit_code': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr})
                    if run.returncode:
                        raise RuntimeError(run.stderr)
                identities.append((spirv.read_bytes(), reflection, entry))
                report['products'].append({'format': extension, 'entry': entry, 'input_sha256': sha(capture / ('fragment-buffer.' + extension)),
                                           'spirv_sha256': sha(spirv), 'reflection': json.loads(reflection),
                                           'reflection_sha256': sha(directory / 'reflection.json'),
                                           'disassembly_sha256': sha(directory / 'shader.spvasm')})
            finally:
                if shader:
                    lib.hn_shader_result_free(shader)
        if identities[0] != identities[1]:
            raise RuntimeError('Actual AIR/metallib translation differs')
        report['air_metallib_identical'] = True
        report['success'] = True
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': True, 'report_sha256': sha(output / 'report.json')}))


if __name__ == '__main__':
    main()
