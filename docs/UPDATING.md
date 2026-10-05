# Updating without losing anything

A Heltec update must not cost anybody their MeshCore, their Meshtastic, their
channel keys or their node database. This document is how that guarantee is
structured, and where it is checked.

## The cut

The system layer is **everything below the first slot**:

| Region | Offsets | Rewritten by an update? |
|---|---|---|
| `bootloader` | `0x000000` | yes — implicit, not a CSV row |
| `partition_tbl` | `0x008000` | yes — implicit, it is the generator's own output |
| `nvs` | `0x009000` | yes |
| `otadata` | `0x013000` | yes |
| `coredump` | `0x015000` | yes |
| — | `0x030000` ← **cut** | — |
| slots and their settings | `0x030000` and above | **never** |

The bootloader and the partition table live outside the CSV because ESP-IDF's
generator rejects any declared partition below `0x9000` — declaring them as rows
was tried and refused by the real tool. Their geometry is still known to the
updater, as constants in `Provisioning.hpp`.

That gives a clean, checkable property:

> **A system update writes the system region or nothing. It never writes a slot,
> and it never writes a filesystem.**

Which is what makes "update everything" a routine operation rather than something
that needs a backup first. There is nothing above the cut to lose.

## Why the partition table is usually not rewritten

Rewriting an identical table works, but it costs an erase cycle on every node in
the field for no reason, and flash in this class of part is rated for a finite
number of cycles. Over a twenty-year deployment that adds up.

The firmware-side planner skips it when the size matches
(`UpdatePlan::partitionTableUnchanged`). `tools/flash.py` deliberately does not
claim to: it cannot know whether the table on the chip is identical without reading
it back, so for a bootloader-only update, leave `--part-table-bin` off.

## The dangerous write

The bootloader is the only genuinely risky piece, because a bad one leaves a board
that nothing will boot. Three defences:

1. Each image's size is checked against its partition **before** anything is written.
2. `planSystemUpdate()` refuses any image that would extend past `0x30000` — that
   is, any image that would land on slot 0. A bootloader built for a different
   board, or an offset mistyped as `0x30000` instead of `0x0`, is caught here
   rather than becoming a brick.
3. `systemRegionIsContained()` re-checks the layout itself, so a hand-edited
   partition table cannot quietly place a system piece above the cut.

Two refusals are reported distinctly, because they are different mistakes:

| Status | Meaning |
|---|---|
| `CrossesSlotBoundary` | Would overwrite slot 0. The serious one. |
| `TooLarge` | Stays inside the system region, overruns its own partition. Costs a boot selection, not a firmware. |

## Recovery

There is always exactly one way back, and it does not depend on any slot holding
firmware: **the bootloader is below the cut, so it survives every slot operation**,
and `bootloaderAlwaysReachable()` asserts it on every table size including after a
full provision/erase/retire cycle.

The `otadata` boot record carries a sequence number, so the bootloader can reject
an image that fails to boot and fall back to the previous slot. If a reflash bricks
a node, holding the boot button at power-on enters the ROM download mode and the
board is recoverable over USB with no further hardware.

## Command line

### Using the unified installer (recommended)

```bash
# what is on the board, and what an update would touch
./install-lora-multiboot.sh --list

# update the bootloader only. Slots and every settings partition are untouched.
./install-lora-multiboot.sh --update-bootloader bootloader.bin

# reflash one slot. Other slots and all settings are untouched.
./install-lora-multiboot.sh --app 1=meshcore-1.2.3.bin

# erase one slot's firmware but keep its settings
./install-lora-multiboot.sh --erase 1

# erase one slot completely (firmware + settings, irreversible)
./install-lora-multiboot.sh --erase-settings 1

# first-time setup: bootloader, partition table and any slots
./install-lora-multiboot.sh --bootloader bootloader.bin \
    --part-table-bin partition-table.bin \
    --meshcore bridge.bin --meshtastic meshtastic.bin
```

### Using `flash.py` directly

```bash
# what is on the board, and what an update would touch
python tools/flash.py list

# update the bootloader only. Slots and every settings partition are untouched.
python tools/flash.py update-system --bootloader bootloader.bin

# reflash one slot. Other slots and all settings are untouched.
python tools/flash.py app 1 --app meshcore-1.2.3.bin

# erase one slot's firmware but keep its settings
python tools/flash.py app 1 --erase

# first-time setup: bootloader, partition table and any slots
python tools/flash.py full --bootloader bootloader.bin \
    --part-table-bin partition-table.bin \
    --app ota_0=bridge.bin --app ota_1=meshtastic.bin
```

Add `--dry-run` to any of them. The tool prints exactly what it would write and
writes nothing.

## Reflashing one slot without losing its settings

The distinction is deliberate and is enforced in `SlotLifecycle`:

| Operation | App rewritten | Settings rewritten | Table changed |
|---|---|---|---|
| `reflash` | that slot only | **no** | no |
| `eraseApp` | that slot only | **no** | no |
| `eraseSlot` | that slot only | **yes**, by the flasher | no |

A bad firmware is not a reason to forget a node's channel keys. Conflating those
two requests is how somebody reflashes to recover from a bad image and silently
loses the node's identity on the mesh, so the destructive variant has to be asked
for by name — and the device records the *intent*, while the flash erase itself is
done by the tool, because firmware cannot sanely erase the filesystem partition it
is running out of.

```bash
python tools/flash.py app 1 --erase          # clear firmware, keep settings
python tools/flash.py app 1 --erase-settings # clear both. Irreversible.
```