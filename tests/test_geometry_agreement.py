"""Cross-checks between the geometry's owner and the three places that restate it.

Provisioning.hpp owns the flash layout. Three other places hold their own copy of
part of it, and none of them was generated or checked:

  * firmware/src/main.cpp           kCompiledLayout -- the table a node reports
  * firmware/src/SlotLifecycle.cpp  the system rows reclaim re-emits
  * tools/flash.py                  the implicit bootloader / partition_tbl

A copy that nobody checks is not a second source of truth, it is a second source
of *truth* that happens to agree today. These assertions are what stop the copies
from quietly becoming different answers, which is the failure that would write a
partition table to a board that does not match its firmware.
"""

from __future__ import annotations

import pathlib
import re
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROVISIONING = ROOT / "firmware" / "include" / "bridge" / "Provisioning.hpp"
QUADBOOT = ROOT / "firmware" / "partitions" / "quadboot.csv"
MAIN_CPP = ROOT / "firmware" / "src" / "main.cpp"
SLOT_LIFECYCLE = ROOT / "firmware" / "src" / "SlotLifecycle.cpp"

_CONST = re.compile(r"constexpr\s+std::uint32_t\s+(k\w+)\s*=\s*([^;]+);")


def geometry_constants() -> dict[str, int]:
    """The uint32 geometry constants from Provisioning.hpp, with the header's own
    simple expressions evaluated.

    Resolved iteratively so a constant may refer to one declared above it, which
    kBootloaderSize does.
    """
    text = PROVISIONING.read_text(encoding="utf-8")
    pending = [(m.group(1), m.group(2).split("//")[0].strip()) for m in _CONST.finditer(text)]

    out: dict[str, int] = {}
    # Loop rather than recurse: the header has no cycles, but a cycle should be a
    # clean "cannot resolve" rather than a RecursionError.
    for _ in range(len(pending) + 1):
        if not pending:
            break
        still: list[tuple[str, str]] = []
        for name, expr in pending:
            # Arithmetic over hex literals and previously-declared names. Letters
            # are allowed here so that a constant may refer to another; whether it
            # actually can is decided by the residual check below.
            if not re.fullmatch(r"[A-Za-z0-9_+\-*/() ]+", expr):
                return out  # not arithmetic; leave it to whoever asked for it
            # Hex literals are removed before looking for names. "0x0" contains the
            # identifier-looking text "x0", and treating that as a name makes every
            # hex constant look unresolvable.
            residual = re.sub(r"0[xX][0-9a-fA-F]+|\b\d+\b", " ", expr)
            names = set(re.findall(r"[A-Za-z_]\w*", residual))
            if not names <= set(out):
                still.append((name, expr))
                continue
            out[name] = int(eval(expr, {"__builtins__": {}}, dict(out)))
        if len(still) == len(pending):
            break  # nothing resolved this pass; a cycle
        pending = still
    return out


def split_rows(block: str) -> list[tuple[str, ...]]:
    """Parse ESP-IDF CSV rows into comparable 5-tuples with normalised numbers."""
    out = []
    for line in block.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = [x.strip() for x in line.split(",")]
        if len(f) < 5:
            continue
        for i in (3, 4):
            try:
                f[i] = int(f[i].strip().rstrip(), 0)
            except ValueError:
                f[i] = None
        out.append(tuple(f[:5]))
    return out


def string_literal_block(text: str, decl: str) -> str:
    """The concatenated contents of a C++ string-literal initialiser.

    The literals carry "\\n" as two characters, so they are unescaped here: without
    that, every row arrives as one line and the row-by-row comparisons below would
    compare a single string. An array declarator ("char sys[160] =") is allowed for,
    because that is how the reclaim preamble is written.
    """
    m = re.search(rf"{decl}\s*(?:\[[^\]]*\])?\s*=\s*(.*?);", text, re.S)
    if not m:
        return ""
    joined = "".join(re.findall(r'"([^"]*)"', m.group(1)))
    return joined.replace("\\n", "\n")


class GeometryConstantsResolve(unittest.TestCase):
    def test_every_geometry_constant_is_readable(self):
        c = geometry_constants()
        for name in (
            "kBootloaderOffset",
            "kPartitionTableOffset",
            "kPartitionTableSize",
            "kBootloaderSize",
            "kNvsOffset",
            "kNvsSize",
            "kOtadataOffset",
            "kOtadataSize",
            "kCoredumpOffset",
            "kCoredumpSize",
            "kFirstSlotOffset",
        ):
            with self.subTest(constant=name):
                self.assertIn(name, c)
                self.assertIsInstance(c[name], int)

    def test_kBootloaderSize_is_derived_not_restated(self):
        c = geometry_constants()
        self.assertEqual(
            c["kBootloaderSize"], c["kPartitionTableOffset"] - c["kPartitionTableSize"]
        )


class CompiledLayoutMatchesShippedTable(unittest.TestCase):
    """main.cpp's kCompiledLayout is the table the node reports about itself."""

    def setUp(self):
        self.c = geometry_constants()
        self.compiled = split_rows(
            string_literal_block(MAIN_CPP.read_text(encoding="utf-8"), "kCompiledLayout")
        )
        self.shipped = split_rows(QUADBOOT.read_text(encoding="utf-8"))

    def test_kCompiledLayout_exists_at_all(self):
        self.assertTrue(self.compiled, "kCompiledLayout not found; the node would report nothing")

    def test_it_names_the_same_rows_in_the_same_order(self):
        self.assertEqual(
            [r[0] for r in self.compiled],
            [r[0] for r in self.shipped],
            "kCompiledLayout and quadboot.csv disagree about which partitions exist",
        )

    def test_every_offset_and_size_matches(self):
        by_label = {r[0]: r for r in self.shipped}
        for r in self.compiled:
            with self.subTest(label=r[0]):
                self.assertIn(r[0], by_label)
                ship = by_label[r[0]]
                self.assertEqual(r[3], ship[3], f"{r[0]} offset differs")
                self.assertEqual(r[4], ship[4], f"{r[0]} size differs")

    def test_it_omits_the_two_implicit_pieces(self):
        # They cannot be rows; a node reporting them as rows is describing a table
        # ESP-IDF would refuse.
        labels = {r[0] for r in self.compiled}
        self.assertNotIn("bootloader", labels)
        self.assertNotIn("partition_tbl", labels)

    def test_it_agrees_with_the_constants_on_the_system_region(self):
        by_label = {r[0]: r for r in self.compiled}
        for label, off, size in (
            ("nvs", "kNvsOffset", "kNvsSize"),
            ("otadata", "kOtadataOffset", "kOtadataSize"),
            ("coredump", "kCoredumpOffset", "kCoredumpSize"),
        ):
            with self.subTest(label=label):
                self.assertEqual(by_label[label][3], self.c[off])
                self.assertEqual(by_label[label][4], self.c[size])

    def test_its_first_slot_is_the_constants_first_slot(self):
        first = min(r[3] for r in self.compiled if r[0].startswith("ota_"))
        self.assertEqual(first, self.c["kFirstSlotOffset"])


def snprintf_call_after(text: str, decl: str) -> str:
    """The statement following a buffer declaration, up to its closing ");".

    The reclaim preamble is built with snprintf rather than initialised from
    literals, so the interesting text is the call's own arguments: the format
    lines, the labels, the subtypes, and whatever the offsets turned out to be.
    """
    start = text.find(decl)
    if start < 0:
        return ""
    end = text.find(");", start)
    return text[start:end] if end > start else ""


class ReclaimSystemRowsMatchConstants(unittest.TestCase):
    """reclaimSlot rebuilds the table from scratch, so the system rows it re-emits
    are the only record of where NVS, otadata and coredump live.

    These are no longer literals: the rows are formatted from the constants
    themselves, so there is nothing left to disagree. What this asserts instead is
    the thing that can still go wrong -- that the labels and subtypes are the ones
    the rest of the firmware looks up, and that no literal offset crept back in.
    """

    def setUp(self):
        self.text = SLOT_LIFECYCLE.read_text(encoding="utf-8")
        self.call = snprintf_call_after(self.text, "char sys[")

    def test_the_preamble_exists(self):
        self.assertTrue(self.call, "the reclaim preamble is gone")

    def test_it_emits_three_rows(self):
        self.assertEqual(
            self.call.count("0x%X"), 6, "three system rows, each with an offset and a size"
        )

    def test_it_names_the_labels_and_subtypes_the_firmware_looks_up(self):
        literals = re.findall(r'"([^"]*)"', self.call)
        # Format lines aside, the arguments are label/subtype pairs in order.
        pairs = [x for x in literals if not x.startswith("%") and "\n" not in x]
        self.assertEqual(pairs, ["nvs", "nvs", "otadata", "ota", "coredump", "coredump"])

    def test_otadata_is_not_used_as_a_subtype(self):
        # 'otadata' is the partition's label. As a subtype it is not an ESP-IDF
        # keyword, and the real generator rejects it -- which is how this table was
        # found to be unbuildable in the first place.
        literals = re.findall(r'"([^"]*)"', self.call)
        self.assertIn("otadata", literals)  # the label
        self.assertNotIn("data, otadata,", self.call)

    def test_the_offsets_come_from_constants_not_literals(self):
        args = self.call.split('",', 1)[-1] if '",' in self.call else self.call
        self.assertEqual(
            re.findall(r"0[xX][0-9a-fA-F]+", args),
            [],
            "the reclaim preamble restates an offset as a literal; it "
            "must format kNvsOffset/kOtadataOffset/kCoredumpOffset",
        )

    def test_it_names_the_constants_it_formats_from(self):
        for name in (
            "kNvsOffset",
            "kNvsSize",
            "kOtadataOffset",
            "kOtadataSize",
            "kCoredumpOffset",
            "kCoredumpSize",
        ):
            with self.subTest(constant=name):
                self.assertIn(name, self.call)

    def test_it_includes_the_constants_header(self):
        self.assertIn(
            '#include "bridge/Provisioning.hpp"',
            self.text,
            "the reclaim path formats from Provisioning.hpp constants, so "
            "it must include them rather than hope they arrived",
        )

    def test_it_rechecks_the_system_rows_after_rebuilding(self):
        # The rebuild is now checked for the system rows the way it always was for
        # slots. Without this, a wrong row survives into a table that validates.
        self.assertIn("rebuilding would move a system partition", self.text)


class FlashToolGeometryMatchesConstants(unittest.TestCase):
    """flash.py is the only thing standing between a mistake and a board."""

    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(ROOT / "tools"))
        import flash

        cls.flash = flash
        cls.c = geometry_constants()

    def test_implicit_pieces_are_the_constants(self):
        pieces = self.flash.system_pieces(QUADBOOT)
        self.assertEqual(
            pieces["bootloader"], (self.c["kBootloaderOffset"], self.c["kBootloaderSize"])
        )
        self.assertEqual(
            pieces["partition_tbl"],
            (self.c["kPartitionTableOffset"], self.c["kPartitionTableSize"]),
        )

    def test_system_layer_ends_below_the_first_slot(self):
        pieces = self.flash.system_pieces(QUADBOOT)
        end = max(off + size for off, size in pieces.values())
        self.assertLessEqual(end, self.c["kFirstSlotOffset"])

    def test_the_shipped_tables_match_the_constants_exactly(self):
        for name in ("quadboot", "dualboot"):
            with self.subTest(table=name):
                rows = {
                    r[0]: r
                    for r in split_rows(
                        (ROOT / "firmware" / "partitions" / f"{name}.csv").read_text(
                            encoding="utf-8"
                        )
                    )
                }
                self.assertEqual(rows["nvs"][3], self.c["kNvsOffset"])
                self.assertEqual(rows["nvs"][4], self.c["kNvsSize"])
                self.assertEqual(rows["otadata"][3], self.c["kOtadataOffset"])
                self.assertEqual(rows["otadata"][4], self.c["kOtadataSize"])
                self.assertEqual(rows["coredump"][3], self.c["kCoredumpOffset"])
                self.assertEqual(rows["coredump"][4], self.c["kCoredumpSize"])
                self.assertEqual(rows["ota_0"][3], self.c["kFirstSlotOffset"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
