<h1 align="center">meshcore-meshtastic-heltec-v4</h1>

<p align="center">
  One Heltec LoRa 32 V4 whose single SX1262 serves <b>both</b> a Meshtastic mesh
  and a MeshCore mesh — at the same time, on the same carrier, with no second
  radio.
</p>

<p align="center">
  <a href="https://github.com/neohiro/meshcore-meshtastic-heltec-v4/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/neohiro/meshcore-meshtastic-heltec-v4/actions/workflows/ci.yml/badge.svg"></a>
  <img alt="License" src="https://img.shields.io/badge/license-MIT-blue.svg">
  <img alt="Board" src="https://img.shields.io/badge/board-Heltec%20LoRa%2032%20V4-16MB%20flash-orange">
</p>

---

> [!IMPORTANT]
> **This is an architecture and its groundwork, not a working repeater.** The
> logic that decides which mesh a frame belongs to, whether a region's radio
> plan is coherent, and whether a flash layout is safe is written and tested. The
> radio bring-up is not written, and the firmware **transmits nothing** today.
> [Status](#status) says exactly what exists.

## Why this is possible at all

A Heltec V4 has one SX1262, and a single radio is a single physical peripheral —
one firmware owns it at a time. That is the constraint everyone hits first.

Then it turns out the constraint does not apply here:

| | Meshtastic `EU_868` LongFast | MeshCore default |
|---|---|---|
| Frequency | **869.525 MHz** | **869.525 MHz** |
| Bandwidth | **250 kHz** | **250 kHz** |
| Spreading factor | **11** | **11** |
| Coding rate | **4/5** | **4/5** |
| Sync word | `0x2B` | `0x12` |

Identical on every field that decides whether two radios can hear each other.
**One byte apart** on the only field that decides which mesh a frame belongs to.

That single byte is read from the preamble, before any decryption or parsing. So
one radio, one antenna, continuous receive, both meshes — no time-slicing, no
airtime given up, no second chip. [`docs/PROTOCOL-ID.md`](docs/PROTOCOL-ID.md)

## The other half: it starts as one board and grows

A fixed multi-slot partition table has to guess, at manufacture, how many
frameworks a board will ever run. Guess low and a later framework needs a
repartition — costing every existing user their settings. Guess high and every
board carries dead partitions forever.

Instead, the layout **starts at one slot and grows as the board is commissioned**:

| State | Connections | Offered |
|---|---|---|
| Nothing provisioned | **1** | `ota_0` — MeshCore, as though it were the only option |
| Slot 0 done | **2** | `ota_0` + the opening `ota_1` (Meshtastic) |
| Slot 1 done | **2** | `ota_1` + the opening `ota_2` |
| At capacity | **0** | nothing, because there is nowhere to put it |

USB, BLE and WiFi are interchangeable labels on the same service. A new owner
connects once and gets one obvious choice; a second connection appears only when
there is genuinely a second thing to connect to.

Three properties make this safe enough to never think about again:

1. **Append-only.** Slot *n*'s address is arithmetic, so the four-slot table
   contains the two-slot table verbatim. There is no code path that can move a
   partition already holding firmware — asserted, not just documented.
2. **Transport-independent.** Nothing in the state machine knows which transport
   is in use.
3. **Derived, not remembered.** Capacity comes from dividing the free flash by
   the slot stride, and the next slot index is the provisioned count — so neither
   can drift out of step after an interrupted flash or a factory reset.

[`docs/PARTITIONS.md`](docs/PARTITIONS.md)

## Taking a slot away again

Growth is the easy direction. Removal is where append-only actually gets tested.

**Erasing a slot's firmware is not deleting the slot.** The row stays exactly
where it was, nothing moves, and the slot becomes available again — which is what
you want almost every time.

**Erasing settings is a separate, explicit choice.** A slot's filesystem survives
a firmware repair by default, because "the image is broken" and "discard my
channel keys" are different requests. Conflating them is how someone reflashes to
recover and silently loses the node's identity on the mesh.

**Only the highest slot can be retired.** Reclaiming a hole in the middle means
moving every slot above it down into the gap — exactly the operation that
overwrites live firmware. So it is refused, with the reason stated:

```
retire slot 1 of 3: only the highest slot can be retired; retiring a lower
one would move every slot above it
```

The bootloader sits below every slot and is never a candidate for any of this.
There is always exactly one way back to a working board, and it does not depend on
any slot holding firmware.

## One antenna feed

This needs **no antenna hardware whatsoever**, and that is structural rather than
lucky.

Only the running slot ever drives the SX1262. A slot that is not running is
storage, not a radio. Every slot shares the one radio and the one antenna because
at most one is ever executing — so growing the slot count adds no RF path, no
multiplexer, no switch, and nothing that drifts out of calibration over twenty
summers.

## Status

| Piece | State |
|---|---|
| `ProtocolId` — one-byte identifier + MeshHeader fallback | done, tested |
| `ChannelPlan` — region resolution, shared-carrier validation, airtime cap | done, tested |
| `SlotTable` — layout geometry and framework isolation | done, tested |
| `Provisioning` — zero-boot state machine, append-only growth | done, tested |
| `SlotLifecycle` — provision, reflash, erase, retire, boot selection | done, tested |
| `tools/flash.py` — validating flasher | done, tested |
| SX1262 bring-up, promiscuous capture | **not written** |
| Frame decode → cross-protocol relay | **not written** |
| OLED / BLE provisioning UX | **not written** |

408 assertions and 16 tool tests pass, compiled under `-Werror` with
`-Wconversion -Wsign-conversion -Wshadow`.

## The gate

The compiler is the only dependency. No hardware, no vendor toolchain, no
PlatformIO.

```bash
python tools/gate.py
```

That is the same command CI runs on Linux, Windows and macOS — deliberately, since
a second CI-only definition of "passing" is a definition that drifts. `make check`
works too and delegates to the same driver.

What it covers, all of which are decisions that would otherwise only be
discoverable on a rooftop:

- a Meshtastic **private** channel, which arrives wearing a sync word the table
  has never seen and is rescued by the plaintext MeshHeader magic
- a corrupted capture where the sync word and the magic disagree
- a frame from **somebody else's LoRa**, which stays `Unknown` instead of being
  guessed at and relayed
- `US_915`, where the two community defaults are **3.65 MHz apart** and the code
  says so instead of trying
- a partition table that overlaps, overruns the flash, or is misaligned
- two firmware sharing one filesystem partition
- growth from one slot to capacity, checking at every step that nothing moved

## Compliance: do not skip this

> [!WARNING]
> **Do not deploy an EU repeater on MeshCore's stock settings.** MeshCore ships a
> **50% software duty-cycle default.** That is not a legal airtime budget at
> 869.525 MHz. The EU's harmonised table permits up to 500 mW e.r.p. in the
> 869.4–869.65 MHz non-specific SRD entry *subject to*, among other routes, a
> duty cycle not exceeding **10%**.
>
> Offering 50% or 100% proves the firmware *can* transmit at that rate. That is
> evidence of capability, not permission. This project defaults the EU airtime
> cap to **10%** and asserts it in the test suite.

Outside the EU the two community defaults do **not** coincide — `US_915` puts
Meshtastic on 906.875 MHz and MeshCore on 910.525 MHz, which on one radio is one
mesh plus noise. `resolvePlan()` reports that as `FrequencyMismatch` with the
drift and the side to retune, at boot, before anything transmits.

None of this is legal advice, and no firmware can certify an arbitrary board,
amplifier, antenna or installation. [`docs/RF-PLAN.md`](docs/RF-PLAN.md)

## Layout

```
firmware/
  include/bridge/     portable logic, no radio dependencies
    ProtocolId.hpp      the one-byte identifier
    ChannelPlan.hpp     region -> shared RF plan, or a loud complaint
    SlotTable.hpp       layout geometry and framework isolation
    Provisioning.hpp    zero-boot state machine, append-only growth
  partitions/         generated: quadboot.csv, dualboot.csv
  src/main.cpp        on-device bring-up (Arduino; transmits nothing)
tests/                the gate: C++ suites + the flashing tool's tests
tools/
  gate.py             the gate driver
  gen_layouts.py      regenerates the tables from the code's geometry
  flash.py            validating flasher
docs/                 ARCHITECTURE, PROTOCOL-ID, RF-PLAN, PARTITIONS
```

The logic that touches the radio is kept free of radio dependencies. That is what
lets it run on a laptop, and it means the suites compile the firmware's **actual**
translation units rather than copies — a test cannot pass while the firmware rots.

## Documents

- [Architecture](docs/ARCHITECTURE.md) — how it fits together, and what is next
- [The identifier](docs/PROTOCOL-ID.md) — one byte, and what it does not cover
- [RF plan](docs/RF-PLAN.md) — the shared carrier, and where it stops working
- [Flash layout](docs/PARTITIONS.md) — progressive provisioning, in full

## Acknowledgements

Built on [MeshCore](https://meshcore.io) and
[Meshtastic](https://meshtastic.org). Neither project is modified; this is
firmware and tooling that stands alongside them.

MIT. © 2026 neohiro