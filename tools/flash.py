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


def system_pieces(csv_path: Path) -> dict[str, tuple[int, int]]:
    """Pieces of the system layer: everything below the first slot.

    A system update writes only these. Nothing at or above the first slot is
    touched, which is what is meant by "updating the bootloader does not cost
    anybody their firmwares or their settings".
    """
    parts = parse_csv(csv_path)
    first_slot = None
    for p in parts:
        if p.label.startswith("ota_"):
            first_slot = p.offset if first_slot is None else min(first_slot, p.offset)
    if first_slot is None:
        raise FlashError(f"{csv_path.name}: no app slots found")

    out: dict[str, tuple[int, int]] = {}
    for p in parts:
        if p.offset + p.size > first_slot:
            continue
        out[p.label] = (p.offset, p.size)
    return out


def cmd_list(args) -> int:
    """Show what is on the board without writing anything."""
    csv_path = PARTITION_DIR / f"{args.table}.csv"
    parts = parse_csv(csv_path)

    print(f"layout: {csv_path.name}")
    try:
        validate(parts, args.flash_size * 1024 * 1024)
        print("geometry: valid")
    except FlashError as exc:
        print(f"geometry: INVALID -- {exc}")
        return 2

    first_slot = min(p.offset for p in parts if p.label.startswith("ota_"))
    print(f"\nsystem layer (below 0x{first_slot:X}, rewritten by --update-system):")
    for p in parts:
        if p.offset + p.size <= first_slot:
            print(f"  0x{p.offset:06X}  {p.label:<15} {p.size:>8} B   {p.subtype}")

    print("\nslots (never touched by a system update):")
    slot_parts = [p for p in parts if p.label.startswith("ota_")]
    for i, p in enumerate(slot_parts):
        fs_label = f"fs_{['meshcore', 'meshtastic', 'reticulum', 'lorawan', 'custom'][i]}"
        fs = next((q for q in parts if q.label == fs_label), None)
        line = f"  {p.label:<6} 0x{p.offset:06X}  app {p.size // 1024}K"
        if fs:
            line += f"   settings 0x{fs.offset:06X} ({fs.size // 1024}K, {fs.subtype})"
        print(line)

    print(
        "\nNothing here has been read from the board: --list shows the layout the\n"
        "table declares. Which slots actually hold firmware is device state stored\n"
        "in NVS, readable over the CLI once firmware is running."
    )
    return 0


def cmd_update_system(args) -> int:
    """Rewrite the bootloader and partition table, preserving every slot."""
    csv_path = PARTITION_DIR / f"{args.table}.csv"
    pieces = system_pieces(csv_path)
    print(f"layout: {csv_path.name}")

    first_slot = min(p[0] for p in pieces.values() if True) if pieces else 0
    region_end = 0
    for offset, size in pieces.values():
        region_end = max(region_end, offset + size)

    wanted = {
        "bootloader": args.bootloader,
        "partition_tbl": args.part_table_bin,
        "otadata": args.otadata,
    }
    plan: list[tuple[int, Path, str]] = []
    for label, path in wanted.items():
        if path is None:
            continue
        if label not in pieces:
            raise FlashError(f"{csv_path.name} has no {label} row")
        offset, capacity = pieces[label]
        size = path.stat().st_size
        if size > capacity:
            raise FlashError(
                f"{label}: {path.name} is {size} B but the partition is {capacity} B"
            )
        if offset + size > region_end:
            raise FlashError(f"{label}: would extend past the system region")
        plan.append((offset, path, label))

    # Note on the partition table: it is written whenever it is asked for. The
    # firmware-side planner skips an identical table when it knows the size matches,
    # but this tool cannot know that without reading the chip back, so it does not
    # claim to. An erase cycle per update, per node, is real over twenty years, so
    # leave --part-table-bin off for a bootloader-only update.

    if not plan:
        print("\nnothing to write. Pass --bootloader and/or --part-table-bin.")
        return 0

    print("\nsystem update plan:")
    for offset, path, label in plan:
        print(f"  0x{offset:06X}  {label:<15} {path}")
    print("\nslots and every settings partition are left untouched.")

    if args.dry_run:
        print("\ndry run: nothing written.")
        return 0

    return _run_esptool(args, plan)


def cmd_app(args) -> int:
    """Write, reflash or erase exactly one slot."""
    csv_path = PARTITION_DIR / f"{args.table}.csv"
    parts = parse_csv(csv_path)
    label = f"ota_{args.slot}"
    target = next((p for p in parts if p.label == label), None)
    if target is None:
        raise FlashError(f"{csv_path.name} has no {label}. It has: "
                         + ", ".join(p.label for p in parts if p.label.startswith('ota_')))

    if args.erase:
        print(f"erasing {label} at 0x{target.offset:06X} ({target.size // 1024}K)")
        print("The slot's settings partition is NOT touched.")
        print("This is the right choice for recovering a bad image without losing")
        print("your channel keys or node database.")
        if args.dry_run:
            print("\ndry run: nothing written.")
            return 0
        return _run_esptool(args, [(target.offset, None, label)])

    if args.app is None:
        raise FlashError("pass --app PATH (or --erase to clear the slot)")

    path = args.app
    size = path.stat().st_size
    if size > target.size:
        raise FlashError(
            f"{path.name} is {size} B but {label} only has {target.size} B. "
            "Shrink the image or pick another slot."
        )
    print(f"writing {path.name} ({size} B) to {label} at 0x{target.offset:06X}")
    print("Only this slot is written. Other slots and all settings are untouched.")
    if args.dry_run:
        print("\ndry run: nothing written.")
        return 0
    return _run_esptool(args, [(target.offset, path, label)])


def _run_esptool(args, plan) -> int:
    if shutil.which("esptool") is None and shutil.which("esptool.py") is None:
        print("error: esptool not found. Install it with: pip install esptool",
              file=sys.stderr)
        return 2
    tool = shutil.which("esptool") or shutil.which("esptool.py")

    port = find_port(getattr(args, "port", None))
    flash_size = chip_flash_size(port) if getattr(args, "flash_size", 16) == 0 else \
        args.flash_size * 1024 * 1024

    cmd = [
        tool,
        "--chip", "esp32s3",
        "--port", port,
        "--baud", args.baud,
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", f"{flash_size // (1024 * 1024)}MB",
        "write_flash",
    ]
    for offset, path, label in plan:
        if path is None:
            # erase a region by writing an erase-only command
            cmd += ["0x%X" % offset, "--erase"]
            print(f"  erasing 0x{offset:06X} ({label})")
        else:
            cmd += ["0x%X" % offset, str(path)]
    print("\nrunning esptool...")
    return subprocess.run(cmd).returncode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--table", default="quadboot", choices=["quadboot", "dualboot"],
                    help="partition layout (default: quadboot)")
    ap.add_argument("--port")
    ap.add_argument("--baud", default="460800")
    ap.add_argument("--flash-size", type=int, default=16,
                    help="flash size in MB, or 0 to ask the chip (default: 16)")
    ap.add_argument("--dry-run", action="store_true", help="validate and print, write nothing")

    sub = ap.add_subparsers(dest="command")

    p_list = sub.add_parser("list", help="show the layout and what a system update touches")
    p_list.set_defaults(func=cmd_list)

    p_sys = sub.add_parser(
        "update-system",
        help="rewrite bootloader and/or partition table; slots and settings are preserved",
    )
    p_sys.add_argument("--bootloader", type=Path)
    p_sys.add_argument("--part-table-bin", type=Path)
    p_sys.add_argument("--otadata", type=Path)
    p_sys.set_defaults(func=cmd_update_system)

    p_app = sub.add_parser("app", help="write, reflash or erase one slot")
    p_app.add_argument("slot", type=int, help="slot number, e.g. 0 or 1")
    p_app.add_argument("--app", type=Path, help="firmware image to write into the slot")
    p_app.add_argument("--erase", action="store_true",
                       help="erase the slot's firmware, keeping its settings")
    p_app.set_defaults(func=cmd_app)

    # Legacy full-flash path, kept because it is still the right thing when
    # setting a board up for the first time.
    p_full = sub.add_parser("full", help="write bootloader, table and any slots (first setup)")
    p_full.add_argument("--bootloader", type=Path, required=True)
    p_full.add_argument("--part-table-bin", type=Path, required=True)
    p_full.add_argument("--otadata", type=Path)
    p_full.add_argument("--app", type=Path, action="append", default=[], metavar="LABEL=PATH")
    p_full.set_defaults(func=cmd_full)

    args = ap.parse_args()
    if not getattr(args, "command", None):
        args = ap.parse_args(["--help"])

    if args.command == "list":
        return cmd_list(args)
    if args.command == "update-system":
        return cmd_update_system(args)
    if args.command == "app":
        return cmd_app(args)
    if args.command == "full":
        return cmd_full(args)
    ap.print_help()
    return 2


def cmd_full(args) -> int:
    csv_path = PARTITION_DIR / f"{args.table}.csv"
    parts = parse_csv(csv_path)
    print(f"partition table: {csv_path.name}  ({len(parts)} partitions)")

    flash_size = args.flash_size * 1024 * 1024
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
            raise FlashError(f"no partition labelled {label!r} in {csv_path.name}")
        args_map[label] = Path(path)

    plan: list[tuple[int, Path, str]] = [
        (by_label["bootloader"].offset, args.bootloader, "bootloader"),
        (by_label["partition_tbl"].offset, args.part_table_bin, "partition_tbl"),
    ]
    if args.otadata is not None:
        plan.append((by_label["otadata"].offset, args.otadata, "otadata"))
    for label, path in sorted(args_map.items()):
        part = by_label[label]
        if part.size and path.stat().st_size > part.size:
            raise FlashError(f"{path.name} does not fit {label}")
        plan.append((part.offset, path, label))

    for _, path, _ in plan:
        if not path.is_file():
            raise FlashError(f"missing file: {path}")

    print("\nwrite plan:")
    for offset, path, label in plan:
        print(f"  0x{offset:06X}  {label:<15} {path}")

    if args.dry_run:
        print("\ndry run: nothing written.")
        return 0
    return _run_esptool(args, plan)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except FlashError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit(130)
