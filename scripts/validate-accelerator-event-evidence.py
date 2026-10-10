#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate captured event dispatch words against actual relocation chains."""
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "build/metal-event-contract"
capture = json.loads((DIRECTORY / "references-v2.json").read_text())
fixups = {}
page_summaries = []
for key, page in capture["chain_pages"].items():
    data = bytes.fromhex(page["hex"])
    if len(data) != page["bytes"] or hashlib.sha256(data).hexdigest() != page["sha256"]:
        raise ValueError("Page byte/hash mismatch")
    _, page_size, fmt, segment_offset, _, _ = struct.unpack("<IHHQIH", bytes.fromhex(page["segment_header_hex"]))
    if fmt != 11 or page["pointer_format"] != fmt or page_size != len(data) or segment_offset != page["segment_offset"]:
        raise ValueError("Actual fixup metadata mismatch")
    position = page["first_chain_offset"]
    visited = set()
    while True:
        if position in visited or position < 0 or position + 8 > len(data):
            raise ValueError("Invalid actual chain")
        visited.add(position)
        raw = struct.unpack_from("<Q", data, position)[0]
        if raw >> 63 or ((raw >> 32) & ((1 << 19) - 1)):
            raise ValueError("Unexpected pointer fields")
        address = int(key) + position
        if address in fixups:
            raise ValueError("Overlapping captured pointer")
        fixups[address] = raw
        distance = (raw >> 51) & 0xFFF
        if not distance:
            break
        if distance < 8:
            raise ValueError("Overlapping pointer words")
        position += distance
    page_summaries.append({"vmaddr": int(key), "chain_nodes": len(visited), "sha256": page["sha256"]})

symbols = {}
for symbol in capture["event_target_symbols"]:
    symbols.setdefault((symbol["cache_level"], symbol["target_offset"]), set()).add(symbol["name"])
resolved = []
for word in capture["event_target_words"]:
    if fixups.get(word["pointer_address"]) != word["raw"]:
        raise ValueError("Reference not reached by actual chain")
    raw = word["raw"]
    level, offset = (raw >> 30) & 3, raw & ((1 << 30) - 1)
    if (level, offset) != (word["cache_level"], word["target_offset"]):
        raise ValueError("Captured target decode mismatch")
    names = sorted(symbols.get((level, offset), set()))
    if not names:
        raise ValueError("Unresolved selected reference")
    resolved.append({**word, "symbol_names": names})
for item in capture["event_stubs"].values():
    data = bytes.fromhex(item["hex"])
    if len(data) != 6 or data[:2] != b"\xff\x25" or item["address"] + 6 + int.from_bytes(data[2:], "little", signed=True) != item["pointer_slot"]:
        raise ValueError("Actual call stub reference mismatch")
    if fixups[item["pointer_slot"]] != int.from_bytes(bytes.fromhex(item["pointer_hex"]), "little"):
        raise ValueError("Stub pointer changed relative to chain page")

table = capture["ranges"]["__ZN14IOAccelDevice214sDeviceMethodsE"]
data = bytes.fromhex(table["hex"])
if len(data) != 480 or hashlib.sha256(data).hexdigest() != table["sha256"]:
    raise ValueError("Actual ten-entry method table mismatch")
dispatch = []
for index in range(10):
    owner, raw, adjustment, flags, count0, count1 = struct.unpack_from("<6Q", data, index * 48)
    address = table["value"] + index * 48 + 8
    if owner or adjustment or fixups.get(address) != raw:
        raise ValueError("Unexpected method-pointer representation or owner")
    target = next(item for item in resolved if item["pointer_address"] == address)
    dispatch.append({"selector": index, "flags": flags, "count0": count0, "count1": count1,
                     "member_adjustment": adjustment, "symbol_names": target["symbol_names"]})
if dispatch[2] != {"selector": 2, "flags": 2, "count0": 0, "count1": 600, "member_adjustment": 0,
                   "symbol_names": ["__ZN14IOAccelDevice217get_event_machineEP29IOAccelDeviceEventMachineData"]}:
    raise ValueError("Actual selector-2 contract differs from observed userspace call")
report = {"validation_passed": True, "pages": page_summaries, "dispatch": dispatch,
          "references": resolved, "direct_call_symbols": [{**item,
               "symbol_names": sorted(symbols.get((1, item["address"]), set()))}
              for item in capture.get("event_direct_calls", {}).values()],
          "source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          "capture_sha256": hashlib.sha256((DIRECTORY / "references-v2.json").read_bytes()).hexdigest(),
          "gpu_submitted": False, "running_kernel_read": False,
          "warning": "Method return/lifetime interpretations require the separately captured callers and public-header evidence; this validates their exact referenced data."}
(DIRECTORY / "references-validated.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
print(json.dumps({"validation_passed": True, "chain_pages": len(page_summaries), "chain_nodes": len(fixups),
                  "resolved_selected_pointer_words": len(resolved), "selector2_output_bytes": 600}))
