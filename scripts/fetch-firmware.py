#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fetch the exact AMD firmware inputs in firmware/manifest.json; no execution."""
import argparse
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("embed_firmware", ROOT / "scripts" / "embed-firmware.py")
EMBED = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EMBED)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=ROOT / ".research" / "firmware")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    manifest = json.loads(EMBED.MANIFEST.read_text(encoding="utf-8"))
    commit = manifest["commit"]
    if len(commit) != 40 or any(c not in "0123456789abcdef" for c in commit):
        raise ValueError("Expected an exact upstream commit hash")
    base = "https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/"
    # License is preserved before any firmware is fetched. Existing mismatched
    # inputs fail instead of being silently replaced with different bytes.
    for item in [manifest["license"]] + manifest["files"]:
        destination = output / item["path"]
        if destination.exists():
            EMBED.checked_resource(output, item)
            print(f'Verified existing {item["path"]}', flush=True)
            continue
        size = item["bytes"]
        if type(size) is not int or not 0 < size <= EMBED.MAX_RESOURCE_BYTES:
            raise ValueError("Invalid resource size")
        relative = item["path"]
        if relative.startswith("/") or ".." in Path(relative).parts or ":" in relative or "\\" in relative:
            raise ValueError("Invalid relative resource path")
        request = urllib.request.Request(base + commit + "/" + relative + "?format=TEXT",
                                         headers={"User-Agent": "HackyNavi-firmware-build"})
        maximum = 4 * ((size + 2) // 3)
        with urllib.request.urlopen(request, timeout=45) as response:
            encoded = response.read(maximum + 1)
        if len(encoded) != maximum:
            raise ValueError(f"Unexpected encoded download size: {relative}")
        data = base64.b64decode(encoded, validate=True)
        if len(data) != size or hashlib.sha256(data).hexdigest() != item["sha256"]:
            raise ValueError(f"Download differs from pinned firmware: {relative}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        EMBED.replace_file(destination, data)
        EMBED.checked_resource(output, item)
        print(f"Downloaded and verified {relative}", flush=True)
    print(f"Pinned AMD firmware and license ready: {output}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Firmware fetch failed: {error}", file=sys.stderr)
        sys.exit(1)
