#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build the separate real NGG adapter using existing pinned local Mesa archives.

No compiler source stage, existing executable, build graph or library is changed.
The existing Meson command records supply the actual dependency slice/defines;
every reused archive and relevant source/configuration is hashed in the report.
"""
import argparse
import ctypes
import hashlib
import json
import shlex
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BUILD = ROOT / '.tools/userspace/build-mesa-compiler'
STAGE = ROOT / '.tools/userspace/mesa-compiler-source'
PIN = '35b085c4c067f198ce0b6437d94678e47b9c645f'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def split_windows(command):
    # Use the real Windows argument decoder, never execute a shell command.
    api = ctypes.windll.shell32.CommandLineToArgvW
    api.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    api.restype = ctypes.POINTER(ctypes.c_wchar_p)
    count = ctypes.c_int()
    result = api(command, ctypes.byref(count))
    if not result:
        raise RuntimeError('Cannot decode recorded compiler arguments')
    try:
        return [result[i] for i in range(count.value)]
    finally:
        free = ctypes.windll.kernel32.LocalFree
        free.argtypes = [ctypes.c_void_p]
        free.restype = ctypes.c_void_p
        free(result)


def dependencies(path):
    # Clang emits forward-slash Windows paths and Make-escaped spaces. The
    # fixed simple target avoids confusing a drive-letter colon with its ':';
    # shlex handles its emitted backslash quoting after line continuations.
    text = path.read_text().replace('\\\n', '')
    if not text.startswith('ngg.obj:'):
        raise RuntimeError('Unexpected compiler dependency target')
    names = shlex.split(text[len('ngg.obj:'):].replace('$$', '$'), posix=True)
    if not names or len(names) > 8192:
        raise RuntimeError('Unexpected compiler dependency count')
    result = set()
    for name in names:
        resolved = (BUILD / name).resolve()
        if not resolved.is_file() or not resolved.is_relative_to(ROOT):
            raise RuntimeError('Compiler dependency is missing or outside workspace: ' + name)
        result.add(resolved)
    return result


def capture_dependencies(paths, manifest, toolchain):
    captured = {}
    for path in sorted(paths):
        value = digest(path)
        if path.is_relative_to(STAGE):
            relative = path.relative_to(STAGE).as_posix()
            if manifest.get(relative) != value:
                raise RuntimeError('Materialized Mesa dependency differs from prior manifest: ' + relative)
            kind = 'manifest_verified_mesa_source'
        elif path.is_relative_to(BUILD):
            kind = 'generated_build_header'
        elif path.is_relative_to(toolchain):
            kind = 'toolchain_header'
        elif path in (HERE / 'ngg.c', HERE / 'hackynavi_gfx11_ngg.h'):
            kind = 'adapter_source'
        else:
            raise RuntimeError('Unclassified compiler dependency: ' + str(path))
        captured[path.relative_to(ROOT).as_posix()] = {'sha256': value, 'kind': kind}
    return captured


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error('Output must be new; existing build evidence is immutable')
    if subprocess.check_output(['git', '-C', str(ROOT / '.research/mesa'), 'rev-parse', 'HEAD'], text=True).strip() != PIN:
        parser.error('Pinned Mesa revision mismatch')
    records = json.loads((BUILD / 'compile_commands.json').read_text())
    record = [r for r in records if r.get('output') == 'hackynavi-compiler/hackynavi-aco.exe.p/compiler.c.obj']
    if len(record) != 1 or Path(record[0]['directory']).resolve() != BUILD.resolve():
        parser.error('Expected actual established Mesa compiler command')
    compile_command = split_windows(record[0]['command'])
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    if Path(compile_command[0]).resolve() != zig.resolve() or compile_command[1] != 'cc':
        parser.error('Unexpected compiler binary')
    stripped = []
    i = 2
    while i < len(compile_command):
        item = compile_command[i]
        if item in ('-MQ', '-MF', '-o'):
            i += 2
            continue
        if item in ('-MD', '-c') or item == record[0]['file']:
            i += 1
            continue
        if item.startswith('-I'):
            # Pinned third-party/generated headers keep their own warning policy;
            # the new adapter itself is compiled with strict warnings as errors.
            stripped.extend(['-isystem', item[2:]])
        elif item == '-D_WIN32_WINNT=0x0A00':
            stripped.append('-D_WIN32_WINNT=0x0a00')
        else:
            stripped.append(item)
        i += 1
    lines = (BUILD / 'build.ninja').read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith('build hackynavi-compiler/hackynavi-aco.exe:'))
    if not lines[start + 1].lstrip().startswith('LINK_ARGS = '):
        parser.error('Unexpected established link record')
    link_args = split_windows('link ' + lines[start + 1].split(' = ', 1)[1])[1:]
    archives = [(BUILD / item).resolve() for item in link_args if item.endswith('.a')]
    if len(archives) != 12 or any(not p.is_file() or not p.is_relative_to(BUILD) for p in archives):
        parser.error('Expected the actual twelve pinned Mesa archives')
    compute = BUILD / 'hackynavi-compiler/hackynavi-aco.exe'
    compute_before = digest(compute)
    manifest_path = HERE / 'source-manifest.json'
    prior_path = ROOT / 'build/gfx11-compiler/build-report.json'
    prior = json.loads(prior_path.read_text())
    manifest = json.loads(manifest_path.read_text())
    if (manifest.get('commit') != PIN or prior.get('mesa_commit') != PIN or
            prior.get('source_manifest_sha256') != digest(manifest_path)):
        parser.error('Prior actual Mesa build does not bind the current source manifest')
    manifest_files = {entry['path']: entry['sha256'] for entry in manifest['files']}
    if len(manifest_files) != len(manifest['files']):
        parser.error('Duplicate prior source-manifest entries')
    output.mkdir(parents=True)
    source_files = [HERE / n for n in ('ngg.c', 'hackynavi_gfx11_ngg.h', 'build-ngg.py')]
    report = {'success': False, 'mesa_commit': PIN, 'gpu_executed': False,
              'source_files': {str(p.relative_to(ROOT)): digest(p) for p in source_files},
              'archives': {str(p.relative_to(ROOT)): digest(p) for p in archives},
              'compile_database_sha256': digest(BUILD / 'compile_commands.json'),
              'build_graph_sha256': digest(BUILD / 'build.ninja'),
              'compiler_sha256': digest(zig), 'compute_before_sha256': compute_before,
              'prior_build_report_sha256': digest(prior_path), 'source_manifest_sha256': digest(manifest_path),
              'archive_provenance': 'Exact existing archive bytes reused; their source reconstruction is not re-proven by this build.',
              'commands': []}
    (output / 'prior-build-report.json').write_bytes(prior_path.read_bytes())
    common = [str(zig), 'cc', *stripped, '-Werror', '-Wextra', '-I' + str(HERE)]
    commands = [
        [*common, '-M', '-MT', 'ngg.obj', '-MF', str(output / 'dependencies-before.d'), str(HERE / 'ngg.c')],
        [*common, '-MD', '-MT', 'ngg.obj', '-MF', str(output / 'dependencies-compiled.d'), '-c', str(HERE / 'ngg.c'), '-o', str(output / 'ngg.obj')],
        [str(zig), 'c++', '-o', str(output / 'hackynavi-aco-ngg.exe'), str(output / 'ngg.obj'), *link_args],
    ]
    try:
        for index, command in enumerate(commands):
            result = subprocess.run(command, cwd=BUILD, capture_output=True, text=True, timeout=120)
            report['commands'].append({'args': command, 'returncode': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr})
            if result.returncode:
                raise RuntimeError(result.stderr)
            if index == 0:
                paths = dependencies(output / 'dependencies-before.d')
                report['dependencies'] = capture_dependencies(paths, manifest_files, zig.parent)
            elif index == 1:
                compiled = dependencies(output / 'dependencies-compiled.d')
                if compiled != paths:
                    raise RuntimeError('Actual compiled dependencies differ from preflight scan')
                if capture_dependencies(compiled, manifest_files, zig.parent) != report['dependencies']:
                    raise RuntimeError('Actual dependency bytes changed during compilation')
                report['compiled_dependencies_match_scan'] = True
        report['artifact_sha256'] = digest(output / 'hackynavi-aco-ngg.exe')
        report['success'] = True
    finally:
        report['compute_after_sha256'] = digest(compute)
        report['compute_unchanged'] = report['compute_after_sha256'] == compute_before
        report['archives_unchanged'] = all(digest(ROOT / name) == value for name, value in report['archives'].items())
        report['source_unchanged'] = all(digest(ROOT / name) == value for name, value in report['source_files'].items())
        report['dependencies_unchanged'] = bool(report.get('dependencies')) and all(
            digest(ROOT / name) == value['sha256'] for name, value in report.get('dependencies', {}).items())
        report['provenance_unchanged'] = (digest(prior_path) == report['prior_build_report_sha256'] and
            digest(manifest_path) == report['source_manifest_sha256'] and
            digest(BUILD / 'compile_commands.json') == report['compile_database_sha256'] and
            digest(BUILD / 'build.ninja') == report['build_graph_sha256'] and digest(zig) == report['compiler_sha256'])
        report['success'] &= all(report[key] for key in ('compute_unchanged', 'archives_unchanged', 'source_unchanged',
                                                       'dependencies_unchanged', 'provenance_unchanged'))
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['success']:
        raise RuntimeError('NGG compiler build failed')
    print(json.dumps({'success': True, 'report': str(output / 'report.json'), 'artifact_sha256': report['artifact_sha256']}))


if __name__ == '__main__':
    main()
