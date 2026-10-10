#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build-input integrity checks; these do not execute GPU firmware."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("embed_firmware", ROOT / "scripts" / "embed-firmware.py")
EMBED = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EMBED)


class ResourceIntegrityTests(unittest.TestCase):
    def test_byte_roundtrip(self):
        data = bytes(range(256)) + b"\0\xff\0\xff\n\r"
        item = {"id": "PspSos", "path": "amdgpu/input.bin", "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest()}
        source = EMBED.render_source([(item, data)]).decode("ascii")
        values = re.findall(r"0x([0-9a-f]{2})", source)
        self.assertEqual(bytes(int(value, 16) for value in values), data)
        self.assertEqual(EMBED.render_source([(item, data)]).decode("ascii"), source)
        self.assertIn('out = FirmwareResource{};', source)

    def test_size_and_content_rejection(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path = directory / "input.bin"
            original = b"firmware input"
            item = {"path": "input.bin", "bytes": len(original),
                    "sha256": hashlib.sha256(original).hexdigest()}
            path.write_bytes(original)
            self.assertEqual(EMBED.checked_resource(directory, item), original)
            for damaged in (original[:-1], original + b"x", b"X" + original[1:]):
                path.write_bytes(damaged)
                with self.assertRaises(ValueError):
                    EMBED.checked_resource(directory, item)

    def test_manifest_input_constraints(self):
        good = {"path": "input.bin", "bytes": 1, "sha256": "0" * 64}
        for field, bad in (("path", "../input.bin"), ("path", "/input.bin"),
                           ("path", "C:/input.bin"), ("path", "a\\input.bin"),
                           ("bytes", 0), ("bytes", True),
                           ("bytes", EMBED.MAX_RESOURCE_BYTES + 1),
                           ("sha256", "bad")):
            with self.subTest(field=field, bad=bad), self.assertRaises(ValueError):
                EMBED.checked_resource(Path("."), dict(good, **{field: bad}))

    def test_official_inputs_and_failure_preserves_output(self):
        corpus = ROOT / ".research" / "firmware"
        manifest = json.loads(EMBED.MANIFEST.read_text(encoding="utf-8"))
        if not all((corpus / item["path"]).is_file() for item in manifest["files"] + [manifest["license"]]):
            self.skipTest("Pinned official firmware corpus not available")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            output = directory / "out"
            count = EMBED.generate(corpus, output)
            self.assertEqual(count, sum(item["bytes"] for item in manifest["files"]))
            generated = output / "FirmwareResources.cpp"
            before = generated.read_bytes()
            self.assertEqual((output / "LICENSE.amdgpu").read_bytes(),
                             (corpus / manifest["license"]["path"]).read_bytes())
            missing = directory / "missing"
            with self.assertRaises(OSError):
                EMBED.generate(missing, output)
            self.assertEqual(generated.read_bytes(), before)
            EMBED.generate(corpus, output)
            self.assertEqual(generated.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
