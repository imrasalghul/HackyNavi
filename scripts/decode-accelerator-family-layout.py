#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate captured on-disk family chains and resolve the measured vtables.

No network, OS driver API, symbol execution, or private declaration generation.
Inputs are the bounded captured evidence beside the resulting report.
"""
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "build/metal-initializer-audit"


def read_json(name):
    return json.loads((DIRECTORY / name).read_text())


def checked_bytes(item):
    data = bytes.fromhex(item["hex"])
    if len(data) != item["bytes"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
        raise ValueError("Captured bytes/hash mismatch")
    return data


def pointer(raw):
    # Pinned Apple dyld fixup-chains.h: format 11 has a one-byte next stride.
    if raw >> 63 or ((raw >> 32) & ((1 << 19) - 1)):
        raise ValueError("Unexpected authenticated/reserved pointer fields")
    return {"raw": hex(raw), "target_offset": raw & ((1 << 30) - 1),
            "cache_level": (raw >> 30) & 3, "next_bytes": (raw >> 51) & 0xFFF}


capture = read_json("kernel-family-layout-v2.json")
metadata = read_json("kernel-collection-metadata-v2.json")
kernel = read_json("kernel-symbols-v3.json")
anchors = read_json("kernel-anchor-symbols.json")
anchor_offsets = {"__ZN11OSMetaClassC2EPKcPKS_j": 0x949940,
                  "__ZN9IOService10gMetaClassE": 0x123EE68,
                  "___cxa_pure_virtual": 0x975800}
base_proofs = [{"name": s["name"], "symbol_address": s["value"],
               "pointer_target_offset": anchor_offsets[s["name"]],
               "derived_base": s["value"] - anchor_offsets[s["name"]]}
              for s in anchors["selected_symbols"] if s["name"] in anchor_offsets]
if len(base_proofs) != 3 or {p["derived_base"] for p in base_proofs} != {kernel["kernel_cache_base"]}:
    raise ValueError("Independent kernel cache base anchors disagree")

symbols = {}
for collection in metadata["collections"].values():
    for entry in collection["entries"]:
        for symbol in entry.get("selected_symbols", []):
            if symbol["section"]:
                symbols.setdefault((1, symbol["value"]), []).append(symbol["name"])
for symbol in kernel["selected_symbols"]:
    symbols.setdefault((0, symbol["target_offset"]), []).append(symbol["name"])

fixups = {}
page_reports = []
for key, page in capture["chain_pages"].items():
    data = checked_bytes(page)
    va = int(key)
    header = bytes.fromhex(page["segment_header_hex"])
    _, page_size, fmt, segment_offset, _, _ = struct.unpack("<IHHQIH", header)
    if page_size != len(data) or fmt != 11 or page["pointer_format"] != fmt or segment_offset != page["segment_offset"]:
        raise ValueError("Actual page metadata mismatch")
    offset = page["first_chain_offset"]
    visited = set()
    while True:
        if offset in visited or offset < 0 or offset + 8 > len(data):
            raise ValueError("Chain escapes page or loops")
        visited.add(offset)
        raw = struct.unpack_from("<Q", data, offset)[0]
        decoded = pointer(raw)
        if va + offset in fixups:
            raise ValueError("Overlapping captured fixups")
        fixups[va + offset] = decoded
        next_bytes = decoded["next_bytes"]
        if not next_bytes:
            break
        if next_bytes < 8:
            raise ValueError("Overlapping pointer fields")
        offset += next_bytes
    page_reports.append({"vmaddr": va, "sha256": page["sha256"], "format": fmt,
                         "stride_bytes": 1, "chain_nodes": len(visited)})


def resolve_at(address, raw):
    decoded = pointer(raw)
    if fixups.get(address) != decoded:
        raise ValueError("Requested word was not reached by actual page chain")
    names = symbols.get((decoded["cache_level"], decoded["target_offset"]), [])
    if not names:
        raise ValueError("Captured pointer target lacks actual symbol identity")
    return {**decoded, "pointer_address": address, "symbol_names": names}


vtables = {}
for name, item in capture["ranges"].items():
    if item["kind"] != "vtable":
        continue
    data = checked_bytes(item)
    if item["truncated"] or item["value"] + len(data) != item["next_symbol_address"]:
        raise ValueError("Vtable symbol range is incomplete")
    words = struct.unpack("<" + "Q" * (len(data) // 8), data)
    # Both captures have an unrelocated zero trailer before the next symbol.
    # The real-header Apple-kext compiler labels this word vcall_offset (0).
    if words[:2] != (0, 0) or words[-1] != 0:
        raise ValueError("Unexpected captured table boundary")
    if any(item["value"] + index * 8 in fixups for index in (0, 1, len(words) - 1)):
        raise ValueError("Header/trailer falsely overlaps a chain pointer")
    slots = []
    for index, raw in enumerate(words[2:-1], 2):
        slots.append({"cpp_virtual_slot": index - 2, "symbol_qword_index": index,
                      **resolve_at(item["value"] + 8 * index, raw)})
    vtables[name] = {"symbol_address": item["value"], "bytes_to_next_symbol": len(data),
                     "sha256": item["sha256"], "abi_header_words": 2,
                     "unrelocated_zero_trailer_words": 1, "virtual_entries": len(slots),
                     "slots": slots}

constructors = []
for name, refs in capture["constructor_references"].items():
    item = capture["ranges"][name]
    data = checked_bytes(item)
    if len(data) != 50 or data[:9] != bytes.fromhex("554889e553504889fb") or data[9:12] != bytes.fromhex("488d35") or data[16:19] != bytes.fromhex("488b15") or data[23] != 0xB9 or data[28] != 0xE8:
        raise ValueError("Constructor instruction shape changed")
    if struct.unpack_from("<I", data, 24)[0] != refs["size_argument_ecx"]:
        raise ValueError("Constructor class-size immediate mismatch")
    for instruction_end, displacement, destination in ((16, 12, "class_name_address"),
                                                       (23, 19, "parent_pointer_slot"),
                                                       (33, 29, "call_target")):
        if item["value"] + instruction_end + struct.unpack_from("<i", data, displacement)[0] != refs[destination]:
            raise ValueError("Constructor RIP-relative reference mismatch")
    stub = bytes.fromhex(refs["call_stub_hex"])
    if stub[:2] != b"\xff\x25" or refs["call_target"] + 6 + struct.unpack_from("<i", stub, 2)[0] != refs["call_pointer_slot"]:
        raise ValueError("Constructor external stub pointer mismatch")
    parent = resolve_at(refs["parent_pointer_slot"], int.from_bytes(bytes.fromhex(refs["parent_pointer_hex"]), "little"))
    callee = resolve_at(refs["call_pointer_slot"], int.from_bytes(bytes.fromhex(refs["call_pointer_hex"]), "little"))
    if callee["symbol_names"] != ["__ZN11OSMetaClassC2EPKcPKS_j"]:
        raise ValueError("Actual constructor target is not the measured OSMetaClass signature")
    constructors.append({"symbol": name, "sha256": item["sha256"], "class_name": refs["class_name"],
                         "instance_size_argument": refs["size_argument_ecx"], "parent": parent, "callee": callee})

base = vtables["__ZTV13IOAccelerator"]
family = vtables["__ZTV22IOGraphicsAccelerator2"]
new_slots = family["slots"][base["virtual_entries"]:]
pure = [s["cpp_virtual_slot"] for s in new_slots if s["symbol_names"] == ["___cxa_pure_virtual"]]
overrides = [{"cpp_virtual_slot": a["cpp_virtual_slot"], "base_symbols": b["symbol_names"],
              "family_symbols": a["symbol_names"]} for a, b in zip(family["slots"], base["slots"])
             if a["symbol_names"] != b["symbol_names"]]
compiler_report = read_json("public-base-layout.json")
llvm = (DIRECTORY / "public-base-layout.ll").read_bytes()
if compiler_report["exit_code"] != 0 or hashlib.sha256(llvm).hexdigest() != compiler_report["llvm_sha256"]:
    raise ValueError("Public-base compiler evidence mismatch")
vtable_line = next(line for line in llvm.decode().splitlines() if line.startswith("@_ZTV15AuditPublicBase ="))
compiler_symbols = re.findall(r"ptr @([^,\]]+)", vtable_line)
if len(compiler_symbols) != base["virtual_entries"]:
    raise ValueError("Public-header compiler vtable extent mismatch")
experiment_overrides = {0: "_ZN15AuditPublicBaseD1Ev", 1: "_ZN15AuditPublicBaseD0Ev", 18: "_ZN15AuditPublicBase4freeEv"}
for index, (symbol, measured) in enumerate(zip(compiler_symbols, base["slots"])):
    if index in experiment_overrides:
        if symbol != experiment_overrides[index]:
            raise ValueError("Unexpected compiler experiment override")
    elif "_" + symbol not in measured["symbol_names"]:
        raise ValueError("Public-header compiler target differs from installed base")
inputs = ["kernel-family-layout-v2.json", "kernel-collection-metadata-v2.json", "kernel-symbols-v3.json",
          "kernel-anchor-symbols.json", "source-reference/fixup-chains.h", "source-reference/provenance.json",
          "public-base-layout.cpp", "public-base-layout.json", "public-base-layout.ll", "public-base-layout.stdout.txt"]
report = {"schema": 1, "validation_passed": True, "source_system": capture["system"],
          "gpu_submitted": False, "running_kernel_read": False, "generated_production_declarations": False,
          "kernel_cache_base_proofs": base_proofs, "pages": page_reports, "vtables": vtables,
          "constructors": constructors, "new_virtual_entries": len(new_slots),
          "new_pure_virtual_cpp_slots": pure, "inherited_overrides": overrides,
          "public_header_compiler_match": {"matched_unchanged_targets": len(compiler_symbols) - 3,
              "experiment_overrides": experiment_overrides, "header": ".research/MacKernelSDK/Headers/IOKit/graphics/IOAccelerator.h",
              "target_executed": False, "compiler_trailer_label": "vcall_offset (0)"},
          "input_sha256": {name: hashlib.sha256((DIRECTORY / name).read_bytes()).hexdigest() for name in inputs},
          "decoder_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
(DIRECTORY / "family-layout-decoded.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
print(json.dumps({"validation_passed": True, "chain_pages": len(page_reports), "chain_nodes": len(fixups),
                  "base_virtual_entries": base["virtual_entries"], "family_virtual_entries": family["virtual_entries"],
                  "new_virtual_entries": len(new_slots), "new_pure_virtual_entries": len(pure),
                  "inherited_overrides": len(overrides)}, indent=2))
