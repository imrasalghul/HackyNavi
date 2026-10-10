#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded host-only validation inside a caller-owned verified macOS stage."""
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


def main():
    stage = Path(sys.argv[1]).resolve()
    if (not re.fullmatch(r'/Users/[^/\r\n]+/Library/Caches/HackyNavi-GraphicsNative\.[A-Za-z0-9]+', str(stage)) or
            Path(__file__).resolve() != stage / 'scripts/run_native.py'):
        raise RuntimeError('Unexpected native graphics stage')
    report = {'success': False, 'complete': False, 'child_processes_reaped': True,
              'gpu_executed': False, 'metal_objects_created': False, 'context_opened': False,
              'stage': str(stage), 'commands': []}
    os.chdir(stage)

    def persist():
        (stage / 'native-report.json').write_text(json.dumps(report, indent=2) + '\n')

    def alive(pid):
        try:
            os.killpg(pid, 0)
            return True
        except ProcessLookupError:
            return False

    def reap(process):
        for sig in (signal.SIGTERM, signal.SIGKILL):
            try:
                os.killpg(process.pid, sig)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
            deadline = time.monotonic() + 2
            while alive(process.pid) and time.monotonic() < deadline:
                time.sleep(.05)
            if not alive(process.pid):
                return True
        return False

    env = os.environ.copy()
    env.update(METAL2VULKAN_LLVM_DIS='/nonexistent/explicit-air-required',
               METAL2VULKAN_SPIRV_VAL='/nonexistent/explicit-validator-required',
               HACKYNAVI_APPLE_LLVM_LIBRARY='/nonexistent/fixed-system-reader-required')

    def run(argv):
        row = {'argv': [str(x) for x in argv], 'exit_code': None}
        report['commands'].append(row)
        persist()
        process = subprocess.Popen(row['argv'], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=120)
        except subprocess.TimeoutExpired:
            row['timeout'] = True
            report['child_processes_reaped'] &= reap(process)
            raise RuntimeError('Bounded native command timeout')
        finally:
            if alive(process.pid):
                report['child_processes_reaped'] &= reap(process)
        row.update(exit_code=process.returncode, stdout=stdout, stderr=stderr)
        persist()
        if process.returncode or not report['child_processes_reaped']:
            raise RuntimeError('Native command failed: ' + stderr)
        return stdout

    try:
        manifest = json.loads((stage / 'manifest.json').read_text())
        relink = (stage / 'relink.json').is_file()
        if len(manifest['files']) > 256 or manifest['bytes'] > (208 if relink else 90) * 1024 * 1024:
            raise RuntimeError('Snapshot budget exceeded')
        for name, item in manifest['files'].items():
            path = stage / name
            if path.is_symlink() or not path.resolve().is_relative_to(stage) or not path.is_file():
                raise RuntimeError('Invalid snapshot path')
            raw = path.read_bytes()
            if len(raw) != item['bytes'] or hashlib.sha256(raw).hexdigest() != item['sha256']:
                raise RuntimeError('Snapshot content changed: ' + name)
        (stage / 'results').mkdir()
        report['platform'] = run(['/usr/bin/sw_vers'])
        report['sdk'] = run(['/usr/bin/xcrun', '--show-sdk-version'])
        report['native_relink'] = relink
        if relink:
            config = json.loads((stage / 'relink.json').read_text())
            archives = config['archives']
            if (len(archives) != 12 or len(set(archives)) != 12 or
                    any(not re.fullmatch(r'relink/archives/lib[a-z0-9_]+\.a', name) for name in archives)):
                raise RuntimeError('Unexpected native archive list')
            clang = run(['/usr/bin/xcrun', '--sdk', 'macosx', '--find', 'clang++']).strip()
            if not Path(clang).is_absolute() or not Path(clang).is_file():
                raise RuntimeError('Actual Apple compiler is unavailable')
            report['clang_version'] = run([clang, '--version'])
            report['clang_sha256'] = hashlib.sha256(Path(clang).read_bytes()).hexdigest()
            common = [clang, '-arch', 'x86_64', '-mmacosx-version-min=15.0', '-dynamiclib',
                      '-nostdlib++', '-Wl,-undefined,error']
            runtime = ['relink/runtime-' + name for name in ('libc++abi.a', 'libc++.a', 'libcompiler_rt.a', 'libubsan_rt.a')]
            if any(name not in manifest['files'] for name in runtime):
                raise RuntimeError('Actual matching Darwin runtime archives missing')
            run([*common, 'relink/ngg_shared.o', 'relink/fragment_shared.o', 'relink/fragment-buffer_shared.o',
                 'relink/vertex-buffer_shared.o', *archives, *runtime,
                 '-Wl,-exported_symbols_list,relink/graphics.exports',
                 '-Wl,-install_name,@rpath/libHackyNaviGraphicsCompiler.dylib',
                 '-o', 'libHackyNaviGraphicsCompiler.dylib'])
            color_archives = ['relink/libaddrlib-layout.a' if name.endswith('/libaddrlib.a') else name for name in archives]
            # Pinned addrlib.cpp already provides its actual Apple __umoddi3;
            # preserve that definition and let Apple clang resolve other builtins.
            color_runtime = [name for name in runtime if not name.endswith('/runtime-libcompiler_rt.a')]
            run([*common, 'relink/ac_surface.o', 'relink/color_adapter.o', 'relink/color_validator.o',
                 *color_archives, *color_runtime, '-Wl,-exported_symbols_list,relink/color.exports',
                 '-Wl,-install_name,@rpath/libHackyNaviColorLayout.dylib', '-o', 'libHackyNaviColorLayout.dylib'])
        expected_exports = {
            'libHackyNaviGraphicsCompiler.dylib': {'_hn_gfx11_ngg_compile_validated_file', '_hn_gfx11_fragment_compile_validated_file',
                '_hn_gfx11_fragment_buffer_compile_validated_file', '_hn_gfx11_fragment_buffer_descriptor',
                '_hn_gfx11_vertex_buffer_compile_validated_file', '_hn_gfx11_vertex_buffer_descriptor'},
            'libHackyNaviColorLayout.dylib': {'_hn_color_target_calculate', '_hn_color_target_validate', '_hn_color_target_validate_inputs'}}
        report['exports'] = {}
        report['dylib_dependencies'] = {}
        for name, expected in expected_exports.items():
            dependencies = run(['/usr/bin/otool', '-L', str(stage / name)])
            libraries = [line.strip().split(' (', 1)[0] for line in dependencies.splitlines()[1:] if line.strip()]
            if (not libraries or libraries[0] != '@rpath/' + name or
                    any(item not in {'@rpath/' + name, '/usr/lib/libSystem.B.dylib', '/usr/lib/libc++.1.dylib'}
                        for item in libraries)):
                raise RuntimeError('Unexpected native dylib dependency/install name')
            report['dylib_dependencies'][name] = libraries
            output = run(['/usr/bin/nm', '-gU', str(stage / name)])
            symbols = {line.split()[-1] for line in output.splitlines() if line.split()}
            if symbols != expected:
                raise RuntimeError('Unexpected dylib public exports')
            report['exports'][name] = sorted(symbols)
        run([sys.executable, str(stage / 'scripts/test-graphics-darwin.py'), str(stage)])
        compiler = json.loads((stage / 'results/compiler/report.json').read_text())
        expected_products = {(ext, kind, mode) for ext in ('air', 'metallib')
                             for kind in ('vertex', 'fragment', 'fragment_buffer', 'vertex_buffer')
                             for mode in ('cli', 'cabi')}
        actual_products = {(row['format'], row['stage'], row['mode']) for row in compiler['products']}
        if not compiler['success'] or len(compiler['products']) != 16 or actual_products != expected_products:
            raise RuntimeError('Actual compiler test incomplete')
        report['compiler_checks'] = compiler['checks']
        report['color_output'] = run([str(stage / 'color-layout-tests'), str(stage / 'results/actual-layouts.bin')])
        if (stage / 'results/actual-layouts.bin').read_bytes() != (stage / 'fixtures/actual-layouts.bin').read_bytes():
            raise RuntimeError('Actual Darwin color results differ from Windows Mesa results')
        report['inputs_unchanged'] = all(hashlib.sha256((stage / name).read_bytes()).hexdigest() == item['sha256']
                                        for name, item in manifest['files'].items())
        if not report['inputs_unchanged']:
            raise RuntimeError('Native run changed inputs')
        report['artifacts'] = {name: {'sha256': hashlib.sha256((stage / name).read_bytes()).hexdigest(),
                                      'bytes': (stage / name).stat().st_size} for name in (
            'hackynavi-aco-ngg', 'hackynavi-aco-fragment', 'hackynavi-aco-fragment-buffer',
            'hackynavi-aco-vertex-buffer', 'libHackyNaviGraphicsCompiler.dylib',
            'libHackyNaviColorLayout.dylib', 'color-layout-tests')}
        report['peak_stage_bytes'] = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())
        if report['peak_stage_bytes'] > (256 if relink else 160) * 1024 * 1024:
            raise RuntimeError('Stage storage budget exceeded')
        report['success'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        report['complete'] = True
        persist()
    print(json.dumps({'success': True, 'compiler_checks': report['compiler_checks'], 'color': report['color_output']}))


if __name__ == '__main__':
    main()
