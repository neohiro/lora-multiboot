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
from typing import NamedTuple

ROOT = Path(__file__).resolve().parent.parent
PARTITION_DIR = ROOT / "firmware" / "partitions"
PROVISIONING = ROOT / "firmware" / "include" / "bridge" / "Provisioning.hpp"

# The two pieces that exist outside the partition table.
#
# Read from Provisioning.hpp rather than written here, because these numbers are
# what an update writes to a real board and a flasher carrying its own copy of the
# geometry is a flasher that will one day write a 28 KB bootloader into a region
# that is not 28 KB. The header is the single owner; this follows it.
_CONST_RE = re.compile(r"constexpr\s+std::uint32_t\s+(k\w+)\s*=\s*([^;]+);")

# Safe arithmetic evaluator: handles +, -, *, /, parentheses, hex/decimal literals,
# and references to previously defined constants. No eval(), no security risk.
def _eval_arithmetic(expr: str, constants: dict[str, int]) -> int:
    """Evaluate a simple arithmetic expression using only the provided constants.

    Supported: +, -, *, // (integer division), parentheses, hex (0x...), decimal.
    All operations are integer. Whitespace is ignored.
    """
    # Tokenize
    tokens = []
    i = 0
    while i < len(expr):
        c = expr[i]
        if c.isspace():
            i += 1
            continue
        if c in "+-*/()":
            # Handle // for integer division
            if c == "/" and i + 1 < len(expr) and expr[i + 1] == "/":
                tokens.append("//")
                i += 2
            else:
                tokens.append(c)
                i += 1
        elif c.isdigit() or (c == "0" and i + 1 < len(expr) and expr[i + 1] in "xX"):
            # Number literal (hex or decimal)
            j = i
            if expr[j] == "0" and j + 1 < len(expr) and expr[j + 1] in "xX":
                j += 2
                while j < len(expr) and expr[j] in "0123456789abcdefABCDEF":
                    j += 1
            else:
                while j < len(expr) and expr[j].isdigit():
                    j += 1
            tokens.append(int(expr[i:j], 0))
            i = j
        elif c.isalpha() or c == "_":
            # Identifier (constant reference)
            j = i
            while j < len(expr) and (expr[j].isalnum() or expr[j] == "_"):
                j += 1
            ident = expr[i:j]
            if ident not in constants:
                raise FlashError(f"undefined constant {ident!r} in expression {expr!r}")
            tokens.append(constants[ident])
            i = j
        else:
            raise FlashError(f"invalid character {c!r} in expression {expr!r}")
    
    # Shunting-yard algorithm to convert to RPN, then evaluate
    # Simplified: only supports the operators we need, all left-associative
    prec = {"+": 1, "-": 1, "*": 2, "//": 2}
    output = []
    ops = []
    
    for tok in tokens:
        if isinstance(tok, int):
            output.append(tok)
        elif tok in prec:
            while ops and ops[-1] in prec and prec[ops[-1]] >= prec[tok]:
                output.append(ops.pop())
            ops.append(tok)
        elif tok == "(":
            ops.append(tok)
        elif tok == ")":
            while ops and ops[-1] != "(":
                output.append(ops.pop())
            if not ops:
                raise FlashError(f"mismatched parentheses in {expr!r}")
            ops.pop()  # pop '('
    
    while ops:
        if ops[-1] in "()":
            raise FlashError(f"mismatched parentheses in {expr!r}")
        output.append(ops.pop())
    
    # Evaluate RPN
    stack = []
    for tok in output:
        if isinstance(tok, int):
            stack.append(tok)
        else:
            if len(stack) < 2:
                raise FlashError(f"malformed expression {expr!r}")
            b = stack.pop()
            a = stack.pop()
            if tok == "+":
                stack.append(a + b)
            elif tok == "-":
                stack.append(a - b)
            elif tok == "*":
                stack.append(a * b)
            elif tok == "//":
                if b == 0:
                    raise FlashError(f"division by zero in {expr!r}")
                stack.append(a // b)
    
    if len(stack) != 1:
        raise FlashError(f"malformed expression {expr!r}")
    return stack[0]


def _geometry_constants() -> dict[str, int]:
    """Resolve the uint32 geometry constants from Provisioning.hpp.

    Iterative rather than recursive so a constant that refers to another declared
    above it resolves, and a hypothetical cycle ends as a KeyError here rather than
    a RecursionError in somebody's release.
    """
    pending = [
        (m.group(1), m.group(2).split("//")[0].strip())
        for m in _CONST_RE.finditer(PROVISIONING.read_text(encoding="utf-8"))
    ]
    out: dict[str, int] = {}
    for _ in range(len(pending) + 1):
        if not pending:
            break
        still = []
        for name, expr in pending:
            # Hex literals are stripped before looking for names: "0x0" contains the
            # identifier-shaped text "x0", which would look like an undeclared name.
            residual = re.sub(r"0[xX][0-9a-fA-F]+|\b\d+\b", " ", expr)
            if not set(re.findall(r"[A-Za-z_]\w*", residual)) <= set(out):
                still.append((name, expr))
                continue
            out[name] = _eval_arithmetic(expr, out)
        if len(still) == len(pending):
            break
        pending = still
    return out


def read_geometry_constants() -> dict[str, int]:
    """_geometry_constants(), but a missing or unreadable header is a FlashError.

    A traceback out of a flashing tool reads like a bug in the tool. It is not: it
    is the tool declining to write to a board with no geometry, which is the one
    situation where the message must be plain and the exit code must be non-zero.
    """
    try:
        return _geometry_constants()
    except OSError as exc:
        raise FlashError(
            f"cannot read {PROVISIONING}, which owns the flash geometry: {exc}"
        ) from exc


def require_file(path: Path, what: str) -> int:
    """Confirm an input file is usable, and return its size.

    Checked before anything is erased, not after. This tool erases a region and
    then writes it; an esptool failure on the write leaves the region erased, so a
    path that does not exist must be discovered while the board is still untouched.

    Every way of being unusable is named, because the caller is a person holding a
    board who typed a path from memory: "missing", "a directory", and "cannot be
    read" are three different mistakes and a traceback distinguishes none of them.
    """
    if not path.exists():
        raise FlashError(f"{what}: no such file: {path}")
    if not path.is_file():
        raise FlashError(f"{what}: not a regular file: {path}")
    try:
        size = path.stat().st_size
        with path.open("rb"):
            pass
    except OSError as exc:
        raise FlashError(f"{what}: cannot read {path}: {exc}") from exc
    return size


def geometry_constant(name: str) -> int:
    """One geometry constant, or a hard error naming the constant.

    A missing or unreadable constant must stop the tool. Falling back to a
    plausible-looking default would mean writing to an address nobody chose.
    """
    consts = read_geometry_constants()
    if name not in consts:
        raise FlashError(
            f"{PROVISIONING.name} does not define {name}; refusing to guess the "
            f"flash geometry. Constants found: {', '.join(sorted(consts)) or 'none'}"
        )
    return consts[name]


class FlashError(RuntimeError):
    pass


class Write(NamedTuple):
    """One thing to do to the flash.

    ``path`` None means erase this region. Keeping both in one type lets the whole
    plan print in one pass, which matters: the operator's last chance to notice the
    wrong offset is that printout.
    """

    label: str
    offset: int
    size: int
    path: Path | None = None


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
    is on. Both implementations are exercised against the same shipped tables and
    against the same adversarial ones -- see tests/test_flash_tool.py and
    tests/test_partition_csv.cpp -- so a rule added to one and forgotten in the
    other shows up as a disagreement between them.
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
    # NOT covered by geometry is the pairing itself: each framework must be handed the
    # filesystem its own code mounts, and a future edit that swaps them makes it
    # format the wrong one on boot. That is the mistake worth catching.
    #
    # Both are SPIFFS today. Meshtastic would rather have LittleFS -- it is the
    # better filesystem -- but the generator inside the Arduino toolchain that
    # builds these tables predates ESP-IDF 5.0 and rejects the keyword, so LittleFS
    # is not a subtype this project can currently ship. Flip fs_meshtastic to
    # "littlefs" when the toolchain can build it.
    expected_fs = {"fs_meshcore": "spiffs", "fs_meshtastic": "spiffs"}
    for label, want in expected_fs.items():
        if label not in labels:
            continue
        part = next(p for p in parts if p.label == label)
        if part.subtype != want:
            raise FlashError(
                f"{label} is declared {part.subtype!r}, expected {want!r}. "
                "Meshtastic and MeshCore must each get the filesystem their own code mounts; "
                "handing either side the other's makes it format the wrong one on boot and "
                "lose the settings on the other side of the pair."
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
    tool = shutil.which("esptool") or shutil.which("esptool.py")
    if tool is None:
        raise FlashError("esptool not found. Install it with: pip install esptool")
    try:
        proc = subprocess.run(
            [tool, "--chip", "esp32s3", "--port", port, "flash_id"],
            capture_output=True,
            text=True,
            timeout=60,
        )
    except subprocess.CalledProcessError as exc:
        raise FlashError(f"esptool flash_id failed with exit code {exc.returncode}") from exc
    except subprocess.TimeoutExpired:
        raise FlashError("esptool flash_id timed out after 60s") from None
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

    The bootloader and the partition table are not rows in the CSV -- ESP-IDF's
    generator rejects any declared partition below 0x9000, and those two live
    lower. They are added here from the same constants the firmware uses, because
    the updater still has to write them.
    """
    parts = parse_csv(csv_path)
    first_slot = None
    for p in parts:
        if p.label.startswith("ota_"):
            first_slot = p.offset if first_slot is None else min(first_slot, p.offset)
    if first_slot is None:
        raise FlashError(f"{csv_path.name}: no app slots found")

    out: dict[str, tuple[int, int]] = {
        # The two pieces that exist outside the table, from the constants that own
        # them rather than from numbers written here.
        "bootloader": (
            geometry_constant("kBootloaderOffset"),
            geometry_constant("kBootloaderSize"),
        ),
        "partition_tbl": (
            geometry_constant("kPartitionTableOffset"),
            geometry_constant("kPartitionTableSize"),
        ),
    }
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
    # system_pieces() includes the bootloader and the partition table, which exist
    # outside the CSV on purpose. Listing only rows here would under-report what an
    # update would actually rewrite.
    for label, (offset, size) in sorted(system_pieces(csv_path).items(), key=lambda kv: kv[1][0]):
        implicit = "" if any(p.label == label for p in parts) else "   (implicit)"
        print(f"  0x{offset:06X}  {label:<15} {size:>8} B{implicit}")

    print("\nslots (never touched by a system update):")
    # The slot -> settings pairing is read out of the table rather than assumed. The
    # firmware owns that mapping, and a hardcoded list here is precisely how a host
    # tool drifts into disagreeing with the device about where a slot's settings live.
    # A slot's filesystem is the fs_* row that begins exactly where its app ends.
    for sp in [q for q in parts if q.label.startswith("ota_")]:
        line = f"  {sp.label:<6} 0x{sp.offset:06X}  app {sp.size // 1024}K"
        fs = next(
            (q for q in parts if q.label.startswith("fs_") and q.offset == sp.offset + sp.size),
            None,
        )
        if fs:
            line += f"   settings 0x{fs.offset:06X} ({fs.size // 1024}K, {fs.subtype})"
        else:
            line += "   settings: none declared"
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

    # The system region ends at kFirstSlotOffset (0x30000). This is the invariant:
    # a system update must never write at or above the first slot.
    first_slot = geometry_constant("kFirstSlotOffset")
    region_end = first_slot

    wanted = {
        "bootloader": args.bootloader,
        "partition_tbl": args.part_table_bin,
        "otadata": args.otadata,
    }
    plan: list[Write] = []
    for label, path in wanted.items():
        if path is None:
            continue
        if label not in pieces:
            raise FlashError(f"{csv_path.name} has no {label} row")
        offset, capacity = pieces[label]
        size = require_file(path, label)
        if size > capacity:
            raise FlashError(f"{label}: {path.name} is {size} B but the partition is {capacity} B")
        if offset + size > region_end:
            raise FlashError(f"{label}: would extend past the system region (ends at 0x{region_end:X})")
        plan.append(Write(label, offset, size, path))

    # Note on the partition table: it is written whenever it is asked for. The
    # firmware-side planner skips an identical table when it knows the size matches,
    # but this tool cannot know that without reading the chip back, so it does not
    # claim to. An erase cycle per update, per node, is real over twenty years, so
    # leave --part-table-bin off for a bootloader-only update.

    if not plan:
        print("\nnothing to write. Pass --bootloader and/or --part-table-bin.")
        return 0

    print("\nsystem update plan:")
    for w in plan:
        print(f"  0x{w.offset:06X}  {w.label:<15} {w.path}")
    print("\nslots and every settings partition are left untouched.")

    if args.dry_run:
        print("\ndry run: nothing written.")
        return 0

    return _run_esptool(args, plan)


def cmd_app(args) -> int:
    """Write, reflash or erase one slot."""
    csv_path = PARTITION_DIR / f"{args.table}.csv"
    parts = parse_csv(csv_path)
    label = f"ota_{args.slot}"
    target = next((p for p in parts if p.label == label), None)
    if target is None:
        available = ", ".join(p.label for p in parts if p.label.startswith("ota_"))
        raise FlashError(f"{csv_path.name} has no {label}. It has: {available}")

    if args.erase_settings:
        # The destructive one. Deliberately a separate, explicit flag rather than a
        # variant of --erase, because a bad image and "forget this node's identity
        # on the mesh" are different requests and conflating them loses channel keys.
        fs = next(
            (
                p
                for p in parts
                if p.label.startswith("fs_") and p.offset == target.offset + target.size
            ),
            None,
        )
        print(f"erasing {label} at 0x{target.offset:06X} ({target.size // 1024}K)")
        if fs is None:
            raise FlashError(
                f"no settings partition found immediately after {label}; refusing to "
                "guess which one you meant"
            )
        print(f"erasing {fs.label} at 0x{fs.offset:06X} ({fs.size // 1024}K) as well")
        print("\nThis DESTROYS the slot's settings: channel keys, node database,")
        print("and its identity on the mesh. It cannot be undone.")
        if args.dry_run:
            print("\ndry run: nothing written.")
            return 0
        return _run_esptool(
            args,
            [
                Write(label, target.offset, target.size, None),
                Write(fs.label, fs.offset, fs.size, None),
            ],
        )

    if args.erase:
        print(f"erasing {label} at 0x{target.offset:06X} ({target.size // 1024}K)")
        print("The slot's settings partition is NOT touched.")
        print("This is the right choice for recovering a bad image without losing")
        print("your channel keys or node database.")
        if args.dry_run:
            print("\ndry run: nothing written.")
            return 0
        return _run_esptool(args, [Write(label, target.offset, target.size, None)])

    if args.app is None:
        raise FlashError("pass --app PATH, --erase, or --erase-settings")

    path = args.app
    size = require_file(path, label)
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
    return _run_esptool(args, [Write(label, target.offset, target.size, path)])


def _run_esptool(args, plan: list[Write]) -> int:
    tool = shutil.which("esptool") or shutil.which("esptool.py")
    if tool is None:
        raise FlashError("esptool not found. Install it with: pip install esptool")

    port = find_port(getattr(args, "port", None))
    flash_size = (
        chip_flash_size(port)
        if getattr(args, "flash_size", 16) == 0
        else args.flash_size * 1024 * 1024
    )

    base = [
        tool,
        "--chip",
        "esp32s3",
        "--port",
        port,
        "--baud",
        args.baud,
    ]
    write_opts = [
        "--flash_mode",
        "dio",
        "--flash_freq",
        "80m",
        "--flash_size",
        f"{flash_size // (1024 * 1024)}MB",
    ]

    # Erasures first, as their own subcommand.
    #
    # `write_flash` has no per-region --erase flag; the subcommand for this is
    # `erase_region`, which takes a start AND a size. Passing "--erase" to
    # write_flash would not erase anything -- it would be rejected as an unknown
    # option, so `app N --erase` would simply fail.
    for w in [x for x in plan if x.path is None]:
        print(f"  erasing 0x{w.offset:06X} +0x{w.size:X} ({w.label})")
        try:
            subprocess.run(
                base + ["erase_region", f"0x{w.offset:X}", f"0x{w.size:X}"],
                check=True,
                timeout=120,
            )
        except subprocess.CalledProcessError as exc:
            raise FlashError(f"esptool erase_region failed with exit code {exc.returncode}") from exc
        except subprocess.TimeoutExpired:
            raise FlashError("esptool erase_region timed out after 120s") from None

    writes = [x for x in plan if x.path is not None]
    if writes:
        cmd = base + write_opts + ["write_flash"]
        for w in writes:
            cmd += [f"0x{w.offset:X}", str(w.path)]
        print("\nrunning esptool...")
        try:
            subprocess.run(cmd, check=True, timeout=300)
        except subprocess.CalledProcessError as exc:
            raise FlashError(f"esptool write_flash failed with exit code {exc.returncode}") from exc
        except subprocess.TimeoutExpired:
            raise FlashError("esptool write_flash timed out after 300s") from None
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--table",
        default="quadboot",
        choices=["quadboot", "dualboot"],
        help="partition layout (default: quadboot)",
    )
    ap.add_argument("--port")
    ap.add_argument("--baud", default="460800")
    ap.add_argument(
        "--flash-size",
        type=int,
        default=16,
        help="flash size in MB, or 0 to ask the chip (default: 16)",
    )
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
    p_app.add_argument(
        "--erase", action="store_true", help="erase the slot's firmware, keeping its settings"
    )
    p_app.add_argument(
        "--erase-settings",
        action="store_true",
        help="erase the firmware AND its settings partition. "
        "Destructive and irreversible: loses channel keys.",
    )
    p_app.set_defaults(func=cmd_app)

    # Legacy full-flash path, kept because it is still the right thing when
    # setting a board up for the first time.
    p_full = sub.add_parser("full", help="write bootloader, table and any slots (first setup)")
    p_full.add_argument("--bootloader", type=Path, required=True)
    p_full.add_argument("--part-table-bin", type=Path, required=True)
    p_full.add_argument("--otadata", type=Path)
    p_full.add_argument("--app", type=str, action="append", default=[], metavar="LABEL=PATH")
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

    # The bootloader and the partition table live outside the CSV on purpose (see
    # system_pieces), so the layout comes from there rather than from a row.
    system = system_pieces(csv_path)
    plan: list[Write] = [
        Write("bootloader", *system["bootloader"], args.bootloader),
        Write("partition_tbl", *system["partition_tbl"], args.part_table_bin),
    ]
    if args.otadata is not None:
        plan.append(
            Write("otadata", by_label["otadata"].offset, by_label["otadata"].size, args.otadata)
        )
    for label, path in sorted(args_map.items()):
        part = by_label[label]
        size = require_file(path, label)
        if part.size and size > part.size:
            raise FlashError(f"{path.name} does not fit {label}")
        plan.append(Write(label, part.offset, part.size, path))

    # Belt and braces: the loop above has already required every path, but a plan
    # that reaches esptool with a missing file would erase the region and then fail
    # to write it, which is the one outcome this tool exists to prevent.
    for w in plan:
        if w.path is not None and not w.path.is_file():
            raise FlashError(f"missing file: {w.path}")

    print("\nwrite plan:")
    for w in plan:
        print(f"  0x{w.offset:06X}  {w.label:<15} {w.path}")

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
