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
import time
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

# One retry, one pause. Long enough to ride out a rate limit, short enough that a
# genuinely missing source still fails inside the workflow's timeout.
RETRY_BACKOFF_SECONDS = 2.0


class Undeterminable(RuntimeError):
    """A source could not be fetched or parsed.

    Distinct from "no drift": a monitor that reports agreement it did not actually
    verify is worse than one that admits ignorance.
    """


def fetch(url: str, attempts: int = 2) -> str:
    """Fetch a source, retrying once.

    One retry, not several: a rate limit or a DNS hiccup is common enough that a
    single attempt turns a working monitor into one that files an issue every time
    it runs, and a monitor that cries wolf weekly is a monitor people stop
    reading. But hammering a URL that is genuinely gone is pointless, so this is
    a single retry and no more -- after that the honest answer is "cannot see".

    Retrying is only safe because a failed fetch is never recorded as a change: the
    result is either real content or Undeterminable, and Undeterminable never
    rewrites the expectations file.
    """
    last: Exception | None = None
    for attempt in range(max(1, attempts)):
        req = urllib.request.Request(
            url, headers={"User-Agent": "lora-multiboot-drift/1"})
        try:
            with urllib.request.urlopen(req, timeout=FETCH_TIMEOUT) as resp:
                return resp.read().decode("utf-8", errors="replace")
        except (urllib.error.URLError, OSError, ValueError) as exc:
            last = exc
            if attempt + 1 < max(1, attempts):
                time.sleep(RETRY_BACKOFF_SECONDS)
    assert last is not None
    raise Undeterminable(
        f"could not fetch {url} after {attempts} attempts: {last}") from last


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
#
# The region class excludes commas, which is what stops the capture from starting
# at the beginning of the sentence: given "For example, USA/Canada (Recommended)
# preset is ..." the leftmost possible match would otherwise swallow "For example,"
# and the key would be a sentence rather than a region. Commas separate the region
# from everything else in the prose, and a region name does not contain one.
_MESHCORE_PRESET = re.compile(
    r"([A-Za-z][A-Za-z0-9 /&()\-]*?)\s+preset\s+is\s+"
    r"([0-9]+(?:\.[0-9]+)?)\s*MHz\s*,\s*"
    r"SF\s*([0-9]+)\s*,\s*"
    r"BW\s*([0-9]+(?:\.[0-9]+)?)\s*,\s*"
    r"CR\s*([0-9]+)",
    re.IGNORECASE,
)

# Sentence boundaries. Applied before matching so a region capture can never span
# two sentences: the FAQ is documentation prose, and prose runs sentences together
# with a single space.
_SENTENCE = re.compile(r"(?<=[.!?])\s+")


def parse_meshcore_presets(text: str) -> list[dict]:
    """Extract recommended presets. Order-preserving; region labels are free text.

    Matched sentence by sentence rather than across the whole document: prose
    concatenates sentences with a single space, so a whole-document match can
    happily capture the end of one sentence and the start of the next as one
    region name.
    """
    out = []
    for sentence in _SENTENCE.split(text):
        for m in _MESHCORE_PRESET.finditer(sentence):
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

def diff_meshcore(expected: dict, observed: list[dict]) -> tuple[list[str], list[str]]:
    """Compare the FAQ's recommended presets, keyed by the region text it gives.

    The FAQ is prose, so the region name is whatever the sentence said -- which is
    why the key is the prose text rather than a region code. That makes the
    comparison stable as long as the sentence is, and an upstream rewrite of the
    wording shows up as a removal plus an addition rather than silently comparing
    two different things.

    Follows diff_upstream's rule: a preset we never recorded is not drift, it is a
    preset we have not started watching.
    """
    changes: list[str] = []
    newly_seen: list[str] = []

    obs_by_key: dict[str, dict] = {}
    for p in observed:
        obs_by_key[p["region_text"]] = p

    for key in sorted(set(expected) | set(obs_by_key)):
        # Keys starting with "_" are this file's own notes to a human. They are not
        # presets, and treating one as a removed preset would manufacture drift out
        # of an explanatory comment.
        if key.startswith("_"):
            continue
        exp = expected.get(key)
        obs = obs_by_key.get(key)
        if exp is None:
            newly_seen.append(f"{key} = {_fmt_mc(obs)}")
            continue
        if obs is None:
            changes.append(f"meshcore: REMOVED preset {key!r} (we track it; the FAQ "
                           f"no longer recommends one)")
            continue
        for field in ("frequency_mhz", "spreading_factor", "bandwidth_khz",
                      "coding_rate_denominator"):
            if field not in exp:
                continue
            old = float(exp[field])
            new = float(obs[field])
            if not close(old, new):
                changes.append(
                    f"meshcore: CHANGED {key!r}.{field}: {exp[field]} -> {obs[field]}")
    return changes, newly_seen


def _fmt_mc(p: dict) -> str:
    return (f"{p['frequency_mhz']}MHz SF{p['spreading_factor']} "
            f"BW{p['bandwidth_khz']} CR{p['coding_rate_denominator']}")


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
        # See diff_meshcore: "_"-prefixed keys are notes to a human, not data.
        if region.startswith("_"):
            continue
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
        if region.startswith("_"):
            continue
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


def record_observations(observed_mt: dict, observed_mc: list[dict],
                       undeterminable: list[str]) -> bool:
    """Rewrite the expectations file with what was actually read. Returns True if written.

    Split out from check_online so it can be tested without a network, and so the
    rules below are stated once rather than being implied by a branch somewhere
    inside the fetch logic.
    """
    data = json.loads(EXPECTATIONS.read_text(encoding="utf-8"))
    today = data.get("checked_on")
    wrote = False

    if observed_mt:
        for region, obs in observed_mt.items():
            entry = (data.setdefault("meshtastic", {})).setdefault(region, {})
            entry.update(obs)
        wrote = True

    if observed_mc:
        for p in observed_mc:
            entry = (data.setdefault("meshcore", {})).setdefault(p["region_text"], {})
            entry.update({
                "frequency_mhz": p["frequency_mhz"],
                "spreading_factor": p["spreading_factor"],
                "bandwidth_khz": p["bandwidth_khz"],
                "coding_rate_denominator": p["coding_rate_denominator"],
                "verified": bool(p.get("verified", True)),
            })
        wrote = True

    # Only rewrite a source that was actually read. A monitor that records a parse
    # failure as a change would make its own broken parser look like upstream
    # drift, and the resulting version bump would be a lie.
    if undeterminable:
        print("\nnot rewriting: a source could not be read, and recording "
              "nothing for it would look like agreement")
        return False
    if not wrote:
        print("\nnot rewriting: neither source produced anything to record")
        return False

    data["checked_on"] = _today()

    # Written atomically. This file is committed and the offline gate compares
    # against it, so a run interrupted between truncate and write -- a cancelled
    # job, a full disk, a killed process -- would otherwise leave invalid JSON that
    # reads as upstream drift on the next run and as a broken gate in CI. Writing a
    # sibling and renaming means the file is either the old one or the new one.
    body = json.dumps(data, indent=2, ensure_ascii=False) + "\n"
    tmp = EXPECTATIONS.with_name(EXPECTATIONS.name + ".tmp")
    try:
        tmp.write_text(body, encoding="utf-8", newline="\n")
        tmp.replace(EXPECTATIONS)
    except OSError:
        # Leave no half-written file behind for the next run to trip over.
        try:
            tmp.unlink()
        except OSError:
            pass
        raise
    print(f"\nrewrote {EXPECTATIONS.name} (was {today})")
    return True


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
        # Keyed by region text rather than compared as a set, so that a preset
        # whose values changed and a preset that is entirely new are both visible,
        # and neither is confused with a region that merely reads differently.
        mc_changes, mc_new = diff_meshcore(data.get("meshcore") or {}, observed_mc)
        changes.extend(mc_changes)
        new_seen.extend(mc_new)

    print()
    for u in undeterminable:
        print(f"UNDETERMINABLE  {u}")
    for c in changes:
        print(f"DRIFT  {c}")
    for n in new_seen:
        print(f"note: newly observed, not previously tracked -- {n}")

    if write:
        record_observations(observed_mt, observed_mc, undeterminable)

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