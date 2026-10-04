#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline tests for the RF drift monitor's parsers.

No network. The point of these is that the *parsing and comparison logic* is
verified without depending on two upstream repositories being reachable, staying
on the same URL, and not rewording their prose.

They run against small verbatim-shaped fixtures rather than live fetches, so a
change upstream shows up as a real drift report rather than as a mysteriously
failing test.
"""

from __future__ import annotations

import importlib.util
import io
import json
import pathlib
import sys
import tempfile
import unittest
import unittest.mock
import urllib.error
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

spec = importlib.util.spec_from_file_location("rf_drift", ROOT / "tools" / "rf_drift.py")
rf = importlib.util.module_from_spec(spec)
sys.modules["rf_drift"] = rf
assert spec.loader is not None
spec.loader.exec_module(rf)

# Verbatim-shaped. Note the spare 0 between the duty limit and the power limit:
# reading the wrong column reports power 0 for every region, which is the mistake
# these fixtures exist to prevent.
MESHTASTIC_FIXTURE = """
#define RDEF(name, start, end, duty, pad, power, ...) \\
    { RegionCode_ ## name, start, end, duty, pad, power }
RDEF(US, 902.0f, 928.0f, 100, 0, 30, true, false, false),
RDEF(EU_868, 869.4f, 869.65f, 10, 0, 27, false, false, false),
RDEF(ANZ, 915.0f, 928.0f, 100, 0, 30, true, false, false),
RDEF(IN, 865.0f, 867.0f, 100, 0, 30, true, false, false),
RDEF(KR, 920.0f, 923.0f, 100, 0, 23, true, false, false),
RDEF(SG, 917.0f, 925.0f, 100, 0, 16, true, false, false),
"""

MESHCORE_FIXTURE = """
USA/Canada (Recommended) preset is 910.525MHz, SF7, BW62.5, CR5.
"""


class MeshtasticParser(unittest.TestCase):
    def setUp(self):
        self.regions = rf.parse_meshtastic_regions(MESHTASTIC_FIXTURE)

    def test_eu_868_band_and_limits(self):
        eu = self.regions["EU_868"]
        self.assertAlmostEqual(eu["band_start_mhz"], 869.4)
        self.assertAlmostEqual(eu["band_end_mhz"], 869.65)
        # The whole reason this project's EU airtime default is 10 rather than
        # MeshCore's stock 50. Corroborated from the primary source.
        self.assertEqual(eu["duty_cycle_percent"], 10)
        # And the spare column is not mistaken for the power limit.
        self.assertEqual(eu["power_limit_dbm"], 27)

    def test_regions_are_keyed_by_our_codes(self):
        # Upstream names are not ours; the mapping is explicit so a fuzzy match
        # cannot silently compare the wrong band.
        for ours in ("EU_868", "US_915", "ANZ_915", "IN_865", "KR_922", "SG_923"):
            self.assertIn(ours, self.regions)
        self.assertNotIn("US", self.regions)
        self.assertEqual(self.regions["US_915"]["power_limit_dbm"], 30)
        self.assertEqual(self.regions["KR_922"]["power_limit_dbm"], 23)

    def test_untracked_regions_are_dropped_not_reported(self):
        # A region we do not track must not be invented as drift.
        text = MESHTASTIC_FIXTURE + "RDEF(JP, 920.5f, 923.5f, 100, 0, 13, false, false, false),\n"
        regions = rf.parse_meshtastic_regions(text)
        self.assertNotIn("JP", regions)

    def test_no_definitions_is_undeterminable_not_empty(self):
        # Reporting "no drift" because the file did not parse is the exact failure
        # this whole monitor exists to prevent.
        with self.assertRaises(rf.Undeterminable):
            rf.parse_meshtastic_regions("int main() { return 0; }\n")


class MeshCoreParser(unittest.TestCase):
    def test_preset_is_extracted(self):
        presets = rf.parse_meshcore_presets(MESHCORE_FIXTURE)
        self.assertEqual(len(presets), 1)
        p = presets[0]
        self.assertAlmostEqual(p["frequency_mhz"], 910.525)
        self.assertEqual(p["spreading_factor"], 7)
        self.assertAlmostEqual(p["bandwidth_khz"], 62.5)
        self.assertEqual(p["coding_rate_denominator"], 5)

    def test_reworded_prose_is_undeterminable(self):
        with self.assertRaises(rf.Undeterminable):
            rf.parse_meshcore_presets("We now recommend a different radio setup.\n")


class FirmwareTableParser(unittest.TestCase):
    SOURCE = """
constexpr RegionDefaults kRegions[] = {
    {Region::EU_868, "EU_868", 869.40f, 869.65f, 869.525f, 869.525f, true, 10},
    {Region::US_915, "US_915", 902.00f, 928.00f, 906.875f, 910.525f, false, 100},
};
"""

    def test_table_is_parsed(self):
        rows = rf.parse_firmware_table(self.SOURCE)
        self.assertEqual(set(rows), {"EU_868", "US_915"})
        self.assertTrue(rows["EU_868"]["meshcore_freq_verified"])
        self.assertFalse(rows["US_915"]["meshcore_freq_verified"])
        self.assertAlmostEqual(rows["US_915"]["meshtastic_freq_mhz"], 906.875)

    def test_changed_table_shape_is_undeterminable(self):
        # Comparing against nothing is worse than not comparing.
        with self.assertRaises(rf.Undeterminable):
            rf.parse_firmware_table("struct RegionDefaults {};\n")

    def test_real_table_still_parses(self):
        # The live file, so a refactor cannot quietly turn the offline check into
        # a no-op that always passes.
        rows = rf.parse_firmware_table(
            (ROOT / "firmware" / "src" / "ChannelPlan.cpp").read_text(encoding="utf-8")
        )
        self.assertIn("EU_868", rows)
        self.assertIn("US_915", rows)


class Expectations(unittest.TestCase):
    def setUp(self):
        self.data = json.loads((ROOT / "firmware" / "rf_defaults.json").read_text(encoding="utf-8"))

    def test_meshtastic_regions_match_firmware_codes(self):
        # If these disagree the offline check compares nothing and reports success.
        # The meshtastic keys are RDEF region codes, which is what ChannelPlan.cpp
        # is keyed by, so they must match it exactly.
        rows = rf.parse_firmware_table(
            (ROOT / "firmware" / "src" / "ChannelPlan.cpp").read_text(encoding="utf-8")
        )
        for region in self.data.get("meshtastic", {}):
            if region.startswith("_"):
                continue
            with self.subTest(region=region):
                self.assertIn(
                    region, rows, f"meshtastic.{region} is not a region in ChannelPlan.cpp"
                )

    def test_meshcore_keys_are_faq_prose_not_region_codes(self):
        # The opposite rule, on purpose. MeshCore states its presets in prose, so
        # the key is the sentence's own region text. Requiring it to be a region
        # code would be requiring the parser to invent a mapping the FAQ does not
        # provide -- which is how a fabricated EU_868 entry got recorded in the
        # first place.
        rows = rf.parse_firmware_table(
            (ROOT / "firmware" / "src" / "ChannelPlan.cpp").read_text(encoding="utf-8")
        )
        keys = [k for k in self.data.get("meshcore", {}) if not k.startswith("_")]
        self.assertTrue(keys, "no meshcore presets recorded at all")
        for key in keys:
            with self.subTest(key=key):
                self.assertNotIn(
                    key,
                    rows,
                    "a meshcore key must be the FAQ's own wording, not "
                    "a region code copied from the firmware table",
                )

    def test_meshcore_presets_carry_a_frequency_and_are_verified(self):
        for key, entry in self.data.get("meshcore", {}).items():
            if key.startswith("_") or not isinstance(entry, dict):
                continue
            with self.subTest(key=key):
                self.assertIn("frequency_mhz", entry)
                self.assertTrue(
                    entry.get("verified"),
                    f"meshcore.{key} is recorded unverified; the monitor "
                    f"must report unconfirmed rather than treat it as fact",
                )

    def test_eu_still_carries_the_shared_carrier(self):
        # The premise. If this ever changes, the architecture is dead and the
        # change must be a deliberate, reviewed edit rather than a silent edit.
        self.assertAlmostEqual(self.data["meshtastic"]["EU_868"]["band_start_mhz"], 869.4)
        self.assertEqual(self.data["meshtastic"]["EU_868"]["duty_cycle_percent"], 10)

    def test_sources_are_declared(self):
        for name in ("meshtastic", "meshcore"):
            with self.subTest(source=name):
                self.assertIn("url", self.data["sources"][name])


class DiffSemantics(unittest.TestCase):
    def test_changed_value_is_drift(self):
        changes, new = rf.diff_upstream(
            {"EU_868": {"duty_cycle_percent": 10}},
            {"EU_868": {"duty_cycle_percent": 1}},
            ["duty_cycle_percent"],
            "test",
        )
        self.assertEqual(len(changes), 1)
        self.assertIn("CHANGED", changes[0])
        self.assertEqual(new, [])

    def test_untracked_region_is_not_drift(self):
        # The false-alarm that would otherwise fire every single run.
        changes, new = rf.diff_upstream(
            {}, {"ANZ_915": {"band_start_mhz": 915.0}}, ["band_start_mhz"], "test"
        )
        self.assertEqual(changes, [])
        self.assertEqual(len(new), 1)

    def test_removed_region_we_track_is_drift(self):
        changes, _ = rf.diff_upstream(
            {"EU_868": {"band_start_mhz": 869.4}}, {}, ["band_start_mhz"], "test"
        )
        self.assertEqual(len(changes), 1)
        self.assertIn("REMOVED", changes[0])

    def test_equal_values_are_not_drift(self):
        changes, _ = rf.diff_upstream(
            {"EU_868": {"band_start_mhz": 869.4}},
            {"EU_868": {"band_start_mhz": 869.400001}},
            ["band_start_mhz"],
            "test",
        )
        self.assertEqual(changes, [])


class MeshCorePresetParsing(unittest.TestCase):
    """The FAQ is prose, so these are the cases prose actually produces."""

    FAQ_SENTENCE = (
        "Recently, as of October 2025, many regions have moved to the "
        "narrow setting, aka using BW62.5 and a lower SF number (instead "
        "of the original SF11). For example, USA/Canada (Recommended) "
        "preset is 910.525MHz, SF7, BW62.5, CR5."
    )

    def test_the_real_sentence_yields_the_region_not_the_sentence(self):
        # This exact sentence is upstream today. The failure it caused: the region
        # capture started at the first capital letter in the sentence, so the key
        # was "For example, USA/Canada (Recommended)" -- a sentence, not a region.
        out = rf.parse_meshcore_presets(self.FAQ_SENTENCE)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0]["region_text"], "USA/Canada (Recommended)")
        self.assertAlmostEqual(out[0]["frequency_mhz"], 910.525)
        self.assertEqual(out[0]["spreading_factor"], 7)
        self.assertAlmostEqual(out[0]["bandwidth_khz"], 62.5)
        self.assertEqual(out[0]["coding_rate_denominator"], 5)

    def test_a_region_capture_never_spans_two_sentences(self):
        text = (
            "EU (Recommended) preset is 869.525MHz, SF11, BW250, CR5. "
            "For example, USA/Canada (Recommended) preset is 910.525MHz, "
            "SF7, BW62.5, CR5."
        )
        keys = [p["region_text"] for p in rf.parse_meshcore_presets(text)]
        self.assertEqual(keys, ["EU (Recommended)", "USA/Canada (Recommended)"])

    def test_reworded_prose_without_a_preset_is_undeterminable(self):
        # Not an empty list, and not silence: the parser failing must be loud,
        # because a silent parser reports "no drift" forever.
        with self.assertRaises(rf.Undeterminable):
            rf.parse_meshcore_presets("The FAQ no longer documents presets at all.")


class MeshCoreDiff(unittest.TestCase):
    KEY = "USA/Canada (Recommended)"

    def _preset(self, **kw):
        base = {
            "region_text": self.KEY,
            "frequency_mhz": 910.525,
            "spreading_factor": 7,
            "bandwidth_khz": 62.5,
            "coding_rate_denominator": 5,
            "verified": True,
        }
        base.update(kw)
        return base

    def test_changed_frequency_is_drift(self):
        changes, _ = rf.diff_meshcore(
            {self.KEY: {"frequency_mhz": 910.525}}, [self._preset(frequency_mhz=915.0)]
        )
        self.assertEqual(len(changes), 1)
        self.assertIn("CHANGED", changes[0])
        self.assertIn("915.0", changes[0])

    def test_changed_spreading_factor_is_drift(self):
        # The narrow-band migration is exactly this: SF drops, carrier stays.
        changes, _ = rf.diff_meshcore(
            {self.KEY: {"spreading_factor": 11}}, [self._preset(spreading_factor=7)]
        )
        self.assertEqual(len(changes), 1)
        self.assertIn("spreading_factor", changes[0])

    def test_untracked_preset_is_newly_seen_not_drift(self):
        changes, new = rf.diff_meshcore({}, [self._preset()])
        self.assertEqual(changes, [])
        self.assertEqual(len(new), 1)

    def test_comment_key_is_not_a_removed_preset(self):
        # rf_defaults.json explains itself in "_comment" keys. Treating one as a
        # preset upstream deleted would manufacture drift out of a note to a human.
        changes, _ = rf.diff_meshcore({"_comment": ["a note"]}, [self._preset()])
        self.assertEqual(changes, [])

    def test_comment_key_is_not_a_removed_region(self):
        changes, new = rf.diff_upstream(
            {"_comment": ["a note"], "EU_868": {"band_start_mhz": 869.4}},
            {"EU_868": {"band_start_mhz": 869.4}},
            ["band_start_mhz"],
            "test",
        )
        self.assertEqual(changes, [])
        self.assertEqual(new, [])

    def test_removed_tracked_preset_is_drift(self):
        changes, _ = rf.diff_meshcore({self.KEY: {"frequency_mhz": 910.525}}, [])
        self.assertEqual(len(changes), 1)
        self.assertIn("REMOVED", changes[0])


class WritePath(unittest.TestCase):
    """--write must record what it read, and nothing it did not."""

    def test_records_meshcore_as_well_as_meshtastic(self):
        # --write only ever rewrote the meshtastic section, so MeshCore drift was
        # reported every week and never adopted.
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            exp = root / "rf_defaults.json"
            exp.write_text(
                json.dumps(
                    {
                        "sources": {},
                        "meshtastic": {},
                        "meshcore": {},
                    }
                ),
                encoding="utf-8",
            )
            saved = rf.EXPECTATIONS
            rf.EXPECTATIONS = exp
            try:
                key = "USA/Canada (Recommended)"
                rf.record_observations(
                    {"EU_868": {"band_start_mhz": 869.4, "verified": True}},
                    [
                        {
                            "region_text": key,
                            "frequency_mhz": 910.525,
                            "spreading_factor": 7,
                            "bandwidth_khz": 62.5,
                            "coding_rate_denominator": 5,
                            "verified": True,
                        }
                    ],
                    undeterminable=[],
                )
            finally:
                rf.EXPECTATIONS = saved
            data = json.loads(exp.read_text(encoding="utf-8"))
            self.assertEqual(data["meshtastic"]["EU_868"]["band_start_mhz"], 869.4)
            self.assertEqual(data["meshcore"][key]["frequency_mhz"], 910.525)
            self.assertEqual(data["meshcore"][key]["spreading_factor"], 7)

    def test_refuses_to_write_when_a_source_was_undeterminable(self):
        with tempfile.TemporaryDirectory() as td:
            exp = pathlib.Path(td) / "rf_defaults.json"
            original = {
                "sources": {},
                "meshtastic": {"EU_868": {"band_start_mhz": 869.4}},
                "meshcore": {},
            }
            exp.write_text(json.dumps(original), encoding="utf-8")
            saved = rf.EXPECTATIONS
            rf.EXPECTATIONS = exp
            try:
                wrote = rf.record_observations(
                    {"EU_868": {"band_start_mhz": 868.0}},
                    [],
                    undeterminable=["meshcore: fetch failed"],
                )
            finally:
                rf.EXPECTATIONS = saved
            self.assertFalse(wrote)
            # and the file is untouched, not half-updated
            self.assertEqual(json.loads(exp.read_text(encoding="utf-8")), original)

    def test_refuses_to_write_when_nothing_was_observed(self):
        # An empty observation is not permission to blank the recorded values.
        with tempfile.TemporaryDirectory() as td:
            exp = pathlib.Path(td) / "rf_defaults.json"
            original = {
                "sources": {},
                "meshtastic": {"EU_868": {"band_start_mhz": 869.4}},
                "meshcore": {},
            }
            exp.write_text(json.dumps(original), encoding="utf-8")
            saved = rf.EXPECTATIONS
            rf.EXPECTATIONS = exp
            try:
                wrote = rf.record_observations({}, [], undeterminable=[])
            finally:
                rf.EXPECTATIONS = saved
            self.assertFalse(wrote)
            self.assertEqual(json.loads(exp.read_text(encoding="utf-8")), original)


class FetchRetries(unittest.TestCase):
    """A monitor that cries wolf every week is a monitor nobody reads."""

    def test_a_transient_failure_is_retried_and_then_succeeds(self):
        calls = []

        def fake_urlopen(req, timeout=None):
            calls.append(req.full_url)
            if len(calls) == 1:
                raise urllib.error.URLError("temporary failure in name resolution")
            return io.BytesIO(b"content")

        with (
            unittest.mock.patch.object(rf.urllib.request, "urlopen", fake_urlopen),
            unittest.mock.patch.object(rf.time, "sleep", lambda _s: None),
        ):
            self.assertEqual(rf.fetch("https://example.invalid/x"), "content")
        self.assertEqual(len(calls), 2, "the first failure must be retried once")

    def test_a_persistent_failure_reports_undeterminable_not_success(self):
        calls = []

        def fake_urlopen(req, timeout=None):
            calls.append(req.full_url)
            raise urllib.error.URLError("404")

        with (
            unittest.mock.patch.object(rf.urllib.request, "urlopen", fake_urlopen),
            unittest.mock.patch.object(rf.time, "sleep", lambda _s: None),
        ):
            with self.assertRaises(rf.Undeterminable) as cm:
                rf.fetch("https://example.invalid/x")
        self.assertEqual(len(calls), 2, "exactly one retry, not a loop")
        msg = str(cm.exception)
        self.assertIn("2 attempts", msg)
        self.assertIn("404", msg)

    def test_retries_are_bounded_so_a_missing_source_still_fails(self):
        # The workflow has a 10 minute timeout. An unbounded retry loop would turn a
        # deleted file into a hung job rather than a reported problem.
        self.assertLessEqual(rf.RETRY_BACKOFF_SECONDS * 2, 30)
        calls = []

        def fake_urlopen(req, timeout=None):
            calls.append(1)
            raise OSError("boom")

        with (
            unittest.mock.patch.object(rf.urllib.request, "urlopen", fake_urlopen),
            unittest.mock.patch.object(rf.time, "sleep", lambda _s: None),
        ):
            with self.assertRaises(rf.Undeterminable):
                rf.fetch("https://example.invalid/x", attempts=2)
        self.assertEqual(len(calls), 2)


class AtomicWrite(unittest.TestCase):
    """rf_defaults.json is committed and compared against by the offline gate.

    A run interrupted between truncating and writing would otherwise leave invalid
    JSON, which reads as upstream drift next week and as a broken gate in CI.
    """

    def _original(self, tmp: pathlib.Path) -> pathlib.Path:
        exp = tmp / "rf_defaults.json"
        exp.write_text(
            json.dumps(
                {
                    "sources": {},
                    "checked_on": "2000-01-01",
                    "meshtastic": {"EU_868": {"duty_cycle_percent": 10}},
                    "meshcore": {},
                }
            ),
            encoding="utf-8",
        )
        return exp

    def test_the_write_leaves_no_temporary_file_behind(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            exp = self._original(tmp)
            saved = rf.EXPECTATIONS
            rf.EXPECTATIONS = exp
            try:
                self.assertTrue(
                    rf.record_observations(
                        {"EU_868": {"duty_cycle_percent": 1}}, [], undeterminable=[]
                    )
                )
            finally:
                rf.EXPECTATIONS = saved
            self.assertEqual(
                json.loads(exp.read_text(encoding="utf-8"))["meshtastic"]["EU_868"][
                    "duty_cycle_percent"
                ],
                1,
            )
            leftovers = [p.name for p in tmp.iterdir() if p.name.endswith(".tmp")]
            self.assertEqual(leftovers, [], f"a temporary file was left behind: {leftovers}")

    def test_a_failed_write_does_not_destroy_the_original(self):
        # Simulate the write failing partway, which is the case atomicity exists for.
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            exp = self._original(tmp)
            original = exp.read_text(encoding="utf-8")

            real_replace = pathlib.Path.replace

            def boom(self, target):
                raise OSError("no space left on device")

            saved = rf.EXPECTATIONS
            rf.EXPECTATIONS = exp
            pathlib.Path.replace = boom
            try:
                with self.assertRaises(OSError):
                    rf.record_observations(
                        {"EU_868": {"duty_cycle_percent": 99}}, [], undeterminable=[]
                    )
            finally:
                pathlib.Path.replace = real_replace
                rf.EXPECTATIONS = saved

            # The old file is still intact and still valid JSON.
            self.assertEqual(exp.read_text(encoding="utf-8"), original)
            json.loads(exp.read_text(encoding="utf-8"))
            self.assertEqual(
                [p.name for p in tmp.iterdir() if p.name.endswith(".tmp")],
                [],
                "the failed write left its temporary file behind",
            )

    def test_it_really_is_atomic_and_not_truncate_then_write(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td)
            exp = self._original(tmp)
            seen = []
            real_write = pathlib.Path.write_text

            def spy(self, *a, **kw):
                # If the destination were written directly its contents would change
                # before the rename. Writing a sibling leaves it untouched until the
                # rename, which is the whole point of doing it this way.
                seen.append(self.name)
                return real_write(self, *a, **kw)

            saved = rf.EXPECTATIONS
            rf.EXPECTATIONS = exp
            pathlib.Path.write_text = spy
            try:
                rf.record_observations({"EU_868": {"duty_cycle_percent": 5}}, [], undeterminable=[])
            finally:
                pathlib.Path.write_text = real_write
                rf.EXPECTATIONS = saved

            self.assertEqual(
                seen,
                ["rf_defaults.json.tmp"],
                "the destination was written directly, so the rename is not providing atomicity",
            )


class OfflineMode(unittest.TestCase):
    def test_offline_reports_no_drift_against_current_files(self):
        # The half that must always work, because it needs no internet.
        self.assertEqual(rf.check_offline(), rf.EXIT_OK)


if __name__ == "__main__":
    unittest.main(verbosity=2)
