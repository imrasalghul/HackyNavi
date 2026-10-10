#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bind actual bounded runtime evidence and production architecture metadata."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build/metal-architecture"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads((ROOT / path).read_text())


captured = read("build/metal-architecture/capture.json")
followup = read("build/metal-architecture/followup.json")
for path, source in (("disassembly.json", captured), ("followup-disassembly.json", followup)):
    decoded = json.loads((BASE / path).read_text())
    for name, item in decoded["helpers"].items():
        original = source["helpers"][name]
        raw = bytes.fromhex(original["code_hex"])
        assert len(raw) == original["bytes"]
        assert hashlib.sha256(raw).hexdigest() == original["sha256"] == item["code_sha256"]
        assert item["all_literal_bytes_match"] and item["all_instruction_bytes_decoded"]
        reassembled = b"".join(bytes.fromhex(inst["hex"]) for inst in item["instructions"])
        reassembled += bytes.fromhex(item["explicit_data_suffix"]["hex"])
        assert reassembled == raw
assert captured["helpers"]["MTLArchitecture::init"]["code_hex"] == "4889f8c3000000"
assert captured["helpers"]["MTLArchitecture::init"]["encoding"] == "@16@0:8"
assert captured["classes"]["MTLArchitecture"]["instance_bytes"] == 32
assert captured["classes"]["MTLArchitecture"]["superclass"] == "NSObject"
cf = followup["helpers"]["_CFMZEnabled"]
assert cf["code_hex"] == "554889e5e81e831c00b10183f806740be812831c0083f8020f94c10fb6c15dc390909090"
assert cf["image_uuid"] == "c204e205-b457-333e-8da9-3d11902a7390"
assert cf["sha256"] == captured["helpers"]["_CFMZEnabled"]["sha256"]
target = followup["explicit_references"]["2004102"]["stub_pointer"]["pointee_location"]
assert target["nearest_symbol"] == "dyld_get_active_platform" and target["symbol_offset"] == 0
assert target["image"] == "/usr/lib/system/libdyld.dylib"
refs = read("build/metal-device-hooks/references.json")["explicit_references"]
assert refs["852528240"]["pointee_location"]["nearest_symbol"] == "objc_release"
assert refs["1887094"]["stub_pointer"]["pointee_location"]["nearest_symbol"] == "objc_msgSendSuper2"
assert refs["870154200"]["selector"]["text"] == "dealloc"
assert refs["1887094"]["location"]["image_uuid"] == followup["helpers"]["MTLArchitecture::dealloc"]["image_uuid"]
paths = [
    "userspace/metal/native/HNMetalArchitecture.hpp", "userspace/metal/native/HNMetalArchitecture.mm",
    "userspace/metal/native/HNMetalCapabilities.hpp", "userspace/metal/native/HNMetalCapabilities.mm",
    "docs/metal-architecture-and-platform.md", "scripts/validate-metal-architecture.py",
    "build/metal-bundle-trust/MTLDevice.h", "build/metal-device-hooks/references.json",
    "build/metal-device-hooks/final.json", "build/metal-public-accelerator-route/constructor-references-disassembly.json",
    "build/metal-architecture/capture-input.py", "build/metal-architecture/capture.json",
    "build/metal-architecture/disassembly.json", "build/metal-architecture/followup-input.py",
    "build/metal-architecture/followup.json", "build/metal-architecture/followup-disassembly.json",
    "build/metal-architecture/execution.json", "build/metal-architecture/followup-execution.json",
]
report = {"scope": "actual bounded runtime evidence and production source freeze; no object/GPU execution",
          "validation": "all captured instruction bytes, platform call target, base init/dealloc verified",
          "native_compilation": "not performed in this report", "object_or_gpu_execution": False,
          "remote_files_created": False,
          "files": {path: {"bytes": (ROOT / path).stat().st_size, "sha256": digest(ROOT / path)} for path in paths}}
output = BASE / "source-freeze.json"
if output.exists():
    raise RuntimeError("Preserve the existing freeze; use a new evidence filename for later changes")
output.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({"files": len(paths), "report": str(output), "sha256": digest(output)}))
