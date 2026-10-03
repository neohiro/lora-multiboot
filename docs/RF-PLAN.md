# RF plan and compliance

## The premise

The whole architecture rests on one fact, and it is worth stating precisely
because it is surprising:

| | Meshtastic `EU_868` LongFast | MeshCore default |
|---|---|---|
| Frequency | **869.525 MHz** | **869.525 MHz** |
| Bandwidth | **250 kHz** | **250 kHz** |
| Spreading factor | **11** | **11** |
| Coding rate | **4/5** | **4/5** |
| Sync word | `0x2B` | `0x12` |

Identical on every field that decides whether two radios can hear each other.
One byte apart on the only field that decides which mesh a frame belongs to.

That is why this needs no second radio, no time-slicing, and no sacrifice of
airtime. One SX1262, one antenna, continuous receive, both meshes.

`tests/test_channel_plan.cpp` asserts this premise explicitly, so if it ever
stops being true the gate fails rather than the architecture.

## Where it stops working

Outside the EU, the two community defaults **do not coincide**, and the bridge
says so rather than trying.

| Region | Meshtastic | MeshCore | Verdict |
|---|---|---|---|
| `EU_868` | 869.525 | 869.525 | **shared** |
| `US_915` | 906.875 | 910.525 | mismatch, 3.650 MHz apart |
| `ANZ_915` | 919.875 | 910.525 | mismatch |
| `IN_865` | 865.875 | 865.875 | frequencies agree, MeshCore figure unverified |
| `KR_922` | 922.875 | 922.875 | frequencies agree, MeshCore figure unverified |
| `SG_923` | 917.875 | 917.875 | frequencies agree, MeshCore figure unverified |
| `Custom` | — | — | operator-owned, never second-guessed |

Region is a build flag, defaulting to `EU_868`:

```bash
pio run -e heltec_v4_bridge \
  --project-option="build_flags=-DBRIDGE_REGION=\\\"US_915\\\" -DFREQUENCY_MHZ=906.875"
```

### What "mismatch" actually means

Two meshes on different carriers on one radio is **one mesh plus noise**. The
radio sits on one frequency; the other mesh is simply not there. A node in this
state looks superficially alive — it repeats, it answers its own mesh — and that
is exactly what makes it expensive to discover on a rooftop.

`resolvePlan()` reports `FrequencyMismatch` with the drift and names the side to
retune. Either side is a legitimate choice; naming one keeps the finding
actionable instead of merely alarming.

`MeshCoreFreqUnknown` is a weaker, separate finding: the two figures happen to
agree but the MeshCore one is not a confirmed published default for that region.
Both are printed at boot.

## Compliance: the EU airtime budget is 10%

**Do not deploy an EU repeater on MeshCore's stock settings.**

MeshCore ships a **50% software duty-cycle default**. That is not a legal airtime
budget at 869.525 MHz. The EU's harmonised table permits up to 500 mW e.r.p. in
the 869.4–869.65 MHz non-specific SRD entry *subject to*, among other routes, a
duty cycle not exceeding **10%**.

Offering 50% or 100% proves the firmware *can* transmit at that rate. It is
evidence of capability, not permission. The route has to be chosen and
documented before transmitting, not inferred from a menu.

This project therefore defaults `dutyCycleFor(EU_868)` to **10**, asserted by
`tests/test_channel_plan.cpp`.

US has no general duty-cycle cap, so the US figure of 100% in the table is a
courtesy bound rather than a licence term.

None of this is legal advice, and no firmware here can certify an arbitrary
board, amplifier, antenna or installation. Check your own licence position.

## Antenna and coexistence

One antenna serves both meshes here because there is one carrier. If you ever run
two boards on the same mast:

- **Do not** share one antenna between two transmitters through a passive
  splitter. There is no port-to-port isolation, so each radio couples its
  transmit power straight into the other's receiver front end. It also costs
  roughly 3 dB of insertion loss each way.
- Co-located 915 MHz transmitters need real physical separation — feet, not
  inches — to limit desense and protect the receivers.
- Combining needs a real RF combiner or cavity duplexer, not a splitter.

## Power

The V4 exists in a low-power 22 dBm variant and a high-power 28 dBm variant.
Check which one you have before computing EIRP, and remember the antenna gain
is part of the number. At +22 dBm conducted with a 5 dBi antenna you are at
roughly 27 dBm EIRP.
