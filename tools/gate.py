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
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Totals observed by the last run, filled in by each half of the gate and then
# checked against the figures the documentation quotes.
COUNT_CPP = 0
COUNT_PY = 0

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
    "firmware/src/Airtime.cpp",
    "firmware/src/RadioPlan.cpp",
    "firmware/src/Statistics.cpp",
    "firmware/src/StatusPanel.cpp",
    "firmware/src/Roles.cpp",
    "firmware/src/SystemUpdate.cpp",
    "firmware/src/RadioProfiles.cpp",
    "firmware/src/Inventory.cpp",
    "firmware/src/SlotTable.cpp",
    "firmware/src/Provisioning.cpp",
    "firmware/src/SlotLifecycle.cpp",
]

TEST_SRC = [
    "tests/main.cpp",
    "tests/test_protocol_id.cpp",
    "tests/test_channel_plan.cpp",
    "tests/test_airtime.cpp",
    "tests/test_radio_stats.cpp",
    "tests/test_roles_system.cpp",
    "tests/test_shared_context.cpp",
    "tests/test_reclaim_profiles.cpp",
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

    # Falling through to a bare "cl" here would surface as a wall of linker output
    # about a program that does not exist, which tells the reader nothing. Say what
    # is actually wrong and what would fix it.
    raise GateError(
        "no C++ compiler found. The gate needs one and nothing else.\n"
        "  Debian/Ubuntu : apt-get install g++\n"
        "  Fedora/RHEL    : dnf install gcc-c++\n"
        "  macOS          : xcode-select --install\n"
        "  Windows        : install MSVC Build Tools, or MSYS2 (pacman -S mingw-w64-x86_64-gcc)\n"
        "  or point at one explicitly:  set CXX=/path/to/g++"
    )


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

    global COUNT_CPP
    m = re.search(r"(\d+) checks, (\d+) failed", run.stdout)
    if m:
        COUNT_CPP = int(m.group(1))
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

    global COUNT_PY
    m = re.search(r"Ran (\d+) tests?", proc.stderr)
    if m:
        COUNT_PY = int(m.group(1))
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
    return proc.returncode


def compile_firmware_entry(verbose: bool) -> int:
    """Type-check firmware/src/main.cpp, the one file the gate used to skip.

    main.cpp includes Arduino.h, which is why it was never in the host gate -- and
    that made it the single file in the project nobody ever compiled. It is the file
    a user flashes first, so a typo in it should not need a board to discover.

    A tiny Arduino shim provides Print and the radix macros. This is a type check,
    not an emulator: nothing is executed, and the point is only that the entry point
    is subject to the same -Werror strictness as everything else. Compiled to an
    object, deliberately not linked -- there is no main() to link against.
    """
    cxx, base = find_compiler(os.environ.get("CXX"))
    is_msvc = "cl" in Path(cxx).name.lower()

    out_dir = ROOT / "build"
    out_dir.mkdir(exist_ok=True)
    obj = out_dir / ("main.o" if not is_msvc else "main.obj")

    shim = ROOT / "tests" / "arduino_shim"
    includes = [f"-I{shim}", f"-I{ROOT / 'firmware' / 'include'}"]
    if is_msvc:
        includes = [f"/I{shim}", f"/I{ROOT / 'firmware' / 'include'}"]

    # No -Werror here beyond what the base flags carry: main.cpp is held to exactly
    # the same standard as the portable modules, which is the entire point.
    cmd = [cxx, *base, *includes, "-c", str(ROOT / "firmware" / "src" / "main.cpp")]
    cmd += [f"/Fo{obj}"] if is_msvc else ["-o", str(obj)]

    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        return 1
    if proc.stderr.strip():
        sys.stderr.write(proc.stderr)
    return 0


def check_documented_counts(cpp_checks: int, py_tests: int) -> int:
    """The README and docs quote the gate's own totals. Keep them honest.

    Stale numbers in a README are the kind of thing nobody notices until it is the
    only thing wrong with the project. Both figures are quoted in prose, so rather
    than hand-maintain them, check them: if a document claims a total, it has to be
    the real one.

    A document that quotes nothing is left alone -- this asserts accuracy, not the
    presence of a boast.
    """
    pattern = re.compile(
        r"([\d,]+)\s+assertions?\s+and\s+(\d+)\s+tool\s+tests", re.IGNORECASE)

    problems: list[str] = []
    for name in ("README.md", *sorted(str(p.relative_to(ROOT))
                                      for p in (ROOT / "docs").glob("*.md"))):
        path = ROOT / name
        if not path.is_file():
            continue
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            for m in pattern.finditer(line):
                claimed_cpp = int(m.group(1).replace(",", ""))
                claimed_py = int(m.group(2))
                if claimed_cpp != cpp_checks or claimed_py != py_tests:
                    problems.append(
                        f"{name}:{lineno}: says {claimed_cpp} assertions / "
                        f"{claimed_py} tool tests, actual is {cpp_checks} / {py_tests}")

    if problems:
        print("\n== documented totals are stale ==", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        print("\n  update the quoted totals, or the gate will keep failing here.",
              file=sys.stderr)
        return 1
    return 0


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

    entry = 0
    print()
    print("== firmware entry point (type check) ==")
    entry = compile_firmware_entry(args.verbose)
    if entry != 0:
        print("main.cpp did not compile", file=sys.stderr)
    else:
        print("main.cpp compiles clean under -Werror")

    # Only meaningful when both halves passed; otherwise the numbers are not real.
    counts_ok = 0
    if cpp == 0 and py == 0:
        counts_ok = check_documented_counts(COUNT_CPP, COUNT_PY)

    print()
    if cpp == 0 and py == 0 and entry == 0 and counts_ok == 0:
        print("GATE PASS")
        return 0
    print(f"GATE FAIL (firmware logic={cpp}, flashing tool={py}, "
          f"entry point={entry}, docs={counts_ok})")
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GateError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit(130)
