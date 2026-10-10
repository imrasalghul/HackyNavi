#!/usr/bin/env python3
"""Exercise actual system AIR reading and reject environment redirection."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

reader = Path(sys.argv[1]).resolve()
air = Path(sys.argv[2]).resolve()
version = subprocess.run([str(reader), "--version"], capture_output=True, text=True, check=True)
assert "Apple AIR reader" in version.stdout and "GPUCompiler" in version.stdout
with tempfile.TemporaryDirectory(prefix="reader-test-", dir=str(reader.parent)) as directory:
    stage = Path(directory)
    output = stage / "actual.ll"
    subprocess.run([str(reader), str(air), "-o", str(output)], check=True)
    text = output.read_text()
    assert "define void @add_one" in text and "store i32" in text
    invalid = stage / "invalid.air"
    invalid.write_bytes(b"BC\xc0\xde" + bytes(20))
    rejected = stage / "rejected.ll"
    result = subprocess.run([str(reader), str(invalid), "-o", str(rejected)], capture_output=True, text=True)
    assert result.returncode != 0 and not rejected.exists(), result
    env = os.environ.copy()
    env["HACKYNAVI_APPLE_LLVM_LIBRARY"] = str(stage / "missing-library.dylib")
    result = subprocess.run([str(reader), "--version"], env=env, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0 and result.stdout == version.stdout, result
    redirected = stage / "environment-ignored.ll"
    result = subprocess.run([str(reader), str(air), "-o", str(redirected)], env=env,
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0 and redirected.read_text() == text, result
    result = subprocess.run([str(reader), str(invalid), "-o", str(rejected)], env=env,
                            capture_output=True, text=True, timeout=30)
    assert result.returncode != 0 and not rejected.exists(), result
print("Apple AIR reader: actual bitcode, invalid bitcode, fixed system reader under bogus environment override, and no error output passed")
