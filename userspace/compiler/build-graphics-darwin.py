#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cross-build actual macOS graphics adapters from verified existing Darwin archives.

Only a new output directory is written. No remote execution, Mesa rebuild, GPU
access, or replacement code generation occurs. Actual dependency scans and
compiled dependency files must match byte for byte by hash.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
from darwin_artifacts import archive, macho

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BUILD = ROOT / '.tools/userspace/build-mesa-compiler-darwin'
STAGE = ROOT / '.tools/userspace/mesa-compiler-source-darwin'
PIN = '35b085c4c067f198ce0b6437d94678e47b9c645f'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def split_windows(command):
    api = ctypes.windll.shell32.CommandLineToArgvW
    api.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    api.restype = ctypes.POINTER(ctypes.c_wchar_p)
    count = ctypes.c_int()
    result = api(command, ctypes.byref(count))
    if not result:
        raise RuntimeError('Cannot decode actual compiler arguments')
    try:
        return [result[i] for i in range(count.value)]
    finally:
        free = ctypes.windll.kernel32.LocalFree
        free.argtypes = [ctypes.c_void_p]
        free.restype = ctypes.c_void_p
        free(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() or not output.is_relative_to(ROOT / 'build'):
        parser.error('Use a new output directory inside build')
    zig = ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe'
    paths = [HERE / 'build-graphics-darwin.py', HERE / 'darwin_artifacts.py',
             HERE / 'ngg.c', HERE / 'hackynavi_gfx11_ngg.h', HERE / 'fragment.c',
             HERE / 'hackynavi_gfx11_fragment.h', ROOT / 'userspace/graphics/color_target.c',
             HERE / 'fragment-buffer.c', HERE / 'hackynavi_gfx11_fragment_buffer.h',
             HERE / 'vertex-buffer.c', HERE / 'hackynavi_gfx11_vertex_buffer.h',
             ROOT / 'include/hackynavi/gfx11_color_target.h', ROOT / 'src/gfx11_color_target.cpp',
             ROOT / 'tests/gfx11_color_target_tests.cpp', HERE / 'source-manifest.json',
             HERE / 'source-manifest-darwin.json', ROOT / 'build/gfx11-compiler/darwin-build-report.json',
             BUILD / 'compile_commands.json', BUILD / 'build.ninja', zig]
    inputs = {p: p.read_bytes() for p in paths}
    prior = json.loads(inputs[ROOT / 'build/gfx11-compiler/darwin-build-report.json'])
    manifest = json.loads(inputs[HERE / 'source-manifest.json'])
    darwin = json.loads(inputs[HERE / 'source-manifest-darwin.json'])
    if (prior['mesa_commit'] != PIN or manifest['commit'] != PIN or darwin['mesa_commit'] != PIN or
            prior['base_manifest_sha256'] != sha(inputs[HERE / 'source-manifest.json']) or
            darwin['base_manifest_sha256'] != prior['base_manifest_sha256']):
        raise RuntimeError('Existing Darwin build does not bind the pinned source manifest')
    original_files = {entry['path']: entry['sha256'] for entry in manifest['files']}
    if len(original_files) != len(manifest['files']):
        raise RuntimeError('Duplicate Mesa manifest paths')
    for key in ('binary', 'library'):
        path = Path(prior[key]).resolve()
        inputs[path] = path.read_bytes()
        if sha(inputs[path]) != prior[key + '_sha256']:
            raise RuntimeError('Prior Darwin product changed')
        macho(inputs[path], 2 if key == 'binary' else 6)
    records = json.loads(inputs[BUILD / 'compile_commands.json'])

    def flags(target, language='cc'):
        found = [r for r in records if r.get('output') == target]
        if len(found) != 1 or Path(found[0]['directory']).resolve() != BUILD:
            raise RuntimeError('Actual Darwin compile record absent')
        row = found[0]
        command = split_windows(row['command'])
        if Path(command[0]).resolve() != zig or command[1:4] != [language, '-target', 'x86_64-macos.15.0']:
            raise RuntimeError('Wrong compiler/target in actual record')
        result, i = [], 2
        while i < len(command):
            item = command[i]
            if item in ('-o', '-MQ', '-MF'):
                i += 2
                continue
            if item in ('-c', '-MD', '-fdiagnostics-color=always') or item == row['file']:
                i += 1
                continue
            if item.startswith('-I'):
                result.extend(['-isystem', item[2:]])
            else:
                result.append(item)
            i += 1
        return result

    compiler_flags = flags('hackynavi-compiler/hackynavi-aco.p/compiler.c.o')
    surface_flags = flags('src/amd/common/libamd_common.a.p/ac_descriptors.c.o')
    lines = inputs[BUILD / 'build.ninja'].decode().splitlines()
    index = next(i for i, s in enumerate(lines) if s.startswith('build hackynavi-compiler/hackynavi-aco:'))
    if not lines[index + 1].lstrip().startswith('LINK_ARGS = '):
        raise RuntimeError('Actual Darwin link record absent')
    recorded_link = split_windows('link ' + lines[index + 1].split(' = ', 1)[1])[1:]
    archives = [(BUILD / s).resolve() for s in recorded_link if s.endswith('.a')]
    if len(archives) != 12 or len(set(archives)) != 12 or any(not p.is_relative_to(BUILD) for p in archives):
        raise RuntimeError('Expected exact twelve existing Darwin archives')
    archive_info = {}
    for path in archives:
        raw = path.read_bytes()
        inputs[path] = raw
        archive_info[path.relative_to(ROOT).as_posix()] = {
            'sha256': sha(raw), 'bytes': len(raw), 'objects': archive(raw)}
    output.mkdir(parents=True)
    surface_original = STAGE / 'src/amd/common/ac_surface.c'
    surface_bytes = surface_original.read_bytes()
    if sha(surface_bytes) != original_files.get('src/amd/common/ac_surface.c'):
        raise RuntimeError('Actual pinned surface source changed')
    inputs[surface_original] = surface_bytes
    marker = b'void ac_surface_print_info(FILE *out, const struct radeon_info *info,'
    if surface_bytes.count(marker) != 1 or surface_bytes.count(b'#include <xf86drm.h>') != 1:
        raise RuntimeError('Unexpected pinned surface diagnostic layout')
    printer_offset = surface_bytes.index(marker)
    removed_printer = surface_bytes[printer_offset:]
    if removed_printer.count(b'{') != removed_printer.count(b'}') or not removed_printer.rstrip().endswith(b'}'):
        raise RuntimeError('Incomplete final surface diagnostic function')
    surface_generated = surface_bytes[:printer_offset].replace(b'#include <xf86drm.h>', b'')
    if surface_generated.count(b'drmGetFormatModifierName') != 1:
        raise RuntimeError('Unexpected remaining libdrm caller')
    # The remaining occurrence is the upstream inactive Windows-only definition.
    surface = output / 'ac_surface_layout.c'
    surface.write_bytes(surface_generated)
    inputs[surface] = surface_generated
    (output / 'prior-build-report.json').write_bytes(inputs[ROOT / 'build/gfx11-compiler/darwin-build-report.json'])
    report = {'success': False, 'mesa_commit': PIN, 'remote_executed': False, 'gpu_executed': False,
              'native_execution_pending': True, 'archives': archive_info, 'commands': [], 'dependencies': {},
              'archive_provenance': 'Exact existing Darwin archive bytes and every object target verified; archive source reconstruction not re-proven.',
              'inputs': {p.relative_to(ROOT).as_posix(): sha(raw) for p, raw in inputs.items()},
              'recorded_link_args': recorded_link,
              'surface_extraction': {'source_sha256': sha(surface_bytes), 'generated_sha256': sha(surface_generated),
                  'removed_function': 'ac_surface_print_info', 'removed_function_sha256': sha(removed_printer),
                  'removed_function_offset': printer_offset, 'removed_include': '#include <xf86drm.h>',
                  'layout_algorithm_changes': False, 'replacement_functions': False}}

    def persist():
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(command, verbose_link=False):
        row = {'args': [str(x) for x in command]}
        report['commands'].append(row)
        persist()
        env = os.environ.copy()
        if verbose_link:
            env['ZIG_VERBOSE_LINK'] = '1'
            row['environment_override'] = {'ZIG_VERBOSE_LINK': '1'}
        result = subprocess.run(row['args'], cwd=BUILD, env=env, capture_output=True, text=True, timeout=180)
        row.update(returncode=result.returncode, stdout=result.stdout, stderr=result.stderr)
        persist()
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result

    def dependency_snapshot(path):
        content = path.read_text().replace('\\\n', '')
        if not content.startswith('unit.o:'):
            raise RuntimeError('Unexpected dependency target')
        names = shlex.split(content[len('unit.o:'):].replace('$$', '$'), posix=True)
        if not names or len(names) > 8192:
            raise RuntimeError('Dependency count limit')
        result = {}
        for name in names:
            path = (BUILD / name).resolve()
            if not path.is_relative_to(ROOT):
                raise RuntimeError('Dependency outside workspace')
            value = sha(path.read_bytes())
            if path.is_relative_to(STAGE):
                if original_files.get(path.relative_to(STAGE).as_posix()) != value:
                    raise RuntimeError('Mesa dependency differs from pinned manifest: ' + str(path))
                kind = 'manifest_verified_mesa_source'
            elif path.is_relative_to(BUILD):
                kind = 'generated_mesa_header'
            elif path.is_relative_to(zig.parent):
                kind = 'toolchain_header'
            elif path in inputs:
                kind = 'authored_input'
            else:
                raise RuntimeError('Unclassified compiler dependency: ' + str(path))
            result[path.relative_to(ROOT).as_posix()] = {'sha256': value, 'kind': kind}
        return result

    def compile_unit(name, source, command):
        run([*command, '-M', '-MT', 'unit.o', '-MF', output / (name + '-before.d'), source])
        before = dependency_snapshot(output / (name + '-before.d'))
        report['dependencies'][name] = before
        run([*command, '-MD', '-MT', 'unit.o', '-MF', output / (name + '-compiled.d'),
             '-c', source, '-o', output / (name + '.o')])
        if dependency_snapshot(output / (name + '-compiled.d')) != before:
            raise RuntimeError('Compiled dependency bytes/set differ from scan')
        macho((output / (name + '.o')).read_bytes(), 1)

    try:
        common = [zig, 'cc', *compiler_flags, '-Werror', '-Wextra', '-I' + str(HERE)]
        # Darwin link arguments are explicit, with no unresolved-symbol bypass.
        link = [zig, 'c++', '-target', 'x86_64-macos.15.0', *archives, '-pthread', '-lm']
        for stage in ('ngg', 'fragment', 'fragment-buffer', 'vertex-buffer'):
            compile_unit(stage, HERE / (stage + '.c'), common)
            run([*link, output / (stage + '.o'), '-o', output / ('hackynavi-aco-' + stage)])
            compile_unit(stage + '_shared', HERE / (stage + '.c'),
                         [*common, '-DHN_GFX11_' + stage.upper().replace('-', '_') + '_SHARED'])
        graphics_exports = output / 'graphics.exports'
        graphics_exports.write_text('_hn_gfx11_ngg_compile_validated_file\n_hn_gfx11_fragment_compile_validated_file\n'
                                    '_hn_gfx11_fragment_buffer_compile_validated_file\n_hn_gfx11_fragment_buffer_descriptor\n'
                                    '_hn_gfx11_vertex_buffer_compile_validated_file\n_hn_gfx11_vertex_buffer_descriptor\n')
        report['expected_graphics_exports'] = sorted(graphics_exports.read_text().splitlines())
        linked = run([*link, output / 'ngg_shared.o', output / 'fragment_shared.o', output / 'fragment-buffer_shared.o',
             output / 'vertex-buffer_shared.o', '-shared',
             '-Wl,-install_name,@rpath/libHackyNaviGraphicsCompiler.dylib',
             '-Wl,-exported_symbols_list,' + str(graphics_exports), '-o', output / 'libHackyNaviGraphicsCompiler.dylib', '-v'], True)
        # Capture the actual archives selected by this link, not guessed cache entries.
        selected = re.findall(r'(?<!\S)([A-Za-z]:[^\s]+\\lib(?:c\+\+abi|c\+\+|compiler_rt|ubsan_rt)\.a)(?!\S)', linked.stderr)
        runtime_paths = [Path(name).resolve() for name in selected]
        if len(runtime_paths) != 4 or {p.name for p in runtime_paths} != {'libc++.a', 'libc++abi.a', 'libcompiler_rt.a', 'libubsan_rt.a'}:
            raise RuntimeError('Actual verbose linker record did not identify four matching Darwin runtimes')
        report['runtime_archives'] = {}
        for path in runtime_paths:
            raw = path.read_bytes()
            name = 'runtime-' + path.name
            (output / name).write_bytes(raw)
            report['runtime_archives'][name] = {'path': str(path), 'sha256': sha(raw), 'bytes': len(raw),
                                                'objects': archive(raw)}
        swizzler_flags = flags('src/amd/addrlib/libaddrlib.a.p/src_core_addrswizzler.cpp.o', 'c++')
        if swizzler_flags.count('-DADDR_ALLOW_SIMD=1') != 1:
            raise RuntimeError('Unexpected addrlib scalar-selection contract')
        swizzler_flags = ['-DADDR_ALLOW_SIMD=0' if x == '-DADDR_ALLOW_SIMD=1' else x for x in swizzler_flags]
        compile_unit('src_core_addrswizzler.cpp', STAGE / 'src/amd/addrlib/src/core/addrswizzler.cpp',
                     [zig, 'c++', *swizzler_flags])
        old_addrlib = BUILD / 'src/amd/addrlib/libaddrlib.a'
        new_addrlib = output / 'libaddrlib-layout.a'
        new_addrlib.write_bytes(inputs[old_addrlib])
        run([zig, 'ar', 'r', new_addrlib, output / 'src_core_addrswizzler.cpp.o'])
        old_members = {x['name']: x for x in archive(inputs[old_addrlib])}
        new_members = {x['name']: x for x in archive(new_addrlib.read_bytes())}
        member_name = 'src_core_addrswizzler.cpp.o'
        if (old_members.keys() != new_members.keys() or len(new_members) != 16 or
                any(old_members[name] != value for name, value in new_members.items() if name != member_name) or
                new_members[member_name]['sha256'] != sha((output / member_name).read_bytes())):
            raise RuntimeError('Scalar addrlib archive changed unexpected members')
        report['scalar_addrlib'] = {'sha256': sha(new_addrlib.read_bytes()), 'changed_member': member_name,
                                   'original_members': old_members, 'result_members': new_members,
                                   'selection': 'Actual Mesa ADDR_ALLOW_SIMD=0 implementation; no feature-query substitute.'}
        link = [new_addrlib if x == old_addrlib else x for x in link]
        compile_unit('ac_surface', surface, [zig, 'cc', *surface_flags])
        compile_unit('color_adapter', ROOT / 'userspace/graphics/color_target.c',
                     [zig, 'cc', *surface_flags, '-fvisibility=default', '-Werror', '-Wextra', '-I' + str(ROOT / 'include')])
        cpp = [zig, 'c++', '-target', 'x86_64-macos.15.0', '-std=c++11', '-O2', '-Wall', '-Wextra',
               '-Wpedantic', '-Werror', '-fno-exceptions', '-fno-rtti', '-I' + str(ROOT / 'include')]
        compile_unit('color_validator', ROOT / 'src/gfx11_color_target.cpp', cpp)
        exported = output / 'color.exports'
        exported.write_text('_hn_color_target_calculate\n_hn_color_target_validate\n_hn_color_target_validate_inputs\n')
        run([*link, output / 'ac_surface.o', output / 'color_adapter.o', output / 'color_validator.o',
             '-shared', '-Wl,-install_name,@rpath/libHackyNaviColorLayout.dylib',
             '-Wl,-exported_symbols_list,' + str(exported), '-o', output / 'libHackyNaviColorLayout.dylib'])
        compile_unit('color_tests', ROOT / 'tests/gfx11_color_target_tests.cpp', [*cpp, '-DHN_COLOR_MESA_TEST'])
        run([zig, 'c++', '-target', 'x86_64-macos.15.0', output / 'color_tests.o',
             '-L' + str(output), '-lHackyNaviColorLayout', '-Wl,-rpath,@loader_path',
             '-o', output / 'color-layout-tests'])
        report['artifacts'] = {}
        for name in ('hackynavi-aco-ngg', 'hackynavi-aco-fragment', 'hackynavi-aco-fragment-buffer',
                     'hackynavi-aco-vertex-buffer', 'libHackyNaviGraphicsCompiler.dylib',
                     'libHackyNaviColorLayout.dylib', 'color-layout-tests'):
            raw = (output / name).read_bytes()
            info = macho(raw, 6 if name.endswith('.dylib') else 2)
            allowed = {'/usr/lib/libSystem.B.dylib', '/usr/lib/libc++.1.dylib'}
            if name == 'color-layout-tests':
                allowed.add('@rpath/libHackyNaviColorLayout.dylib')
            if any(dep not in allowed for dep in info['dependencies']):
                raise RuntimeError('Unexpected runtime library dependency: ' + str(info))
            if name.endswith('.dylib') and info['identifiers'] != ['@rpath/' + name]:
                raise RuntimeError('Wrong library install name')
            report['artifacts'][name] = {'sha256': sha(raw), 'bytes': len(raw), 'macho': info}
        report['native_relink_inputs'] = {name: {'sha256': sha((output / name).read_bytes()),
                                               'bytes': (output / name).stat().st_size} for name in (
            'ngg_shared.o', 'fragment_shared.o', 'fragment-buffer_shared.o', 'vertex-buffer_shared.o',
            'ac_surface.o', 'color_adapter.o', 'color_validator.o',
            'libaddrlib-layout.a', 'graphics.exports', 'color.exports')}
        report['native_relink_inputs'].update({name: {'sha256': row['sha256'], 'bytes': row['bytes']}
                                              for name, row in report['runtime_archives'].items()})
        report['native_export_verification_required'] = True
        report['cross_dylibs_not_approved_for_frontend'] = True
        report['success'] = True
    finally:
        report['inputs_unchanged'] = all(sha(path.read_bytes()) == sha(data) for path, data in inputs.items())
        report['dependencies_unchanged'] = all(sha((ROOT / path).read_bytes()) == item['sha256']
            for group in report['dependencies'].values() for path, item in group.items())
        report['runtime_archives_unchanged'] = bool(report.get('runtime_archives')) and all(
            sha(Path(row['path']).read_bytes()) == row['sha256'] and sha((output / name).read_bytes()) == row['sha256']
            for name, row in report.get('runtime_archives', {}).items())
        report['success'] &= report['inputs_unchanged'] and report['dependencies_unchanged'] and report['runtime_archives_unchanged']
        persist()
    print(json.dumps({'success': report['success'], 'report': str(output / 'report.json'),
                      'artifact_bytes': sum(x['bytes'] for x in report.get('artifacts', {}).values())}))


if __name__ == '__main__':
    main()
