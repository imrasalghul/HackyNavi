"""Hardware-neutral tests for the Linux reference collector."""

import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "collect_linux", Path(__file__).resolve().parents[1] / "scripts" / "collect-linux.py")
collector = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(collector)


class WindowsFixturePath(type(Path())):
    """Map only fixture BDF directory names; Windows forbids colons in names.

    No production path rules are changed. Enumeration still presents real BDF
    strings to the collector's selection and identity validation logic.
    """
    def __truediv__(self, key):
        if collector.BDF_RE.fullmatch(str(key)):
            key = str(key).replace(":", "_")
        return super().__truediv__(key)

    @property
    def name(self):
        raw = super().name
        candidate = raw.replace("_", ":")
        return candidate if collector.BDF_RE.fullmatch(candidate) else raw


class CollectorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        if os.name == "nt":
            patch = mock.patch.object(collector, "Path", WindowsFixturePath)
            patch.start()
            self.addCleanup(patch.stop)
            self.base = WindowsFixturePath(self.temp.name)
        else:
            self.base = Path(self.temp.name)
        self.root = self.base / "sys"
        (self.root / "bus/pci/devices").mkdir(parents=True)

    def device(self, bdf="0000:03:00.0", device="0x744c"):
        path = self.root / "bus/pci/devices" / bdf
        path.mkdir()
        for name, value in {"vendor": "0x1002", "device": device,
                            "subsystem_vendor": "0x148c", "subsystem_device": "0x2422",
                            "revision": "0xc8", "class": "0x030000",
                            "resource": "0x0000000100000000 0x00000001ffffffff 0x000000000014220c\n",
                            "mem_info_vram_total": "25769803776"}.items():
            (path / name).write_text(value + "\n", encoding="ascii")
        return path

    def symlink(self, link, target, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except OSError as exc:
            self.skipTest("symlinks unavailable on this host: " + str(exc))

    def test_collect_identity_and_discovery(self):
        device = self.device()
        ip = device / "ip_discovery/die/0/11/0"
        ip.mkdir(parents=True)
        (ip / "major").write_text("11\n", encoding="ascii")
        (ip / "base_addr").write_bytes(b"0x00001234\n0x00005678\n")
        result = collector.collect(self.root)
        self.assertEqual(result["identity"]["device"], 0x744C)
        self.assertEqual(result["identity"]["subsystem_device"], 0x2422)
        self.assertEqual(result["attributes"]["mem_info_vram_total"], "25769803776")
        self.assertEqual(result["ip_discovery"]["die/0/11/0/major"], "11")
        self.assertEqual(result["ip_discovery"]["die/0/11/0/base_addr"], "0x00001234\n0x00005678")
        self.assertEqual(result["driver"]["state"], "unbound")
        self.assertTrue(any(w["path"] == "driver" for w in result["warnings"]))

    def test_multiple_requires_selection(self):
        self.device()
        self.device("0000:05:00.0")
        with self.assertRaisesRegex(collector.CollectionError, "multiple"):
            collector.collect(self.root)
        self.assertEqual(collector.collect(self.root, "0000:05:00.0")["pci_bdf"], "0000:05:00.0")

    def test_selection_rejects_malformed_bdf_and_wrong_identity(self):
        self.device(device="0x73bf")
        for bdf in ("../outside", "03:00.0", "0000:03:20.0", "0000:03:00.8"):
            with self.assertRaisesRegex(collector.CollectionError, "full PCI BDF"):
                collector.collect(self.root, bdf)
        with self.assertRaisesRegex(collector.CollectionError, "not AMD"):
            collector.collect(self.root, "0000:03:00.0")

    def test_missing_target(self):
        self.device(device="0x73bf")
        with self.assertRaisesRegex(collector.CollectionError, "no readable AMD"):
            collector.collect(self.root)

    def test_oversized_and_unreadable_optional_attributes_are_warnings(self):
        device = self.device()
        (device / "vbios_version").write_bytes(b"x" * (collector.MAX_ATTRIBUTE_BYTES + 1))
        original_open = Path.open

        def selective_open(path, *args, **kwargs):
            if path.name == "mem_info_vram_total":
                raise PermissionError("fixture denied read")
            return original_open(path, *args, **kwargs)

        with mock.patch.object(Path, "open", selective_open):
            result = collector.collect(self.root)
        self.assertNotIn("vbios_version", result["attributes"])
        self.assertNotIn("mem_info_vram_total", result["attributes"])
        self.assertTrue(any("size limit" in w["reason"] for w in result["warnings"]))
        self.assertTrue(any("fixture denied" in w["reason"] for w in result["warnings"]))

    def test_recursion_and_file_limits(self):
        device = self.device()
        deep = device / "ip_discovery" / Path(*("a" for _ in range(collector.MAX_DEPTH + 1)))
        deep.mkdir(parents=True)
        (deep / "value").write_text("never read", encoding="ascii")
        result = collector.collect(self.root)
        self.assertFalse(result["ip_discovery"])
        self.assertTrue(any("recursion limit" in w["reason"] for w in result["warnings"]))
        with mock.patch.object(collector, "MAX_FILES", 6):
            result = collector.collect(self.root)
        self.assertLessEqual(result["read_stats"]["files"], 6)
        self.assertTrue(any("read limit" in w["reason"] for w in result["warnings"]))

    def test_byte_and_directory_entry_limits(self):
        device = self.device()
        reader = collector.Reader(device)
        with mock.patch.object(collector, "MAX_TOTAL_BYTES", 10):
            self.assertEqual(reader.text(device / "vendor"), "0x1002")
            self.assertIsNone(reader.text(device / "device"))
        self.assertTrue(any("size limit" in w["reason"] for w in reader.warnings))
        with self.assertRaisesRegex(collector.CollectionError, "entry limit"):
            collector.bounded_entries(device, limit=2)

    def test_only_expected_device_text_is_read(self):
        device = self.device()
        for name in ("rom", "config", "resource0", "reset", "remove"):
            (device / name).write_bytes(b"forbidden")
        original_open = Path.open
        observed = []

        def audited_open(path, mode="r", *args, **kwargs):
            self.assertEqual(mode, "rb")
            self.assertNotIn(path.name, ("rom", "config", "resource0", "reset", "remove"))
            observed.append(path.name)
            return original_open(path, mode, *args, **kwargs)

        with mock.patch.object(Path, "open", audited_open):
            collector.collect(self.root)
        self.assertIn("resource", observed)

    def test_symlink_device_allowed_inside_sysfs_and_driver_bound(self):
        device = self.device()
        physical = self.root / "devices/pci-root" / "0000:03:00.0"
        physical.parent.mkdir(parents=True)
        device.rename(physical)
        self.symlink(device, physical, directory=True)
        driver = self.root / "bus/pci/drivers/amdgpu"
        driver.mkdir(parents=True)
        self.symlink(physical / "driver", driver, directory=True)
        result = collector.collect(self.root)
        self.assertEqual(result["driver"], {"state": "bound", "name": "amdgpu"})

    def test_outside_device_and_discovery_symlinks_are_rejected(self):
        device = self.device()
        outside = self.base / "outside"
        outside.mkdir()
        (outside / "secret").write_text("must not be read", encoding="ascii")
        self.symlink(device / "ip_discovery", outside, directory=True)
        result = collector.collect(self.root)
        self.assertFalse(result["ip_discovery"])
        self.assertTrue(any("symlinks" in w["reason"] for w in result["warnings"]))
        self.symlink(self.root / "bus/pci/devices/0000:04:00.0", outside, directory=True)
        with self.assertRaisesRegex(collector.CollectionError, "outside sysfs"):
            collector.collect(self.root, "0000:04:00.0")

    def test_cli_new_output_only_and_no_sysfs_writes(self):
        self.device()
        output = self.base / "baseline.json"
        arguments = ["--sysfs-root", str(self.root), "--output", str(output)]
        self.assertEqual(collector.main(arguments), 0)
        original = output.read_bytes()
        self.assertEqual(json.loads(original)["schema_version"], 1)
        self.assertEqual(collector.main(arguments), 1)
        self.assertEqual(output.read_bytes(), original)
        forbidden = self.root / "output.json"
        self.assertEqual(collector.main(["--sysfs-root", str(self.root), "--output", str(forbidden)]), 1)
        self.assertFalse(forbidden.exists())


if __name__ == "__main__":
    unittest.main()
