#!/usr/bin/env python3
"""Load the built native compiler library and exercise its exported C ABI."""
import ctypes as c
import json
import re
from pathlib import Path
import sys

library = c.CDLL(str(Path(sys.argv[1]).resolve()))
handle = c.c_void_p
word_pointer = c.POINTER(c.c_uint32)
library.hn_shader_abi_version.restype = c.c_uint32
library.hn_shader_compile.argtypes = [c.c_void_p, c.c_size_t, c.c_uint32, c.c_uint32, c.c_char_p] + [c.c_uint32] * 5
library.hn_shader_compile.restype = handle
library.hn_shader_result_words.argtypes = [handle, c.POINTER(c.c_size_t)]
library.hn_shader_result_words.restype = word_pointer
for name in ["reflection", "error", "spirv_entry_point"]:
    function = getattr(library, "hn_shader_result_" + name)
    function.argtypes = [handle]
    function.restype = c.c_char_p
library.hn_shader_result_free.argtypes = [handle]
library.hn_shader_result_free.restype = None
assert library.hn_shader_abi_version() == 1
source = (Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).parent / "fixtures/add_one.ll").read_bytes()
is_air = source.startswith(b"BC\xc0\xde") or source.startswith(b"\xde\xc0\x17\x0b")
for valid in [True, False]:
    data = source
    if not valid and is_air:
        data = b"BAD!" + source[4:]
    elif not valid:
        data, count = re.subn(rb"(%[A-Za-z0-9_.]+) = add i32 (%[A-Za-z0-9_.]+), 1", rb"\1 = call i32 @llvm.agx3.emask.i32(i32 \2)", source)
        assert count == 1, "negative test requires exactly one add instruction to replace"
    memory = c.create_string_buffer(data)
    result = library.hn_shader_compile(memory, len(data), 0 if is_air else 1, 0, b"add_one", 64, 1, 1, 1, 0)
    assert result
    try:
        size = c.c_size_t(999)
        words = library.hn_shader_result_words(result, c.byref(size))
        error = library.hn_shader_result_error(result)
        if valid:
            assert not error, error
            assert bool(words) and size.value > 5 and words[0] == 0x07230203
            assert library.hn_shader_result_spirv_entry_point(result)
            reflection = json.loads(library.hn_shader_result_reflection(result))
            assert reflection["entry_point"] == "add_one"
            assert len(reflection["bindings"]) == 2
        else:
            assert not bool(words) and size.value == 0
            assert (b"not LLVM bitcode" if is_air else b"@llvm.agx3.") in error, error
    finally:
        library.hn_shader_result_free(result)
print("Native C ABI: DLL/dylib load, compiled words, reflection, entry name, error result, and disposal passed")
