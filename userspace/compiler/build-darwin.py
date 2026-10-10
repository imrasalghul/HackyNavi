from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

from build import ROOT, HERE, SOURCE, PIN


def main():
    stage = ROOT / '.tools/userspace/mesa-compiler-source-darwin'
    build = ROOT / '.tools/userspace/build-mesa-compiler-darwin'
    if subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip() != PIN:
        raise SystemExit('Mesa source pin does not match')
    if shutil.disk_usage(ROOT).free < 4 * 1024**3:
        raise SystemExit('At least 4 GiB free local storage is required')
    manifest_path = HERE / 'source-manifest.json'
    manifest = json.loads(manifest_path.read_text())
    for entry in manifest['files']:
        data = (SOURCE / entry['path']).read_bytes()
        if hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise SystemExit('Mesa source hash mismatch: ' + entry['path'])
        destination = stage / entry['path']
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists() or destination.read_bytes() != data:
            destination.write_bytes(data)
    for directory in ['src/gallium/frontends', 'src/gallium/drivers/llvmpipe', 'src/virtio/virtio-gpu']:
        (stage / directory).mkdir(parents=True, exist_ok=True)
    root_meson = stage / 'meson.build'
    root_meson.write_text(root_meson.read_text().replace('with_gfx_compute = [', 'with_gfx_compute = true\n_unused_gfx_compute = [') + "\ninc_amd = include_directories('src/amd')\nsubdir('src/amd/addrlib')\nsubdir('src/amd/common')\nsubdir('src/amd/compiler')\nsubdir('hackynavi-compiler')\n")
    root_meson.write_text(root_meson.read_text().replace("find_program('nm')", "find_program('nm', required: with_tests)"))
    root_meson.write_text(root_meson.read_text().replace('pre_args = [', "pre_args = ['-DHAVE_TSERVER',", 1))
    gpu_source = (SOURCE / 'src/amd/common/ac_gpu_info.c').read_text()
    start = gpu_source.index('void\nac_fill_compiler_info(')
    end = gpu_source.index('\nvoid\nac_fill_tiling_info(', start)
    function = gpu_source[start:end]
    profile = gpu_source[:gpu_source.index('*/') + 2] + '\n#include "ac_gpu_info.h"\n#include "drm-uapi/amdgpu_drm.h"\n#include "util/macros.h"\n#include "util/u_math.h"\n\n' + function
    profile_file = stage / 'src/amd/common/ac_compiler_profile.c'
    profile_file.write_text(profile)
    common_meson = stage / 'src/amd/common/meson.build'
    common = common_meson.read_text()
    start = common.index('amd_common_files = files(')
    end = common.index('if with_gfx_compute', start)
    common = common[:start] + "amd_common_files = files('ac_binary.c', 'ac_shader_args.c', 'ac_shader_util.c', 'ac_compiler_profile.c', 'ac_descriptors.c', 'ac_formats.c', 'amd_family.c')\n\n" + common[end:]
    linux_block = "if not with_platform_windows\n  amd_common_files += files(\n    'ac_linux_drm.c',\n  )\nendif\n"
    if linux_block not in common:
        raise SystemExit('Pinned Mesa common Meson layout has changed')
    common_meson.write_text(common.replace(linux_block, ''))
    adapter = stage / 'hackynavi-compiler'
    adapter.mkdir(exist_ok=True)
    for name in ['compiler.c', 'hackynavi_gfx11.h', 'meson.build']:
        shutil.copyfile(HERE / name, adapter / name)
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / '.tools/userspace/python-build')
    env['PATH'] = str(ROOT / '.tools/build-python/bin') + os.pathsep + env.get('PATH', '')
    zig = (ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe').as_posix()
    cross = HERE / 'darwin.local.ini'
    cross.write_text("[binaries]\nc = ['" + zig + "', 'cc', '-target', 'x86_64-macos.15.0']\ncpp = ['" + zig + "', 'c++', '-target', 'x86_64-macos.15.0']\nobjc = ['" + zig + "', 'cc', '-target', 'x86_64-macos.15.0']\nar = ['" + zig + "', 'ar']\nstrip = ['" + zig + "', 'objcopy', '--strip-all']\n[host_machine]\nsystem = 'darwin'\ncpu_family = 'x86_64'\ncpu = 'x86_64'\nendian = 'little'\n[properties]\nneeds_exe_wrapper = true\n")
    command = [sys.executable, '-m', 'mesonbuild.mesonmain', 'setup', str(build), str(stage), '--cross-file', str(cross), '-Dgallium-drivers=', '-Dvulkan-drivers=', '-Dplatforms=', '-Dglx=disabled', '-Degl=disabled', '-Dgbm=disabled', '-Dgles1=disabled', '-Dgles2=disabled', '-Dopengl=false', '-Dllvm=disabled', '-Damd-use-llvm=false', '-Dshared-glapi=disabled', '-Dbuild-tests=false', '-Dtools=', '-Dxmlconfig=disabled', '-Dzstd=disabled', '-Dzlib=disabled', '-Dshader-cache=disabled', '-Dvalgrind=disabled', '-Dlibunwind=disabled', '-Dbuildtype=debugoptimized', '-Db_ndebug=false', '--wrap-mode=nofallback']
    if (build / 'meson-private/coredata.dat').exists():
        command.insert(4, '--reconfigure')
    provenance = {
        'mesa_commit': PIN, 'base_manifest_sha256': hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
        'extraction': {'source': 'src/amd/common/ac_gpu_info.c', 'function': 'ac_fill_compiler_info',
                       'function_sha256_utf8_lf': hashlib.sha256(function.encode()).hexdigest(),
                       'compiled_file_sha256': hashlib.sha256(profile_file.read_bytes()).hexdigest()},
        'staged_patches': ['Enable compiler dependencies without GPU API frontends.',
                          'Compile AMD common compiler helpers and NIR only; omit DRM/winsys/platform query sources.',
                          'Require nm only when the disabled upstream test suite needs symbol checks.',
                          'Define HAVE_TSERVER to select addrlib public typedefs instead of Apple-private addrExtDef.h.',
                          'Extract exact pinned ac_fill_compiler_info with original notice and genuine DRM type definitions.'],
        'configure_command': command,
    }
    (HERE / 'source-manifest-darwin.json').write_text(json.dumps(provenance, indent=2) + '\n')
    subprocess.run(command, env=env, check=True)
    subprocess.run([str(ROOT / '.tools/build-python/bin/ninja.exe'), '-C', str(build), '-j4', 'hackynavi-compiler/hackynavi-aco', 'hackynavi-compiler/libhackynavi_gfx11.dylib'], env=env, check=True)
    binary = build / 'hackynavi-compiler/hackynavi-aco'
    library = build / 'hackynavi-compiler/libhackynavi_gfx11.dylib'
    provenance.update({'binary': str(binary), 'binary_bytes': binary.stat().st_size,
                       'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                       'library': str(library), 'library_bytes': library.stat().st_size,
                       'library_sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
                       'adapter_sources': {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest() for name in
                                           ['compiler.c', 'hackynavi_gfx11.h', 'meson.build', 'build-darwin.py']}})
    destination = ROOT / 'build/gfx11-compiler'
    destination.mkdir(exist_ok=True)
    (destination / 'darwin-build-report.json').write_text(json.dumps(provenance, indent=2) + '\n')


if __name__ == '__main__':
    main()
