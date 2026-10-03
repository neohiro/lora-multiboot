#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for the flashing tool's layout validation.

tools/flash.py deliberately reimplements the geometry rules that SlotTable.cpp
enforces, because a new user validating a partition table should not have to
install a C++ compiler first. Duplication like that rots unless something holds
it still, so these tests pin both halves to the same shipped tables and to the
same set of failures.

Run: python tests/test_flash_tool.py
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

spec = importlib.util.spec_from_file_location("flash_tool", ROOT / "tools" / "flash.py")
flash = importlib.util.module_from_spec(spec)
sys.modules["flash_tool"] = flash  # dataclass needs the module registered
assert spec.loader is not None
spec.loader.exec_module(flash)

FLASH_16MB = 16 * 1024 * 1024
PARTITION_DIR = ROOT / "firmware" / "partitions"


class ParseSize(unittest.TestCase):
    def test_units(self):
        self.assertEqual(flash.parse_size("0x10000"), 0x10000)
        self.assertEqual(flash.parse_size("4K"), 4096)
        self.assertEqual(flash.parse_size("1536K"), 1536 * 1024)
        self.assertEqual(flash.parse_size("1M"), 1024 * 1024)
        self.assertEqual(flash.parse_size("4096"), 4096)

    def test_rejects_garbage(self):
        for bad in ("", "nonsense", "12X", "0x"):
            with self.assertRaises(flash.FlashError, msg=bad):
                flash.parse_size(bad)


class ShippedTables(unittest.TestCase):
    """The tables people actually flash have to pass."""

    def test_tables_exist(self):
        for name in ("quadboot", "dualboot"):
            self.assertTrue((PARTITION_DIR / f"{name}.csv").is_file(), name)

    def test_layouts_are_valid(self):
        for name in ("quadboot", "dualboot"):
            with self.subTest(name):
                parts = flash.parse_csv(PARTITION_DIR / f"{name}.csv")
                flash.validate(parts, FLASH_16MB)

    def test_quadboot_reserves_five_frameworks(self):
        parts = flash.parse_csv(PARTITION_DIR / "quadboot.csv")
        labels = {p.label for p in parts}
        for slot in ("ota_0", "ota_1", "ota_2", "ota_3", "ota_4"):
            self.assertIn(slot, labels)

    def test_meshcore_and_meshtastic_filesystems_are_separate(self):
        for name in ("quadboot", "dualboot"):
            parts = {p.label: p for p in flash.parse_csv(PARTITION_DIR / f"{name}.csv")}
            self.assertNotEqual(
                parts["fs_meshcore"].offset,
                parts["fs_meshtastic"].offset,
                f"{name}: one shared filesystem destroys the other side's settings",
            )

    def test_app_slots_are_aligned(self):
        # A misaligned app partition makes the board look simply dead, with
        # nothing on the serial log to explain it.
        for name in ("quadboot", "dualboot"):
            for p in flash.parse_csv(PARTITION_DIR / f"{name}.csv"):
                if p.ptype == "app":
                    self.assertEqual(p.offset % 0x10000, 0, f"{name}/{p.label}")

    def test_leaves_free_flash(self):
        for name in ("quadboot", "dualboot"):
            parts = flash.parse_csv(PARTITION_DIR / f"{name}.csv")
            highest = max(p.offset + p.size for p in parts if not p.blank)
            self.assertLess(highest, FLASH_16MB, f"{name} fills the chip exactly")


class Rejections(unittest.TestCase):
    """Every failure the tool exists to prevent."""

    def _parts(self, csv: str):
        import tempfile

        f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False)
        f.write(csv)
        f.close()
        self.addCleanup(lambda: Path(f.name).unlink(missing_ok=True))
        return flash.parse_csv(Path(f.name))

    def test_overlap(self):
        # b sits wholly inside a. Both are 64KB aligned so the alignment check
        # cannot fire first and mask what is being tested here.
        csv = "a, app, factory, 0x10000, 0x100000,\nb, app, ota_0,   0x10000,0x8000,\n"
        with self.assertRaises(flash.FlashError) as cm:
            flash.validate(self._parts(csv), FLASH_16MB)
        self.assertIn("overlap", str(cm.exception))

    def test_partial_overlap(self):
        csv = "a, app, factory, 0x10000, 0x100000,\nb, app, ota_0,   0x100000,0x100000,\n"
        with self.assertRaises(flash.FlashError):
            flash.validate(self._parts(csv), FLASH_16MB)

    def test_misaligned_app_partition(self):
        csv = "a, app, factory, 0x11000, 0x100000,\n"
        with self.assertRaises(flash.FlashError) as cm:
            flash.validate(self._parts(csv), FLASH_16MB)
        self.assertIn("64KB aligned", str(cm.exception))

    def test_misaligned_data_partition_is_fine(self):
        csv = "d, data, nvs, 0x11000, 0x1000,\n"
        flash.validate(self._parts(csv), FLASH_16MB)

    def test_overruns_flash(self):
        csv = "a, app, factory, 0x10000, 0x2000000,\n"
        with self.assertRaises(flash.FlashError) as cm:
            flash.validate(self._parts(csv), FLASH_16MB)
        self.assertIn("past", str(cm.exception))

    def test_swapped_filesystem_subtypes_are_refused(self):
    # Distinct offsets, so geometry is happy -- but the pairing is backwards.
    # Each firmware would then mount and format the other's filesystem.
        csv = (
            "fs_meshcore,   data, littlefs, 0x10000, 0x100000,\n"
            "fs_meshtastic, data, spiffs,   0x110000, 0x100000,\n"
        )
        with self.assertRaises(flash.FlashError) as cm:
            flash.validate(self._parts(csv), FLASH_16MB)
        self.assertIn("expected", str(cm.exception))

    def test_duplicate_labels(self):
        csv = "a, app, factory, 0x0,    0x7000,\na, app, ota_0,   0x10000,0x100000,\n"
        with self.assertRaises(flash.FlashError) as cm:
            flash.validate(self._parts(csv), FLASH_16MB)
        self.assertIn("duplicate", str(cm.exception))

    def test_v3_size_is_caught(self):
        # Flashing the five-slot table onto an 8MB V3 fails obscurely. Catch it
        # here, where the message can say what is actually wrong.
        parts = flash.parse_csv(PARTITION_DIR / "quadboot.csv")
        with self.assertRaises(flash.FlashError):
            flash.validate(parts, 8 * 1024 * 1024)


if __name__ == "__main__":
    unittest.main(verbosity=2)
