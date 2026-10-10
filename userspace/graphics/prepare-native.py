#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create immutable small native graphics validation inputs; never contact a host."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import tarfile

ROOT = Path(__file__).resolve().parents[2]


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-report', type=Path, required=True)
    parser.add_argument('--compiler-report', type=Path, required=True)
    parser.add_argument('--reader-report', type=Path, required=True)
    parser.add_argument('--native-relink', action='store_true',
                        help='Use Apple clang to enforce the exact dylib export lists from actual Darwin objects')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or not output.is_relative_to(ROOT / 'build'):
        parser.error('Use a fresh directory inside build')
    data, origins = {}, {}

    def add(name, path, expected=None):
        if name in data:
            raise RuntimeError('Duplicate snapshot path')
        path = path.resolve()
        raw = path.read_bytes()
        if expected and sha(raw) != expected:
            raise RuntimeError('Verified source/artifact changed: ' + str(path))
        data[name] = raw
        origins[name] = path
        return raw

    build = json.loads(add('provenance/darwin-build.json', args.build_report))
    compiler = json.loads(add('provenance/compiler-native.json', args.compiler_report))
    reader = json.loads(add('provenance/reader-native.json', args.reader_report))
    for prior in (compiler, reader):
        if not all(prior.get(k) for k in ('success', 'remote_job_finished', 'remote_stage_removed', 'artifacts_exported', 'native_tests_passed')):
            raise RuntimeError('Compiler/helper native evidence incomplete')
    if not build['success'] or not build['inputs_unchanged'] or not build['dependencies_unchanged']:
        raise RuntimeError('Cross-build was not verified')
    exports = {'_hn_gfx11_ngg_compile_validated_file', '_hn_gfx11_fragment_compile_validated_file',
               '_hn_gfx11_fragment_buffer_compile_validated_file', '_hn_gfx11_fragment_buffer_descriptor',
               '_hn_gfx11_vertex_buffer_compile_validated_file', '_hn_gfx11_vertex_buffer_descriptor'}
    if set(build.get('expected_graphics_exports', [])) != exports or 'hackynavi-aco-vertex-buffer' not in build['artifacts']:
        raise RuntimeError('Requires the four actual adapters and six graphics C ABI exports')
    for name, value in build['inputs'].items():
        if sha((ROOT / name).read_bytes()) != value:
            raise RuntimeError('Cross-build source/dependency changed: ' + name)
    for group in build['dependencies'].values():
        for name, value in group.items():
            if sha((ROOT / name).read_bytes()) != value['sha256']:
                raise RuntimeError('Cross-build included dependency changed: ' + name)
    for name, item in build['artifacts'].items():
        if args.native_relink and name.endswith('.dylib'):
            continue
        add(name, args.build_report.parent / name, item['sha256'])
    if args.native_relink:
        order = []
        for name, item in build['archives'].items():
            basename = Path(name).name
            add('relink/archives/' + basename, ROOT / name, item['sha256'])
            order.append('relink/archives/' + basename)
        for name, item in build['native_relink_inputs'].items():
            add('relink/' + name, args.build_report.parent / name, item['sha256'])
        data['relink.json'] = (json.dumps({'archives': order, 'reason':
            'Zig Mach-O outputs expose unwanted weak runtime exports; actual Apple linker enforces exact C ABI lists.'}, indent=2) + '\n').encode()
    add('libhackynavi_metal_compiler.dylib', args.compiler_report.parent / 'libhackynavi_metal_compiler.dylib', compiler['native']['dylib']['sha256'])
    add('apple-air-dis', args.reader_report.parent / 'apple-air-dis', reader['native']['apple_air_dis']['sha256'])
    add('spirv-val', ROOT / '.tools/userspace/build-spirv-tools-macos/tools/spirv-val', compiler['source_files']['spirv-val']['sha256'])
    baseline_path = ROOT / 'build/apple-graphics-integration-1/report.json'
    baseline = json.loads(add('fixtures/apple-baseline.json', baseline_path))
    if not baseline['success'] or len(baseline['products']) != 4:
        raise RuntimeError('Actual Apple product evidence incomplete')
    for ext in ('air', 'metallib'):
        expected = {x['input_sha256'] for x in baseline['products'] if x['input_format'] == ext}
        if len(expected) != 1:
            raise RuntimeError('Ambiguous actual input provenance')
        add('fixtures/graphics.' + ext, ROOT / ('userspace/compiler/fixtures/apple-graphics/graphics.' + ext), expected.pop())
    add('fixtures/apple-provenance.json', ROOT / 'userspace/compiler/fixtures/apple-graphics/provenance.json')
    add('fixtures/graphics.metal', ROOT / 'userspace/compiler/fixtures/apple-graphics/triangle.metal', baseline['source_sha256'])
    buffer_baseline = json.loads(add('fixtures/buffer-baseline.json', ROOT / 'build/gfx11-fragment-buffer-tests-4/report.json'))
    buffer_translation = json.loads(add('provenance/buffer-translation.json', ROOT / 'build/apple-fragment-buffer-translation-1/report.json'))
    if not buffer_baseline['success'] or not buffer_translation['success']:
        raise RuntimeError('Missing actual buffer compiler evidence')
    for ext in ('air', 'metallib'):
        expected = next(row['input_sha256'] for row in buffer_translation['products'] if row['format'] == ext)
        add('fixtures/fragment-buffer.' + ext, ROOT / ('userspace/compiler/fixtures/apple-fragment-buffer/fragment-buffer.' + ext), expected)
    add('fixtures/buffer-provenance.json', ROOT / 'userspace/compiler/fixtures/apple-fragment-buffer/provenance.json')
    vertex_baseline = json.loads(add('fixtures/vertex-buffer-baseline.json', ROOT / 'build/gfx11-vertex-buffer-tests-1/report.json'))
    vertex_translation_raw = add('provenance/vertex-buffer-translation.json', ROOT / 'build/apple-vertex-buffer-translation-1/report.json')
    vertex_translation = json.loads(vertex_translation_raw)
    vertex_build_raw = add('provenance/vertex-buffer-windows-build.json', ROOT / 'build/gfx11-vertex-buffer-compiler-2/report.json')
    vertex_build = json.loads(vertex_build_raw)
    if (not vertex_baseline['success'] or not vertex_translation['success'] or not vertex_build['success'] or
            len(vertex_baseline['products']) != 4 or
            vertex_baseline['compiler_report_sha256'] != sha(vertex_build_raw) or
            vertex_baseline['translation_report_sha256'] != sha(vertex_translation_raw)):
        raise RuntimeError('Missing actual vertex buffer compiler evidence')
    for name, expected in vertex_build['source_files'].items():
        if sha((ROOT / name).read_bytes()) != expected:
            raise RuntimeError('Windows vertex buffer source differs from the validated baseline: ' + name)
    provenance = json.loads(add('fixtures/vertex-buffer-provenance.json',
        ROOT / 'userspace/compiler/fixtures/apple-vertex-buffer/provenance.json', vertex_translation['capture_report_sha256']))
    if not provenance['success'] or not provenance['stage_removed'] or not provenance['export_hashes_verified']:
        raise RuntimeError('Genuine Apple vertex buffer capture incomplete')
    for ext in ('air', 'metallib'):
        expected = next(row['input_sha256'] for row in vertex_translation['products'] if row['format'] == ext)
        if expected != provenance['artifacts']['vertex-buffer.' + ext]['sha256']:
            raise RuntimeError('Actual Apple input provenance differs from translated vertex input')
        add('fixtures/vertex-buffer.' + ext,
            ROOT / ('userspace/compiler/fixtures/apple-vertex-buffer/vertex-buffer.' + ext), expected)
    add('fixtures/vertex-buffer.metal', ROOT / 'userspace/compiler/fixtures/apple-vertex-buffer/vertex-buffer.metal',
        provenance['source_sha256'])
    # These are actual SPIRV-Tools-validated unsupported input programs, not byte-corrupt positives.
    add('fixtures/unsupported-vertex.spv', ROOT / 'build/gfx11-ngg-tests-3/attribute_input/shader.spv')
    add('fixtures/unsupported-fragment.spv', ROOT / 'build/gfx11-fragment-tests-4/discard/shader.spv')
    bad_buffer = next(row for row in buffer_baseline['negative_inputs'] if row['label'] == 'dynamic_offset')
    add('fixtures/unsupported-fragment_buffer.spv', ROOT / 'build/gfx11-fragment-buffer-tests-4/reject-dynamic_offset/shader.spv', bad_buffer['spirv_sha256'])
    for item in vertex_baseline['negative_inputs']:
        label = item['label']
        if not re.fullmatch(r'[a-z0-9_]{1,64}', label):
            raise RuntimeError('Unexpected negative vertex program label')
        add('fixtures/unsupported-vertex-buffer-' + label + '.spv',
            ROOT / ('build/gfx11-vertex-buffer-tests-1/reject-' + label + '/shader.spv'), item['spirv_sha256'])
    color = json.loads(add('provenance/color-windows.json', ROOT / 'build/gfx11-color-target-local-6/report.json'))
    if not color['success']:
        raise RuntimeError('Missing actual color baseline')
    add('fixtures/actual-layouts.bin', ROOT / 'build/gfx11-color-target-local-6/actual-layouts.bin', color['artifacts']['actual-layouts.bin'])
    for name in ('verify-ngg.py', 'verify-fragment.py', 'verify-fragment-buffer.py',
                 'verify-vertex-buffer.py', 'test-graphics-darwin.py'):
        add('scripts/' + name, ROOT / 'userspace/compiler' / name)
    for name in ('prepare-native.py', 'run_native.py'):
        add('scripts/' + name, ROOT / 'userspace/graphics' / name)
    for path in [ROOT / 'LICENSE', ROOT / 'userspace/compiler/NOTICE.md',
                 *sorted((ROOT / 'userspace/compiler/licenses').rglob('*'))]:
        if path.is_file():
            add('licenses/' + path.relative_to(ROOT).as_posix(), path)
    if args.native_relink:
        for name in ('LICENSE', 'lib/libcxx/LICENSE.TXT', 'lib/libcxxabi/LICENSE.TXT'):
            add('licenses/zig/' + name, ROOT / '.tools/zig-x86_64-windows-0.17.0' / name)
    size = sum(len(raw) for raw in data.values())
    if size > (208 if args.native_relink else 90) * 1024 * 1024:
        raise RuntimeError('Native snapshot exceeds selected budget')
    rows = {name: {'sha256': sha(raw), 'bytes': len(raw)} for name, raw in sorted(data.items())}
    data['manifest.json'] = (json.dumps({'files': rows, 'bytes': size}, indent=2) + '\n').encode()
    output.mkdir(parents=True)
    archive_path = output / 'inputs.tar.gz'
    with tarfile.open(archive_path, 'w:gz', compresslevel=6) as tar:
        for name, raw in sorted(data.items()):
            info = tarfile.TarInfo(name)
            info.size = len(raw)
            info.mode = 0o755 if '/' not in name and not name.endswith(('.json', '.dylib')) else 0o644
            info.mtime = 0
            tar.addfile(info, io.BytesIO(raw))
    storage_cap = (256 if args.native_relink else 160) * 1024 * 1024
    # Bootstrap holds the compressed input only until verified extraction has
    # completed. Reserve space for that actual peak, including its small runner.
    extraction_peak = size + len(data['manifest.json']) + archive_path.stat().st_size + 256 * 1024
    if extraction_peak > storage_cap:
        raise RuntimeError('Compressed input plus verified extraction exceeds the remote stage budget')
    unchanged = all(path.read_bytes() == data[name] for name, path in origins.items())
    report = {'prepared': True, 'success': unchanged, 'remote_executed': False, 'gpu_executed': False,
              'source_files': rows, 'source_bytes': size, 'archive_bytes': archive_path.stat().st_size,
              'archive_sha256': sha(archive_path.read_bytes()), 'inputs_unchanged': unchanged,
              'native_command': 'python3 <stage>/scripts/run_native.py <stage>',
              'native_stage_prefix': '/Users/<owner>/Library/Caches/HackyNavi-GraphicsNative.',
              'remote_storage_cap_bytes': storage_cap, 'extraction_peak_budget_bytes': extraction_peak,
              'native_relink': args.native_relink, 'native_execution_pending': True,
              'expected_compiler_products': 16, 'expected_graphics_exports': sorted(exports),
              'expected_vertex_negative_inputs': len(vertex_baseline['negative_inputs'])}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not unchanged:
        raise RuntimeError('Source changed while preparing immutable snapshot')
    print(json.dumps({'prepared': True, 'bytes': size, 'archive_bytes': report['archive_bytes']}))


if __name__ == '__main__':
    main()
