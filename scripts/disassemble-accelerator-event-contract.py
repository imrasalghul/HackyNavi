#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Disassemble captured code locally and verify encoding bytes and coverage."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("capture", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
tool = root / ".tools/userspace/src/clang+llvm-20.1.8-x86_64-pc-windows-msvc/bin/llvm-mc.exe"
capture = json.loads(args.capture.read_text())
report = {"tool": str(tool), "tool_sha256": hashlib.sha256(tool.read_bytes()).hexdigest(),
          "capture_sha256": hashlib.sha256(args.capture.read_bytes()).hexdigest(),
          "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          "target_executed": False, "methods": {}}
for name, item in capture["ranges"].items():
    if item["kind"] != "code":
        continue
    data = bytes.fromhex(item["hex"])
    if len(data) != item["bytes"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
        raise ValueError("Captured bytes/hash mismatch")
    result = subprocess.run([str(tool), "--disassemble", "--triple=x86_64-apple-macosx15", "--show-encoding"],
                            input=" ".join(hex(byte) for byte in data), text=True, capture_output=True, timeout=30)
    if result.returncode or result.stderr:
        raise ValueError("Disassembly failed or produced diagnostics: " + result.stderr)
    position = 0
    instructions = []
    for line in result.stdout.splitlines():
        match = re.search(r"^(.*?)\s*## encoding: \[(.*?)\]", line)
        if not match:
            continue
        tokens = match.group(2).split(",")
        count = len(tokens)
        raw = data[position:position + count]
        if len(raw) != count:
            raise ValueError("Disassembly extends past captured bytes")
        for index, token in enumerate(tokens):
            if token.startswith("0x") and int(token, 16) != raw[index]:
                raise ValueError("Literal re-encoding differs from actual bytes")
        address = item["value"] + position
        instruction = {"address": address, "hex": raw.hex(), "assembly": match.group(1).strip()}
        if raw[0] in (0xE8, 0xE9) and count == 5:
            instruction["direct_target"] = address + count + int.from_bytes(raw[1:], "little", signed=True)
        elif (raw[0] == 0xEB or 0x70 <= raw[0] <= 0x7F) and count == 2:
            instruction["branch_target"] = address + count + int.from_bytes(raw[1:], "little", signed=True)
        elif raw[0] == 0x0F and count == 6 and 0x80 <= raw[1] <= 0x8F:
            instruction["branch_target"] = address + count + int.from_bytes(raw[2:], "little", signed=True)
        instructions.append(instruction)
        position += count
    if position != len(data):
        raise ValueError("Disassembly does not cover all captured bytes")
    report["methods"][name] = {"instructions": instructions, "code_sha256": item["sha256"],
                              "all_literal_encoding_bytes_match": True, "all_captured_bytes_decoded": True,
                              "symbol_range_truncated": item["truncated"]}
args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
print(json.dumps({"methods": len(report["methods"]), "target_executed": False,
                  "output_sha256": hashlib.sha256(args.output.read_bytes()).hexdigest()}))
