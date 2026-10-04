<h1 align="center">lora-multiboot</h1>

<p align="center">
  A role-aware <b>multi-slot firmware platform</b> for single-radio LoRa devices.
  Five switchable firmware roles on one antenna, progressive provisioning from
  nothing, and system updates that cost nobody their settings.
</p>

<p align="center">
  <a href="https://github.com/neohiro/lora-multiboot/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/neohiro/lora-multiboot/actions/workflows/ci.yml/badge.svg"></a>
  <img alt="License" src="https://img.shields.io/badge/license-MIT-blue.svg">
  <img alt="Board" src="https://img.shields.io/badge/board-Heltec%20LoRa%2032%20V4-16MB%20flash-orange">
  <img alt="Shared RAM" src="https://img.shields.io/badge/shared%20RAM-560%20B%20for%20all%20slots-blue">
</p>

---

> [!IMPORTANT]
> **This is an architecture and its groundwork, not a working repeater.** The logic
> that decides which mesh a frame belongs to, whether a region's radio plan is
> coherent, whether a flash layout is safe, and how much airtime is left is written
> and tested. The radio bring-up is not written, and the firmware **transmits
> nothing** today. [Status](#status) says exactly what exists.

## Why "multi-boot" and not "meshtastic + meshcore"

The name says what the project turned out to be.

It started as "run Meshtastic and MeshCore on one Heltec V4", which was a question
about two projects and one board. It is now a question about **any number of LoRa
firmware roles on one radio**, of which Meshtastic and MeshCore are the first two.

What that scope added:

- A **role model** — repeater, companion, room server, router, tracker, analyser,
  and this project's own two-mesh bridge image — with an audit of which
  combinations of slots are actually valid. [docs/ROLES.md](docs/ROLES.md)
- **Progressive provisioning** from a blank board: one connection and one obvious
  choice, then two, then three. [docs/PARTITIONS.md](docs/PARTITIONS.md)
- **Non-destructive updating**: a Heltec update must not cost anybody their
  firmware or their channel keys. [docs/UPDATING.md](docs/UPDATING.md)
- A **shared RAM block** so adding a slot costs flash and zero shared RAM.

## The enabling fact

A Heltec V4 has one SX1262, and a single radio is a single physical peripheral —
one firmware owns it at a time. That is the constraint everyone hits first.

Then it turns out the constraint does not apply to the interesting case:

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
| Slot 0 done (MeshCore) | **2** | `ota_0` + the opening `ota_1` (Meshtastic) |
| Slot 1 done (Meshtastic) | **2** | `ota_1` + the opening `ota_2` (Reticulum) |
| Slot 2 done (Reticulum) | **2** | `ota_2` + the opening `ota_3` (LoRaWAN) |
| Slot 3 done (LoRaWAN) | **2** | `ota_3` + the opening `ota_4` (Custom) |
| At capacity (4 usable) | **0** | nothing, because there is nowhere to put it |

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

## Memory: adding a slot costs flash, not RAM

A multi-slot board is only interesting if it is cheap, and on a microcontroller
"cheap" is measured in bytes. So the fixed overhead lives in **one shared block**:

```
SharedContext = 560 bytes total, for a five-slot board
  airtime governor   280 B   (32 time buckets)
  frame counters     192 B
  radio config        32 B
  plan, boot, slots   36 B
```

Everything in that block is identical no matter how many slots are installed,
because there is exactly one radio, one RF plan and one airtime limit on the board.
Slot count is **one byte**; sixteen provisioned slots cost **two bytes** of mask.

That is the claim that makes the architecture viable: **installing a fifth
firmware role adds no shared RAM at all.** The alternative — each slot keeping its
own plan and counters — would multiply the fixed overhead by the slot count to
store information that is by construction the same in all of them.

Three things enforce it rather than merely asserting it:

- `static_assert(sizeof(SharedContext) <= 1024)` fails the build if the block ever
  outgrows its budget.
- `static_assert(is_trivially_copyable<SharedContext>)` keeps it a plain data
  structure, so a slot can read the block without linking this code, and it can live
  in a fixed RAM section at a known address.
- The block is version-stamped. With several firmwares on one board a layout
  mismatch is a realistic failure, and a stale slot refuses rather than misreading
  fields it does not understand.

The airtime budget is shared for a compliance reason too: the 10% limit applies to
the **antenna**, not to each slot individually. A board with five slots that each
believed they had the whole budget would be five times over it.

## About "running both at once"

Worth being precise, because it is easy to misread a slot table:

**Slots are exclusive. One runs at a time, and it owns the single SX1262.** Two
images cannot both drive the transceiver, and every role needs it — a "companion"
carries traffic over LoRa as well as BLE, and the analyser needs the receiver
promiscuous. Five slots means five *installed, switchable* frameworks with their
settings preserved. Not five running ones.

The one thing that genuinely serves both meshes simultaneously is the **bridge
image**: a single firmware carrying both protocol stacks, which is exactly what the
one-byte identifier is for. Slots and the bridge are not alternatives — the bridge
is one slot that happens to serve two meshes.

[`docs/ROLES.md`](docs/ROLES.md) covers what each slot can be and audits which
combinations are worth deploying. It catches the one that bites in practice:
**two slots with the same framework *and* the same role would advertise the same
node identity**, so two "repeaters" form a single repeater that appears twice rather
than a mesh of two. Different roles are fine — a MeshCore room server and a
MeshCore repeater on one board is an ordinary deployment.

## One slot is always free

Every board reserves its final slot, named **"Free Heltec LoRa 32 V4 slot"** — the
name says what it is for, which is what somebody holding a screwdriver needs. It
cannot be provisioned, so the usable count is **N−1**:

| Layout | Slots | Usable |
|---|---|---|
| `quadboot.csv` | 5 | 4 |
| `dualboot.csv` | 3 | 2 |

## Quick start: flashing a board

1. **Plug in the board** over USB (or connect via BLE/WiFi — same service, different transport).
2. **List the layout** to see what slots exist: `python tools/flash.py list --table quadboot`
3. **Provision the first slot** (MeshCore): `python tools/flash.py app 0 --app meshcore.bin`
4. **Reboot** — the board now offers two connections: the MeshCore slot you just filled and the next empty slot.
5. **Provision the second slot** (Meshtastic): `python tools/flash.py app 1 --app meshtastic.bin`
6. **Select which slot boots** (optional): `python tools/flash.py app 0` to boot MeshCore, or `app 1` for Meshtastic.
7. **Update the system layer** (bootloader/partition table) without touching slots: `python tools/flash.py update-system --bootloader boot.bin --part-table-bin table.bin`
8. **Re-flash a bad image** without losing settings: `python tools/flash.py app 1 --app new-meshtastic.bin`
9. **Erase a slot's firmware only** (keep settings): `python tools/flash.py app 1 --erase`
10. **Erase a slot completely** (firmware + settings, irreversible): `python tools/flash.py app 1 --erase-settings`

All commands validate the partition table against the actual flash size before writing. Nothing is erased until the input file is confirmed readable. The `--dry-run` flag prints the plan without writing.

## Uninstalling from the middle: no loss, and no compaction

Removing a framework from a middle slot loses no memory and breaks nothing. Closing
the gap by moving the slots above it down would reclaim everything and destroy live
firmware — it is exactly how a repartition ruins somebody's mesh, and it breaks the
arithmetic addressing (`0x30000 + n × 0x300000`) that makes growth safe at all.

So a reclaimed slot does not become a numbered slot again. Its 3 MB becomes an **OTA
staging region**: somewhere to receive the next image for a slot before switching to
it. Reclaiming what you have finished with pays for in-place updates of what you
kept, with nothing moving and no USB cable on a board that is on a mast.

The one real trade: growth is refused once staging space is held, because growth is
arithmetic and would land a slot on top of the hole. Deferring growth is a decision
you make on your own schedule; an overlapping partition table is a brick.

## Per-framework radio settings, and one master RX

Frameworks bring their own TX settings. Reconciling them by *averaging* would be
physically wrong — SF11 is 2048 chips per symbol and SF9 is 512, so there is no
value between them that decodes either, and the same is true of frequency and coding
rate.

But **bandwidth and preamble do have compatible supersets**, so the "most powerful"
RX configuration masters the weaker ones honestly: a receiver wider than the signal
decodes it perfectly well, so the merge is the **maximum** bandwidth and the
**longer** preamble. Spreading factor and coding rate are not masterable in either
direction, and the code says so rather than producing a receiver that hears nothing.

So on a board whose firmware all shares a carrier — the normal case, and the premise
here — one master configuration hears every installed framework, differing only in
the sync words it accepts promiscuously.

None of it is mandatory: a profile may declare that it sets nothing, and the board
defaults are a complete, legal configuration on their own.

## Updating without losing anything

The system layer is everything below the first slot, and an update writes only
that. Nothing at or above the cut is touched, which makes "update the bootloader" a
routine operation rather than something needing a backup first.

```bash
python tools/flash.py list                                    # what an update touches
python tools/flash.py update-system --bootloader boot.bin     # slots + settings preserved
python tools/flash.py app 1 --app meshcore-1.2.3.bin          # reflash one slot only
python tools/flash.py app 1 --erase                           # clear firmware, keep settings
```

A bad image is reflashable **without** losing channel keys — a firmware problem is
not a reason to forget a node's identity on the mesh — and the destructive variant
has to be asked for by name. [`docs/UPDATING.md`](docs/UPDATING.md)

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
| `Airtime` — LoRa airtime math + sliding-window duty-cycle governor | done, tested |
| `RadioPlan` — promiscuous-capture SX1262 config, regional power ceilings | done, tested |
| `Statistics` — per-protocol counters, RSSI/SNR, airtime accounting | done, tested |
| `StatusPanel` — 3-line OLED/CLI rendering | done, tested |
| `Roles` — slot purposes + combination validity audit | done, tested |
| `SystemUpdate` — bootloader/table updates that preserve every slot | done, tested |
| `tools/flash.py` — validating flasher | done, tested |
| SX1262 bring-up, promiscuous capture | **not written** |
| Frame decode → cross-protocol relay | **not written** |
| OLED / BLE provisioning UX | **not written** |

1,262 assertions and 101 tool tests pass, compiled under `-Werror` with
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

## Vanity & Functional Keypair Mining

<p align="center">
  <img alt="mining" src="https://img.shields.io/badge/mining-Ed25519%20prefix%20%2B%20suffix-7c4dff">
  <img alt="meshtastic" src="https://img.shields.io/badge/Meshtastic-PSK%20%2B%20node%20ID-00b0d9">
  <img alt="offline" src="https://img.shields.io/badge/mining-100%25%20in--browser-2ea043">
</p>

A board that runs both stacks side by side is a board where both stacks will
eventually want an identity you can read from across a room — a callsign on a
repeater's OLED, a `!` ID dictated over VHF, a MeshCore node name read out during
a build session. None of those identities are designed. They are random bytes out
of a CSPRNG, and mining is the one case where brute force is the right answer: pay
for the search once, and every later contact with the node is cheaper.

[**meshcore-vanity-key**](https://neohiro.github.io/meshcore-meshtastic-vanity-key/) does
that. It mines Ed25519 keypairs until the encoded public key matches a pattern,
entirely in the browser — Web Workers plus libsodium WASM, no network round-trip,
no telemetry, working offline once loaded.

### Vanity and functional are two different goals

| | Vanity | Functional |
|---|---|---|
| **Why** | the identity reads well and is memorable | the identity is *checkable* by a human under bad conditions |
| **Typical target** | `mc1qneohiro…`, `!a1b2c3…` | a prefix **and** a suffix, so a half-transcribed key fails loudly |
| **Cost** | `16ⁿ` attempts for `n` hex characters — 4 is instant, 6 is minutes | that, squared: each constrained end multiplies rather than adds |
| **Classic mistake** | asking for 9+ characters. That is a lottery ticket, not a mnemonic | mining a *reserved* prefix and then wondering why the client refuses the key |

Same code path, same flags. The distinction only matters when deciding what to ask
for — and in both cases the pattern is matched against the **encoded public key**,
never the private one, which never leaves your machine.

### Meshtastic prefix and suffix mining

Meshtastic has two unrelated things people both call "the key", and they are mined
by two unrelated means. Conflating them is the usual first mistake.

**Channel PSK — symmetric, minable directly.** A channel is a name plus a
pre-shared key written `base64:…`. `AQ==` is the single byte `0x01` and is the
well-known default on every device — not a secret. `Ag==`–`Cg==` are the
`simple1`–`simple9` shorthands. A private channel is 16 bytes (AES-128) or 32 bytes
(AES-256). A PSK is raw key material rather than a signing key, so there is no
keypair to derive — the bytes *are* the key, which makes a vanity PSK a genuinely
**functional** target rather than a decoration. `--encoding base64` mines exactly
this form:

```bash
# browser: neohiro.github.io/meshcore-meshtastic-vanity-key — prefix box, suffix box, go
meshcore-vanity NHI --encoding base64              # channel key starting "NHI…"
meshcore-vanity NHI --encoding base64 --suffix 0   # …and ending "…0"
meshcore-vanity --encoding base64 --suffix qw      # suffix only
meshcore-vanity mc1qneohiro --encoding bech32      # MeshCore name, prefix form
```

Memorable here means **transcribable**. A group reads a PSK out over an FM handheld
before anybody has a phone paired, and a key with recognisable ends survives that
round trip when a bare 24-character base64 blob does not. Note that base64's last
character is constrained, so a base64 suffix has to end in one of
`048AEIMQUYcgkosw`.

**Node key and `!` user ID — a different curve, and one more derivation.** A
Meshtastic node's key is **Curve25519**, not Ed25519, and the `!` + hex ID the
firmware advertises is a *further* derivation from that node key — since firmware
2.8, from the public-key identity rather than from a hardware MAC address, which is
what lets a node keep its identity across a factory reset. Two separate things
therefore have to line up:

```
   seed ─▶ Curve25519 node key ─▶ firmware derivation ─▶ !a1b2c3d4
            ▲ the keypair                              ▲ what you read out of the UI
              that matters
```

The trap worth naming is the curve. `--encoding hex` chooses how a key is
*printed*; it does not choose which key it is. On the default Ed25519 derivation
you get a valid MeshCore device key and **not** a Meshtastic node key, and the
node will simply refuse the import — which reads like a firmware bug and is not
one. Meshtastic node-key mining is a different algorithm, not a different
encoding.

Which leaves the `!` ID itself, and three things worth knowing before spending an
afternoon on it:

- It is fixed width, so there is no short form to ask for. `!a1b2c3d4` is four
  bytes of derivation and nothing truncates it away.
- An ID pattern is **not** a key pattern. Constraining `!a1b2c3d4` constrains a
  derivation *of* the key, not the key, so the search is no cheaper than mining
  the key and usually dearer.
- Node keys are TOFU-bound: the first public key a node hears for a given node
  number is the one it keeps. Change a key after it has been seen and peers treat
  you as a stranger who replaced somebody.

### One caveat, stated plainly

Every device in this family generates its own key on first boot, and **nothing
here changes the identity a shipped firmware hands you**. Mining is for a node you
are deliberately provisioning: a fresh key imported over USB, a companion client,
or a factory-reset device whose identity you are re-establishing anyway. If a node
already has an identity, mine a *new* one and swap it in deliberately. Never
overwrite a key that peers already hold.

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
- [Roles](docs/ROLES.md) — what each slot can be, and which combinations are valid
- [Slots](docs/SLOTS.md) - the reserved free slot, reclaiming space, live-app views
- [Radio profiles](docs/RADIO-PROFILES.md) - per-framework TX settings, master RX
- [Updating](docs/UPDATING.md) — changing the system layer without losing a thing

## Acknowledgements

Built on [MeshCore](https://meshcore.io) and
[Meshtastic](https://meshtastic.org). Neither project is modified; this is
firmware and tooling that stands alongside them.

MIT. © 2026 neohiro
