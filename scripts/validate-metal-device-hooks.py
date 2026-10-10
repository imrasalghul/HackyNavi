#!/usr/bin/env python3
"""Hash-bind bounded captured device hooks. No SSH or target execution."""
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build/metal-device-hooks"
UUID = "c3efbabe-6b06-33ab-bc45-43fe86297aa1"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read(name):
    return json.loads((BASE / name).read_text(encoding="utf-8"))


def pair(measured, decoded):
    capture, disassembly = read(measured), read(decoded)
    assert disassembly["capture_sha256"] == digest((BASE / measured).read_bytes())
    assert disassembly["target_executed"] is False
    for flag in ("device_enumerated", "device_instantiated", "gpu_submitted",
                 "objc_messages_sent", "remote_files_created", "user_client_opened"):
        assert capture[flag] is False
    rows = {}
    for name, item in disassembly["helpers"].items():
        actual = capture["helpers"][name]
        if name != "gpu_bundle_find_trusted":
            assert actual["image_uuid"] == UUID
        assert actual["function_start_matched"] and actual["function_end_bounded"]
        assert not actual["truncated"]
        code = bytes.fromhex(actual["code_hex"])
        assert len(code) == actual["bytes"]
        assert digest(code) == actual["sha256"] == item["code_sha256"]
        offset, instructions = actual["image_offset"], bytearray()
        for instruction in item["instructions"]:
            assert instruction["image_offset"] == offset
            raw = bytes.fromhex(instruction["hex"])
            instructions.extend(raw)
            offset += len(raw)
        suffix = item["explicit_data_suffix"]
        trailing = bytes.fromhex(suffix["hex"])
        assert suffix["offset"] == len(instructions)
        assert bytes(instructions) + trailing == code
        assert item["all_literal_bytes_match"] and item["all_instruction_bytes_decoded"]
        if name != "_MTLDevice::initGPUFamilySupport":
            assert trailing == b"\0" * len(trailing)
        rows[name] = {"offset": actual["image_offset"], "bytes": len(code),
                      "code_sha256": digest(code), "data_bytes": len(trailing)}
    return rows


def main():
    sets = {a: pair(a, b) for a, b in (
        ("capture.json", "disassembly.json"),
        ("references.json", "references-disassembly.json"),
        ("hooks-code.json", "hooks-disassembly-final.json"),
        ("final.json", "final-disassembly.json"))}
    derived = read("hooks-code.json")
    original = read("hooks-followup.json")
    assert derived["source_capture_sha256"] == digest((BASE / "hooks-followup.json").read_bytes())
    assert derived["helpers"] == {k: v for k, v in original["helpers"].items() if "code_hex" in v}
    name = "_MTLDevice::initGPUFamilySupport"
    decoded = read("hooks-disassembly-final.json")["helpers"][name]
    suffix = decoded["explicit_data_suffix"]
    table = bytes.fromhex(suffix["hex"])
    assert len(table) == 48
    table_offset = original["helpers"][name]["image_offset"] + suffix["offset"]
    assert table_offset == 0x55f4
    targets = [table_offset + struct.unpack_from("<i", table, n * 4)[0] for n in range(12)]
    assert all(t in {i["image_offset"] for i in decoded["instructions"]} for t in targets)
    # The earlier decode incorrectly classified the final RET as padding.
    # Preserve it as historical output and use the corrected complete prefix.
    tier = read("hooks-disassembly-final.json")["helpers"]["_MTLDevice::argumentBuffersSupport"]
    assert tier["explicit_data_suffix"] == {"offset": 31, "hex": "00"}
    assert tier["instructions"][-1]["hex"] == "c3"
    metadata = read("capture.json")["protocols"]["MTLDeviceSPI"]
    required = {i["selector"]: i["encoding"] for i in metadata["required_instance"]}
    assert len(required) == 392 and len(metadata["optional_instance"]) == 80
    assert required["indirectArgumentBufferCapabilities"] == "{IndirectArgumentBufferCapabilities=b1b1b1b29}16@0:8"
    final = read("final.json")
    assert final["explicit_references"][str(0x33dda850)]["selector"]["text"] == "_deviceWrapper"
    for selector in ("featureProfile", "maxBufferLength", "registryID"):
        assert final["base_selected_encodings"][selector] == {"present": False}
    for p in BASE.glob("*execution.json"):
        execution = json.loads(p.read_text())
        assert execution["exit_code"] == 0 and not execution["stderr"]
    paths = [p for p in BASE.iterdir() if p.is_file() and p.name != "validation.json"]
    paths += [Path(__file__).resolve(), ROOT / "scripts/measure-metal-device-hooks.py",
              ROOT / "scripts/measure-metal-initializers.py", ROOT / "scripts/disassemble-metal-loader.py",
              ROOT / "scripts/metal-initializer-reference-reader.py", ROOT / "docs/metal-device-hook-contract.md"]
    report = {"status": "measured_hook_lifetime_and_typed_capability_contract",
              "metal_uuid": UUID, "target_executed": False, "gpu_executed": False,
              "checked_sets": sets, "family_table": {"offset": table_offset, "targets": targets},
              "required_spi_methods": len(required), "optional_spi_methods": len(metadata["optional_instance"]),
              "remaining": ["owned limits and query state", "typed unsupported SPI methods",
                            "argument-buffer tier implementation", "real device construction and GPU execution"],
              "files": {str(p.relative_to(ROOT)).replace("\\", "/"):
                        {"bytes": p.stat().st_size, "sha256": digest(p.read_bytes())}
                        for p in sorted(set(paths))}}
    output = BASE / "validation.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(output.relative_to(ROOT)), "sha256": digest(output.read_bytes()),
                      "files_bound": len(paths), "function_occurrences": sum(map(len, sets.values()))}))


if __name__ == "__main__":
    main()
