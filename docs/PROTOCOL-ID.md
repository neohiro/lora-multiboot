# The identifier: one byte, read once

The brief for this project was a cross-protocol bridge where the difference
between Meshtastic and MeshCore is "one small identifier I read once." That is
exactly what is implemented, and this document is the honest account of what it
catches and what it does not.

## The one byte

Both protocols ride the same carrier, so the only thing separating a frame is
the LoRa **sync word**, part of the preamble:

| Protocol | Sync word |
|---|---|
| MeshCore | `0x12` |
| Meshtastic | `0x2B` |
| Reticulum / RNode | `0x42` |
| LoRaWAN | `0x34` |

Reading it costs nothing: no decryption, no key lookup, no parsing, and it works
on a frame too short to carry anything else.

## How the radio has to be configured to hand it over

This is the part that is easy to get wrong, because in normal packet mode the
SX1262 **strips the sync word and only reports a frame if it matches the one it
was configured with**. A radio locked to `0x12` is blind to Meshtastic entirely,
and a radio locked to `0x2B` is blind to MeshCore.

To see both, the radio has to run **promiscuous** — sync matching relaxed, so
every frame is delivered — and the bridge reads the sync byte per frame. That is
the mode this design requires.

Whether the byte arrives intact depends on the RadioLib path in use. Some
configurations strip it and only expose it through a status register. When the
byte is not available, `identify()` is told so (`hasSyncWord = false`) and falls
back — see below. The code does not assume the happy path.

## The fallback, and why it is not optional

**Meshtastic private channels do not use `0x2B`.** A private channel derives its
sync word from the channel hash, so a private-channel frame arrives wearing a
byte this table has never seen. A sync-word-only design would drop every private
frame on the floor.

What saves it: Meshtastic encrypts only the payload. The **MeshHeader magic
`0x95 0x33 0x16` is sent in the clear**, in band, in the same buffer as the
payload. Three unambiguous bytes, readable even when the radio gave us no sync
byte at all.

## Precedence

Implemented in `firmware/src/ProtocolId.cpp`:

1. **A real sync byte wins.** Cheapest signal, and the only one available for a
   frame too short to carry a MeshHeader.
2. **Otherwise the MeshHeader magic.** This is the path that keeps private
   channels working.
3. **Otherwise `Unknown`.**

When both are present and *disagree*, the magic wins — it was read in-band from
the same buffer, so it is the stronger witness against a corrupted preamble.

A sync word of `0x00` is treated as a **wildcard**, recorded as such, and barred
from voting. A wildcard carrying no information must never decide anything.

## Unknown means unknown

An unrecognised sync word with no magic returns `Unknown`, not a guess. On a
shared band somebody else's LoRa is far more likely to be arriving than ours,
and defaulting to a mesh is how a bridge starts relaying foreign traffic and
earning its operator an angry message from an unrelated community.

## What it reports, and why

`Identification` carries the protocol, the **evidence** that produced it, the raw
sync byte, whether it was corroborated, whether the byte was a wildcard, and a
short `reason` string.

The reason string is part of the contract, not a debug extra. "Why was this frame
labelled MeshCore" is the first question anyone asks when a mesh misbehaves, and
the answer fits on a 128×64 OLED:

```
MC  sync 0x12
MT  sync+magic agree
MT  magic only (private channel?)
??  unknown sync word
```

## What this does not do

Honest limits, all of which matter:

- **It identifies, it does not decrypt.** Naming a frame MeshCore says nothing
  about whether it carries a channel you hold a key for. Verification belongs to
  the protocol stack, not here.
- **It does not bridge the two meshes' payloads.** Decoding a MeshCore `GRP_TXT`
  and re-encoding it as a Meshtastic protobuf is a separate and much larger piece
  of work. This module is the routing decision in front of it.
- **It only works because both meshes share a carrier.** In a region where their
  defaults differ, there is no single carrier to listen on. See
  [RF-PLAN.md](RF-PLAN.md).
- **It is untested against real air.** The decision logic is covered by
  `tests/test_protocol_id.cpp`, including the private-channel and contradiction
  cases. It has not yet been driven from an SX1262, because the radio layer is
  not written.
