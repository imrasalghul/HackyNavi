#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode bounded captured userland helpers locally, checking every code byte."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("capture", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--data-suffix", action="append", default=[],
                    help="Explicit helper:byte-offset for a separately reviewed embedded data suffix")
args = parser.parse_args()
if args.output.exists():
    raise RuntimeError("Use a fresh disassembly path")
root = Path(__file__).resolve().parents[1]
tool = root / ".tools/userspace/src/clang+llvm-20.1.8-x86_64-pc-windows-msvc/bin/llvm-mc.exe"
captured = json.loads(args.capture.read_text())
data_suffixes = {name: int(offset, 0) for name, offset in
                 (value.rsplit(":", 1) for value in args.data_suffix)}
report = {"capture_sha256": hashlib.sha256(args.capture.read_bytes()).hexdigest(),
          "tool_sha256": hashlib.sha256(tool.read_bytes()).hexdigest(),
          "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          "target_executed": False, "helpers": {}}
for name, item in captured["helpers"].items():
    if "code_hex" not in item:
        continue
    data = bytes.fromhex(item["code_hex"])
    if len(data) != item["bytes"] or hashlib.sha256(data).hexdigest() != item["sha256"]:
        raise RuntimeError("Code digest or length mismatch")
    full_data = data
    suffix = data_suffixes.get(name, len(data))
    if not 0 < suffix <= len(data):
        raise RuntimeError("Invalid explicit data suffix")
    data = data[:suffix]
    result = subprocess.run([str(tool), "--disassemble", "--triple=x86_64-apple-macosx15", "--show-encoding"],
                            input=" ".join(hex(byte) for byte in data), text=True,
                            capture_output=True, timeout=30)
    if result.returncode or result.stderr:
        raise RuntimeError("Decoder failed or emitted diagnostics: " + result.stderr)
    position, instructions = 0, []
    for line in result.stdout.splitlines():
        match = re.search(r"^(.*?)\s*## encoding: \[(.*?)\]", line)
        if not match:
            continue
        tokens = [token.strip() for token in match.group(2).split(",")]
        count = len(tokens)
        # LLVM canonicalizes the actual 66 90 two-byte NOP to a one-byte NOP.
        # Recognize only these exact bytes, and retain both in the output.
        prefixed_nop = tokens == ["0x90"] and data[position:position + 2] == b"\x66\x90"
        if prefixed_nop:
            count = 2
        raw = data[position:position + count]
        if len(raw) != count:
            raise RuntimeError("Decoder exceeded captured range")
        for index, token in enumerate([] if prefixed_nop else tokens):
            if token.startswith("0x") and int(token, 16) != raw[index]:
                raise RuntimeError("Literal encoding differs from captured bytes")
        address = item["image_offset"] + position
        assembly = match.group(1).strip()
        instruction = {"image_offset": address, "hex": raw.hex(), "assembly": assembly}
        if raw[0] in (0xe8, 0xe9) and count == 5:
            instruction["direct_target"] = address + count + int.from_bytes(raw[1:], "little", signed=True)
        elif (raw[0] == 0xeb or 0x70 <= raw[0] <= 0x7f) and count == 2:
            instruction["branch_target"] = address + count + int.from_bytes(raw[1:], "little", signed=True)
        elif raw[0] == 0x0f and count == 6 and 0x80 <= raw[1] <= 0x8f:
            instruction["branch_target"] = address + count + int.from_bytes(raw[2:], "little", signed=True)
        rip = re.search(r"(-?(?:0x[0-9a-fA-F]+|[0-9]+))\(%rip\)", assembly)
        if rip:
            instruction["rip_target"] = address + count + int(rip.group(1), 0)
        instructions.append(instruction)
        position += count
    if position != len(data):
        raise RuntimeError("Decoder failed to cover complete captured range")
    report["helpers"][name] = {"code_sha256": item["sha256"], "instructions": instructions,
                               "all_literal_bytes_match": True, "all_instruction_bytes_decoded": True,
                               "all_captured_bytes_decoded_as_instructions": suffix == len(full_data),
                               "explicit_data_suffix": {"offset": suffix, "hex": full_data[suffix:].hex()}}
args.output.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({"functions": len(report["helpers"]), "output": str(args.output)}))
