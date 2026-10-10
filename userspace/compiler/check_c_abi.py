import ctypes
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def main():
    library, validator, input_path, expected = map(lambda value: Path(value).resolve(), sys.argv[1:5])
    version = subprocess.check_output([str(validator), '--version'], text=True)
    if 'SPIRV-Tools' not in version:
        raise RuntimeError('Real SPIRV-Tools validator required')
    with tempfile.TemporaryDirectory(prefix='abi-', dir=library.parent) as name:
        stage = Path(name)
        invalid = stage / 'invalid.spv'
        invalid.write_bytes(struct.pack('<5I', 0x07230203, 0x10400, 0, 1, 0))
        if subprocess.run([str(validator), '--target-env', 'vulkan1.2', str(invalid)], capture_output=True).returncode == 0:
            raise RuntimeError('Validator accepted an invalid module')
        subprocess.run([str(validator), '--target-env', 'vulkan1.2', str(input_path)], check=True)
        lib = ctypes.CDLL(str(library))
        lib.hn_gfx11_abi_version.restype = ctypes.c_uint32
        assert lib.hn_gfx11_abi_version() == 1
        descriptor = lib.hn_gfx11_raw_buffer_descriptor
        descriptor.argtypes = [ctypes.c_uint64, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
        descriptor.restype = ctypes.c_int
        words = (ctypes.c_uint32 * 4)()
        assert descriptor(0x1234000000, 16384, words) == 0
        assert list(words) == [0x34000000, 0x12, 16384, 805400492]
        assert descriptor(0xffffffffffff, 16384, words) != 0
        assert descriptor(0x1000, 4, None) != 0
        compile_file = lib.hn_gfx11_compile_validated_file
        compile_file.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_char_p]
        compile_file.restype = ctypes.c_int
        local = (ctypes.c_uint32 * 3)(64, 1, 1)
        target = stage / 'product'
        target.mkdir()
        assert compile_file(str(input_path).encode(), b'main', local, str(target).encode()) == 0
        assert (target / 'shader.bin').read_bytes() == (expected / 'shader.bin').read_bytes()
        metadata = json.loads((target / 'shader.json').read_text())
        assert metadata == json.loads((expected / 'shader.json').read_text())
        failed = stage / 'failed'
        failed.mkdir()
        for _ in range(16):
            assert compile_file(str(input_path).encode(), b'absent', local, str(failed).encode()) != 0
        assert not list(failed.iterdir())
        assert compile_file(None, b'main', local, str(failed).encode()) != 0
        assert compile_file(str(input_path).encode(), b'main', None, str(failed).encode()) != 0
        repeat = stage / 'repeat'
        repeat.mkdir()
        assert compile_file(str(input_path).encode(), b'main', local, str(repeat).encode()) == 0
        assert (repeat / 'shader.bin').read_bytes() == (expected / 'shader.bin').read_bytes()
        print(json.dumps({'success': True, 'library_sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
                          'shader_sha256': hashlib.sha256((target / 'shader.bin').read_bytes()).hexdigest(),
                          'checks': ['ABI version', 'real validator and invalid module rejection', 'descriptor words and bounds',
                                     'native C ABI compiled Apple-derived SPIR-V', 'code and metadata match Windows baseline',
                                     'repeated wrong-entry failures publish no output', 'null argument rejection',
                                     'valid compile remains deterministic after failures'],
                          'hardware_executed': False}))


if __name__ == '__main__':
    main()
