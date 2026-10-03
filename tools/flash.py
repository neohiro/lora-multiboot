#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Flash a Heltec V4 with a shared-radio bridge image.

Exists so the first thing a new user does is not a hand-typed esptool command
with six hex offsets, where one wrong digit writes a partition table over a
firmware and the board stops booting.

Refuses to flash a partition table that does not validate, and refuses a layout
that does not match the flash the chip actually reports.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PARTITION_DIR = ROOT / "firmware" / "partitions"


class FlashError(RuntimeError):
    pass


@dataclass
class Part:
    label: str
    ptype: str
    subtype: str
    offset: int
    size: int
    blank: bool


def parse_size(text: str) -> int:
    """Parse 0x10000, 4K, 1M, 4096. Mirrors parseSize() in SlotTable.cpp."""
    t = text.strip()
    if not t:
        raise FlashError("empty size")
    mult = 1
    if t[-1] in "Kk":
        mult, t = 1024, t[:-1]
    elif t[-1] in "Mm":
        mult, t = 1024 * 1024, t[:-1]
    t = t.strip()
    base = 16 if t[:2].lower() == "0x" else 10
    try:
        return int(t, base) * mult
    except ValueError as exc:
        raise FlashError(f"cannot parse size {text!r}") from exc


def parse_csv(path: Path) -> list[Part]:
    parts: list[Part] = []
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        f = [x.strip() for x in line.split(",")]
        if len(f) < 5:
            raise FlashError(f"{path.name}:{lineno}: fewer than five fields")
        blank = f[4] == ""
        try:
            offset = parse_size(f[3])
            size = 0 if blank else parse_size(f[4])
        except FlashError as exc:
            raise FlashError(f"{path.name}:{lineno}: {exc}") from exc
        parts.append(Part(f[0], f[1], f[2], offset, size, blank))
    if not parts:
        raise FlashError(f"{path.name}: no partitions")
    return parts


def validate(parts: list[Part], flash_size: int) -> None:
    """The same geometry rules SlotTable::validate() enforces, in Python.

    Duplicated on purpose: the flashing tool has to be able to refuse a bad table
    on a machine with no C++ compiler, which is exactly the machine a new user
    is on. The two implementations are cross-checked by the host test suite.
    """
    for p in parts:
        if p.blank:
            continue
        if p.offset + p.size > flash_size:
            raise FlashError(
                f"{p.label} ends at 0x{p.offset + p.size:X}, past the "
                f"{flash_size // (1024 * 1024)}MB this board reports"
            )
        if p.ptype == "app" and p.size and p.offset % 0x10000:
            raise FlashError(
                f"{p.label} is at 0x{p.offset:X}, not 64KB aligned. The bootloader "
                "will refuse this and the board will look simply dead."
            )

    ordered = sorted(parts, key=lambda p: p.offset)
    for prev, cur in zip(ordered, ordered[1:]):
        if prev.blank:
            continue
        if cur.offset < prev.offset + prev.size:
            raise FlashError(
                f"{cur.label} at 0x{cur.offset:X} overlaps {prev.label} "
                f"(0x{prev.offset:X}+0x{prev.size:X})"
            )

    labels = [p.label for p in parts]
    if len(labels) != len(set(labels)):
        raise FlashError("duplicate partition labels")

    # The filesystem pairing. Distinct offsets are already guaranteed by the
    # overlap check above, so re-testing that here would be decoration. What is
    # NOT covered by geometry is the pairing itself: Meshtastic mounts LittleFS
    # and MeshCore mounts SPIFFS, and a future edit that hands either side the
    # other's filesystem makes it format the wrong one on boot. That is the
    # mistake worth catching, so that is what is checked.
    expected_fs = {"fs_meshcore": "spiffs", "fs_meshtastic": "littlefs"}
    for label, want in expected_fs.items():
        if label not in labels:
            continue
        part = next(p for p in parts if p.label == label)
        if part.subtype != want:
            raise FlashError(
                f"{label} is declared {part.subtype!r}, expected {want!r}. "
                "Meshtastic mounts LittleFS and MeshCore mounts SPIFFS; handing "
                "either side the other's filesystem makes it format the wrong one "
                "on boot and lose the settings on the other side of the pair."
            )


def find_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    if sys.platform.startswith("win"):
        found = _windows_serial_ports()
    else:
        found = [
            f"/dev/{n}"
            for n in ("ttyUSB0", "ttyACM0", "tty.usbserial-0001")
            if Path(f"/dev/{n}").exists()
        ]
    if not found:
        raise FlashError(
            "no serial port found. Pass --port explicitly, or check the cable: "
            "many USB-C cables carry power and no data."
        )
    return found[0]


def _windows_serial_ports() -> list[str]:
    try:
        import serial.tools.list_ports  # type: ignore
    except ImportError:
        return []
    return [p.device for p in serial.tools.list_ports.comports()]


def chip_flash_size(port: str) -> int:
    """Ask the chip how big it is instead of assuming.

    The V4 ships 16MB of external flash, but a V3 does not, and flashing a
    five-slot table onto an 8MB board fails in a way that looks like a bad
    download rather than a wrong assumption.
    """
    if shutil.which("esptool") is None and shutil.which("esptool.py") is None:
        raise FlashError(
            "esptool not found. Install it with: pip install esptool"
        )
    tool = shutil.which("esptool") or shutil.which("esptool.py")
    proc = subprocess.run(
        [tool, "--chip", "esp32s3", "--port", port, "flash_id"],
        capture_output=True,
        text=True,
        timeout=60,
    )
    blob = proc.stdout + proc.stderr
    m = re.search(r"Detected flash size:\s*(\d+)MB", blob) or re.search(
        r"flash size:\s*(\d+)MB", blob
    )
    if not m:
        raise FlashError(
            "could not read the flash size from the chip. Is it in bootloader "
            "mode?\n" + blob.strip()
        )
    return int(m.group(1)) * 1024 * 1024


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--table",
        default="quadboot",
        choices=["quadboot", "dualboot"],
        help="partition layout to flash (default: quadboot, five slots)",
    )
    ap.add_argument("--bootloader", type=Path, required=True)
    ap.add_argument("--part-table-bin", type=Path, required=True)
    ap.add_argument("--otadata", type=Path, required=True)
    ap.add_argument(
        "--app",
        type=Path,
        action="append",
        default=[],
        metavar="LABEL=PATH",
        help="application image for a slot, e.g. meshcore=.pio/build/.../firmware.bin",
    )
    ap.add_argument("--port")
    ap.add_argument("--baud", default="460800")
    ap.add_argument("--dry-run", action="store_true", help="validate and print, write nothing")
    args = ap.parse_args()

    csv_path = PARTITION_DIR / f"{args.table}.csv"
    if not csv_path.is_file():
        raise FlashError(f"no such partition table: {csv_path}")

    try:
        parts = parse_csv(csv_path)
    except FlashError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print(f"partition table: {csv_path.name}  ({len(parts)} partitions)")

    port = args.port
    if not args.dry_run and not port:
        port = find_port(None)

    flash_size = chip_flash_size(port) if port and not args.dry_run else 16 * 1024 * 1024
    print(f"flash size:      {flash_size // (1024 * 1024)}MB")

    try:
        validate(parts, flash_size)
    except FlashError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print("layout:          valid")

    by_label = {p.label: p for p in parts}
    args_map: dict[str, Path] = {}
    for item in args.app:
        if "=" not in item:
            raise FlashError(f"--app expects LABEL=PATH, got {item!r}")
        label, path = item.split("=", 1)
        if label not in by_label:
            raise FlashError(
                f"no partition labelled {label!r} in {csv_path.name}. "
                f"It has: {', '.join(sorted(by_label))}"
            )
        args_map[label] = Path(path)

    for required in ("bootloader", "partition_tbl"):
        if required not in by_label:
            raise FlashError(f"{csv_path.name} has no {required} row")

    plan: list[tuple[int, Path]] = [
        (by_label["bootloader"].offset, args.bootloader),
        (by_label["partition_tbl"].offset, args.part_table_bin),
    ]
    if "otadata" in by_label:
        plan.append((by_label["otadata"].offset, args.otadata))
    for label, path in sorted(args_map.items()):
        part = by_label[label]
        if part.size and path.stat().st_size > part.size:
            raise FlashError(
                f"{path.name} is {path.stat().st_size} bytes but {label} only has "
                f"{part.size}. Shrink the image or pick a bigger slot."
            )
        plan.append((part.offset, path))

    for offset, path in plan:
        if not path.is_file():
            raise FlashError(f"missing file: {path}")

    print("\nwrite plan:")
    for offset, path in plan:
        print(f"  0x{offset:06X}  {path}")

    if not args_map:
        print(
            "\nnote: no --app given, so only the bootloader, partition table and "
            "otadata\n      will be written. Pass --app meshcore=... --app "
            "meshtastic=...\n      to fill the slots."
        )

    if args.dry_run:
        print("\ndry run: nothing written.")
        return 0

    if shutil.which("esptool") is None and shutil.which("esptool.py") is None:
        print("error: esptool not found. Install it with: pip install esptool", file=sys.stderr)
        return 2
    tool = shutil.which("esptool") or shutil.which("esptool.py")

    cmd = [
        tool,
        "--chip", "esp32s3",
        "--port", port,
        "--baud", args.baud,
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", f"{flash_size // (1024 * 1024)}MB",
        "write_flash",
        "-z",
    ]
    for offset, path in plan:
        cmd += [f"0x{offset:X}", str(path)]

    print("\nrunning esptool...")
    return subprocess.run(cmd).returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except FlashError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit(130)
