"""Bounded discovery scanner subprocess tests; no hardware access.

Run directly with --tool PATH, or set HACKYNAVI_DISCOVERY_TOOL (also accepts
HACKYNAVI_DISCOVERY). Tests skip when
the host tool has not been built. Set HACKYNAVI_ROM to check the user's ROM too.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


TOOL_OVERRIDE = None
SIGNATURE = bytes.fromhex("07142128")


def fixture():
    """Original minimal binary v1/IP v3 fixture: one die and one GC record."""
    data = bytearray(156)
    struct.pack_into("<IHH", data, 0, 0x28211407, 1, 4)
    struct.pack_into("<H", data, 10, len(data))
    struct.pack_into("<HHHH", data, 12, 60, 0, 96, 0)
    struct.pack_into("<IHHIH", data, 60, 0x53445049, 3, 96, 0, 1)
    struct.pack_into("<HH", data, 74, 0, 140)
    struct.pack_into("<HH", data, 140, 0, 1)
    struct.pack_into("<HBBBBBBI", data, 144, 11, 0, 1, 11, 0, 0, 0, 0x1234)
    struct.pack_into("<H", data, 14, sum(data[60:]) & 0xFFFF)
    struct.pack_into("<H", data, 8, sum(data[10:]) & 0xFFFF)
    return bytes(data)


class DiscoveryCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        explicit = (TOOL_OVERRIDE or os.environ.get("HACKYNAVI_DISCOVERY_TOOL") or
                    os.environ.get("HACKYNAVI_DISCOVERY"))
        paths = [Path(explicit)] if explicit else [
            root / "build" / "host" / "hackynavi-discovery.exe",
            root / "build" / "host" / "hackynavi-discovery",
            root / "build" / "ci" / "hackynavi-discovery.exe",
            root / "build" / "ci" / "hackynavi-discovery",
            root / "build" / "cmake" / "hackynavi-discovery.exe",
            root / "build" / "cmake" / "hackynavi-discovery",
            root / "build" / "cmake" / "Release" / "hackynavi-discovery.exe",
            root / "build" / "hackynavi-discovery.exe",
            root / "build" / "hackynavi-discovery",
        ]
        cls.tool = next((p.resolve() for p in paths if p.is_file()), None)
        if cls.tool is None:
            raise unittest.SkipTest("hackynavi-discovery is not built; set HACKYNAVI_DISCOVERY_TOOL or --tool")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "input.bin"

    def invoke(self, data, scan=True):
        self.path.write_bytes(data)
        args = [str(self.tool)] + (["--scan"] if scan else []) + [str(self.path)]
        result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", timeout=15)
        parsed = json.loads(result.stdout) if result.stdout else None
        return result, parsed

    def test_default_output_compatibility(self):
        result, data = self.invoke(fixture(), scan=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(data["schema_version"], 1)
        self.assertEqual(data["status"], "ok")
        self.assertEqual(data["binary_bytes"], 156)
        self.assertEqual(data["ip_count"], 1)
        self.assertEqual(data["ips"][0]["base_addresses_raw"], ["0x1234"])
        self.assertNotIn("mode", data)
        self.assertNotIn("scan_complete", data)

    def test_two_unaligned_copies_and_trailing_partial_candidate(self):
        binary = fixture()
        result, data = self.invoke(b"abc" + binary + b"pad12" + binary + SIGNATURE)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(data["scan_complete"])
        self.assertEqual(data["validated_count"], 2)
        self.assertEqual([c["offset"] for c in data["candidates"]], [3, 164, 320])
        self.assertEqual([c["status"] for c in data["candidates"]], ["ok", "ok", "truncated"])
        self.assertEqual(data["candidates"][1]["offset_hex"], "0xa4")
        self.assertNotIn("ips", data["candidates"][2])

    def test_bad_declared_length_does_not_skip_nested_valid_candidate(self):
        data = bytearray(512)
        data[:4] = SIGNATURE
        struct.pack_into("<H", data, 10, 65535)
        data[37:37 + len(fixture())] = fixture()
        result, parsed = self.invoke(data)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([c["offset"] for c in parsed["candidates"]], [0, 37])
        self.assertEqual([c["status"] for c in parsed["candidates"]], ["truncated", "ok"])

    def test_checksum_failure_is_reported_and_not_output_as_ip_records(self):
        data = bytearray(fixture())
        data[-1] ^= 1
        result, parsed = self.invoke(data)
        self.assertEqual(result.returncode, 3)
        self.assertTrue(parsed["scan_complete"])
        self.assertEqual(parsed["validated_count"], 0)
        self.assertEqual(parsed["candidates"][0]["status"], "checksum mismatch")
        self.assertNotIn("ips", parsed["candidates"][0])

    def test_no_signature_and_short_input(self):
        for blob in (b"x", SIGNATURE[:3], b"plain data" * 100):
            with self.subTest(length=len(blob)):
                result, data = self.invoke(blob)
                self.assertEqual(result.returncode, 3)
                self.assertTrue(data["scan_complete"])
                self.assertEqual(data["scan_status"], "complete")
                self.assertEqual(data["candidates"], [])
                self.assertEqual(data["parser_input_bytes"], 0)

    def test_candidate_limit_is_not_silent_success(self):
        candidate = SIGNATURE + bytes(8)
        result, data = self.invoke(candidate * 129)
        self.assertEqual(result.returncode, 4)
        self.assertFalse(data["scan_complete"])
        self.assertEqual(data["scan_status"], "candidate_limit_reached")
        self.assertEqual(data["candidate_count"], 128)
        self.assertEqual(data["next_candidate_offset"], 128 * 12)
        self.assertIn("incomplete", result.stderr)
        result, data = self.invoke(candidate * 128)
        self.assertEqual(result.returncode, 3)
        self.assertTrue(data["scan_complete"])
        self.assertEqual(data["candidate_count"], 128)

    def test_parser_byte_limit_is_not_silent_success(self):
        # Each independent window is charged without trusting its declared size.
        block = fixture() + bytes(65536 - len(fixture()))
        result, data = self.invoke(block * 65)
        self.assertEqual(result.returncode, 4)
        self.assertFalse(data["scan_complete"])
        self.assertEqual(data["scan_status"], "parser_byte_limit_reached")
        self.assertEqual(data["candidate_count"], 64)
        self.assertEqual(data["validated_count"], 64)
        self.assertEqual(data["parser_input_bytes"], 64 * 65535)
        self.assertLessEqual(data["parser_input_bytes"], data["scan_limits"]["max_parser_input_bytes"])
        self.assertEqual(data["next_candidate_offset"], 64 * 65536)
        self.assertIn("incomplete", result.stderr)

    def test_input_limits_and_usage(self):
        for blob in (b"", bytes(16 * 1024 * 1024 + 1)):
            result, data = self.invoke(blob)
            self.assertEqual(result.returncode, 2)
            self.assertIsNone(data)
            self.assertIn("16 MiB", result.stderr)
        result = subprocess.run([str(self.tool), "--scan"], capture_output=True,
                                text=True, encoding="utf-8", timeout=15)
        self.assertEqual(result.returncode, 2)
        self.assertIn("Usage", result.stderr)

    @unittest.skipUnless(os.environ.get("HACKYNAVI_ROM"), "optional user ROM not selected")
    def test_user_rom_embedded_evidence(self):
        rom = Path(os.environ["HACKYNAVI_ROM"]).read_bytes()
        result, data = self.invoke(rom)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(data["scan_complete"])
        valid = [c for c in data["candidates"] if c["status"] == "ok"]
        self.assertEqual([c["offset"] for c in valid], [0xB5D00, 0x195D00])
        for item in valid:
            self.assertEqual(item["binary_bytes"], 1756)
            self.assertEqual(item["ip_count"], 61)
            binary = rom[item["offset"]:item["offset"] + item["binary_bytes"]]
            self.assertEqual(hashlib.sha256(binary).hexdigest(),
                             "e52312475450105abdf58683db2016cb5b407b617cb6ba78b4f10830cd37d34b")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--tool")
    arguments, remaining = parser.parse_known_args()
    TOOL_OVERRIDE = arguments.tool
    unittest.main(argv=[sys.argv[0]] + remaining)
