#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Freeze a small real graphics runtime test after native compiler libraries pass."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[2]


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--graphics-report', type=Path, required=True)
    parser.add_argument('--compiler-report', type=Path, required=True)
    parser.add_argument('--reader-report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or not output.is_relative_to(ROOT / 'build'):
        parser.error('Use a fresh output directory inside build')
    data, origins = {}, {}

    def add(name, path, expected=None):
        if name in data:
            raise RuntimeError('Duplicate stage path')
        raw = path.read_bytes()
        if expected and sha(raw) != expected:
            raise RuntimeError('Verified input changed: ' + str(path))
        data[name], origins[name] = raw, path
        return raw

    reports = {}
    for kind, path in [('graphics', args.graphics_report), ('compiler', args.compiler_report), ('reader', args.reader_report)]:
        value = json.loads(add('provenance/' + kind + '.json', path))
        if not all(value.get(key) for key in ('success', 'native_tests_passed', 'artifacts_exported',
                                             'remote_job_finished', 'remote_stage_removed')):
            raise RuntimeError('Prior actual native validation incomplete')
        reports[kind] = value
    if set(reports['graphics']['native']['exports']['libHackyNaviGraphicsCompiler.dylib']) != {
            '_hn_gfx11_ngg_compile_validated_file', '_hn_gfx11_fragment_compile_validated_file',
            '_hn_gfx11_fragment_buffer_compile_validated_file', '_hn_gfx11_fragment_buffer_descriptor',
            '_hn_gfx11_vertex_buffer_compile_validated_file', '_hn_gfx11_vertex_buffer_descriptor'}:
        raise RuntimeError('Actual native buffer compiler exports were not verified')
    for name in ('libHackyNaviGraphicsCompiler.dylib', 'libHackyNaviColorLayout.dylib'):
        add(name, args.graphics_report.parent / name, reports['graphics']['native']['artifacts'][name]['sha256'])
    add('libhackynavi_metal_compiler.dylib', args.compiler_report.parent / 'libhackynavi_metal_compiler.dylib',
        reports['compiler']['native']['dylib']['sha256'])
    add('apple-air-dis', args.reader_report.parent / 'apple-air-dis', reports['reader']['native']['apple_air_dis']['sha256'])
    add('spirv-val', ROOT / '.tools/userspace/build-spirv-tools-macos/tools/spirv-val',
        reports['compiler']['source_files']['spirv-val']['sha256'])
    for path in (
        'userspace/runtime/graphics_shader.hpp', 'userspace/runtime/graphics_shader.cpp',
        'userspace/runtime/graphics_build.hpp', 'userspace/runtime/graphics_build.cpp',
        'userspace/runtime/graphics_snapshot.hpp', 'userspace/runtime/graphics_snapshot.cpp',
        'userspace/runtime/graphics_buffer_snapshot.hpp', 'userspace/runtime/graphics_buffer_snapshot.cpp',
        'userspace/runtime/compute_shader.hpp', 'userspace/runtime/compute_context.hpp',
        'userspace/transport/iokit_client.hpp', 'include/hackynavi/native_abi.hpp',
        'include/hackynavi/gfx11_color_target.h', 'include/hackynavi/gfx11_graphics_product.hpp',
        'src/gfx11_graphics_product.cpp', 'userspace/metal/include/hackynavi_shader.h',
        'userspace/metal/include/hackynavi_dispatch.h', 'userspace/metal/include/hackynavi_pipeline.h',
        'userspace/compiler/hackynavi_gfx11_ngg.h', 'userspace/compiler/hackynavi_gfx11_fragment.h',
        'userspace/compiler/hackynavi_gfx11_fragment_buffer.h',
        'userspace/compiler/hackynavi_gfx11_vertex_buffer.h',
        'userspace/graphics/graphics_build_native.cpp', 'userspace/graphics/prepare-runtime-native.py',
        'userspace/graphics/run_runtime_native.py'):
        add(path, ROOT / path)
    baseline = json.loads(add('provenance/apple-baseline.json', ROOT / 'build/apple-graphics-integration-1/report.json'))
    if not baseline['success']:
        raise RuntimeError('Missing actual Apple product baseline')
    for ext in ('air', 'metallib'):
        expected = {x['input_sha256'] for x in baseline['products'] if x['input_format'] == ext}
        if len(expected) != 1:
            raise RuntimeError('Ambiguous real input')
        add('fixtures/graphics.' + ext, ROOT / ('userspace/compiler/fixtures/apple-graphics/graphics.' + ext), expected.pop())
    for kind in ('vertex', 'fragment'):
        expected = next(x for x in baseline['products'] if x['input_format'] == 'air' and x['stage'] == kind)
        add('fixtures/' + kind + '.bin', ROOT / ('build/apple-graphics-integration-1/air-' + kind + '/product/shader.bin'),
            expected['code_sha256'])
    buffer = json.loads(add('provenance/buffer-baseline.json', ROOT / 'build/gfx11-fragment-buffer-tests-4/report.json'))
    translation = json.loads(add('provenance/buffer-translation.json', ROOT / 'build/apple-fragment-buffer-translation-1/report.json'))
    if not buffer['success'] or not translation['success']:
        raise RuntimeError('Missing actual buffer product baseline')
    for ext in ('air', 'metallib'):
        expected = next(row['input_sha256'] for row in translation['products'] if row['format'] == ext)
        add('fixtures/fragment-buffer.' + ext,
            ROOT / ('userspace/compiler/fixtures/apple-fragment-buffer/fragment-buffer.' + ext), expected)
    expected = next(row['code_sha256'] for row in buffer['products'] if row['input_format'] == 'air')
    add('fixtures/fragment-buffer.bin', ROOT / 'build/gfx11-fragment-buffer-tests-4/air/shader.bin', expected)
    vertex = json.loads(add('provenance/vertex-buffer-baseline.json', ROOT / 'build/gfx11-vertex-buffer-tests-1/report.json'))
    vertex_translation = json.loads(add('provenance/vertex-buffer-translation.json', ROOT / 'build/apple-vertex-buffer-translation-1/report.json'))
    if not vertex['success'] or not vertex_translation['success']:
        raise RuntimeError('Missing actual vertex-buffer product baseline')
    for ext in ('air', 'metallib'):
        expected = next(row['input_sha256'] for row in vertex_translation['products'] if row['format'] == ext)
        add('fixtures/vertex-buffer.' + ext,
            ROOT / ('userspace/compiler/fixtures/apple-vertex-buffer/vertex-buffer.' + ext), expected)
    expected = next(row['code_sha256'] for row in vertex['products'] if row['input_format'] == 'air' and row['route'] == 'cli')
    add('fixtures/vertex-buffer.bin', ROOT / 'build/gfx11-vertex-buffer-tests-1/air-cli/shader.bin', expected)
    size = sum(len(raw) for raw in data.values())
    if size > 60 * 1024 * 1024:
        raise RuntimeError('Runtime snapshot exceeds60MiB budget')
    rows = {name: {'sha256': sha(raw), 'bytes': len(raw)} for name, raw in sorted(data.items())}
    data['manifest.json'] = (json.dumps({'files': rows, 'bytes': size}, indent=2) + '\n').encode()
    output.mkdir(parents=True)
    archive = output / 'inputs.tar.gz'
    with tarfile.open(archive, 'w:gz', compresslevel=6) as tar:
        for name, raw in sorted(data.items()):
            info = tarfile.TarInfo(name)
            info.size = len(raw)
            info.mode = 0o755 if name in ('apple-air-dis', 'spirv-val') else 0o644
            info.mtime = 0
            tar.addfile(info, io.BytesIO(raw))
    unchanged = all(path.read_bytes() == data[name] for name, path in origins.items())
    report = {'prepared': True, 'success': unchanged, 'inputs_unchanged': unchanged,
              'source_files': rows, 'source_bytes': size, 'archive_bytes': archive.stat().st_size,
              'archive_sha256': sha(archive.read_bytes()), 'remote_execution_pending': True,
              'remote_storage_cap_bytes': 96 * 1024 * 1024, 'gpu_executed': False,
              'native_command': 'python3 <stage>/userspace/graphics/run_runtime_native.py <stage>'}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not unchanged:
        raise RuntimeError('Snapshot input changed')
    print(json.dumps({'prepared': True, 'bytes': size, 'archive_bytes': report['archive_bytes']}))


if __name__ == '__main__':
    main()
