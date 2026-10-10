#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Retain real owned pipeline/standalone ACO products for decoder research.

No GPU execution or native admission. Requires existing compiler/translator
tools; METAL2VULKAN_LLVM_DIS and METAL2VULKAN_SPIRV_VAL select those real tools.
"""
import argparse
import ctypes as c
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--compiler', required=True, type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).parent / 'fixtures/compute-products/products.json')
    args = parser.parse_args()
    library = c.CDLL(str(args.library.resolve()))
    compiler = args.compiler.resolve()
    handle, u32, triplet = c.c_void_p, c.c_uint32, c.c_uint32 * 3
    library.hn_shader_compile.argtypes = [handle, c.c_size_t, u32, u32, c.c_char_p] + [u32] * 5
    library.hn_shader_compile.restype = handle
    library.hn_pipeline_build.argtypes = [handle, u32, c.POINTER(u32), c.POINTER(u32), c.c_char_p]
    library.hn_pipeline_build.restype = handle
    for name in ('hn_shader_result_error', 'hn_pipeline_error'):
        function = getattr(library, name)
        function.argtypes, function.restype = [handle], c.c_char_p
    for name in ('hn_shader_result_free', 'hn_pipeline_free'):
        function = getattr(library, name)
        function.argtypes, function.restype = [handle], None
    library.hn_pipeline_blob_size.argtypes = [handle, u32, u32]
    library.hn_pipeline_blob_size.restype = c.c_size_t
    library.hn_pipeline_copy_blob.argtypes = [handle, u32, u32, handle, c.c_size_t]
    library.hn_pipeline_copy_blob.restype = c.c_int

    def blob(product, kind, index=0):
        size = library.hn_pipeline_blob_size(product, kind, index)
        if not 0 < size <= 1024 * 1024:
            raise RuntimeError('absent or oversized pipeline blob')
        memory = c.create_string_buffer(size)
        if library.hn_pipeline_copy_blob(product, kind, index, memory, size) != 1:
            raise RuntimeError('pipeline blob copy failed')
        return memory.raw

    fixtures = ROOT / 'userspace/metal/tests/fixtures'
    variants = []
    for name, path, fmt in [('add_one', fixtures / 'apple/add_one.air', 0),
                            ('affine_xor', fixtures / 'affine_xor.ll', 1)]:
        source = path.read_bytes()
        memory = c.create_string_buffer(source)
        shader = library.hn_shader_compile(memory, len(source), fmt, 0, name.encode(), 64, 1, 1, 1, 0)
        if not shader:
            raise RuntimeError('shader allocation failed')
        try:
            error = library.hn_shader_result_error(shader)
            if error:
                raise RuntimeError(error.decode())
            product = library.hn_pipeline_build(shader, 1, triplet(4099, 1, 1), triplet(64, 1, 1), str(compiler).encode())
        finally:
            library.hn_shader_result_free(shader)
        if not product:
            raise RuntimeError('pipeline allocation failed')
        try:
            error = library.hn_pipeline_error(product)
            if error:
                raise RuntimeError(error.decode())
            spv, entry = blob(product, 2), blob(product, 4).decode()
            for index, local in enumerate((64, 3)):
                code, metadata = blob(product, 0, index), blob(product, 1, index)
                with tempfile.TemporaryDirectory(prefix='hn-decoder-product-') as temporary:
                    directory = Path(temporary)
                    (directory / 'input.spv').write_bytes(spv)
                    subprocess.run([str(compiler), str(directory / 'input.spv'), entry, str(local), '1', '1', str(directory)],
                                   check=True, capture_output=True, timeout=120)
                    if code != (directory / 'shader.bin').read_bytes() or json.loads(metadata) != json.loads((directory / 'shader.json').read_bytes()):
                        raise RuntimeError('owned product differs from standalone ACO')
                variants.append({'kernel': name, 'local': [local, 1, 1],
                    'source': path.relative_to(ROOT).as_posix(), 'source_sha256': sha(source),
                    'spirv_sha256': sha(spv), 'code_sha256': sha(code), 'code_hex': code.hex(),
                    'metadata': json.loads(metadata), 'standalone_equal': True})
        finally:
            library.hn_pipeline_free(product)
    result = {'schema': 1, 'hardware_executed': False, 'native_admission': False,
              'compiler_sha256': sha(compiler.read_bytes()),
              'pipeline_library_sha256': sha(args.library.read_bytes()), 'variants': variants}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f'Retained {len(variants)} real variants in {args.output}; no GPU execution')


if __name__ == '__main__':
    main()
