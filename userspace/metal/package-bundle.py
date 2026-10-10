#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Package verified native artifacts locally. Never install, sign, load or execute."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import plistlib
import tarfile
import tomllib

from bundle_format import validate_image, GRAPHICS_LIBRARIES, GRAPHICS_EXPORTS, COLOR_EXPORTS

ROOT = Path(__file__).resolve().parents[2]
MAX_ARCHIVE = 256 * 1024 * 1024
BUNDLE = 'HackyNaviMetal.bundle'


def sha(value):
    return hashlib.sha256(value).hexdigest()


def safe_name(name):
    path = PurePosixPath(name)
    if (not name or path.is_absolute() or '\\' in name or ':' in name
            or any(part in ('', '.', '..') for part in name.split('/'))):
        raise ValueError('Unsafe package member: ' + name)
    return name


def read(path, maximum=MAX_ARCHIVE):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > maximum:
        raise ValueError('Expected bounded regular input: ' + str(path))
    value = path.read_bytes()
    if len(value) > maximum:
        raise ValueError('Input grew beyond bound')
    return value


def report(path):
    value = read(path, 8 * 1024 * 1024)
    data = json.loads(value)
    if not all(data.get(key) is True for key in ('success', 'artifacts_exported', 'remote_stage_removed')):
        raise ValueError('Incomplete native evidence: ' + str(path))
    native = data.get('native', {})
    if not native.get('success') or not native.get('complete') or not native.get('child_processes_reaped'):
        raise ValueError('Unconfirmed native completion')
    if any(command.get('exit_code') != 0 for command in native.get('commands', [])):
        raise ValueError('Failed native command')
    return data, value


def inputs(path, evidence, *, embedded_manifest=False):
    raw = read(path)
    if sha(raw) != evidence['archive_sha256']:
        raise ValueError('Input archive hash mismatch')
    expected = evidence['source_files']
    if sum(item['bytes'] for item in expected.values()) > MAX_ARCHIVE:
        raise ValueError('Input manifest exceeds bound')
    result = {}
    manifest = None
    with tarfile.open(fileobj=io.BytesIO(raw), mode='r:gz') as archive:
        for item in archive:
            name = safe_name(item.name)
            if embedded_manifest and name == 'manifest.json':
                if manifest is not None or not item.isfile() or item.size > 1024 * 1024:
                    raise ValueError('Invalid embedded manifest')
                manifest = json.loads(archive.extractfile(item).read())
                continue
            if name not in expected or name in result or not item.isfile() or item.size != expected[name]['bytes']:
                raise ValueError('Input archive member mismatch')
            value = archive.extractfile(item).read()
            if len(value) != item.size or sha(value) != expected[name]['sha256']:
                raise ValueError('Input member hash mismatch: ' + name)
            result[name] = value
    if set(result) != set(expected):
        raise ValueError('Missing input members')
    if embedded_manifest and (manifest is None or manifest['files'] != expected or
                              manifest['bytes'] != sum(len(v) for v in result.values())):
        raise ValueError('Embedded manifest does not match native evidence')
    return result


def graphics_sources(staged, source, files):
    """Retain the exact authored/generated source inputs behind the real relink."""
    raw = staged['provenance/darwin-build.json']
    build = json.loads(raw)
    if not build['success'] or not build['inputs_unchanged'] or not build['dependencies_unchanged']:
        raise ValueError('Graphics cross-build provenance incomplete')
    if build['mesa_commit'] != json.loads(source['userspace/compiler/source-manifest.json'])['commit']:
        raise ValueError('Graphics Mesa source revision differs')
    wanted = {name: digest for name, digest in build['inputs'].items()
              if name.startswith(('userspace/', 'include/', 'src/', 'tests/'))}
    for dependencies in build['dependencies'].values():
        for name, entry in dependencies.items():
            if entry['kind'] != 'toolchain_header':
                if name in wanted and wanted[name] != entry['sha256']:
                    raise ValueError('Conflicting graphics source provenance')
                wanted[name] = entry['sha256']
    for name, digest in wanted.items():
        value = read(ROOT / safe_name(name), 16 * 1024 * 1024)
        if sha(value) != digest or name in source and source[name] != value:
            raise ValueError('Graphics source differs from compiled input: ' + name)
        source[name] = value
    for name, value in staged.items():
        if name.startswith(('scripts/', 'fixtures/')):
            source['graphics-native-validation/' + name] = value
        elif name.startswith('licenses/'):
            files['Contents/Resources/Licenses/Graphics/' + name[len('licenses/'):]] = value
        elif name.startswith('provenance/') or name == 'relink.json' or name.endswith('.exports'):
            files['Contents/Resources/Provenance/Graphics/' + name] = value
    files['Contents/Resources/Provenance/Graphics/SOURCE-SCOPE.txt'] = (
        b'Exact existing Darwin Mesa archives were reused and all object targets/hashes checked.\n'
        b'The included source slice and fresh adapter/generated dependencies are hash-bound.\n'
        b'Reconstruction of every reused archive from these sources was not re-proven in this build.\n'
        b'Matching Zig libc++/libc++abi/compiler-runtime bytes are privately linked; their licenses are retained.\n'
        b'The color library uses actual Mesa __umoddi3 and omits the conflicting compiler-runtime object.\n'
        b'Toolchains and Apple frameworks remain external prerequisites.\n')


def artifact(path, record):
    value = read(path, 64 * 1024 * 1024)
    if len(value) != record['bytes'] or sha(value) != record['sha256']:
        raise ValueError('Artifact differs from native report: ' + str(path))
    return value


def tar_bytes(files, modes=None):
    if sum(map(len, files.values())) > MAX_ARCHIVE:
        raise ValueError('Archive content exceeds bound')
    raw = io.BytesIO()
    # Explicit archive modes survive creation on Windows. No source symlinks,
    # absolute paths, owner IDs, timestamps or filesystem xattrs are imported.
    with gzip.GzipFile(fileobj=raw, mode='wb', mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as archive:
            directories = set()
            for name in files:
                safe_name(name)
                directories.update(str(p) for p in PurePosixPath(name).parents if str(p) != '.')
            for name in sorted(directories):
                entry = tarfile.TarInfo(name); entry.type = tarfile.DIRTYPE; entry.mode = 0o755
                archive.addfile(entry)
            for name, value in sorted(files.items()):
                entry = tarfile.TarInfo(name); entry.size = len(value)
                entry.mode = (modes or {}).get(name, 0o644)
                archive.addfile(entry, io.BytesIO(value))
    return raw.getvalue()


def compiler_sources(evidence, frontend_inputs):
    source = {p: v for p, v in frontend_inputs.items() if p.startswith(('userspace/', 'include/', 'src/'))}
    for name, digest in evidence['compiler_source_sha256'].items():
        value = read(ROOT / safe_name(name), 8 * 1024 * 1024)
        if sha(value) != digest:
            raise ValueError('Current compiler source differs from actual artifact: ' + name)
        source[name] = value
    # Keep the complete retained LGPL source subset and its build/license files,
    # including files not compiled into this target, with local changes visible.
    manifest_path = ROOT / 'userspace/metal/vendor/source-manifest.json'
    source['userspace/metal/vendor/source-manifest.json'] = read(manifest_path)
    for path in sorted((ROOT / 'userspace/metal/vendor/metal2vulkan').rglob('*')):
        if path.is_file():
            name = path.relative_to(ROOT).as_posix()
            if '/target/' in name or '/.git/' in name:
                continue
            value = read(path, 8 * 1024 * 1024)
            if name in source and source[name] != value:
                raise ValueError('Compiler snapshot changed while collecting source')
            source[name] = value
    # Exact locked Cargo registry source archives provide the complete source
    # for the compiler's permissively licensed dependencies, not just notices.
    lock = tomllib.loads(source['userspace/metal/Cargo.lock'].decode())
    cache = ROOT / '.tools/userspace/cargo/registry/cache/index.crates.io-1949cf8c6b5b557f'
    for package in lock['package']:
        if 'source' not in package:
            continue
        name = package['name'] + '-' + package['version'] + '.crate'
        value = read(cache / name, 16 * 1024 * 1024)
        if sha(value) != package['checksum']:
            raise ValueError('Locked Cargo source checksum mismatch: ' + name)
        source['cargo-registry-source/' + name] = value
    return source


def licenses(files, source):
    paths = ['LICENSE', 'userspace/metal/NOTICE.md', 'userspace/metal/EXPLICIT-TOOLS.md',
             'userspace/metal/vendor/LOCAL-CHANGES.md', 'userspace/metal/vendor/metal2vulkan/LICENSE',
             'userspace/metal/vendor/metal2vulkan/COPYING', 'userspace/compiler/NOTICE.md',
             'userspace/compiler/licenses/mesa-license.rst', 'userspace/transport/Cobalt64-NOTICE.txt']
    paths += [p.relative_to(ROOT).as_posix() for p in sorted((ROOT / 'userspace/compiler/licenses/mesa').rglob('*')) if p.is_file()]
    manifest = read(ROOT / 'userspace/metal/licenses/manifest.json')
    source['userspace/metal/licenses/manifest.json'] = manifest
    packages = json.loads(manifest)
    lock = tomllib.loads(source['userspace/metal/Cargo.lock'].decode())
    listed = {(p['name'], p['version']) for p in packages}
    if any((p['name'], p['version']) not in listed for p in lock['package'] if 'source' in p):
        raise ValueError('Cargo license manifest omits a locked dependency')
    for package in packages:
        for name, digest in package['files'].items():
            path = 'userspace/metal/' + safe_name(name)
            value = read(ROOT / path)
            if sha(value) != digest:
                raise ValueError('Cargo license changed: ' + path)
            source[path] = value; files['Contents/Resources/Licenses/' + path] = value
    for name in paths:
        value = read(ROOT / name); source[name] = value
        files['Contents/Resources/Licenses/' + name] = value
    for label, name in [('SPIRV-Tools', '.tools/userspace/src/SPIRV-Tools-2025.2/LICENSE'),
                        ('SPIRV-Headers', '.tools/userspace/src/SPIRV-Headers-aa6cef192b8e693916eb713e7a9ccadf06062ceb/LICENSE')]:
        files['Contents/Resources/Licenses/' + label + '/LICENSE'] = read(ROOT / name)
    runtime_notices = [
        ('Rust-1.90/COPYRIGHT-library.html', '.tools/userspace/rustup/toolchains/1.90.0-x86_64-pc-windows-gnu/share/doc/rust/COPYRIGHT-library.html'),
        ('Zig-0.17/LICENSE', '.tools/zig-x86_64-windows-0.17.0/LICENSE'),
        ('LLVM-libcxx/LICENSE.TXT', '.tools/zig-x86_64-windows-0.17.0/lib/libcxx/LICENSE.TXT'),
        ('LLVM-libcxxabi/LICENSE.TXT', '.tools/zig-x86_64-windows-0.17.0/lib/libcxxabi/LICENSE.TXT'),
        ('LLVM-libunwind/LICENSE.TXT', '.tools/zig-x86_64-windows-0.17.0/lib/libunwind/LICENSE.TXT')]
    provenance = {}
    for label, name in runtime_notices:
        value = read(ROOT / name)
        files['Contents/Resources/Licenses/' + label] = value
        provenance[label] = {'source_path': name, 'sha256': sha(value), 'bytes': len(value)}
    files['Contents/Resources/Provenance/runtime-notices.json'] = json.dumps(provenance, indent=2).encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('frontend-report', 'compiler-report', 'reader-report', 'aco-report', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--graphics-report', type=Path,
                        help='Required only for an actual graphics frontend; native compiler/layout evidence')
    parser.add_argument('--graphics-runtime-report', type=Path,
                        help='Required with graphics; actual source-to-pipeline native sanitizer evidence')
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error('Choose a fresh empty output directory')
    front, front_raw = report(args.frontend_report)
    graphics = front.get('graphics', False)
    if (not isinstance(graphics, bool) or graphics != bool(args.graphics_report) or
            graphics != bool(args.graphics_runtime_report)):
        raise ValueError('Graphics frontend and both graphics evidence arguments must agree')
    compiler, compiler_raw = report(args.compiler_report)
    reader, reader_raw = report(args.reader_report)
    if not front.get('device_layer') or not front['native'].get('device_layer'):
        raise ValueError('Full actual frontend required; component library is insufficient')
    if not reader.get('helper_only') or reader.get('compiler_reused_from_report_sha256') != sha(compiler_raw):
        raise ValueError('Expected reviewed helper-only successor of this compiler')
    front_inputs = inputs(args.frontend_report.parent / 'inputs.tar.gz', front)
    reader_inputs = inputs(args.reader_report.parent / 'inputs.tar.gz', reader)
    library = artifact(args.compiler_report.parent / 'libhackynavi_metal_compiler.dylib', compiler['native']['dylib'])
    if front_inputs.get('libhackynavi_metal_compiler.dylib') != library or reader_inputs.get('libhackynavi_metal_compiler.dylib') != library:
        raise ValueError('Frontend/helper did not validate this exact compiler')
    if front_inputs.get('provenance/compiler-report.json') != compiler_raw or reader_inputs.get('prior-compiler-report.json') != compiler_raw:
        raise ValueError('Native input provenance does not bind this compiler report')
    aco_raw = read(args.aco_report, 8 * 1024 * 1024); aco = json.loads(aco_raw)
    if sha(reader_inputs['hackynavi-aco']) != aco['binary_sha256']:
        raise ValueError('Actual validated ACO differs from source build report')
    helper = artifact(args.reader_report.parent / 'apple-air-dis', reader['native']['apple_air_dis'])
    actual = reader['native']['explicit_tools_integration']['tools']
    if any(actual[key]['sha256'] != sha(value) for key, value in [('compiler', library), ('air_dis', helper),
            ('spirv_val', reader_inputs['spirv-val']), ('aco', reader_inputs['hackynavi-aco'])]):
        raise ValueError('Native explicit-tools tests did not use packaged bytes')
    if b'getenv(' in reader_inputs['apple_air_dis.c']:
        raise ValueError('Bundled AIR helper still has an environment-selected reader')
    files = {'Contents/MacOS/HackyNaviMetal': artifact(args.frontend_report.parent / 'HackyNaviMetal', front['native']['artifacts']['HackyNaviMetal']),
             'Contents/Frameworks/libhackynavi_metal_compiler.dylib': library,
             'Contents/Helpers/apple-air-dis': helper,
             'Contents/Helpers/spirv-val': reader_inputs['spirv-val'],
             'Contents/Helpers/hackynavi-aco': reader_inputs['hackynavi-aco']}
    graphics_raw, graphics_inputs, runtime_raw, runtime_inputs = None, None, None, None
    if graphics:
        graphics_evidence, graphics_raw = report(args.graphics_report)
        if not graphics_evidence.get('native_tests_passed') or not graphics_evidence.get('remote_job_finished'):
            raise ValueError('Graphics offline native tests incomplete')
        if front_inputs.get('provenance/graphics-report.json') != graphics_raw:
            raise ValueError('Frontend did not bind this graphics report')
        graphics_inputs = inputs(args.graphics_report.parent / 'inputs.tar.gz', graphics_evidence,
                                 embedded_manifest=True)
        if (graphics_inputs.get('provenance/compiler-native.json') != compiler_raw or
                graphics_inputs.get('provenance/reader-native.json') != reader_raw):
            raise ValueError('Graphics test provenance differs from compiler/reader evidence')
        if (graphics_inputs.get('libhackynavi_metal_compiler.dylib') != library or
                graphics_inputs.get('apple-air-dis') != helper or
                graphics_inputs.get('spirv-val') != reader_inputs['spirv-val']):
            raise ValueError('Graphics validation tools differ from packaged tools')
        for name, expected in zip(GRAPHICS_LIBRARIES, (GRAPHICS_EXPORTS, COLOR_EXPORTS)):
            value = artifact(args.graphics_report.parent / name, graphics_evidence['native']['artifacts'][name])
            if front_inputs.get(name) != value or graphics_evidence['native']['exports'][name] != expected:
                raise ValueError('Frontend graphics dependency differs from exact native validation')
            files['Contents/Frameworks/' + name] = value
        for name in ('hackynavi-aco-ngg', 'hackynavi-aco-fragment', 'hackynavi-aco-fragment-buffer', 'hackynavi-aco-vertex-buffer'):
            value = artifact(args.graphics_report.parent / name, graphics_evidence['native']['artifacts'][name])
            if graphics_inputs.get(name) != value:
                raise ValueError('Graphics helper differs from native test input')
            files['Contents/Helpers/' + name] = value
        runtime_evidence, runtime_raw = report(args.graphics_runtime_report)
        if (not runtime_evidence.get('native_tests_passed') or not runtime_evidence.get('remote_job_finished') or
                '96 checks, 0 failures, 8 actual compiler-backed graphics pipelines;' not in runtime_evidence['native'].get('test_output', '')):
            raise ValueError('Graphics runtime native integration incomplete')
        runtime_inputs = inputs(args.graphics_runtime_report.parent / 'inputs.tar.gz', runtime_evidence,
                                embedded_manifest=True)
        if (runtime_inputs.get('provenance/graphics.json') != graphics_raw or
                runtime_inputs.get('provenance/compiler.json') != compiler_raw or
                runtime_inputs.get('provenance/reader.json') != reader_raw):
            raise ValueError('Graphics runtime did not validate these exact native dependencies')
        for name, value in runtime_inputs.items():
            if name in front_inputs and value != front_inputs[name]:
                raise ValueError('Graphics runtime and frontend used different input: ' + name)
        for name in GRAPHICS_LIBRARIES + ['libhackynavi_metal_compiler.dylib']:
            if runtime_inputs.get(name) != files['Contents/Frameworks/' + name]:
                raise ValueError('Runtime tested a different graphics/compiler binary')
        for name in ('hackynavi-metal-compute-smoke', 'hackynavi-metal-render-smoke', 'hackynavi-metal-render-buffer-smoke',
                     'hackynavi-metal-render-vertex-buffer-smoke'):
            files['Contents/Resources/Tests/' + name] = artifact(
                args.frontend_report.parent / name, front['native']['artifacts'][name])
    modes = {name: 0o755 for name in files}
    images = {}
    for name, value in files.items():
        role = ('frontend' if '/MacOS/' in name else 'graphics_compiler' if name.endswith(GRAPHICS_LIBRARIES[0])
                else 'color_layout' if name.endswith(GRAPHICS_LIBRARIES[1])
                else 'compiler' if '/Frameworks/' in name else 'helper')
        images[name] = validate_image(value, role, ['_' + x for x in compiler['exports']] if role == 'compiler' else None,
                                      graphics=graphics)
    if graphics:
        tests = 'Contents/Resources/Tests/'
        files[tests + 'graphics.metallib'] = graphics_inputs['fixtures/graphics.metallib']
        files[tests + 'graphics-provenance.json'] = graphics_inputs['fixtures/apple-provenance.json']
        files[tests + 'fragment-buffer.metallib'] = graphics_inputs['fixtures/fragment-buffer.metallib']
        files[tests + 'buffer-provenance.json'] = graphics_inputs['fixtures/buffer-provenance.json']
        files[tests + 'vertex-buffer.metallib'] = graphics_inputs['fixtures/vertex-buffer.metallib']
        files[tests + 'vertex-buffer-provenance.json'] = graphics_inputs['fixtures/vertex-buffer-provenance.json']
        files[tests + 'add_one.metallib'] = reader_inputs['tests/fixtures/metallib/apple-32023-macos15-add-one.metallib']
        files[tests + 'add-one-provenance.json'] = reader_inputs['tests/fixtures/metallib/apple-32023-macos15-provenance.json']
        if (sha(files[tests + 'graphics.metallib']) != 'b99ae2fa65e9cfda2e13c146434fee1f0c0ef0cd2956beeba1cec01750641197' or
                len(files[tests + 'graphics.metallib']) != 6354):
            raise ValueError('Render tool requires the exact captured Apple graphics fixture')
        if (sha(files[tests + 'fragment-buffer.metallib']) != 'd5feaa1df63810fbb0ee3013497d01cb814f7cbb02a99c32ea25867859acdefd' or
                len(files[tests + 'fragment-buffer.metallib']) != 3496):
            raise ValueError('Buffer render tool requires the exact captured Apple buffer fixture')
        if (sha(files[tests + 'vertex-buffer.metallib']) != 'a98e9a44782d8de99124e71ac5ff52477880005471ff2029648b3d5da9cd43f4' or
                len(files[tests + 'vertex-buffer.metallib']) != 3542):
            raise ValueError('Vertex render tool requires the exact captured Apple vertex-buffer fixture')
        compute_fixture = json.loads(files[tests + 'add-one-provenance.json'])['artifacts']['add_one.metallib']
        if (sha(files[tests + 'add_one.metallib']) != compute_fixture['sha256'] or
                len(files[tests + 'add_one.metallib']) != compute_fixture['bytes']):
            raise ValueError('Compute fixture differs from real Apple compilation provenance')
        files[tests + 'RENDER-SMOKE.md'] = read(ROOT / 'userspace/metal/native/RENDER-SMOKE.md')
        files[tests + 'FRAGMENT-BUFFER-SMOKE.md'] = read(ROOT / 'docs/metal-fragment-buffer-render.md')
        files[tests + 'VERTEX-BUFFER-SMOKE.md'] = read(ROOT / 'docs/metal-vertex-buffer-render.md')
        files[tests + 'README.txt'] = (
            b'Target-only tests; never run on the build Mac. Packaging does not execute them.\n'
            b'Run only after the matching driver is loaded and the frontend is installed/discoverable on Navi31.\n'
            b'ACCELERATOR_ID is the current public Metal registry ID of HackyNaviAccelerator, not its parent owner ID.\n'
            b'No tool falls back to another Metal device; --run is mandatory.\n'
            b'Copy this Tests directory to a writable user directory before running.\n'
            b'From this Tests directory, replace ACCELERATOR_ID with the actual target value:\n\n'
            b'./hackynavi-metal-compute-smoke --run --registry ACCELERATOR_ID --metallib "$PWD/add_one.metallib" > compute-report.txt 2> compute-errors.txt\n'
            b'./hackynavi-metal-render-smoke --run --registry ACCELERATOR_ID --metallib "$PWD/graphics.metallib" > render-report.json 2> render-errors.txt\n'
            b'./hackynavi-metal-render-buffer-smoke --run --registry ACCELERATOR_ID --vertex-metallib "$PWD/graphics.metallib" --fragment-metallib "$PWD/fragment-buffer.metallib" > render-buffer-report.json 2> render-buffer-errors.txt\n\n'
            b'./hackynavi-metal-render-vertex-buffer-smoke --run --registry ACCELERATOR_ID --vertex-metallib "$PWD/vertex-buffer.metallib" --constant-metallib "$PWD/graphics.metallib" --fragment-metallib "$PWD/fragment-buffer.metallib" > render-vertex-buffer-report.json 2> render-vertex-buffer-errors.txt\n\n'
            b'Compute checks real Managed A->B->C results, 4099 elements/local64, 64 guard words and callbacks.\n'
            b'Render checks one actual offscreen 64x64 RGBA8 triangle, readback, orientation and guards.\n'
            b'Buffer render checks actual fragment-buffer colors, retained binding offsets and GPU readback.\n'
            b'Vertex-buffer render checks two uploaded shapes through both vertex-only and combined buffer pipelines.\n'
            b'Exit0 means that tool completed its actual checks. Preserve stdout, stderr and exit code on failure.\n'
            b'These tools do not verify display scanout or general Metal compatibility. See RENDER-SMOKE.md and FRAGMENT-BUFFER-SMOKE.md.\n')
        files[tests + 'manifest.json'] = json.dumps({
            name[len(tests):]: {'bytes': len(value), 'sha256': sha(value),
                               'archive_mode': oct(modes.get(name, 0o644))}
            for name, value in files.items() if name.startswith(tests)}, indent=2).encode()
    info = {'CFBundleIdentifier': 'org.hackynavi.driver.HackyNaviMetal', 'CFBundleName': 'HackyNaviMetal',
            'CFBundleExecutable': 'HackyNaviMetal', 'CFBundlePackageType': 'BNDL', 'CFBundleInfoDictionaryVersion': '6.0',
            'CFBundleVersion': '0.1.0', 'CFBundleShortVersionString': '0.1.0', 'NSPrincipalClass': 'HNMetalDevice',
            'CFBundleSupportedPlatforms': ['MacOSX'], 'LSMinimumSystemVersion': '15.0'}
    files['Contents/Info.plist'] = plistlib.dumps(info, sort_keys=True)
    source = compiler_sources(compiler, front_inputs)
    source['userspace/metal/tools/apple_air_dis.c'] = reader_inputs['apple_air_dis.c']
    source['userspace/metal/tests/check_apple_reader.py'] = reader_inputs['tests/check_apple_reader.py']
    source['userspace/metal/test-macos-staged.py'] = reader_inputs['test-macos-staged.py']
    source['userspace/metal/tools/run_native_compiler_tests.py'] = reader_inputs['run-native.py']
    for name, value in reader_inputs.items():
        if name.startswith('tests/'):
            source['userspace/metal/' + name] = value
    for name in ('package-bundle.py', 'bundle_format.py', 'BUNDLE-PACKAGING.md', 'tests/check_bundle_packaging.py'):
        source['userspace/metal/' + name] = read(ROOT / 'userspace/metal' / name)
    licenses(files, source)
    # Retain the exact materialized Mesa source slice, including individual
    # copyright notices, and the real Darwin adapter/build description.
    mesa_manifest = read(ROOT / 'userspace/compiler/source-manifest.json')
    mesa = json.loads(mesa_manifest)
    if mesa['commit'] != aco['mesa_commit'] or sha(mesa_manifest) != aco['base_manifest_sha256']:
        raise ValueError('Mesa source pin differs from actual ACO report')
    for item in mesa['files']:
        name = safe_name(item['path']); value = read(ROOT / '.research/mesa' / name, 16 * 1024 * 1024)
        if sha(value) != item['sha256']:
            raise ValueError('Pinned Mesa source mismatch: ' + name)
        source['.research/mesa/' + name] = value
    source['userspace/compiler/source-manifest.json'] = mesa_manifest
    for name, digest in aco['adapter_sources'].items():
        value = read(ROOT / 'userspace/compiler' / safe_name(name))
        if sha(value) != digest:
            raise ValueError('ACO adapter source mismatch: ' + name)
        source['userspace/compiler/' + name] = value
    for name in ('build.py', 'source-manifest-darwin.json'):
        source['userspace/compiler/' + name] = read(ROOT / 'userspace/compiler' / name)
    if graphics:
        graphics_sources(graphics_inputs, source, files)
        fragment_provenance = json.loads(graphics_inputs['fixtures/buffer-provenance.json'])
        fragment_source_path = 'userspace/compiler/fixtures/apple-fragment-buffer/fragment-buffer.metal'
        fragment_source = read(ROOT / fragment_source_path)
        if (fragment_provenance['source_path'] != fragment_source_path or
                sha(fragment_source) != fragment_provenance['source_sha256'] or
                len(fragment_source) != fragment_provenance['source_bytes']):
            raise ValueError('Fragment-buffer MSL differs from genuine Apple capture provenance')
        source[fragment_source_path] = fragment_source
        source['userspace/metal/native/RENDER-SMOKE.md'] = files['Contents/Resources/Tests/RENDER-SMOKE.md']
        source['docs/metal-fragment-buffer-render.md'] = files['Contents/Resources/Tests/FRAGMENT-BUFFER-SMOKE.md']
        source['docs/metal-vertex-buffer-render.md'] = files['Contents/Resources/Tests/VERTEX-BUFFER-SMOKE.md']
        for name, value in runtime_inputs.items():
            if name.startswith(('userspace/', 'include/', 'src/')):
                if name in source and source[name] != value:
                    raise ValueError('Graphics runtime source differs from packaged source')
                source[name] = value
            elif name.startswith(('fixtures/', 'provenance/')):
                source['graphics-native-runtime-validation/' + name] = value
    source['REBUILD.txt'] = (b'Experimental development sources; no device/GPU success is implied.\n'
        b'The frontend native source snapshot and exact patched LGPL translator are included.\n'
        b'Cargo.lock pins dependencies; complete checksum-verified .crate sources are in cargo-registry-source.\n'
        b'The compiler dynamic library can be rebuilt/replaced independently; no signing keys are supplied.\n'
        b'Build procedures: userspace/metal/build-darwin-objects.py, test-macos-staged.py,\n'
        b'userspace/runtime/build-frontend.py, userspace/compiler/build-darwin.py,\n'
        b'and for graphics userspace/compiler/build-graphics-darwin.py.\n'
        b'Apple system frameworks/LLVM and toolchains are external dependencies, not redistributed here.\n')
    files['Contents/Resources/Sources/HackyNaviMetal-source.tar.gz'] = tar_bytes(source)
    for name, raw in [('frontend-report.json', front_raw), ('compiler-report.json', compiler_raw),
                      ('reader-report.json', reader_raw), ('aco-report.json', aco_raw)]:
        files['Contents/Resources/Provenance/' + name] = raw
    if graphics:
        files['Contents/Resources/Provenance/graphics-report.json'] = graphics_raw
        files['Contents/Resources/Provenance/graphics-runtime-report.json'] = runtime_raw
    files['Contents/Resources/Provenance/source-files.json'] = json.dumps({p: {'bytes': len(v), 'sha256': sha(v)} for p, v in source.items()}, indent=2).encode()
    files['Contents/Resources/DEVELOPMENT.txt'] = (b'Experimental HackyNavi Metal development frontend.\n'
        b'Candidate loader location: /Library/GPUBundles/HackyNaviMetal.bundle\n'
        b'This package has not been installed, code-signed, loaded or tested on a GPU by the packager.\n'
        b'Full rendering/display support and compatibility with every Metal application are not established.\n'
        b'No policy changes are included. Loader trust, dyld/library validation and actual discovery remain target tests.\n')
    packed = {BUNDLE + '/' + name: value for name, value in files.items()}
    packed_modes = {BUNDLE + '/' + name: mode for name, mode in modes.items()}
    archive = tar_bytes(packed, packed_modes)
    output.mkdir(parents=True, exist_ok=True)
    for name, value in files.items():
        path = output / BUNDLE / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(value)
        if name in modes:
            path.chmod(modes[name])
    (output / 'HackyNaviMetal-development.tar.gz').write_bytes(archive)
    result = {'success': True, 'claim': 'Local package structure, provenance and native dependency validation only',
              'bundle': BUNDLE, 'candidate_install_path': '/Library/GPUBundles/' + BUNDLE,
              'frontend_report_sha256': sha(front_raw), 'compiler_report_sha256': sha(compiler_raw),
              'reader_report_sha256': sha(reader_raw), 'aco_report_sha256': sha(aco_raw),
              'graphics': graphics, 'graphics_report_sha256': sha(graphics_raw) if graphics else None,
              'graphics_runtime_report_sha256': sha(runtime_raw) if graphics else None,
              'files': {p: {'bytes': len(v), 'sha256': sha(v), 'archive_mode': oct(modes.get(p, 0o644))} for p, v in files.items()},
              'macho': images, 'archive_sha256': sha(archive), 'archive_bytes': len(archive),
              'source_file_count': len(source), 'source_bytes': sum(map(len, source.values())),
              'installed': False, 'policy_modified': False, 'signed_by_packager': False,
              'metal_objects_created': False, 'gpu_executed': False}
    result['packager_sha256'] = sha(source['userspace/metal/package-bundle.py'])
    result['macho_checker_sha256'] = sha(source['userspace/metal/bundle_format.py'])
    (output / 'package-report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ('success', 'archive_bytes', 'archive_sha256', 'source_file_count')}, indent=2))


if __name__ == '__main__':
    main()
