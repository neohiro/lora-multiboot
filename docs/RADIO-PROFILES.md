# Per-framework radio settings, and the master RX configuration

Different firmware on different slots wants different radio settings. MeshCore has a
sync word and a channel; Meshtastic has a sync word and a channel; LoRaWAN has
another; a bespoke application may set nothing at all and expect sensible defaults.
One board, one radio — so those wants have to be reconciled, and the reconciliation
has to be physically honest rather than merely plausible.

## Settings are not "averaged"

Averaging is the obvious approach and it is wrong in two different ways.

### Most settings have no average. They must match exactly.

Spreading factor is the clearest case: SF11 is 2048 chips per symbol, SF9 is 512.
There is no value between them that demodulates either. The same is true of
frequency — a receiver at 869.525 MHz hears nothing of a 869.4 MHz frame — and,
less obviously, of **coding rate**. LoRa's forward error correction rate is a link
parameter the demodulator needs in order to interpret the bit stream; configure it
differently and the frame does not decode, however close the average is. CRC, header
mode and the low-data-rate optimisation behave the same way.

Any scheme producing a blended value for these produces a receiver that hears
nothing.

### Two settings *do* have a compatible superset — and this is the real answer

This is the part worth having, because it means boards with genuinely different
firmware often still need only one receiver configuration.

- **Bandwidth is a superset.** A receiver configured *wider* than the transmitted
  signal decodes it perfectly well, because the whole signal fits inside the
  receiver's window. Narrowing below the signal does not. So the correct merge of
  several bandwidths is their **maximum**, and this is not a compromise — it is
  strictly more capable. A receiver at 250 kHz decodes a 125 kHz frame as a matter
  of course. The cost is sensitivity: a wider window admits more noise, which is the
  trade to state rather than hide.

- **Preamble is a superset.** A receiver needs at least as many preamble symbols as
  the transmitter sent. Longer is fine and costs only airtime. So the merge is the
  **maximum**. Both ecosystems use 8, so it never actually bites.

### Frequency has a third option, which is bandwidth

Profiles on genuinely different carriers cannot be merged. But "close enough" is a
question about the receiver's bandwidth, not about the arithmetic mean. Two channels
200 kHz apart *can* be heard by one receiver at BW500 centred between them — which is
worth reporting, because it turns an apparent forever-slicing situation into a
single configuration.

## The master configuration

The relation is stated precisely in code as `dominates()`, so nothing depends on
comparing a few fields informally:

| Profiles differ in | Outcome |
|---|---|
| bandwidth only | merged — receiver widened |
| preamble only | merged — longer preamble taken |
| frequency, within half a receiver bandwidth | merged — widened to span both carriers |
| **spreading factor** | **time-slicing. No merge exists.** |
| **coding rate** | **time-slicing. No merge exists.** |
| frequency, further apart than the radio can span | time-slicing |

The master is the **least upper bound**: the narrowest configuration that hears
everything, because widening past that point costs sensitivity and buys nothing.
`leastUpperBound()` computes it and then *verifies* it by checking `dominates()`
against every input — a mistake there would produce a master that silently fails to
hear one of the profiles it was built from, which is precisely the failure this
module exists to prevent.

The common case on this board is the first row: every profile shares a carrier and
only the sync word differs, which is the premise the whole project rests on.

## Nothing here is mandatory

A special-purpose slot application — future-bound, knowing what it is doing — must be
able to boot and transmit **without ever configuring the radio**. So a profile may
declare `setsOwnSettings = false`, and the board defaults are a complete, working,
legal configuration on their own:

```
869.525 MHz · BW 250 kHz · SF11 · 4/5 · +22 dBm · sync 0x12
```

`reconcile()` treats an empty set of profiles as a valid state rather than a special
case. Firmware that wants nothing need do nothing.

## Switching costs deafness

Per-message switching is a **decision**, not a free action. Reconfiguring means
leaving receive, changing five parameters, and re-entering receive — several
milliseconds of silence on a shared band, where not listening means missing the
beginnings of other people's frames.

`decideSwitch()` therefore defers:

- **Mid-transmission** — switching would truncate the frame being sent, and the
  sender would simply never be heard again.
- **When the airtime budget is nearly spent** — reconfiguration costs no airtime but
  does cost deafness, and with no budget there is little worth transmitting anyway.

And it never reconfigures to the profile already in force, which is the case that
would otherwise be hit most often on a board where every framework shares a carrier.

## Which framework gets which sync word

| Framework | Sync word | Note |
|---|---|---|
| MeshCore | `0x12` | its published channel |
| Meshtastic | `0x2B` | the public LongFast channel |
| bridge image | `0x12` | transmits as MeshCore; that is the identity it holds |
| sniffer | `0x00` | promiscuous — an analyser wants everything |
| custom | `0x00` | and may set nothing at all |

A profile whose sync word is `0x00` makes the reconciled RX plan promiscuous with no
specific word, which is what an analyser needs and what no other role should ask
for.