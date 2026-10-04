"""End-to-end rehearsal of the drift -> record -> version path, offline.

Feeds the checker a doctored upstream text where Meshtastic's EU duty cycle has
moved and MeshCore's preset has gained an SF the monitor has not seen, then asserts
that what would be recorded and what would be reported are both right. The point
is that the workflow's version branch is exercised against a real change, because
the branch that has never run is the branch that is wrong.
"""
import importlib.util
import json
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("rf", ROOT / "tools" / "rf_drift.py")
rf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rf)

failures = []


def check(label, cond, detail=""):
    print(f"  {'ok  ' if cond else 'FAIL'} {label}{(' -- ' + detail) if detail and not cond else ''}")
    if not cond:
        failures.append(label)


# Only regions the project tracks are returned; US_915 is deliberately not one of
# them, so this text yields exactly EU_868. Asserting on the count rather than on
# the names is what makes that visible if the alias table ever moves.
DOCTORED_MT = """
RDEF(EU_868, 869.4f, 869.60f, 1, 0, 20, 0, 0, 0, 0, 0)
RDEF(US_915, 902.0f, 928.0f, 100, 0, 30, 0, 0, 0, 0, 0)
"""

UNCHANGED_MC = ('Recently, as of October 2025, many regions have moved to the narrow '
                'setting, aka using BW62.5 and a lower SF number (instead of the '
                'original SF11). For example, USA/Canada (Recommended) preset is '
                '910.525MHz, SF7, BW62.5, CR5.')

print("== a real upstream change is detected ==")
mt = rf.parse_meshtastic_regions(DOCTORED_MT)
mc = rf.parse_meshcore_presets(UNCHANGED_MC)
check("tracked region parsed", list(mt) == ["EU_868"], str(list(mt)))
check("meshcore preset parsed", len(mc) == 1)
check("untracked US_915 is not returned", "US_915" not in mt, "US_915 is not a tracked plan")

data = json.loads((ROOT / "firmware" / "rf_defaults.json").read_text(encoding="utf-8"))
changes, new_seen = rf.diff_upstream(
    data.get("meshtastic") or {}, mt,
    ["band_start_mhz", "band_end_mhz", "duty_cycle_percent", "power_limit_dbm"],
    "meshtastic")
mc_changes, mc_new = rf.diff_meshcore(data.get("meshcore") or {}, mc)

joined = " ".join(changes)
check("duty cycle move is reported", "duty_cycle_percent: 10 -> 1" in joined, joined)
check("band end move is reported", "band_end_mhz: 869.65 -> 869.6" in joined, joined)
check("power limit move is reported", "power_limit_dbm: 27 -> 20" in joined, joined)
check("unchanged band start is not reported", "band_start_mhz" not in joined, joined)
check("unchanged MeshCore preset is not drift", not mc_changes, str(mc_changes))
check("nothing newly seen", not new_seen and not mc_new, f"{new_seen} {mc_new}")

print("== what gets recorded is what was observed ==")
with tempfile.TemporaryDirectory() as td:
    exp = pathlib.Path(td) / "rf_defaults.json"
    exp.write_text(json.dumps({"sources": {}, "meshtastic": data["meshtastic"],
                               "meshcore": data["meshcore"]}), encoding="utf-8")
    saved = rf.EXPECTATIONS
    rf.EXPECTATIONS = exp
    try:
        wrote = rf.record_observations(mt, mc, undeterminable=[])
    finally:
        rf.EXPECTATIONS = saved
    out = json.loads(exp.read_text(encoding="utf-8"))
    check("file was written", wrote)
    check("duty cycle recorded", out["meshtastic"]["EU_868"]["duty_cycle_percent"] == 1)
    check("power limit recorded", out["meshtastic"]["EU_868"]["power_limit_dbm"] == 20)
    check("MeshCore still recorded", "USA/Canada (Recommended)" in out["meshcore"])
    check("EU MeshCore preset not invented", "EU_868" not in out["meshcore"],
          "a preset the FAQ does not state must never appear")

print("== a parse failure is not drift and not a rewrite ==")
with tempfile.TemporaryDirectory() as td:
    exp = pathlib.Path(td) / "rf_defaults.json"
    original = {"sources": {}, "meshtastic": {"EU_868": {"duty_cycle_percent": 10}},
                "meshcore": {}}
    exp.write_text(json.dumps(original), encoding="utf-8")
    saved = rf.EXPECTATIONS
    rf.EXPECTATIONS = exp
    try:
        wrote = rf.record_observations(mt, [], undeterminable=["meshcore: reworded"])
    finally:
        rf.EXPECTATIONS = saved
    check("nothing written", not wrote)
    check("file untouched", json.loads(exp.read_text(encoding="utf-8")) == original)

print("== version bump arithmetic, as the workflow does it ==")
for cur, want in (("0.1.0", "0.2.0"), ("1.9.4", "1.10.0"), ("0.10.0", "0.11.0")):
    major = cur.split(".")[0]
    minor = int(cur.split(".")[1])
    check(f"{cur} -> {want}", f"{major}.{minor + 1}.0" == want)

print()
if failures:
    print(f"FAILED: {failures}")
    sys.exit(1)
print("drift path rehearsed clean")