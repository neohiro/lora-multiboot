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
import json
import sys
import tempfile
import unittest
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
            (ROOT / "firmware" / "src" / "ChannelPlan.cpp").read_text(encoding="utf-8"))
        self.assertIn("EU_868", rows)
        self.assertIn("US_915", rows)


class Expectations(unittest.TestCase):
    def setUp(self):
        self.data = json.loads(
            (ROOT / "firmware" / "rf_defaults.json").read_text(encoding="utf-8"))

    def test_regions_match_firmware_codes(self):
        # If these disagree the offline check compares nothing and reports success.
        rows = rf.parse_firmware_table(
            (ROOT / "firmware" / "src" / "ChannelPlan.cpp").read_text(encoding="utf-8"))
        for section in ("meshtastic", "meshcore"):
            for region in self.data.get(section, {}):
                with self.subTest(section=section, region=region):
                    self.assertIn(region, rows,
                                  f"{section}.{region} is not a region in ChannelPlan.cpp")

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
            {"EU_868": {"duty_cycle_percent": 10}}, {"EU_868": {"duty_cycle_percent": 1}},
            ["duty_cycle_percent"], "test")
        self.assertEqual(len(changes), 1)
        self.assertIn("CHANGED", changes[0])
        self.assertEqual(new, [])

    def test_untracked_region_is_not_drift(self):
        # The false-alarm that would otherwise fire every single run.
        changes, new = rf.diff_upstream(
            {}, {"ANZ_915": {"band_start_mhz": 915.0}}, ["band_start_mhz"], "test")
        self.assertEqual(changes, [])
        self.assertEqual(len(new), 1)

    def test_removed_region_we_track_is_drift(self):
        changes, _ = rf.diff_upstream(
            {"EU_868": {"band_start_mhz": 869.4}}, {}, ["band_start_mhz"], "test")
        self.assertEqual(len(changes), 1)
        self.assertIn("REMOVED", changes[0])

    def test_equal_values_are_not_drift(self):
        changes, _ = rf.diff_upstream(
            {"EU_868": {"band_start_mhz": 869.4}},
            {"EU_868": {"band_start_mhz": 869.400001}},
            ["band_start_mhz"], "test")
        self.assertEqual(changes, [])


class OfflineMode(unittest.TestCase):
    def test_offline_reports_no_drift_against_current_files(self):
        # The half that must always work, because it needs no internet.
        self.assertEqual(rf.check_offline(), rf.EXIT_OK)


if __name__ == "__main__":
    unittest.main(verbosity=2)