#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Local pure barrier tests against the real pinned Mesa host command encoder.

Reuses exact existing Mesa archives. Never opens a GPU or invokes a remote host.
The actual archive bytes are bound; their original reconstruction is not rerun.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / '.tools/userspace/build-mesa-compiler'
STAGE = ROOT / '.tools/userspace/mesa-compiler-source'
PIN = '35b085c4c067f198ce0b6437d94678e47b9c645f'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error('Output must be new; earlier evidence is immutable')
    helper = ROOT / 'userspace/compiler/build-fragment.py'
    spec = importlib.util.spec_from_file_location('fragment_build_helpers', helper)
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)
    prior_path = ROOT / 'build/gfx11-fragment-local-6/report.json'
    prior_bytes = prior_path.read_bytes()
    prior = json.loads(prior_bytes)
    if not prior['success'] or prior['mesa_commit'] != PIN:
        raise RuntimeError('Expected successful reviewed actual Mesa archive build')
    records_path = BUILD / 'compile_commands.json'
    ninja_path = BUILD / 'build.ninja'
    manifest_path = ROOT / 'userspace/compiler/source-manifest.json'
    for path, key in [(helper, None), (records_path, 'compile_database_sha256'),
                      (ninja_path, 'build_graph_sha256'), (manifest_path, 'source_manifest_sha256')]:
        expected = prior['source_files'][str(helper.relative_to(ROOT))] if key is None else prior[key]
        if sha(path) != expected:
            raise RuntimeError('Reviewed Mesa build input changed: ' + str(path))
    manifest = json.loads(manifest_path.read_text())
    if manifest['commit'] != PIN:
        raise RuntimeError('Wrong source manifest pin')
    materialized = {item['path']: item['sha256'] for item in manifest['files']}
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    if sha(zig) != prior['compiler_sha256']:
        raise RuntimeError('Actual reviewed compiler changed')
    records = json.loads(records_path.read_text())
    record = [r for r in records if r.get('output') == 'src/amd/common/libamd_common.a.p/ac_barrier.c.obj']
    if len(record) != 1 or Path(record[0]['directory']).resolve() != BUILD:
        raise RuntimeError('Expected actual ac_barrier compile record')
    command = helpers.split_windows(record[0]['command'])
    if Path(command[0]).resolve() != zig or command[1] != 'cc':
        raise RuntimeError('Unexpected Mesa compiler')
    stripped = []
    index = 2
    while index < len(command):
        item = command[index]
        if item in ('-o', '-MQ', '-MF'):
            index += 2
            continue
        if item in ('-MD', '-c') or item == record[0]['file']:
            index += 1
            continue
        if item.startswith('-I'):
            stripped.extend(['-isystem', item[2:]])
        else:
            stripped.append(item.replace('-D_WIN32_WINNT=0x0A00', '-D_WIN32_WINNT=0x0a00'))
        index += 1
    lines = ninja_path.read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith('build hackynavi-compiler/hackynavi-aco.exe:'))
    if not lines[start + 1].lstrip().startswith('LINK_ARGS = '):
        raise RuntimeError('Unexpected actual link record')
    link_args = helpers.split_windows('link ' + lines[start + 1].split(' = ', 1)[1])[1:]
    archives = [(BUILD / item).resolve() for item in link_args if item.endswith('.a')]
    if len(archives) != 12 or any(sha(p) != prior['archives'][str(p.relative_to(ROOT))] for p in archives):
        raise RuntimeError('Reviewed actual reused archive changed')
    oracle = ROOT / 'tests/gfx11_graphics_barrier_mesa_oracle.c'
    sources = [ROOT / name for name in ('include/hackynavi/gfx11_graphics_barrier.hpp',
               'src/gfx11_graphics_barrier.cpp', 'tests/gfx11_graphics_barrier_tests.cpp',
               'tests/gfx11_graphics_barrier_mesa_oracle.c', 'scripts/check-gfx11-graphics-barrier.py')]
    references = [ROOT / '.research/mesa' / name for name in ('src/amd/common/ac_barrier.c',
        'src/amd/common/ac_barrier.h', 'src/amd/common/ac_cmdbuf_cp.c', 'src/amd/common/ac_cmdbuf_base.h',
        'src/amd/registers/pkt3.json', 'src/amd/packets/cp_pm4_table_data_gfx11.json')]
    for path in references:
        if sha(path) != materialized[path.relative_to(ROOT / '.research/mesa').as_posix()]:
            raise RuntimeError('Pinned source reference changed')
    bound = [*sources, *references, helper, prior_path, records_path, ninja_path, manifest_path, zig, *archives]
    report = {'success': False, 'gpu_executed': False, 'remote_executed': False,
              'mesa_commit': PIN, 'files': {str(p.relative_to(ROOT)): sha(p) for p in bound},
              'archive_provenance': prior['archive_provenance'], 'commands': []}
    output.mkdir(parents=True)
    (output / 'prior-build-report.json').write_bytes(prior_bytes)

    def run(command, cwd=ROOT):
        result = subprocess.run([str(x) for x in command], cwd=cwd, text=True, capture_output=True, timeout=120)
        report['commands'].append({'args': [str(x) for x in command], 'returncode': result.returncode,
                                   'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def dependency_snapshot(path):
        result = {}
        for item in helpers.dependencies(path):
            value = sha(item)
            if item.is_relative_to(STAGE):
                if materialized.get(item.relative_to(STAGE).as_posix()) != value:
                    raise RuntimeError('Actual Mesa header differs from manifest')
                kind = 'manifest_verified_mesa_source'
            elif item.is_relative_to(BUILD):
                kind = 'generated_build_header'
            elif item.is_relative_to(zig.parent):
                kind = 'toolchain_header'
            elif item == oracle:
                kind = 'authored_oracle_caller'
            else:
                raise RuntimeError('Unclassified actual dependency: ' + str(item))
            result[item.relative_to(ROOT).as_posix()] = {'sha256': value, 'kind': kind}
        return result

    try:
        common = [zig, 'cc', *stripped, '-Werror', '-Wextra']
        run([*common, '-M', '-MT', 'fragment.obj', '-MF', output / 'oracle-before.d', oracle], BUILD)
        report['oracle_dependencies'] = dependency_snapshot(output / 'oracle-before.d')
        run([*common, '-MD', '-MT', 'fragment.obj', '-MF', output / 'oracle-compiled.d', '-c', oracle,
             '-o', output / 'mesa-oracle.obj'], BUILD)
        if dependency_snapshot(output / 'oracle-compiled.d') != report['oracle_dependencies']:
            raise RuntimeError('Oracle dependencies changed during actual compile')
        run([zig, 'c++', '-o', output / 'mesa-oracle.exe', output / 'mesa-oracle.obj', *link_args], BUILD)
        run([output / 'mesa-oracle.exe', output / 'mesa-barrier.bin'])
        common_cpp = [zig, 'c++', '-std=c++11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-fno-exceptions',
                      '-fno-rtti', '-I' + str(ROOT / 'include'), ROOT / 'src/gfx11_graphics_barrier.cpp',
                      ROOT / 'tests/gfx11_graphics_barrier_tests.cpp']
        run([*common_cpp, '-O2', '-o', output / 'tests.exe'])
        run([output / 'tests.exe', output / 'mesa-barrier.bin'])
        run([*common_cpp, '-O1', '-g', '-fsanitize=undefined', '-fno-sanitize-recover=all', '-o', output / 'tests-ubsan.exe'])
        run([output / 'tests-ubsan.exe', output / 'mesa-barrier.bin'])
        run([zig, 'c++', '-target', 'x86_64-macos', '-mmacosx-version-min=15.0', '-std=c++11',
             '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-Wframe-larger-than=2048',
             '-fno-exceptions', '-fno-rtti', '-ffreestanding', '-I' + str(ROOT / 'include'),
             '-c', ROOT / 'src/gfx11_graphics_barrier.cpp', '-o', output / 'gfx11_graphics_barrier-macos.o'])
        report['artifacts'] = {p.name: sha(p) for p in output.iterdir() if p.suffix in ('.exe', '.obj', '.o', '.bin')}
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(ROOT / p) == value for p, value in report['files'].items())
        report['dependencies_unchanged'] = bool(report.get('oracle_dependencies')) and all(
            sha(ROOT / p) == value['sha256'] for p, value in report.get('oracle_dependencies', {}).items())
        report['success'] &= report['inputs_unchanged'] and report['dependencies_unchanged']
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['success']:
        raise RuntimeError('Graphics barrier validation failed')
    print(json.dumps({'success': True, 'report': str(output / 'report.json'), 'sha256': sha(output / 'report.json')}))


if __name__ == '__main__':
    main()
