#!/usr/bin/env python3
"""Compile the authored MSL on an existing Mac; install nothing and clean staging."""
import argparse
import base64
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.@-]+", args.host) or args.host.startswith("-"):
        parser.error("Use an SSH host alias or user@hostname")
    inputs = {"add_one.metal": (HERE / "tests/fixtures/add_one.metal").read_bytes(),
              "capture_apple_air.m": (HERE / "tools/capture_apple_air.m").read_bytes(),
              "apple_air_dis.c": (HERE / "tools/apple_air_dis.c").read_bytes(),
              "check_apple_reader.py": (HERE / "tests/check_apple_reader.py").read_bytes()}
    encoded = {key: base64.b64encode(data).decode("ascii") for key, data in inputs.items()}
    script = '''import base64,json,os,pathlib,subprocess,tempfile
inputs = json.loads(INPUTS)
report = {"commands": [], "files": {}, "cleanup": False}
def run(argv, timeout=90):
    try:
        result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)
        record = {"argv": argv, "exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr}
    except subprocess.TimeoutExpired as error:
        record = {"argv": argv, "exit_code": None, "error": "timed out"}
    report["commands"].append(record)
    return record
cache = pathlib.Path.home() / "Library/Caches"
with tempfile.TemporaryDirectory(prefix="HackyNavi-AppleAIR-", dir=str(cache)) as name:
    report["staging"] = name
    stage = pathlib.Path(name)
    for filename,data in inputs.items():
        (stage/filename).write_bytes(base64.b64decode(data))
    run(["sw_vers"])
    run(["df", "-k", str(cache)])
    compiler = run(["xcrun", "--sdk", "macosx", "metal", "-std=macos-metal2.4", "-c", str(stage/"add_one.metal"), "-o", str(stage/"add_one.air")], 60)
    if compiler["exit_code"] == 0 and (stage/"add_one.air").is_file():
        report["compiler_path"] = "xcrun metal"
        run(["xcrun", "--sdk", "macosx", "metal", "--version"], 30)
    else:
        helper = run(["xcrun", "--sdk", "macosx", "clang", "-arch", "x86_64", "-mmacosx-version-min=15.0", "-fobjc-arc", "-framework", "Foundation", "-framework", "Metal", str(stage/"capture_apple_air.m"), "-o", str(stage/"capture-apple-air")])
        if helper["exit_code"] == 0:
            result = run([str(stage/"capture-apple-air"), str(stage/"add_one.metal"), str(stage/"add_one.air")])
            if result["exit_code"] == 0:
                report["compiler_path"] = "system Metal runtime compiler, MTLFunction bitcodeData capture"
    if (stage/"add_one.air").is_file():
        report["files"]["add_one.air"] = base64.b64encode((stage/"add_one.air").read_bytes()).decode("ascii")
        reader = run(["xcrun", "--sdk", "macosx", "clang", "-arch", "x86_64", "-mmacosx-version-min=15.0", "-Wall", "-Wextra", "-Werror", "-O2", str(stage/"apple_air_dis.c"), "-o", str(stage/"apple-air-dis")])
        if reader["exit_code"] == 0:
            checks = run(["/usr/bin/python3", str(stage/"check_apple_reader.py"), str(stage/"apple-air-dis"), str(stage/"add_one.air")])
            decode = run([str(stage/"apple-air-dis"), str(stage/"add_one.air"), "-o", str(stage/"add_one.ll")])
            report["reader_success"] = checks["exit_code"] == 0 and decode["exit_code"] == 0
            if report["reader_success"]:
                for filename in ["apple-air-dis", "add_one.ll"]:
                    report["files"][filename] = base64.b64encode((stage/filename).read_bytes()).decode("ascii")
    sdk = run(["xcrun", "--sdk", "macosx", "--show-sdk-path"], 30)
    if sdk["exit_code"] == 0:
        for filename in ["libiconv.tbd", "libiconv.2.tbd"]:
            path = pathlib.Path(sdk["stdout"].strip()) / "usr/lib" / filename
            if path.is_file():
                report["files"][filename] = base64.b64encode(path.read_bytes()).decode("ascii")
    report["staged_peak_bytes"] = sum(p.stat().st_size for p in stage.rglob("*") if p.is_file())
report["cleanup"] = not pathlib.Path(report["staging"]).exists()
print(json.dumps(report))
'''.replace("INPUTS", repr(json.dumps(encoded)))
    result = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", args.host,
                             "/usr/bin/python3", "-"], input=script, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600)
    output = ROOT / "build/metal-compiler/apple-input"
    output.mkdir(parents=True, exist_ok=True)
    if result.returncode:
        (output / "transport-error.txt").write_text(result.stderr, encoding="utf-8")
        raise RuntimeError(f"Mac fixture capture failed: {result.stderr}")
    report = json.loads(result.stdout)
    files = report.pop("files")
    report["files"] = {}
    for name, encoded_file in files.items():
        if name not in ("add_one.air", "libiconv.tbd", "libiconv.2.tbd", "apple-air-dis", "add_one.ll"):
            raise RuntimeError("unexpected remote artifact name")
        data = base64.b64decode(encoded_file, validate=True)
        (output / name).write_bytes(data)
        report["files"][name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    report["source_sha256"] = {name: hashlib.sha256(data).hexdigest() for name, data in inputs.items()}
    (output / "capture-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if "add_one.air" in report["files"] and report.get("reader_success") and report["cleanup"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
