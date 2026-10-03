# Architecture

## What this is

One Heltec WiFi LoRa 32 V4 whose single SX1262 serves **both** a Meshtastic mesh
and a MeshCore mesh at the same time, on the same carrier, with no time-slicing
and no second radio.

The enabling fact is in [RF-PLAN.md](RF-PLAN.md): in `EU_868` the two community
defaults agree on frequency, bandwidth, spreading factor and coding rate, and
differ by one byte in the preamble. Everything else follows from that.

## Status: read this before trusting anything

| Piece | State |
|---|---|
| `ProtocolId` — sync-word-first identifier, MeshHeader fallback | **Done, tested** |
| `ChannelPlan` — region resolution, shared-carrier validation, airtime cap | **Done, tested** |
| `SlotTable` — layout geometry and framework isolation | **Done, tested** |
| `Provisioning` — zero-boot state machine, append-only slot growth | **Done, tested** |
| `SlotLifecycle` — provision, reflash, erase, retire, boot selection | **Done, tested** |
| `tools/gate.py` — the gate driver | **Done, tested** |
| `tools/flash.py` — validating flasher | **Done, tested** |
| Radio bring-up (SX1262, promiscuous capture) | **Not written** |
| Frame decode → cross-protocol relay | **Not written** |
| OLED / BLE provisioning UX | **Not written** |

408 assertions and 16 tool tests pass, compiled under `-Werror` with
`-Wconversion -Wsign-conversion -Wshadow`. The current firmware boots, validates
its own plan and layout, prints both, and **transmits nothing**.
`firmware/src/main.cpp` says so on the serial log rather than pretending
otherwise. What exists is the part worth getting right before any RF leaves the
board, and the part that is testable without hardware.

## Why it is built this way

The logic that decides *which mesh a frame belongs to*, *whether a region's plan
is coherent*, and *whether a flash layout is safe* is deliberately free of radio
dependencies. That buys three things:

1. **It runs on a laptop.** `make check` needs a C++ compiler and nothing else —
   no hardware, no vendor toolchain, no PlatformIO.
2. **It is the same code on device.** The suites compile the firmware's actual
   translation units, not copies. A test cannot pass while the firmware rots.
3. **The dangerous failures are caught at boot**, before the board is on a mast.

Both failure modes encoded here present on the bench as an inexplicably dead
board or a node quietly hearing one mesh:

- An app partition not 64 KB aligned is refused by the bootloader with no log.
- Two firmware sharing a filesystem partition destroys one side's settings with
  no error anywhere.

## Layout

```
firmware/
  include/bridge/     portable logic, no radio dependencies
    ProtocolId.hpp      one byte, read once
    ChannelPlan.hpp     region -> shared RF plan, or a loud complaint
    SlotTable.hpp       partition geometry + framework isolation
    Provisioning.hpp    zero-boot state machine, append-only growth
    SlotLifecycle.hpp   provision, reflash, erase, retire, boot selection
  src/                those five .cpp files
  src/main.cpp        on-device bring-up (Arduino; not in the host gate)
  partitions/         quadboot.csv, dualboot.csv  (generated)
tests/                the gate: C++ suites + the flashing tool's own tests
tools/
  gate.py             the gate driver
  gen_layouts.py      regenerates the tables from the code's geometry
  flash.py            validating flasher
docs/                 this directory
```

`firmware/src/main.cpp` includes `Arduino.h` and is therefore not part of the
host gate — only the portable modules are. The gate compiles the code that is
meant to be portable, and the flasher tests the code a new user runs before they
have a C++ compiler.

## Module responsibilities

### `ProtocolId`

Takes a payload, an optional sync byte, and answers which mesh it belongs to.
Sync word first (cheapest, works on the shortest frames), MeshHeader magic as the
fallback that keeps **private** Meshtastic channels working, `Unknown` rather
than a guess otherwise. Full reasoning in [PROTOCOL-ID.md](PROTOCOL-ID.md).

### `ChannelPlan`

Region to RF plan, plus the two findings that matter: `FrequencyMismatch` when the
two communities do not share a carrier, and the region's airtime cap. Also
returns a one-line status string sized for the OLED.

### `SlotTable`

Parses an ESP-IDF partition CSV and refuses the layouts that brick a board or
destroy settings: past end of flash, overlapping, misaligned, or two frameworks
sharing a filesystem label.

`validateFrameworks()` judges **isolation, not presence**. A one-slot board is a
legitimate state — it runs MeshCore alone — so demanding Meshtastic's slot would
make the first successful provisioning look like a broken table.

### `Provisioning`

The zero-boot state machine and the slot allocator. A virgin board offers exactly
one connection; each completed slot opens the next. Growth is append-only because
slot *n*'s address is arithmetic and independent of how many slots exist, so a
rewrite cannot relocate live firmware — a property `growTable()` verifies at every
step rather than asserting in a comment.

Capacity is derived from the flash size, and the next slot index is the
provisioned count, so neither can drift out of step with reality after an
interrupted flash or a factory reset.

## Next

In order, because each depends on the one before:

1. **SX1262 bring-up.** Configure promiscuous capture, confirm the sync byte
   actually reaches `identify()` from real hardware, and pin down what RadioLib
   exposes. Everything above assumes that answer and nothing is built on it yet.
2. **The zero-boot bootloader.** An app that presents one endpoint when
   `provisionedCount == 0` and rewrites the partition table to open the second
   slot after the first is flashed. The policy exists and is tested; the app does
   not.
3. **Frame counters.** Packet-count and RSSI/SNR per protocol, so the OLED can
   show that both meshes are alive.
4. **Airtime governor.** Enforce the region's cap in software, with the EU
   default at 10%.
5. **Decode and relay.** The large piece. Decoding a MeshCore `GRP_TXT` and
   re-encoding it as a Meshtastic protobuf is two protocol stacks, not a switch
   statement.
6. **UX.** Boot selector on the OLED, and separate BLE identities so a phone
   keeps one bond per firmware across a switch.
