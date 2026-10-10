from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compile_shader(source, output, entry='main', local=(64, 1, 1), compiler=None, validator=None):
    source, output = Path(source).resolve(), Path(output).resolve()
    compiler = Path(compiler or ROOT / '.tools/userspace/build-mesa-compiler/hackynavi-compiler/hackynavi-aco.exe').resolve()
    validator = Path(validator or ROOT / '.tools/userspace/build-spirv-tools/tools/spirv-val.exe').resolve()
    if output.exists():
        raise ValueError('Output directory already exists')
    if not source.is_file() or not 20 <= source.stat().st_size <= 64 * 1024 * 1024:
        raise ValueError('SPIR-V input must be a regular file of 20 bytes to 64 MiB')
    if len(local) != 3 or any(not 1 <= n <= 1024 for n in local) or local[0] * local[1] * local[2] > 1024:
        raise ValueError('Unsupported local workgroup size')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='hn-aco-', dir=output.parent) as temporary:
        scratch = Path(temporary)
        module = scratch / 'input.spv'
        module.write_bytes(source.read_bytes())
        version = subprocess.check_output([str(validator), '--version'], text=True)
        if 'SPIRV-Tools' not in version:
            raise RuntimeError('The validator is not SPIRV-Tools')
        invalid = scratch / 'invalid.spv'
        invalid.write_bytes(struct.pack('<5I', 0x07230203, 0x00010400, 0, 1, 0))
        rejected = subprocess.run([str(validator), '--target-env', 'vulkan1.2', str(invalid)], capture_output=True, text=True)
        if rejected.returncode == 0:
            raise RuntimeError('The validator accepted an invalid header-only module')
        subprocess.run([str(validator), '--target-env', 'vulkan1.2', str(module)], check=True)
        product = scratch / 'product'
        product.mkdir()
        command = [str(compiler), str(module), entry, *map(str, local), str(product)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError(f'ACO compilation failed ({result.returncode}):\n{result.stdout}{result.stderr}')
        code, metadata = product / 'shader.bin', product / 'shader.json'
        config = json.loads(metadata.read_text())
        if config['target'] != 'gfx1100' or config['wave_size'] != 32 or config['workgroup_size'] != list(local):
            raise RuntimeError('Compiler output target or specialization mismatch')
        if code.stat().st_size != config['code_bytes'] or not 0 < config['executable_bytes'] <= config['code_bytes']:
            raise RuntimeError('Compiler output size mismatch')
        report = {'input_sha256': digest(module), 'spirv_entry_point': entry,
                  'local_size_specialization': dict(zip(('0', '1', '2'), local)),
                  'compiler_sha256': digest(compiler), 'validator_sha256': digest(validator),
                  'validator_version': version.strip(), 'shader_sha256': digest(code),
                  'compiler_stdout': result.stdout, 'compiler_stderr': result.stderr,
                  'hardware_executed': False}
        (product / 'provenance.json').write_text(json.dumps(report, indent=2) + '\n')
        os.rename(product, output)
    return config


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--entry', default='main')
    parser.add_argument('--local', nargs=3, type=int, default=(64, 1, 1))
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--validator', type=Path)
    args = parser.parse_args()
    print(json.dumps(compile_shader(args.input, args.output, args.entry, args.local, args.compiler, args.validator), indent=2))


if __name__ == '__main__':
    main()
