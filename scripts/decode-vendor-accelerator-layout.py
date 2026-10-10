#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Resolve actual vendor overrides at the measured family's pure slots."""
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "build/metal-vendor-abi-audit"
BASE = ROOT / "build/metal-initializer-audit"


def load(path):
    return json.loads(path.read_text())


def checked_bytes(item):
    data = bytes.fromhex(item["hex"])
    if len(data) != item["bytes"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
        raise ValueError("Captured byte/hash mismatch")
    return data


def pointer(raw):
    if raw >> 63 or ((raw >> 32) & ((1 << 19) - 1)):
        raise ValueError("Unexpected authenticated/reserved fields")
    return {"raw": hex(raw), "cache_level": (raw >> 30) & 3,
            "target_offset": raw & ((1 << 30) - 1), "next_bytes": (raw >> 51) & 0xFFF}


capture = load(DIRECTORY / "layout.json")
metadata = load(DIRECTORY / "metadata.json")
family_metadata = load(BASE / "kernel-collection-metadata-v2.json")
family = load(BASE / "family-layout-decoded.json")
kernel = load(BASE / "kernel-symbols-v3.json")
symbols = {}
for entry in metadata["entries"] + [e for c in family_metadata["collections"].values() for e in c["entries"]]:
    for symbol in entry.get("selected_symbols", []):
        if symbol["section"]:
            symbols.setdefault((1, symbol["value"]), set()).add(symbol["name"])
for symbol in kernel["selected_symbols"]:
    symbols.setdefault((0, symbol["target_offset"]), set()).add(symbol["name"])
fixups = {}
page_reports = []
for key, page in capture["chain_pages"].items():
    data = checked_bytes(page)
    _, size, fmt, offset, _, _ = struct.unpack("<IHHQIH", bytes.fromhex(page["segment_header_hex"]))
    if fmt != 11 or page["pointer_format"] != fmt or size != len(data) or offset != page["segment_offset"]:
        raise ValueError("Page fixup metadata mismatch")
    position = page["first_chain_offset"]
    visited = set()
    while True:
        if position in visited or position < 0 or position + 8 > len(data):
            raise ValueError("Invalid page chain")
        visited.add(position)
        decoded = pointer(struct.unpack_from("<Q", data, position)[0])
        address = int(key) + position
        if address in fixups:
            raise ValueError("Repeated pointer address")
        fixups[address] = decoded
        if not decoded["next_bytes"]:
            break
        if decoded["next_bytes"] < 8:
            raise ValueError("Overlapping pointer words")
        position += decoded["next_bytes"]
    page_reports.append({"vmaddr": int(key), "chain_nodes": len(visited), "sha256": page["sha256"]})


def resolve_at(address, raw, require_name=True):
    decoded = pointer(raw)
    if fixups.get(address) != decoded:
        raise ValueError("Requested pointer absent from actual chain")
    names = sorted(symbols.get((decoded["cache_level"], decoded["target_offset"]), set()))
    if not names and require_name:
        raise ValueError("Unresolved requested symbol")
    return {**decoded, "pointer_address": address, "symbol_names": names}


constructors = []
for name, refs in capture["constructor_references"].items():
    item = capture["ranges"][name]
    data = checked_bytes(item)
    if len(data) != 50 or data[:9] != bytes.fromhex("554889e553504889fb") or data[9:12] != bytes.fromhex("488d35") or data[16:19] != bytes.fromhex("488b15") or data[23] != 0xB9 or data[28] != 0xE8:
        raise ValueError("Constructor instruction shape changed")
    if struct.unpack_from("<I", data, 24)[0] != refs["size_argument_ecx"]:
        raise ValueError("Class size immediate mismatch")
    for end, displacement, destination in ((16, 12, "class_name_address"), (23, 19, "parent_pointer_slot"), (33, 29, "call_target")):
        if item["value"] + end + struct.unpack_from("<i", data, displacement)[0] != refs[destination]:
            raise ValueError("Constructor reference mismatch")
    stub = bytes.fromhex(refs["call_stub_hex"])
    if stub[:2] != b"\xff\x25" or refs["call_target"] + 6 + struct.unpack_from("<i", stub, 2)[0] != refs["call_pointer_slot"]:
        raise ValueError("Constructor stub reference mismatch")
    parent = resolve_at(refs["parent_pointer_slot"], int.from_bytes(bytes.fromhex(refs["parent_pointer_hex"]), "little"))
    callee = resolve_at(refs["call_pointer_slot"], int.from_bytes(bytes.fromhex(refs["call_pointer_hex"]), "little"))
    if parent["symbol_names"] != ["__ZN22IOGraphicsAccelerator210gMetaClassE"] or callee["symbol_names"] != ["__ZN11OSMetaClassC2EPKcPKS_j"]:
        raise ValueError("Vendor class does not have the measured direct parent/constructor")
    constructors.append({"symbol": name, "class_name": refs["class_name"], "size_argument": refs["size_argument_ecx"],
                         "parent": parent, "callee": callee, "sha256": item["sha256"]})

tables = {}
for name, item in capture["ranges"].items():
    if item["kind"] != "vtable":
        continue
    data = checked_bytes(item)
    words = struct.unpack("<" + "Q" * (len(data) // 8), data)
    if item["truncated"] or item["value"] + len(data) != item["next_symbol_address"] or words[:2] != (0, 0) or words[-1]:
        raise ValueError("Vendor vtable boundary mismatch")
    if any(item["value"] + 8 * i in fixups for i in (0, 1, len(words) - 1)):
        raise ValueError("ABI header/trailer unexpectedly relocated")
    slots = [{"cpp_virtual_slot": index - 2, "symbol_qword_index": index,
              **resolve_at(item["value"] + 8 * index, raw, require_name=False)} for index, raw in enumerate(words[2:-1], 2)]
    if len(slots) < family["vtables"]["__ZTV22IOGraphicsAccelerator2"]["virtual_entries"]:
        raise ValueError("Vendor table shorter than its measured direct parent")
    tables[name] = {"sha256": item["sha256"], "virtual_entries": len(slots), "slots": slots}


def signature(symbol):
    # Decode only the simple nested-name/parameter encodings actually present.
    # No return type is available in any of these ordinary method symbols.
    if symbol == "___cxa_pure_virtual":
        return None
    if not symbol.startswith("__ZN"):
        raise ValueError("Unexpected override mangling")
    rest = symbol[4:]
    names = []
    for _ in range(2):
        m = re.match(r"(\d+)", rest)
        if not m:
            raise ValueError("Missing nested-name length")
        length = int(m.group(1)); rest = rest[len(m.group(1)):]
        names.append(rest[:length]); rest = rest[length:]
    params = {"Ev": "()", "EPj": "(unsigned int*)", "EP13IOAccelConfig": "(IOAccelConfig*)", "EP11IOPCIDevice": "(IOPCIDevice*)"}
    if rest not in params:
        raise ValueError("Unmeasured parameter encoding")
    return {"declaring_class": names[0], "method": names[1], "parameters": params[rest], "return_type": "unresolved"}


required = []
for slot in family["new_pure_virtual_cpp_slots"]:
    implementations = []
    for name, table in tables.items():
        target = table["slots"][slot]
        if len(target["symbol_names"]) != 1:
            raise ValueError("Ambiguous override symbol")
        implementations.append({"vtable": name, **target, "signature": signature(target["symbol_names"][0])})
    recovered = [item["signature"] for item in implementations if item["signature"]]
    if not recovered or len({(item["method"], item["parameters"]) for item in recovered}) != 1:
        raise ValueError("Vendor override signatures absent or disagree")
    required.append({"cpp_virtual_slot": slot, "method": recovered[0]["method"],
                     "parameters": recovered[0]["parameters"], "return_type": "unresolved",
                     "implementations": implementations})
inputs = [DIRECTORY / "layout.json", DIRECTORY / "metadata.json", BASE / "family-layout-decoded.json",
          BASE / "kernel-collection-metadata-v2.json", BASE / "kernel-symbols-v3.json"]
report = {"validation_passed": True, "gpu_submitted": False, "running_kernel_read": False,
          "production_declarations_generated": False, "constructors": constructors, "pages": page_reports,
          "vtables": tables, "required_methods": required,
          "unresolved_other_slot_names": [{"vtable": name, **slot} for name, table in tables.items()
              for slot in table["slots"] if not slot["symbol_names"]],
          "amd_c1_symbol_range_note": "60-byte symbol-bounded range includes a second code sequence after the first return; the independently captured 50-byte C2 establishes the measured AMD parent/size.",
          "input_sha256": {str(p.relative_to(ROOT)).replace('\\', '/'): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
          "decoder_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
(DIRECTORY / "decoded.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
print(json.dumps({"validation_passed": True, "pages": len(page_reports), "chain_nodes": len(fixups),
                  "required_methods_resolved": len(required),
                  "virtual_entries": {name: item["virtual_entries"] for name, item in tables.items()}}, indent=2))
