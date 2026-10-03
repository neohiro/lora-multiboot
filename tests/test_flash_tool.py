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


class SystemLayer(unittest.TestCase):
    """The system layer must be everything below the first slot, and no more."""

    def test_pieces_are_all_below_the_first_slot(self):
        for name in ("quadboot", "dualboot"):
            pieces = flash.system_pieces(PARTITION_DIR / f"{name}.csv")
            self.assertIn("bootloader", pieces, name)
            self.assertIn("partition_tbl", pieces, name)
            self.assertIn("otadata", pieces, name)
            self.assertIn("nvs", pieces, name)

            parts = flash.parse_csv(PARTITION_DIR / f"{name}.csv")
            first_slot = min(p.offset for p in parts if p.label.startswith("ota_"))
            for label, (offset, size) in pieces.items():
                self.assertLessEqual(
                    offset + size, first_slot,
                    f"{name}: {label} runs past the first slot",
                )

    def test_no_slot_is_in_the_system_layer(self):
        # A slot in the system layer would mean a system update could overwrite a
        # firmware image, which is the one thing it must never do.
        for name in ("quadboot", "dualboot"):
            pieces = flash.system_pieces(PARTITION_DIR / f"{name}.csv")
            for label in pieces:
                self.assertFalse(label.startswith("ota_"), f"{name}: {label} is a slot")
                self.assertFalse(label.startswith("fs_"), f"{name}: {label} is a filesystem")

    def test_matches_the_first_slot_cut(self):
        # 0x30000 is the cut, and it is the invariant the C++ side asserts too.
        pieces = flash.system_pieces(PARTITION_DIR / "quadboot.csv")
        self.assertEqual(pieces["coredump"][0] + pieces["coredump"][1], 0x30000)

    def test_refuses_a_table_with_no_slots(self):
        import tempfile

        f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False)
        f.write("bootloader, app, factory, 0x0, 0x7000,\n")
        f.close()
        self.addCleanup(lambda: Path(f.name).unlink(missing_ok=True))
        with self.assertRaises(flash.FlashError):
            flash.system_pieces(Path(f.name))


class EsptoolInvocation(unittest.TestCase):
    """The command actually built.

    `write_flash` has no per-region `--erase` flag; the subcommand is
    `erase_region`, which takes a start *and* a size. Getting that wrong produces a
    command esptool rejects, so `app N --erase` fails at the one moment somebody is
    trying to recover a node. Worth pinning.
    """

    def _capture(self, plan):
        import types

        calls = []

        class Result:
            returncode = 0

        real_run = flash.subprocess.run
        real_which = flash.shutil.which
        real_port = flash.find_port
        real_size = flash.chip_flash_size
        try:
            flash.shutil.which = lambda name, *a, **k: (
                "C:/fake/esptool" if name == "esptool" else real_which(name, *a, **k))
            flash.find_port = lambda explicit=None: "COM7"
            flash.chip_flash_size = lambda port: 16 * 1024 * 1024

            def fake_run(cmd, *a, **k):
                calls.append(list(cmd))
                return Result()

            flash.subprocess.run = fake_run
            args = types.SimpleNamespace(port="COM7", baud="460800", flash_size=16)
            rc = flash._run_esptool(args, plan)
            return rc, calls
        finally:
            flash.subprocess.run = real_run
            flash.shutil.which = real_which
            flash.find_port = real_port
            flash.chip_flash_size = real_size

    def test_erase_uses_erase_region_not_write_flash(self):
        rc, calls = self._capture(
            [flash.Write("ota_1", 0x330000, 0x200000, None)])
        self.assertEqual(rc, 0)
        self.assertEqual(len(calls), 1)
        cmd = calls[0]
        self.assertIn("erase_region", cmd)
        self.assertNotIn("write_flash", cmd)
        self.assertIn("0x330000", cmd)
        # erase_region needs a size, not just an address.
        self.assertIn("0x200000", cmd)

    def test_write_uses_write_flash(self):
        import tempfile
        from pathlib import Path as P

        f = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        f.write(b"\x00" * 1024)
        f.close()
        self.addCleanup(lambda: P(f.name).unlink(missing_ok=True))

        rc, calls = self._capture(
            [flash.Write("ota_0", 0x30000, 0x200000, P(f.name))])
        self.assertEqual(rc, 0)
        self.assertIn("write_flash", calls[0])
        self.assertIn("0x30000", calls[0])

    def test_erase_happens_before_write(self):
        import tempfile
        from pathlib import Path as P

        f = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        f.write(b"\x00" * 1024)
        f.close()
        self.addCleanup(lambda: P(f.name).unlink(missing_ok=True))

        rc, calls = self._capture([
            flash.Write("ota_1", 0x330000, 0x200000, P(f.name)),
            flash.Write("ota_0", 0x30000, 0x200000, None),
        ])
        self.assertEqual(rc, 0)
        self.assertEqual(len(calls), 2)
        # A region must be erased before anything is written into the chip, or the
        # new image lands on top of the old one and the erase silently did nothing.
        self.assertIn("erase_region", calls[0])
        self.assertIn("write_flash", calls[1])

    def test_write_record_carries_the_size(self):
        # The size is what makes erase_region possible, so a plan that omits it is
        # the bug this whole class exists to prevent.
        w = flash.Write("ota_0", 0x30000, 0x200000)
        self.assertEqual(w.size, 0x200000)
        self.assertEqual(w.offset, 0x30000)
        self.assertIsNone(w.path)


if __name__ == "__main__":
    unittest.main(verbosity=2)
