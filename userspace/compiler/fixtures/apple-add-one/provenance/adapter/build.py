from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SOURCE = ROOT / '.research/mesa'
STAGE = ROOT / '.tools/userspace/mesa-compiler-source'
BUILD = ROOT / '.tools/userspace/build-mesa-compiler'
PIN = '35b085c4c067f198ce0b6437d94678e47b9c645f'


def main():
    if subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip() != PIN:
        raise SystemExit('Mesa source pin does not match')
    for args in [['diff', '--quiet'], ['diff', '--cached', '--quiet']]:
        if subprocess.run(['git', '-C', str(SOURCE), *args]).returncode:
            raise SystemExit('Pinned Mesa source has tracked modifications')
    if shutil.disk_usage(ROOT).free < 4 * 1024**3:
        raise SystemExit('At least 4 GiB free local storage is required')
    manifest = []
    for name in subprocess.check_output(['git', '-C', str(SOURCE), 'ls-files'], text=True).splitlines():
        src = SOURCE / name
        if not src.is_file():
            continue
        dst = STAGE / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        data = src.read_bytes()
        if not dst.exists() or dst.read_bytes() != data:
            dst.write_bytes(data)
        manifest.append({'path': name, 'sha256': hashlib.sha256(data).hexdigest()})
    for directory in ['src/gallium/frontends', 'src/gallium/drivers/llvmpipe', 'src/virtio/virtio-gpu']:
        (STAGE / directory).mkdir(parents=True, exist_ok=True)
    root_meson = STAGE / 'meson.build'
    root_meson.write_text(root_meson.read_text().replace('with_gfx_compute = [', 'with_gfx_compute = true\n_unused_gfx_compute = [') + "\ninc_amd = include_directories('src/amd')\nsubdir('src/amd/addrlib')\nsubdir('src/amd/common')\nsubdir('src/amd/compiler')\nsubdir('hackynavi-compiler')\n")
    gpu_info = STAGE / 'src/amd/common/ac_gpu_info.c'
    gpu_info.write_text('#include <errno.h>\n' + gpu_info.read_text())
    addrlib_meson = STAGE / 'src/amd/addrlib/meson.build'
    addrlib_meson.write_text(addrlib_meson.read_text().replace("cpp_args_addrlib += '-DADDR_ALLOW_SIMD=1'", "cpp_args_addrlib += '-DADDR_ALLOW_SIMD=0'"))
    adapter = STAGE / 'hackynavi-compiler'
    adapter.mkdir(exist_ok=True)
    for name in ['compiler.c', 'hackynavi_gfx11.h', 'meson.build']:
        shutil.copyfile(HERE / name, adapter / name)
    (HERE / 'source-manifest.json').write_text(json.dumps({'repository': 'https://gitlab.freedesktop.org/mesa/mesa.git', 'commit': PIN, 'files': manifest, 'staged_patches': ['Enable with_gfx_compute without enabling GPU driver frontends; append AMD common, ACO, and HackyNavi compiler subdirectories to root Meson.', 'Prepend errno.h to src/amd/common/ac_gpu_info.c for missing Windows EINVAL/ENODEV declarations.', 'Select actual scalar addrlib implementation with ADDR_ALLOW_SIMD=0 because the Zig Windows runtime lacks __cpu_model for AVX2 feature detection.']}, indent=2) + '\n')
    env = os.environ.copy()
    env['PYTHONPATH'] = str(ROOT / '.tools/userspace/python-build')
    env['PATH'] = str(ROOT / '.tools/build-python/bin') + os.pathsep + env.get('PATH', '')
    zig = (ROOT / '.tools/zig-x86_64-windows-0.17.0/zig.exe').as_posix()
    native = HERE / 'native.local.ini'
    native.write_text("[binaries]\nc = ['" + zig + "', 'cc']\ncpp = ['" + zig + "', 'c++']\nar = ['" + zig + "', 'ar']\nstrip = ['" + zig + "', 'objcopy', '--strip-all']\n")
    command = [sys.executable, '-m', 'mesonbuild.mesonmain', 'setup', str(BUILD), str(STAGE), '--native-file', str(native), '-Dgallium-drivers=', '-Dvulkan-drivers=', '-Dplatforms=', '-Dglx=disabled', '-Degl=disabled', '-Dgbm=disabled', '-Dgles1=disabled', '-Dgles2=disabled', '-Dopengl=false', '-Dllvm=disabled', '-Damd-use-llvm=false', '-Dshared-glapi=disabled', '-Dbuild-tests=false', '-Dtools=', '-Dxmlconfig=disabled', '-Dzstd=disabled', '-Dshader-cache=disabled', '-Dvalgrind=disabled', '-Dlibunwind=disabled', '-Dbuildtype=debugoptimized', '-Db_ndebug=false', '--wrap-mode=nofallback']
    command += ['-Dzlib=disabled', '-Dmin-windows-version=7', '-Dplatforms=windows']
    if (BUILD / 'meson-private/coredata.dat').exists():
        command.insert(4, '--reconfigure')
    subprocess.run(command, env=env, check=True)
    subprocess.run([str(ROOT / '.tools/build-python/bin/ninja.exe'), '-C', str(BUILD), '-j4', 'hackynavi-compiler/hackynavi-aco.exe', 'hackynavi-compiler/libhackynavi_gfx11.dll'], env=env, check=True)
    binary = BUILD / 'hackynavi-compiler/hackynavi-aco.exe'
    library = BUILD / 'hackynavi-compiler/libhackynavi_gfx11.dll'
    report = {
        'mesa_commit': PIN,
        'source_manifest_sha256': hashlib.sha256((HERE / 'source-manifest.json').read_bytes()).hexdigest(),
        'adapter_sources': {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest() for name in ['compiler.c', 'hackynavi_gfx11.h', 'meson.build', 'build.py']},
        'binary': str(binary), 'binary_bytes': binary.stat().st_size,
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'library': str(library), 'library_bytes': library.stat().st_size,
        'library_sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
        'zig_version': subprocess.check_output([zig, 'version'], text=True).strip(),
        'configure_command': command,
    }
    destination = ROOT / 'build/gfx11-compiler'
    destination.mkdir(exist_ok=True)
    (destination / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
