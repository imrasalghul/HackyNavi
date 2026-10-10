#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate a real package and reject corrupt versions of its actual binaries."""
import hashlib
import io
import json
from pathlib import Path
import plistlib
import struct
import sys
import tarfile

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
from bundle_format import validate_image, GRAPHICS_LIBRARIES


def main():
    package, output = map(Path, sys.argv[1:3])
    if output.exists():
        raise ValueError('Choose fresh evidence path')
    report_bytes = (package / 'package-report.json').read_bytes()
    report = json.loads(report_bytes)
    if not report['success']:
        raise ValueError('Package incomplete')
    bundle = package / report['bundle']
    graphics = report.get('graphics', False)
    sha = lambda value: hashlib.sha256(value).hexdigest()
    data = {name: (bundle / name).read_bytes() for name in report['files']}
    for name, value in data.items():
        assert len(value) == report['files'][name]['bytes'] and sha(value) == report['files'][name]['sha256']
    archive_bytes = (package / 'HackyNaviMetal-development.tar.gz').read_bytes()
    assert sha(archive_bytes) == report['archive_sha256']
    with tarfile.open(fileobj=io.BytesIO(archive_bytes), mode='r:gz') as tar:
        seen = set()
        for member in tar:
            assert member.isdir() or member.isfile()
            assert member.name.startswith(report['bundle'] + '/') or member.name == report['bundle']
            assert member.uid == member.gid == member.mtime == 0
            if member.isdir():
                assert member.mode == 0o755
                continue
            name = member.name[len(report['bundle']) + 1:]
            assert name in data and name not in seen
            assert member.mode == int(report['files'][name]['archive_mode'], 8)
            assert tar.extractfile(member).read() == data[name]
            seen.add(name)
        assert seen == set(data)
    info = plistlib.loads(data['Contents/Info.plist'])
    assert info['NSPrincipalClass'] == 'HNMetalDevice' and info['CFBundleExecutable'] == 'HackyNaviMetal'
    assert info['CFBundlePackageType'] == 'BNDL'
    failures = []
    def rejects(label, value, role, exports=None):
        try:
            validate_image(value, role, exports, graphics=graphics)
        except (ValueError, struct.error):
            failures.append({'name': label, 'rejected': True})
        else:
            raise AssertionError('Malformed image accepted: ' + label)
    frontend = data['Contents/MacOS/HackyNaviMetal']
    compiler = data['Contents/Frameworks/libhackynavi_metal_compiler.dylib']
    expected = report['macho']['Contents/Frameworks/libhackynavi_metal_compiler.dylib']['exports']
    for name, value in data.items():
        if name in report['macho']:
            role = ('frontend' if '/MacOS/' in name else 'graphics_compiler' if name.endswith(GRAPHICS_LIBRARIES[0])
                    else 'color_layout' if name.endswith(GRAPHICS_LIBRARIES[1])
                    else 'compiler' if '/Frameworks/' in name else 'helper')
            assert validate_image(value, role, expected if role == 'compiler' else None, graphics=graphics) == report['macho'][name]
    if graphics:
        for name in GRAPHICS_LIBRARIES:
            assert 'Contents/Frameworks/' + name in report['macho']
        assert all('Contents/Helpers/' + name in report['macho'] for name in ('hackynavi-aco-ngg', 'hackynavi-aco-fragment', 'hackynavi-aco-fragment-buffer', 'hackynavi-aco-vertex-buffer'))
        assert report['graphics_report_sha256'] == sha(data['Contents/Resources/Provenance/graphics-report.json'])
        assert report['graphics_runtime_report_sha256'] == sha(data['Contents/Resources/Provenance/graphics-runtime-report.json'])
        tests = 'Contents/Resources/Tests/'
        tools = ('hackynavi-metal-compute-smoke', 'hackynavi-metal-render-smoke', 'hackynavi-metal-render-buffer-smoke',
                 'hackynavi-metal-render-vertex-buffer-smoke')
        actual_tests = json.loads(data[tests + 'manifest.json'])
        assert set(actual_tests) == {n[len(tests):] for n in data if n.startswith(tests) and n != tests + 'manifest.json'}
        for name, entry in actual_tests.items():
            value = data[tests + name]
            assert len(value) == entry['bytes'] and sha(value) == entry['sha256']
            assert entry['archive_mode'] == ('0o755' if name in tools else '0o644')
        assert all(tests + name in report['macho'] for name in tools)
        assert sha(data[tests + 'graphics.metallib']) == 'b99ae2fa65e9cfda2e13c146434fee1f0c0ef0cd2956beeba1cec01750641197'
        assert sha(data[tests + 'fragment-buffer.metallib']) == 'd5feaa1df63810fbb0ee3013497d01cb814f7cbb02a99c32ea25867859acdefd'
        assert sha(data[tests + 'vertex-buffer.metallib']) == 'a98e9a44782d8de99124e71ac5ff52477880005471ff2029648b3d5da9cd43f4'
        assert sha(data[tests + 'add_one.metallib']) == 'ed77099cd3e56ac404331fa68dd9401a7b326a7b7ea80d942b29b8b1fe0af865'
        rejects('graphics library cannot substitute for color layout', data['Contents/Frameworks/' + GRAPHICS_LIBRARIES[0]], 'color_layout')
        rejects('color layout cannot substitute for graphics compiler', data['Contents/Frameworks/' + GRAPHICS_LIBRARIES[1]], 'graphics_compiler')
        try:
            validate_image(frontend, 'frontend', graphics=False)
        except ValueError:
            failures.append({'name': 'graphics frontend cannot use compute-only dependency closure', 'rejected': True})
        else:
            raise AssertionError('Graphics frontend accepted without required libraries')
        for dependency in GRAPHICS_LIBRARIES:
            at = 32; found = False
            for _ in range(struct.unpack_from('<I', frontend, 16)[0]):
                command, size = struct.unpack_from('<II', frontend, at)
                if command == 0xC and ('@rpath/' + dependency).encode() in frontend[at:at + size]:
                    damaged = bytearray(frontend); struct.pack_into('<I', damaged, at, 0)
                    rejects('missing actual graphics dependency: ' + dependency, damaged, 'frontend'); found = True
                at += size
            assert found
        for role, name in [('graphics_compiler', GRAPHICS_LIBRARIES[0]), ('color_layout', GRAPHICS_LIBRARIES[1])]:
            actual = data['Contents/Frameworks/' + name]
            identity = ('@rpath/' + name).encode(); assert identity in actual
            rejects('wrong actual install identity: ' + name, actual.replace(identity, b'@oopsx/' + name.encode(), 1), role)
    rejects('truncated actual header', frontend[:31], 'frontend')
    rejects('actual bundle used as helper', frontend, 'helper')
    rejects('actual compiler export omitted from contract', compiler, 'compiler', expected[:-1])
    damaged = bytearray(frontend); struct.pack_into('<I', damaged, 4, 0x0100000C)
    rejects('wrong target architecture', damaged, 'frontend')
    damaged = bytearray(frontend); struct.pack_into('<I', damaged, 20, len(frontend))
    rejects('load commands exceed file', damaged, 'frontend')
    damaged = bytearray(frontend); struct.pack_into('<I', damaged, 36, 0)
    rejects('zero command length', damaged, 'frontend')
    for old, new, label in [(b'@loader_path/../Frameworks', b'@executable_path/../Oopsxx', 'host-controlled rpath'),
                            (b'@rpath/libhackynavi_metal_compiler.dylib', b'@rpath/libwrongxxxx_metal_compiler.dylib', 'unpackaged dependency')]:
        assert len(old) == len(new) and old in frontend
        rejects(label, frontend.replace(old, new, 1), 'frontend')
    at = 32
    version = None
    for _ in range(struct.unpack_from('<I', frontend, 16)[0]):
        command, size = struct.unpack_from('<II', frontend, at)
        if command == 0x32:
            assert version is None and size == 32
            version = at
        at += size
    assert version is not None
    for label, offset, value in [('iOS platform on actual x86_64 bundle', 8, 2),
                                  ('Mac Catalyst platform', 8, 6),
                                  ('missing explicit target command', 0, 0),
                                  ('zero minimum OS', 12, 0),
                                  ('newer minimum OS', 12, 0x00100000),
                                  ('build-tool count exceeds command', 20, 0xFFFFFFFF),
                                  ('unclaimed build-tool record', 20, 0)]:
        damaged = bytearray(frontend); struct.pack_into('<I', damaged, version + offset, value)
        rejects(label, damaged, 'frontend')
    # A real-image metadata mutation exercises the supported legacy macOS
    # layout without changing file offsets; this is parser evidence only.
    legacy = bytearray(frontend)
    minimum, sdk = struct.unpack_from('<II', frontend, version + 12)
    struct.pack_into('<4I', legacy, version, 0x24, 16, minimum, sdk)
    struct.pack_into('<4I', legacy, version + 16, 0, 16, 0, 0)
    struct.pack_into('<I', legacy, 16, struct.unpack_from('<I', frontend, 16)[0] + 1)
    observed = validate_image(legacy, 'frontend', graphics=graphics)
    assert observed['platform'] == 1 and observed['version_command'] == 0x24
    assert observed['minimum_os_raw'] == minimum and observed['build_tools'] == []
    damaged = bytearray(legacy); struct.pack_into('<I', damaged, version, 0x25)
    rejects('legacy iOS minimum target', damaged, 'frontend')
    damaged = bytearray(legacy); struct.pack_into('<4I', damaged, version + 16, 0x24, 16, minimum, sdk)
    rejects('duplicate target commands', damaged, 'frontend')
    source_manifest = json.loads(data['Contents/Resources/Provenance/source-files.json'])
    if graphics:
        graphics_evidence = json.loads(data['Contents/Resources/Provenance/graphics-report.json'])
        staged_fixtures = {name: entry for name, entry in graphics_evidence['source_files'].items()
                           if name.startswith('fixtures/')}
        assert 'fixtures/vertex-buffer.metal' in staged_fixtures
        assert len([name for name in staged_fixtures if name.startswith('fixtures/unsupported-vertex-buffer-') and name.endswith('.spv')]) == 22
        for name, entry in staged_fixtures.items():
            assert source_manifest['graphics-native-validation/' + name] == entry
        fragment_provenance = json.loads(data[tests + 'buffer-provenance.json'])
        assert source_manifest['userspace/compiler/fixtures/apple-fragment-buffer/fragment-buffer.metal'] == {
            'bytes': fragment_provenance['source_bytes'], 'sha256': fragment_provenance['source_sha256']}
    with tarfile.open(fileobj=io.BytesIO(data['Contents/Resources/Sources/HackyNaviMetal-source.tar.gz']), mode='r:gz') as tar:
        seen = set()
        for member in tar:
            assert member.isdir() or member.isfile()
            if not member.isfile():
                continue
            assert member.name in source_manifest and member.name not in seen
            entry = source_manifest[member.name]; value = tar.extractfile(member).read()
            assert len(value) == entry['bytes'] and sha(value) == entry['sha256']
            seen.add(member.name)
        assert seen == set(source_manifest)
    result = {'success': True, 'package_report_sha256': sha(report_bytes),
              'test_sha256': sha(Path(__file__).read_bytes()), 'checker_sha256': sha((HERE / 'bundle_format.py').read_bytes()),
              'real_macho_images': len(report['macho']), 'package_files': len(data),
              'source_files': len(source_manifest), 'archive_modes_verified': True, 'negative_mutations': failures,
              'legacy_macos_real_image_metadata_mutation_checked': True,
              'gpu_executed': False, 'loaded_or_installed': False}
    output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
