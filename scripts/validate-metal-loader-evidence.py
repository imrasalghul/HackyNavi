#!/usr/bin/env python3
"""Validate already captured loader evidence; no SSH or target execution."""
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "build/metal-public-accelerator-route"
UUID = "c3efbabe-6b06-33ab-bc45-43fe86297aa1"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read(name):
    return json.loads((EVIDENCE / name).read_text(encoding="utf-8"))


def validate_pair(capture_name, decoded_name):
    capture = read(capture_name)
    decoded = read(decoded_name)
    assert decoded["capture_sha256"] == sha((EVIDENCE / capture_name).read_bytes())
    assert decoded["target_executed"] is False
    rows = {}
    for name, dis in decoded["helpers"].items():
        measured = capture["helpers"][name]
        raw = bytes.fromhex(measured["code_hex"])
        assert measured["image_uuid"] == UUID
        assert measured["function_start_matched"]
        assert measured["function_end_bounded"] and not measured["truncated"]
        assert len(raw) == measured["bytes"]
        assert sha(raw) == measured["sha256"] == dis["code_sha256"]
        position = measured["image_offset"]
        prefix = bytearray()
        for instruction in dis["instructions"]:
            assert instruction["image_offset"] == position
            literal = bytes.fromhex(instruction["hex"])
            prefix.extend(literal)
            position += len(literal)
        suffix = dis["explicit_data_suffix"]
        trailing = bytes.fromhex(suffix["hex"]) if suffix else b""
        if suffix:
            assert len(prefix) == suffix["offset"]
        assert bytes(prefix) + trailing == raw
        assert dis["all_literal_bytes_match"] and dis["all_instruction_bytes_decoded"]
        rows[name] = {"image_offset": measured["image_offset"],
                      "bytes": len(raw), "code_sha256": sha(raw),
                      "instruction_count": len(dis["instructions"]),
                      "explicit_data_bytes": len(trailing)}
    return rows


def jump_table(name, count, padding):
    capture = read("followup2.json")["helpers"][name]
    decoded = read("followup2-disassembly.json")["helpers"][name]
    suffix = decoded["explicit_data_suffix"]
    raw = bytes.fromhex(suffix["hex"])
    assert len(raw) == count * 4 + padding
    assert raw[count * 4:] == b"\0" * padding
    base = capture["image_offset"] + suffix["offset"]
    targets = [base + struct.unpack_from("<i", raw, i * 4)[0] for i in range(count)]
    boundaries = {i["image_offset"] for i in decoded["instructions"]}
    assert all(t in boundaries for t in targets)
    return {"name": name, "table_offset": base, "entries": count,
            "targets": targets, "padding_bytes": padding,
            "all_targets_are_captured_instruction_boundaries": True}


def main():
    facts = read("publication-references.json")
    for field in ("device_enumerated", "device_instantiated", "gpu_submitted",
                  "objc_messages_sent", "remote_files_created", "user_client_opened"):
        assert facts[field] is False
    assert facts["publication_method_encodings"]["_setAcceleratorService:"] == {"present": False}
    assert {v["name"] for v in facts["publication_protocols"].values()} == {"MTLDevice", "MTLDeviceSPI"}
    assert {v["text"] for v in facts["constant_property_keys"].values()} == {
        "MetalPluginName", "MetalPluginClassName", "Removable", "SafeEjectRequested"}
    pairs = [("followup2.json", "followup2-disassembly.json"),
             ("constructor-references.json", "constructor-references-disassembly.json")]
    checked = {a: validate_pair(a, b) for a, b in pairs}
    for file in EVIDENCE.glob("*execution.json"):
        execution = json.loads(file.read_text())
        assert execution["exit_code"] == 0 and not execution["stderr"]
        if "remote_files_created" in execution:
            assert execution["remote_files_created"] is False
    tables = [jump_table("loader_at_0x11752a", 6, 3),
              jump_table("loader_at_0x118236", 5, 0)]
    suffix = read("constructor-references-disassembly.json")["helpers"]["publication_at_0x2110"]["explicit_data_suffix"]
    assert suffix == {"offset": 459, "hex": "00"}
    paths = [p for p in EVIDENCE.iterdir() if p.is_file() and p.name != "loader-validation.json"]
    paths += [ROOT / "docs/metal-loader-contract.md", Path(__file__).resolve(),
              ROOT / "scripts/measure-metal-loader.py", ROOT / "scripts/disassemble-metal-loader.py",
              ROOT / "scripts/measure-metal-initializers.py", ROOT / "scripts/metal-initializer-reference-reader.py"]
    report = {"status": "validated_measured_loader_route_with_explicit_remaining_contracts",
              "system": facts["system"], "metal_uuid": UUID,
              "production_source_changed": False, "target_executed_by_validation": False,
              "remote_files_created": False, "checked_function_sets": checked,
              "jump_tables": tables, "MTLAddDevice_explicit_trailing_zero_bytes": 1,
              "preferred_route": "real IOAccelerator child plus actual _MTLDevice subclass connected to existing native Context",
              "still_required": ["trusted bundle path policy", "service association and removal lifetime",
                                  "inherited initialization and capability bodies", "required SPI selector contracts",
                                  "real target device construction and hardware execution"],
              "files": {str(p.relative_to(ROOT)).replace("\\", "/"):
                        {"bytes": p.stat().st_size, "sha256": sha(p.read_bytes())}
                        for p in sorted(set(paths))}}
    output = EVIDENCE / "loader-validation.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(output.relative_to(ROOT)), "sha256": sha(output.read_bytes()),
                      "validated_function_occurrences": sum(map(len, checked.values())),
                      "files_bound": len(report["files"])}))


if __name__ == "__main__":
    main()
