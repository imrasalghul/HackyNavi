#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Inspect the real immutable fourth-adapter Darwin snapshot without executing it."""
import argparse
import ast
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[2]
FILES = ['userspace/compiler/build-graphics-darwin.py', 'userspace/compiler/test-graphics-darwin.py',
         'userspace/graphics/prepare-native.py', 'userspace/graphics/run_native.py',
         'userspace/graphics/validate-native.py', 'userspace/graphics/check-vertex-native-preparation.py']
REFERENCES = ['userspace/compiler/darwin_artifacts.py', 'userspace/compiler/verify-vertex-buffer.py',
              'userspace/metal/bundle_format.py']


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cross', required=True, type=Path)
    parser.add_argument('--prepared', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    if out.exists():
        parser.error('Use fresh evidence output')
    out.mkdir(parents=True)
    report = {'success': False, 'checks': 0, 'remote_executed': False, 'gpu_executed': False,
              'native_execution_pending': True, 'source_files': {p: sha((ROOT / p).read_bytes()) for p in FILES + REFERENCES},
              'scope': 'Actual snapshot, source/dependency hashes, Mach-O targets/exports and genuine fixture identities only'}

    def check(value, description):
        report['checks'] += 1
        if not value:
            raise AssertionError(description)

    try:
        for path in FILES:
            ast.parse((ROOT / path).read_text(), path)
        cross_raw, prepared_raw = (args.cross / 'report.json').read_bytes(), (args.prepared / 'report.json').read_bytes()
        cross, prepared = json.loads(cross_raw), json.loads(prepared_raw)
        report.update(cross_report_sha256=sha(cross_raw), preparation_report_sha256=sha(prepared_raw))
        check(cross['success'] and prepared['success'] and prepared['native_relink'], 'Completed cross build and native-relink preparation')
        for path, digest in cross['inputs'].items():
            check(sha((ROOT / path).read_bytes()) == digest, 'Cross-build input unchanged: ' + path)
        for values in cross['dependencies'].values():
            for path, item in values.items():
                check(sha((ROOT / path).read_bytes()) == item['sha256'], 'Actual compiled dependency unchanged: ' + path)
        archive = (args.prepared / 'inputs.tar.gz').read_bytes()
        check(sha(archive) == prepared['archive_sha256'], 'Immutable archive identity')
        data = {}
        with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as tar:
            members = tar.getmembers()
            check(len(members) == len(prepared['source_files']) + 1, 'No missing or duplicate snapshot members')
            check({m.name for m in members} == set(prepared['source_files']) | {'manifest.json'}, 'Exact snapshot membership')
            for member in members:
                check(member.isfile() and not Path(member.name).is_absolute() and '..' not in Path(member.name).parts,
                      'Regular confined snapshot member')
                raw = tar.extractfile(member).read()
                if member.name != 'manifest.json':
                    expected = prepared['source_files'][member.name]
                    check(len(raw) == expected['bytes'] and sha(raw) == expected['sha256'], 'Actual member hash and extent')
                data[member.name] = raw
        manifest = json.loads(data['manifest.json'])
        check(manifest['files'] == prepared['source_files'], 'Remote manifest binds the same exact inputs')
        peak = sum(map(len, data.values())) + len(archive) + 256 * 1024
        check(peak == prepared['extraction_peak_budget_bytes'] <= 256 * 1024 * 1024, 'Compressed/extracted peak fits unchanged stage cap')
        report['extraction_peak_budget_bytes'] = peak
        exports = set(cross['expected_graphics_exports'])
        check(len(exports) == 6 and set(data['relink/graphics.exports'].decode().splitlines()) == exports,
              'Exact six-symbol native export list')
        check('relink/vertex-buffer_shared.o' in data and 'hackynavi-aco-vertex-buffer' in data,
              'Actual fourth adapter CLI and shared object included')
        check('libHackyNaviGraphicsCompiler.dylib' not in data and 'libHackyNaviColorLayout.dylib' not in data,
              'Cross dylibs requiring native relink are excluded')
        macho = load('hn_darwin_snapshot_format', ROOT / 'userspace/compiler/darwin_artifacts.py')
        for name in ['hackynavi-aco-ngg', 'hackynavi-aco-fragment', 'hackynavi-aco-fragment-buffer',
                     'hackynavi-aco-vertex-buffer', 'color-layout-tests', 'apple-air-dis', 'spirv-val']:
            # Existing verified helper can have an older macOS minimum; all new
            # graphics products are required to carry the explicit15.0 target.
            if name not in ('apple-air-dis', 'spirv-val'):
                check(macho.macho(data[name], 2)['architecture'] == 'x86_64', 'Actual Darwin graphics executable target')
        check(macho.macho(data['relink/vertex-buffer_shared.o'], 1)['architecture'] == 'x86_64', 'Actual Darwin shared adapter object')
        bundle = load('hn_darwin_snapshot_exports', ROOT / 'userspace/metal/bundle_format.py')
        image = bundle.macho((args.cross / 'libHackyNaviGraphicsCompiler.dylib').read_bytes())
        authored = {name for name in image['exports'] if name.startswith('_hn_')}
        check(authored == exports, 'Actual cross image contains all and only six authored C ABI exports')
        report['cross_image_exports'] = image['exports']
        report['extra_cross_exports_require_native_relink'] = sorted(set(image['exports']) - exports)
        baseline = json.loads(data['fixtures/vertex-buffer-baseline.json'])
        translation = json.loads(data['provenance/vertex-buffer-translation.json'])
        provenance = json.loads(data['fixtures/vertex-buffer-provenance.json'])
        check(baseline['success'] and translation['success'] and provenance['success'], 'Actual fixture evidence succeeded')
        check(len(baseline['products']) == 4 and len(baseline['negative_inputs']) == 22, 'Four genuine Windows products and22 negative programs')
        verifier = load('hn_vertex_snapshot_verifier', ROOT / 'userspace/compiler/verify-vertex-buffer.py')
        for row in baseline['products']:
            path = ROOT / ('build/gfx11-vertex-buffer-tests-1/' + row['input_format'] + '-' + row['route'] + '/shader.bin')
            code = path.read_bytes()
            check(sha(code) == row['code_sha256'] and verifier.verify(code, row['metadata'])['success'],
                  'Actual independently generated Windows machine product')
        for row in translation['products']:
            name = 'vertex-buffer.' + row['format']
            check(sha(data['fixtures/' + name]) == row['input_sha256'] == provenance['artifacts'][name]['sha256'],
                  'Genuine captured Apple input equals independently translated input')
            binding = row['reflection']['bindings'][0]
            check(binding['declared_size'] == 16 and binding['extent'] == 'Unbounded', 'No invented48-byte reflection declaration')
        for row in baseline['negative_inputs']:
            check(sha(data['fixtures/unsupported-vertex-buffer-' + row['label'] + '.spv']) == row['spirv_sha256'],
                  'Exact valid unsupported SPIR-V staged')
        check(len(baseline['actual_mesa_descriptors']) == 6, 'Six actual Mesa descriptor references staged')
        check(prepared['expected_compiler_products'] == 16 and prepared['expected_vertex_negative_inputs'] == 22,
              'Native validation requires all profiles and negative programs')
        for name, source in [('scripts/test-graphics-darwin.py', FILES[1]), ('scripts/prepare-native.py', FILES[2]),
                             ('scripts/run_native.py', FILES[3])]:
            check(data[name] == (ROOT / source).read_bytes(), 'Exact current native harness staged')
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha((ROOT / p).read_bytes()) == digest for p, digest in report['source_files'].items())
        report['success'] &= report['inputs_unchanged']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'success': report['success'], 'checks': report['checks'],
                      'report_sha256': sha((out / 'report.json').read_bytes())}))


if __name__ == '__main__':
    main()
