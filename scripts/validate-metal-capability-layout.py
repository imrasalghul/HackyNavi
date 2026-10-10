#!/usr/bin/env python3
"""Validate measured capability layout locally; never open a device or SSH."""
import base64
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build/metal-capability-layout"
UUID = "c3efbabe-6b06-33ab-bc45-43fe86297aa1"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def main():
    captured, decoded = read(BASE / "capture.json"), read(BASE / "disassembly.json")
    assert decoded["capture_sha256"] == sha((BASE / "capture.json").read_bytes())
    assert decoded["target_executed"] is False
    for key in ("device_enumerated", "device_instantiated", "gpu_submitted",
                "objc_messages_sent", "remote_files_created", "user_client_opened"):
        assert captured[key] is False
    fields, counted = {}, 0
    for name, item in decoded["helpers"].items():
        actual = captured["helpers"][name]
        assert actual["image_uuid"] == UUID
        assert actual["function_start_matched"] and actual["function_end_bounded"] and not actual["truncated"]
        code = bytes.fromhex(actual["code_hex"])
        assert sha(code) == actual["sha256"] == item["code_sha256"]
        assert len(code) == actual["bytes"] <= 512
        counted += len(code)
        offset, raw = actual["image_offset"], bytearray()
        for instruction in item["instructions"]:
            assert instruction["image_offset"] == offset
            data = bytes.fromhex(instruction["hex"])
            raw.extend(data); offset += len(data)
        suffix = item["explicit_data_suffix"]
        tail = bytes.fromhex(suffix["hex"])
        assert suffix["offset"] == len(raw) and tail == b"\0" * len(tail)
        assert bytes(raw) + tail == code
        assert item["all_literal_bytes_match"] and item["all_instruction_bytes_decoded"]
        matches = [(n, re.fullmatch(r"mov(l|ss)\s+(?:(\d+))?\(%rax\), %(eax|xmm0)", i["assembly"]))
                   for n, i in enumerate(item["instructions"])]
        matches = [(n, match) for n, match in matches if match]
        if len(matches) != 1:
            continue
        index, match = matches[0]
        field_offset = int(match.group(2) or 0)
        assert field_offset not in fields
        refs = [i["rip_target"] for i in item["instructions"][:index]
                if "rip_target" in i and i["assembly"].endswith("%rsi")]
        assert refs == [870164952]
        fields[field_offset] = {"name": name.split("::")[1],
                                "type": "float" if match.group(1) == "ss" else "uint32_t",
                                "selector_ref": refs, "code_sha256": actual["sha256"]}
    assert counted == captured["total_code_bytes"] == 1869
    assert len(decoded["helpers"]) == 75
    assert set(fields) == set(range(0, 256, 4)) - {116}
    assert {key for key, value in fields.items() if value["type"] == "float"} == {104, 108}
    assert read(BASE / "field-map.json") == {"capture_sha256": sha((BASE / "capture.json").read_bytes()),
                                            "fields": {str(k): v for k, v in fields.items()}}
    resolved = read(BASE / "reference-sdk.json")
    assert resolved["metal_uuid"] == UUID and resolved["selector_ref"] == 870164952
    assert resolved["selector"] == "limits" and resolved["header_bytes"] <= 131072
    for name, item in resolved["headers"].items():
        data = base64.b64decode(item["base64"])
        assert sha(data) == item["sha256"] and len(data) == item["bytes"]
        assert (BASE / "sdk-headers" / name).read_bytes() == data
    original = read(ROOT / "build/metal-device-hooks/references.json")
    assert original["explicit_references"][str(870164944)]["selector"]["text"] == "maxBufferLength"
    init = read(ROOT / "build/metal-device-hooks/references-disassembly.json")["helpers"]["_MTLDevice::initLimits"]["instructions"]
    assert any(i["image_offset"] == 11139 and i["hex"] == "b900000000" for i in init)
    assert any(i["image_offset"] == 11144 and i["hex"] == "894b7c" for i in init)
    assert any(i["image_offset"] == 12118 and i["hex"] == "48898308010000" for i in init)
    protocol = captured["protocols"]["MTLDeviceSPI"]["required_instance"]
    present = captured["required_spi_implementations"]
    assert {m["selector"] for m in protocol} == set(present) and len(present) == 392
    missing = [m for m in protocol if present[m["selector"]].get("present") is False]
    assert len(missing) == 19
    for name in ("execution.json", "reference-sdk-execution.json"):
        execution = read(BASE / name)
        assert execution["exit_code"] == 0 and not execution["stderr"]
    paths = [p for p in BASE.rglob("*") if p.is_file() and p.name != "validation.json"]
    paths += [Path(__file__).resolve(), ROOT / "docs/metal-capability-state.md",
              ROOT / "scripts/disassemble-metal-loader.py",
              ROOT / "build/metal-device-hooks/validation.json",
              ROOT / "build/metal-device-hooks/references.json",
              ROOT / "build/metal-device-hooks/references-disassembly.json"]
    report = {"status": "measured_owned_limits_ABI_with_typed_SPI_remainder",
              "metal_uuid": UUID, "code_bytes": counted, "functions": 75,
              "scalar_fields_mapped": len(fields), "limits_bytes": 264,
              "required_spi_inherited": 373, "required_spi_absent": missing,
              "target_executed": False, "remote_files_created": False,
              "files": {str(p.relative_to(ROOT)).replace("\\", "/"):
                        {"bytes": p.stat().st_size, "sha256": sha(p.read_bytes())}
                        for p in sorted(set(paths))}}
    output = BASE / "validation.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"path": str(output.relative_to(ROOT)), "sha256": sha(output.read_bytes()),
                      "files": len(report["files"])}))


if __name__ == "__main__":
    main()
