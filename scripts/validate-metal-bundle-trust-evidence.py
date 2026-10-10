#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate bounded loader evidence locally; no policy, device or GPU calls."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build/metal-bundle-trust"
OUT = BASE / "validation.json"
if OUT.exists():
    raise RuntimeError("Preserve prior validation; use a new evidence directory")

def sha(raw):
    return hashlib.sha256(raw).hexdigest()

captures = [("capture", "disassembly"), ("references-2", "references-2-disassembly"),
            ("policy-2", "policy-2-disassembly")]
for name, assembly_name in captures:
    raw = (BASE / (name + ".json")).read_bytes()
    capture = json.loads(raw)
    assembly = json.loads((BASE / (assembly_name + ".json")).read_bytes())
    execution = json.loads((BASE / (name + "-execution.json")).read_bytes())
    assert execution["exit_code"] == 0 and not execution["stderr"]
    assert execution["input_sha256"] == sha((BASE / (name + "-input.py")).read_bytes())
    assert assembly["capture_sha256"] == sha(raw)
    assert not assembly["target_executed"] and not capture["policy_called"]
    assert not capture["metal_object_created"] and not capture["gpu_executed"]
    assert not capture["remote_files_created"]
    total = 0
    for helper_name, helper in capture["helpers"].items():
        code = bytes.fromhex(helper["code_hex"])
        assert len(code) == helper["bytes"] and sha(code) == helper["sha256"]
        assert helper["function_start_matched"] and helper["function_end_bounded"]
        assert not helper["truncated"]
        decoded = assembly["helpers"][helper_name]
        assert decoded["code_sha256"] == helper["sha256"]
        assert decoded["all_literal_bytes_match"] and decoded["all_captured_bytes_decoded_as_instructions"]
        position = helper["image_offset"]
        combined = bytearray()
        for instruction in decoded["instructions"]:
            assert instruction["image_offset"] == position
            piece = bytes.fromhex(instruction["hex"])
            combined.extend(piece)
            position += len(piece)
        assert bytes(combined) == code
        total += len(code)
    assert total == capture["total_code_bytes"] and total <= 16384

policy = json.loads((BASE / "policy-2.json").read_bytes())
assembly = json.loads((BASE / "policy-2-disassembly.json").read_bytes())
assert policy["image"]["uuid"] == "4f88fd83-3d9b-30c6-aac7-f9d3d2ace5b9"
assert [entry["text"] for entry in policy["paths"]] == ["/Library/GPUBundles", "/System/Library/Extensions"]
array = policy["path_array"]
assert array["image_offset"] == 0x32d12120 and array["bytes"] == 16
assert sha(bytes.fromhex(array["hex"])) == array["sha256"]
refs = policy["explicit_references"]
assert refs["0x6070"]["text"] == "KernelExtensionManagement"
assert refs["0x644b"]["text"] == "file-write-data"
assert refs["0x5c45"]["text"] == "Sandbox"
assert refs["0x580c"]["pointee"]["nearest_symbol"] == "csr_check"
assert refs["0x57a6"]["pointee"]["nearest_symbol"] == "__mac_syscall"
assert refs["0x57b8"]["pointee"]["nearest_symbol"] == "_dyld_shared_cache_contains_path"
instructions = {i["image_offset"]: i for i in assembly["helpers"]["selected_tail_at_0x87b"]["instructions"]}
for address, code in {0x898: "bf02000000", 0x89d: "e86a4f0000", 0x8a2: "4531e4",
                      0x8a5: "85c0", 0x8a7: "0f842d010000", 0x9da: "4489e0", 0x9ee: "c3"}.items():
    assert instructions[address]["hex"] == code
assert instructions[0x89d]["direct_target"] == 0x580c
assert instructions[0x8a7]["branch_target"] == 0x9da
assert any(line["text"] == "KernelExtensionManagement\t/Library/GPUBundles"
           for line in policy["rootless_configuration"]["selected_lines"])
assert bytes.fromhex(policy["path_metadata"]["/Library/GPUBundles"]["com.apple.rootless_hex"]) == b"KernelExtensionManagement"

for source_dir, names in [(BASE / "apple-source", None), (BASE / "apple-source/xnu", None)]:
    provenance = json.loads((source_dir / "provenance.json").read_bytes())
    for name, metadata in provenance["files"].items():
        path = source_dir / (Path(name).name if source_dir.name == "xnu" else name)
        raw = path.read_bytes()
        assert len(raw) == metadata["bytes"] and sha(raw) == metadata["sha256"]
        assert provenance["commit"] in metadata["url"]
csr = (BASE / "apple-source/xnu/csr.h").read_text()
kernel = (BASE / "apple-source/xnu/kern_csr.c").read_text()
assert "#define CSR_ALLOW_UNRESTRICTED_FS               (1 << 1)" in csr
assert "ret = ((config & mask) == mask) ? 0 : EPERM;" in kernel
for key, name in [("public_sdk_header", "MTLDevice.h"), ("public_sdk_tbd", "Metal.tbd")]:
    raw = (BASE / name).read_bytes()
    assert len(raw) == policy[key]["bytes"] and sha(raw) == policy[key]["sha256"]
assert all(value["matches_runtime"] and value["address"] == value["runtime_address"]
           for value in policy["device_class_exports"].values())
native = json.loads((ROOT / "build/native-compute-command-final/report.json").read_bytes())
assert native["native"]["public_metal_headers"]["MTLDevice.h"]["sha256"] == policy["public_sdk_header"]["sha256"]

paths = sorted(path for path in BASE.rglob("*") if path.is_file())
paths += [ROOT / name for name in ["scripts/measure-metal-bundle-trust.py",
          "scripts/measure-metal-bundle-trust-references.py", "scripts/measure-metal-bundle-trust-policy.py",
          "scripts/disassemble-metal-loader.py", "scripts/validate-metal-bundle-trust-evidence.py",
          "docs/metal-bundle-trust.md"]]
report = {"success": True, "scope": "Measured bundle lookup/trust and explicit filesystem-development branch",
          "files": {str(path.relative_to(ROOT)).replace("\\", "/"): {"bytes": path.stat().st_size,
                    "sha256": sha(path.read_bytes())} for path in paths},
          "unique_code_bytes": policy["total_code_bytes"], "path_array_order_verified": True,
          "csr_allow_unrestricted_fs_early_success_verified": True,
          "historical_stager_separate_from_loader": True, "sdk_header_matches_native_build": True,
          "real_class_and_metaclass_exports_verified": True,
          "policy_function_called": False, "policy_changed": False, "bundle_installed": False,
          "metal_object_created": False, "gpu_executed": False, "remote_files_created": False,
          "limitations": ["Measured15.8 build24H22, not target15.8.1",
                          "Private kernel normal-policy decision not reimplemented",
                          "Current installed staging-service equivalence to published source unproved",
                          "NSBundle/dyld loading and target device construction untested"]}
OUT.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({"success": True, "bound_files": len(report["files"]), "report_sha256": sha(OUT.read_bytes())}))
