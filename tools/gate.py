#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The gate.

Compiles the portable firmware logic with the same strict flags CI uses, runs its
suites against the shipped partition tables, then runs the flashing tool's own
tests.

Why a Python driver rather than a bare `make`: the compiler is the only thing
this needs, and a C++ compiler is the one dependency we cannot avoid. `make` is
avoidable, and GitHub's Windows and macOS images do not guarantee it, so a
Makefile-only gate fails on the runner for a reason that has nothing to do with
the code. The Makefile still exists and still works; it delegates here.

    python tools/gate.py
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Mirrors CXXFLAGS in the Makefile. -Werror is the point: a warning that would
# once have been a note fails the build instead of quietly accumulating.
STRICT_GCC = [
    "-std=c++17",
    "-O1",
    "-g",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wshadow",
    "-Wconversion",
    "-Wsign-conversion",
    "-Werror",
]

FIRMWARE_SRC = [
    "firmware/src/ProtocolId.cpp",
    "firmware/src/ChannelPlan.cpp",
    "firmware/src/SlotTable.cpp",
    "firmware/src/Provisioning.cpp",
    "firmware/src/SlotLifecycle.cpp",
]

TEST_SRC = [
    "tests/main.cpp",
    "tests/test_protocol_id.cpp",
    "tests/test_channel_plan.cpp",
    "tests/test_slot_table.cpp",
    "tests/test_provisioning.cpp",
    "tests/test_slot_lifecycle.cpp",
    "tests/test_partition_csv.cpp",
]

# MSVC has no -W flags and no notion of -Wconversion, so it gets its own set.
STRICT_MSVC = ["/std:c++17", "/W4", "/WX", "/permissive-", "/EHsc", "/Zi", "/Od"]


class GateError(RuntimeError):
    pass


def find_compiler(explicit: str | None) -> tuple[str, list[str]]:
    """Return (compiler, base_flags). Prefers whatever the caller named."""
    if explicit:
        for cand in (explicit, f"{explicit}.exe"):
            found = shutil.which(cand)
            if found:
                if "cl" in Path(found).name.lower():
                    return found, STRICT_MSVC
                return found, STRICT_GCC
        raise GateError(f"compiler not found: {explicit}")

    # MSVC first when this is a Visual Studio developer shell.
    if os.environ.get("VSCMD_ARG_TGT_ARCH"):
        cl = shutil.which("cl")
        if cl:
            return cl, STRICT_MSVC

    for cand in ("g++", "clang++", "c++"):
        found = shutil.which(cand)
        if found:
            return found, STRICT_GCC
    return "cl", STRICT_MSVC


def compile_and_run(verbose: bool) -> int:
    cxx, base = find_compiler(os.environ.get("CXX"))
    is_msvc = "cl" in Path(cxx).name.lower()

    out_dir = ROOT / "build"
    out_dir.mkdir(exist_ok=True)
    binary = out_dir / ("bridge-tests.exe" if platform.system() == "Windows" else "bridge-tests")

    includes = [f"-I{ROOT / 'firmware' / 'include'}", f"-I{ROOT / 'tests'}"]
    if is_msvc:
        includes = [f"/I{ROOT / 'firmware' / 'include'}", f"/I{ROOT / 'tests'}"]

    cmd = [cxx, *base, *includes]
    # The output has to be named explicitly. Appending the binary path with no
    # -o makes the driver treat it as an object to link, which fails with a
    # baffling "cannot find <the file we just asked it to create>".
    cmd += ["/Fe:" + str(binary)] if is_msvc else ["-o", str(binary)]
    cmd += [str(ROOT / s) for s in FIRMWARE_SRC + TEST_SRC]

    print(f"compiler: {cxx}")
    print("flags:    " + " ".join(base))
    print()

    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise GateError("compilation failed")
    if proc.stderr.strip():
        # With -Werror there should be nothing here, but say so rather than
        # discarding it if a compiler disagrees about a warning.
        sys.stderr.write(proc.stderr)
    if verbose and proc.stdout.strip():
        sys.stdout.write(proc.stdout)

    run = subprocess.run(
        [str(binary), str(ROOT / "firmware" / "partitions")],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    sys.stdout.write(run.stdout)
    sys.stderr.write(run.stderr)
    return run.returncode


def run_python_tests(verbose: bool) -> int:
    print()
    print("== flashing tool ==")
    proc = subprocess.run(
        [sys.executable, "tests/test_flash_tool.py"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    tail = proc.stderr.strip().splitlines()
    for line in tail[-4:]:
        print(line)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
    return proc.returncode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    try:
        cpp = compile_and_run(args.verbose)
    except GateError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    py = run_python_tests(args.verbose)

    print()
    if cpp == 0 and py == 0:
        print("GATE PASS")
        return 0
    print(f"GATE FAIL (firmware logic={cpp}, flashing tool={py})")
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GateError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit(130)
