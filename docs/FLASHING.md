# Complete Flashing Guide for lora-multiboot

This guide covers everything you need to flash a Heltec WiFi LoRa 32 V4 with the lora-multiboot multi-slot firmware platform.

## Quick Reference

| Task | Command (unified installer) | Command (direct flash.py) |
|---|---|---|
| First-time setup (both firmwares) | `./install-lora-multiboot.sh --bootloader boot.bin --part-table-bin part.bin --meshcore mc.bin --meshtastic mt.bin` | `python tools/flash.py full --bootloader boot.bin --part-table-bin part.bin --app ota_0=mc.bin --app ota_1=mt.bin` |
| Add Meshtastic to existing board | `./install-lora-multiboot.sh --meshtastic mt.bin` | `python tools/flash.py app 1 --app mt.bin` |
| Add MeshCore to existing board | `./install-lora-multiboot.sh --meshcore mc.bin` | `python tools/flash.py app 0 --app mc.bin` |
| Update bootloader only | `./install-lora-multiboot.sh --update-bootloader boot.bin` | `python tools/flash.py update-system --bootloader boot.bin` |
| Update partition table only | `./install-lora-multiboot.sh --update-part-table part.bin` | `python tools/flash.py update-system --part-table-bin part.bin` |
| List layout | `./install-lora-multiboot.sh --list` | `python tools/flash.py list` |
| Reflash slot (keep settings) | `./install-lora-multiboot.sh --app 1=new.bin` | `python tools/flash.py app 1 --app new.bin` |
| Erase firmware (keep settings) | `./install-lora-multiboot.sh --erase 1` | `python tools/flash.py app 1 --erase` |
| Erase firmware + settings | `./install-lora-multiboot.sh --erase-settings 1` | `python tools/flash.py app 1 --erase-settings` |
| Dry-run any command | Add `--dry-run` | Add `--dry-run` |

## Prerequisites

1. **Python 3.8+**
2. **esptool** — `pip install esptool`
3. **Heltec WiFi LoRa 32 V4** (16 MB flash) connected via USB
4. **Firmware binaries** — MeshCore and/or Meshtastic compiled for the bridge

### Getting Firmware Binaries

#### MeshCore
```bash
# Build from source or download release
# The bridge image includes both protocol stacks
```

#### Meshtastic
```bash
# Build for Heltec V4 with the quadboot partition table
# Or use a pre-built release compatible with the partition layout
```

#### Bootloader & Partition Table
```bash
# Generated from the project:
python tools/gen_layouts.py
# This produces firmware/partitions/quadboot.csv and dualboot.csv
# Use ESP-IDF's gen_esp32part.py to convert to binary:
python gen_esp32part.py firmware/partitions/quadboot.csv quadboot.bin
```

## Partition Tables

### quadboot.csv (default, 5 slots)
| Slot | Framework | App Label | FS Label | Usable |
|---|---|---|---|---|
| 0 | MeshCore | `ota_0` | `fs_meshcore` | Yes |
| 1 | Meshtastic | `ota_1` | `fs_meshtastic` | Yes |
| 2 | Reserved (Reticulum) | `ota_2` | `fs_reticulum` | Reserved |
| 3 | Reserved (LoRaWAN) | `ota_3` | `fs_lorawan` | Reserved |
| 4 | Reserved (Custom) | `ota_4` | `fs_custom` | Reserved |
| — | **Free slot** | `ota_4` | `fs_custom` | **Never** (recovery) |

**Usable slots: 4** (the 5th is permanently reserved for recovery)

### dualboot.csv (minimal, 3 slots)
| Slot | Framework | App Label | FS Label | Usable |
|---|---|---|---|---|
| 0 | MeshCore | `ota_0` | `fs_meshcore` | Yes |
| 1 | Meshtastic | `ota_1` | `fs_meshtastic` | Yes |
| 2 | Reserved | `ota_2` | `fs_reticulum` | **Never** (recovery) |

**Usable slots: 2** (the 3rd is permanently reserved for recovery)

## Step-by-Step: First-Time Setup

### 1. Prepare Files
```bash
# You need:
# - bootloader.bin (ESP32-S3 bootloader)
# - partition-table.bin (generated from quadboot.csv)
# - meshcore.bin (MeshCore bridge firmware)
# - meshtastic.bin (Meshtastic firmware for slot 1)
```

### 2. Run the Installer
```bash
./install-lora-multiboot.sh \
    --bootloader bootloader.bin \
    --part-table-bin partition-table.bin \
    --meshcore meshcore.bin \
    --meshtastic meshtastic.bin
```

### 3. Verify
```bash
# Check what was written
./install-lora-multiboot.sh --list
```

### 4. Boot
The board will boot into slot 0 (MeshCore) by default. Connect via USB/Serial to verify.

## Step-by-Step: Adding a Second Framework

If you already have MeshCore on slot 0 and want to add Meshtastic:

```bash
./install-lora-multiboot.sh --meshtastic meshtastic.bin
```

The installer will:
1. Detect the existing partition table on the board
2. Provision slot 1 with the Meshtastic firmware
3. Keep slot 0 (MeshCore) and its settings intact

## System Updates (Bootloader / Partition Table)

**Key property:** System updates **never touch slots or settings**.

```bash
# Update bootloader only (recommended for most updates)
./install-lora-multiboot.sh --update-bootloader new_bootloader.bin

# Update partition table (rarely needed, costs an erase cycle)
./install-lora-multiboot.sh --update-part-table new_partition_table.bin

# Update both
./install-lora-multiboot.sh --update-bootloader new_bootloader.bin --update-part-table new_partition_table.bin
```

The partition table is only rewritten if you explicitly pass `--update-part-table`. This saves flash erase cycles over the device's lifetime.

## Recovery Operations

### Reflash a Bad Image (Keep Settings)
```bash
# Slot 1 (Meshtastic) has a bad image, reflash it
./install-lora-multiboot.sh --app 1=new_meshtastic.bin

# Or using flash.py directly
python tools/flash.py app 1 --app new_meshtastic.bin
```

**Settings are preserved** — channel keys, node database, and mesh identity remain.

### Erase Firmware Only (Keep Settings)
```bash
./install-lora-multiboot.sh --erase 1
```

Use this when you want to remove a framework but might reinstall it later.

### Erase Firmware AND Settings (Destructive)
```bash
./install-lora-multiboot.sh --erase-settings 1
```

**WARNING:** This destroys channel keys, node database, and the node's identity on the mesh. You will be prompted to type `ERASE SETTINGS` to confirm.

### Full Erase and Repartition
Not supported — the design is append-only. To change the partition layout, you must use `flash.py full` with a new table, which erases everything.

## Selecting the Boot Slot

After provisioning multiple slots, you can choose which boots by default:

```bash
# Requires firmware with CLI running on the board
# The flash.py tool sets the boot slot in NVS via the bootloader
python tools/flash.py app 0  # Boot MeshCore (slot 0)
python tools/flash.py app 1  # Boot Meshtastic (slot 1)
```

## Common Options

| Option | Description |
|---|---|
| `--table TABLE` | Partition table: `quadboot` (default) or `dualboot` |
| `--port PORT` | Serial port (e.g., `COM3`, `/dev/ttyUSB0`). Auto-detected if omitted. |
| `--baud BAUD` | Baud rate (default: `460800`) |
| `--flash-size MB` | Flash size in MB, or `0` to ask the chip (default: `16`) |
| `--dry-run` | Validate and print plan, write nothing |
| `-h, --help` | Show help |

## Troubleshooting

### "No serial port found"
```bash
# Specify port explicitly
./install-lora-multiboot.sh --port COM3 --list
./install-lora-multiboot.sh --port /dev/ttyUSB0 --list
```
Many USB-C cables are power-only. Try a data cable.

### "Image does not fit this slot"
The firmware binary is larger than the 2 MB slot. Options:
- Use a smaller firmware build
- Rebuild with `dualboot` table if you only need 2 slots (same slot size)
- Modify `Provisioning.hpp` to increase `kSlotAppBytes` and regenerate layouts

### "Geometry: INVALID"
The partition table doesn't match the flash size. Common causes:
- Flashing quadboot (5 slots) on an 8 MB V3 board — use `dualboot` instead
- Corrupted partition table on the board

### "Could not read flash size from chip"
Put the board in bootloader mode:
1. Hold BOOT button
2. Press and release RESET
3. Release BOOT
4. Run the command again

### Board appears dead (no serial output)
- Check that app partitions are 64 KB aligned (the tool enforces this)
- A misaligned app partition causes silent boot failure
- Verify with `--list` that all offsets are multiples of 0x10000

## Advanced: Slot Reclaiming

When you remove a framework from a middle slot (not the highest), its space becomes an **OTA staging region** — a place to receive the next image before switching to it.

```bash
# Erase slot 1 (middle slot)
./install-lora-multiboot.sh --erase 1

# Reclaim it as OTA staging space
# (Requires device-side CLI, not yet in the installer)
```

The reclaimed space is never compacted — slots above it stay at their addresses. This is the append-only guarantee.

## Architecture Notes

- **One radio, one antenna:** Only the running slot drives the SX1262
- **Promiscuous RX:** The radio receives both MeshCore (0x12) and Meshtastic (0x2B) sync words simultaneously
- **Shared RAM (560 bytes):** Airtime budget, frame counters, radio config shared across all slots
- **Append-only growth:** Slot addresses are arithmetic (`0x30000 + n × 0x300000`), never moved
- **Separate filesystems:** MeshCore uses SPIFFS, Meshtastic uses SPIFFS (LittleFS not yet supported by Arduino partition generator)

## Validation

Every flash operation validates:
1. Partition table geometry (no overlaps, correct alignment, within flash size)
2. Input file readability (before any erase)
3. Image size vs partition capacity
4. Framework/filesystem pairing correctness

Run the full test suite:
```bash
python tools/gate.py -v
```

## Files in This Project

```
install-lora-multiboot.sh      # Unified installer (Linux/macOS/WSL/Git Bash)
install-lora-multiboot.bat     # Windows batch wrapper
tools/flash.py                 # Core flashing tool
tools/gen_layouts.py           # Regenerates partition CSVs from Provisioning.hpp
tools/gate.py                  # Runs all tests (compilation + unit)
firmware/partitions/quadboot.csv   # 5-slot partition table
firmware/partitions/dualboot.csv   # 3-slot partition table
firmware/include/bridge/Provisioning.hpp  # Owns all flash geometry constants
docs/UPDATING.md               # Update procedures
docs/PARTITIONS.md             # Flash layout documentation
```

## Support

- Issues: https://github.com/neohiro/lora-multiboot/issues
- Architecture: See `docs/ARCHITECTURE.md`
- RF Plan: See `docs/RF-PLAN.md`
- Protocol ID: See `docs/PROTOCOL-ID.md`