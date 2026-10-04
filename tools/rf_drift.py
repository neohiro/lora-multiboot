#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""RF drift monitor -- watch the assumption the whole project rests on.

This project assumes Meshtastic and MeshCore share a carrier, so one radio can
serve both. That is true today (EU_868: both 869.525 MHz) and it is an *external*
fact about two projects we do not control. If either moves its default, this
architecture does not degrade politely -- it becomes one mesh plus noise while
still looking healthy. That is the worst failure mode available to us: silent, and
only discoverable by somebody standing on a roof with a spectrum analyser.

So the assumption gets monitored.

What is checked, and what is deliberately not:

  CHECKED   Meshtastic's RDEF() band limits and duty-cycle limit, read from
            firmware source. These are literal constants, so parsing them is exact.
            They also independently corroborate our own EU airtime cap.
  CHECKED   MeshCore's recommended presets per region, read from its FAQ.
  NOT CHECKED
            The *derived* default slot centre for each Meshtastic region. That
            value is computed by a channel-hashing algorithm, and re-deriving it
            here from memory would risk encoding a subtly wrong algorithm that then
            reports confident, false drift. A monitor that guesses is worse than no
            monitor, so instead the band is checked and a human is asked to confirm
            the centre when the band moves.

Exit codes are distinct on purpose, because "everything is fine" and "I could not
tell" must never look the same:

  0  no drift
  3  drift detected -- values changed, rewrite the expectations
  4  undeterminable -- a source was unreachable or unparseable

Usage:
  python tools/rf_drift.py              # report only
  python tools/rf_drift.py --write      # record newly observed values
  python tools/rf_drift.py --offline    # compare the recorded values against
                                        # firmware/src/ChannelPlan.cpp, no network
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXPECTATIONS = ROOT / "firmware" / "rf_defaults.json"
CHANNEL_PLAN = ROOT / "firmware" / "src" / "ChannelPlan.cpp"

EXIT_OK = 0
EXIT_DRIFT = 3
EXIT_UNDETERMINABLE = 4

FETCH_TIMEOUT = 30


class Undeterminable(RuntimeError):
    """A source could not be fetched or parsed.

    Distinct from "no drift": a monitor that reports agreement it did not actually
    verify is worse than one that admits ignorance.
    """


def fetch(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": "lora-multiboot-drift/1"})
    try:
        with urllib.request.urlopen(req, timeout=FETCH_TIMEOUT) as resp:
            return resp.read().decode("utf-8", errors="replace")
    except (urllib.error.URLError, OSError, ValueError) as exc:
        raise Undeterminable(f"could not fetch {url}: {exc}") from exc


# --- Meshtastic --------------------------------------------------------------

# RDEF(EU_868, 869.4f, 869.65f, 10, 0, 27, false, false, false),
#        ^name    ^start    ^end     ^duty ^pad ^power
#
# Note the spare field between the duty-cycle limit and the power limit. It is
# easy to read the wrong column and quietly conclude power is 0 for every region --
# which is what happened before this was checked against the real source.
_MESHTASTIC_RDEF = re.compile(
    r"RDEF\(\s*([A-Z0-9_]+)\s*,\s*"
    r"([0-9]+(?:\.[0-9]+)?)f\s*,\s*"
    r"([0-9]+(?:\.[0-9]+)?)f\s*,\s*"
    r"([0-9]+(?:\.[0-9]+)?)\s*,\s*"
    r"[0-9]+(?:\.[0-9]+)?\s*,\s*"      # spare field, deliberately unnamed
    r"([0-9]+(?:\.[0-9]+)?)\s*,",
    re.IGNORECASE,
)

# Meshtastic's region names are not ours. Mapped explicitly rather than guessed,
# because a fuzzy match would silently compare the wrong band.
MT_REGION_ALIASES = {
    "EU_868": "EU_868",
    "US": "US_915",
    "ANZ": "ANZ_915",
    "IN": "IN_865",
    "KR": "KR_922",
    "SG": "SG_923",
}


def parse_meshtastic_regions(text: str) -> dict[str, dict]:
    """Extract region band limits and the duty-cycle/power limits beside them.

    Keyed by *our* region code via MT_REGION_ALIASES. Regions we do not track are
    counted and dropped: this project has no US_915 plan worth comparing, and
    reporting 20 unrelated regions as "ADDED" every run would train everyone to
    ignore the output -- including the day it matters.
    """
    raw: dict[str, dict] = {}
    for m in _MESHTASTIC_RDEF.finditer(text):
        name, start, end, duty, power = m.groups()
        raw[name.upper()] = {
            "band_start_mhz": float(start),
            "band_end_mhz": float(end),
            "duty_cycle_percent": int(float(duty)),
            "power_limit_dbm": int(float(power)),
            "verified": True,
        }
    if not raw:
        raise Undeterminable(
            "no RDEF() region definitions found in the Meshtastic source. The layout "
            "of that file has probably changed; the monitor needs updating rather "
            "than reporting a false all-clear.")
    return {MT_REGION_ALIASES[k]: v for k, v in raw.items() if k in MT_REGION_ALIASES}


# --- MeshCore ----------------------------------------------------------------

# "USA/Canada (Recommended) preset is 910.525MHz, SF7, BW62.5, CR5."
_MESHCORE_PRESET = re.compile(
    r"([A-Za-z][A-Za-z0-9 /&()\-,]*?)\s+preset\s+is\s+"
    r"([0-9]+(?:\.[0-9]+)?)\s*MHz\s*,\s*"
    r"SF\s*([0-9]+)\s*,\s*"
    r"BW\s*([0-9]+(?:\.[0-9]+)?)\s*,\s*"
    r"CR\s*([0-9]+)",
    re.IGNORECASE,
)


def parse_meshcore_presets(text: str) -> list[dict]:
    """Extract recommended presets. Order-preserving; region labels are free text."""
    out = []
    for m in _MESHCORE_PRESET.finditer(text):
        region, freq, sf, bw, cr = m.groups()
        out.append({
            "region_text": region.strip(" .,-"),
            "frequency_mhz": float(freq),
            "spreading_factor": int(sf),
            "bandwidth_khz": float(bw),
            "coding_rate_denominator": int(cr),
            "verified": True,
        })
    if not out:
        raise Undeterminable(
            "no recommended presets found in the MeshCore FAQ. The prose has probably "
            "been reworded; the monitor needs updating rather than reporting a false "
            "all-clear.")
    return out


# --- firmware table ----------------------------------------------------------

# RegionDefaults table rows look like:
#   {Region::EU_868, "EU_868", 869.40f, 869.65f, 869.525f, 869.525f, true, 10},
_FW_ROW = re.compile(
    r"\{Region::(\w+),\s*\"(\w+)\",\s*"
    r"([0-9]+(?:\.[0-9]+)?)f,\s*([0-9]+(?:\.[0-9]+)?)f,\s*"
    r"([0-9]+(?:\.[0-9]+)?)f,\s*([0-9]+(?:\.[0-9]+)?)f,\s*"
    r"(true|false),\s*([0-9]+)\s*\}",
)


def parse_firmware_table(text: str) -> dict[str, dict]:
    """Extract the regions table the firmware actually compiles."""
    out: dict[str, dict] = {}
    for m in _FW_ROW.finditer(text):
        (_enum, name, start, end, mt_freq, mc_freq, verified, duty) = m.groups()
        out[name] = {
            "band_start_mhz": float(start),
            "band_end_mhz": float(end),
            "meshtastic_freq_mhz": float(mt_freq),
            "meshcore_freq_mhz": float(mc_freq),
            "meshcore_freq_verified": verified == "true",
            "duty_cycle_percent": int(duty),
        }
    if not out:
        raise Undeterminable(
            "could not find the RegionDefaults table in ChannelPlan.cpp. If that "
            "struct's shape changed, the offline cross-check is silently comparing "
            "nothing, which is the failure this monitor exists to avoid.")
    return out


def close(a: float, b: float, tol: float = 0.001) -> bool:
    return abs(a - b) <= tol


# --- comparison --------------------------------------------------------------

def diff_upstream(expected: dict, observed: dict, keys: list[str], where: str) -> tuple[list[str], list[str]]:
    """Compare only the regions we actually track.

    A region we have never recorded is not drift -- it is a region we have not
    started watching. Reporting it as drift trains people to ignore this output,
    and they will ignore it on the day a real band limit moves.

    Returns (changes, newly_seen).
    """
    changes: list[str] = []
    newly_seen: list[str] = []

    for region in sorted(set(expected) | set(observed)):
        exp = expected.get(region)
        obs = observed.get(region)
        if exp is None:
            newly_seen.append(f"{region} = {_fmt(obs)}")
            continue
        if obs is None:
            changes.append(f"{where}: REMOVED {region} (we track it; upstream no longer "
                           f"defines it)")
            continue
        for key in keys:
            if key not in exp or key not in obs:
                continue
            if isinstance(exp[key], bool) or isinstance(obs[key], bool):
                if exp[key] != obs[key]:
                    changes.append(
                        f"{where}: CHANGED {region}.{key}: {exp[key]!r} -> {obs[key]!r}")
            elif not close(float(exp[key]), float(obs[key])):
                changes.append(
                    f"{where}: CHANGED {region}.{key}: {exp[key]} -> {obs[key]}")
    return changes, newly_seen


def _fmt(d: dict) -> str:
    return ", ".join(f"{k}={v}" for k, v in sorted(d.items()) if k != "note")


def check_offline() -> int:
    """No network: confirm the recorded expectations match what firmware compiles.

    This is the half that must always work, because it needs no internet and it is
    the half that catches an edit to ChannelPlan.cpp that was never reconciled
    with upstream.
    """
    if not EXPECTATIONS.is_file():
        print(f"error: {EXPECTATIONS} is missing", file=sys.stderr)
        return EXIT_UNDETERMINABLE

    data = json.loads(EXPECTATIONS.read_text(encoding="utf-8"))
    try:
        fw = parse_firmware_table(CHANNEL_PLAN.read_text(encoding="utf-8"))
    except Undeterminable as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_UNDETERMINABLE

    changes: list[str] = []
    for region, exp in (data.get("meshtastic") or {}).items():
        row = fw.get(region)
        if row is None:
            changes.append(f"firmware: no region {region} in ChannelPlan.cpp")
            continue
        if "band_start_mhz" in exp and not close(exp["band_start_mhz"], row["band_start_mhz"]):
            changes.append(
                f"firmware: CHANGED {region}.band_start_mhz: "
                f"{exp['band_start_mhz']} -> {row['band_start_mhz']}")
        if "band_end_mhz" in exp and not close(exp["band_end_mhz"], row["band_end_mhz"]):
            changes.append(
                f"firmware: CHANGED {region}.band_end_mhz: "
                f"{exp['band_end_mhz']} -> {row['band_end_mhz']}")
        if "duty_cycle_percent" in exp and exp["duty_cycle_percent"] != row["duty_cycle_percent"]:
            changes.append(
                f"firmware: CHANGED {region}.duty_cycle_percent: "
                f"{exp['duty_cycle_percent']} -> {row['duty_cycle_percent']}")

    # The premise itself, checked locally: do the two still land on one carrier?
    print("shared-carrier premise, as compiled:")
    for region, row in sorted(fw.items()):
        shared = close(row["meshtastic_freq_mhz"], row["meshcore_freq_mhz"])
        mark = "shared" if shared else "MISMATCH"
        extra = "" if row["meshcore_freq_verified"] else "  (MeshCore default unconfirmed)"
        print(f"  {region:<10} {row['meshtastic_freq_mhz']:>9.3f} / "
              f"{row['meshcore_freq_mhz']:>9.3f} MHz  {mark}{extra}")

    if changes:
        print("\nDRIFT between recorded expectations and the firmware table:")
        for c in changes:
            print(f"  {c}")
        return EXIT_DRIFT

    print(f"\nno drift against the recorded expectations (checked {data.get('checked_on')})")
    return EXIT_OK


def check_online(write: bool) -> int:
    """Fetch both upstreams and compare against the recorded expectations."""
    if not EXPECTATIONS.is_file():
        print(f"error: {EXPECTATIONS} is missing", file=sys.stderr)
        return EXIT_UNDETERMINABLE
    data = json.loads(EXPECTATIONS.read_text(encoding="utf-8"))

    undeterminable: list[str] = []
    changes: list[str] = []
    observed_mt: dict = {}
    observed_mc: list = []

    src = data.get("sources", {})

    try:
        text = fetch(src["meshtastic"]["url"])
        observed_mt = parse_meshtastic_regions(text)
        print(f"meshtastic: read {len(observed_mt)} region definitions")
    except (Undeterminable, KeyError) as exc:
        undeterminable.append(f"meshtastic: {exc}")

    try:
        text = fetch(src["meshcore"]["url"])
        observed_mc = parse_meshcore_presets(text)
        print(f"meshcore:   read {len(observed_mc)} recommended presets")
    except (Undeterminable, KeyError) as exc:
        undeterminable.append(f"meshcore: {exc}")

    new_seen: list[str] = []

    if observed_mt:
        changes, new_seen = diff_upstream(
            data.get("meshtastic") or {}, observed_mt,
            ["band_start_mhz", "band_end_mhz", "duty_cycle_percent", "power_limit_dbm"],
            "meshtastic")

    if observed_mc:
        # Region labels in the FAQ are prose, so compare on the values that matter
        # rather than trying to match names we cannot map reliably.
        known = (data.get("meshcore") or {}).get("US_915") or {}
        for p in observed_mc:
            if not close(p["frequency_mhz"], known.get("frequency_mhz", -1.0)):
                changes.append(
                    f"meshcore: CHANGED/ADDED preset {p['region_text']!r}: "
                    f"{p['frequency_mhz']}MHz SF{p['spreading_factor']} "
                    f"BW{p['bandwidth_khz']} CR{p['coding_rate_denominator']}")
                break

    print()
    for u in undeterminable:
        print(f"UNDETERMINABLE  {u}")
    for c in changes:
        print(f"DRIFT  {c}")
    for n in new_seen:
        print(f"note: newly observed, not previously tracked -- {n}")

    if write and observed_mt and not undeterminable:
        today = data.get("checked_on")
        for region, obs in observed_mt.items():
            entry = (data.setdefault("meshtastic", {})).setdefault(region, {})
            entry.update(obs)
        data["checked_on"] = _today()
        EXPECTATIONS.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8",
                                newline="\n")
        print(f"\nrewrote {EXPECTATIONS.name} (was {today})")

    if undeterminable:
        # Never report agreement we did not verify.
        return EXIT_UNDETERMINABLE
    if changes:
        return EXIT_DRIFT
    print("\nno drift")
    return EXIT_OK


def _today() -> str:
    import datetime
    return datetime.date.today().isoformat()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--offline", action="store_true",
                    help="compare the recorded expectations against ChannelPlan.cpp only")
    ap.add_argument("--write", action="store_true",
                    help="record newly observed upstream values")
    args = ap.parse_args()

    if args.offline:
        return check_offline()
    return check_online(args.write)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Undeterminable as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(EXIT_UNDETERMINABLE)
    except KeyboardInterrupt:
        sys.exit(130)