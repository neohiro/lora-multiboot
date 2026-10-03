# Flash layout and progressive provisioning

The Heltec WiFi LoRa 32 V4 carries **16 MB** of external flash and 2 MB of PSRAM.
A fixed multi-slot table has to decide, at manufacture, how many frameworks a
board will ever run. Choose too few and a later framework needs a repartition,
which costs every existing user their settings and a trip to the rooftop.
Choose too many and every board carries dead partitions forever.

So the layout here is not fixed. It starts as one slot and grows as the board is
commissioned, in a way that is safe enough that nobody has to think about it for
the next twenty years.

## The rule: slots are arithmetic, and append-only

Slot *n* lives at a fixed address whether there is one slot or five:

```
slot n  =  0x30000  +  n × 0x300000
app       2 MB at slot n
fs        1 MB immediately after
```

| | |
|---|---|
| Stride | `0x300000` (3 MB) |
| App image | `0x200000` (2 MB) |
| Filesystem | `0x100000` (1 MB) |
| First slot | `0x30000` |

Everything below the slots is fixed forever and never relocated:

| Label | Offset | Size |
|---|---|---|
| `bootloader` | `0x0` | 28 KB |
| `partition_tbl` | `0x8000` | 48 KB |
| `otadata` | `0x14000` | 8 KB |
| `nvs` | `0x16000` | 40 KB |
| `coredump` | `0x20000` | 64 KB |

The coredump sits *below* the slots rather than above them specifically so that
capacity is a single subtraction and adding a slot never disturbs it.

Because slot *n*'s address does not depend on how many slots exist, **the
four-slot CSV contains the two-slot CSV verbatim plus more.** Growth is therefore
provably append-only: there is no code path that can move a partition which
already holds firmware. That is the entire safety argument for rewriting a
partition table in place, and it is asserted directly — `growTable()` walks the
whole way up and checks that every pre-existing slot kept its exact offset and
size at every step.

Capacity is derived, never declared: `maxSlotsForFlash()` divides the space above
the coredump by the stride, which is why an 8 MB V3 reports two slots from the
same code that reports five for a 16 MB V4.

## Provisioning: one connection, then two, then three

The first thing a new owner does should not require understanding slots.

| State | Connections offered | What you see |
|---|---|---|
| Nothing provisioned | **1** | `ota_0` — MeshCore, as though it were the only option |
| Slot 0 done | **2** | `ota_0` and the newly opening `ota_1` (Meshtastic) |
| Slot 1 done | **2** | `ota_1` and the newly opening `ota_2` |
| At capacity | **0** | Nothing to offer, because there is nowhere to put it |

Each connection is an independent USB, BLE or WiFi endpoint. USB, BLE and WiFi
are interchangeable labels on one service — the state machine does not depend on
which is in use, so adding a fourth transport is an enumeration entry rather than
a redesign.

Slot 0 opens with **MeshCore** deliberately: it is the image that knows it shares
a radio, so the radio is in the hands of firmware that can reason about both
meshes from the first boot rather than after a later reflash. Meshtastic is stock
firmware and needs no such awareness, which is why it opens second.

`nextSlotIndex()` is `provisionedCount`, never a stored pointer. A derived value
cannot drift out of step with reality after an interrupted flash or a factory
reset; a stored one can, and would then write firmware past the end of flash.

## Removing and replacing a slot

Growth is the easy direction. Removal is where the append-only guarantee is
actually tested, because the whole safety argument rests on it.

**Erasing a slot's firmware is not deleting the slot.** Clearing the app and
marking the slot unprovisioned leaves its partition row exactly where it was.
Nothing moves, nothing shrinks, the freed flash is simply unused, and the slot
becomes available for provisioning again. This is what anybody wants almost every
time — a bad image, a firmware to replace, a node being repurposed.

**Erasing settings is a separate, explicit operation.** A slot's filesystem
survives an app erase by default, because "the firmware is broken" and "discard
the channel keys and node database" are different requests. Conflating them is
how somebody reflashes to recover from a bad image and silently loses the node's
identity on the mesh.

**Only the highest-numbered slot can be retired.** Reclaiming a hole in the
middle means moving every slot above it down into the gap, which is precisely the
operation that overwrites live firmware. So retirement is allowed at the top and
nowhere else, and `retireSlot()` refuses otherwise — saying *why*, rather than
doing the dangerous thing quietly.

| Operation | App rewritten | Settings rewritten | Table changed |
|---|---|---|---|
| `provision` | that slot only | no | no |
| `reflash` | that slot only | **no** | no |
| `eraseApp` | that slot only | **no** | no |
| `eraseSlot` | that slot only | **yes** | no |
| `retireSlot` | no | no | yes, and only the top |
| `setBootSlot` | nothing | no | no |
| `ensureRoom` | nothing | no | yes, appends only |

Every operation returns the set of slots it touched, so "one operation writes one
slot" is a checkable property of a return value rather than a claim in a comment.

The device state — which slots hold firmware, and which one boots — lives in NVS,
not in the partition table. That split is what lets a slot be erased and rewritten
without ever rewriting the table. The `otadata` boot record carries a sequence
number so the bootloader can reject an image that fails to boot and fall back, and
erasing the active slot reassigns the boot target to a slot that actually holds
something rather than leaving the board pointing at nothing.

## The bootloader is never a candidate

The bootloader sits at `0x0`, below every slot, and is never a candidate for any
lifecycle operation — not erased, not retired, not moved. `bootloaderAlwaysReachable()`
checks it on every table size, including after a full provision/erase/retire cycle.

There is always exactly one way back to a working board, and it does not depend on
any slot being provisioned.

## One antenna feed

This needs **no antenna hardware at all**, and that is structural rather than
lucky.

Only the running slot ever drives the SX1262. A slot that is not running is
storage, not a radio. Every slot therefore shares the one radio and the one
antenna because at most one is ever executing — so growing the slot count adds no
RF path, no multiplexer, no switch, and nothing that can drift out of calibration
over twenty summers.

## The rule that is easy to get wrong

Each slot brings its own filesystem partition, and the type is not arbitrary:

| Slot | Framework | Filesystem | Type |
|---|---|---|---|
| 0 | MeshCore | `fs_meshcore` | SPIFFS |
| 1 | Meshtastic | `fs_meshtastic` | LittleFS |
| 2+ | *reserved* | `fs_reticulum`, `fs_lorawan`, `fs_custom` | SPIFFS |

Meshtastic mounts LittleFS. MeshCore mounts SPIFFS. Handed the same partition,
each finds at boot a filesystem it does not recognise, reformats it, and the
other side's settings are gone — no error message, no way back.

Three independent places enforce this, on purpose:

- `SlotTable::validateFrameworks()` fails if any framework present is malformed,
  and if two frameworks ever share a filesystem label.
- `tests/test_partition_csv.cpp` asserts SPIFFS for MeshCore and LittleFS for
  Meshtastic on the actual shipped files.
- `tools/flash.py` refuses to write a table that pairs either side with the
  other's filesystem type.

`validateFrameworks()` judges **isolation, not presence**. A one-slot board is a
legitimate state — it runs MeshCore alone — so requiring Meshtastic's slot would
make the first successful provisioning look like a broken table.

## Shipped tables

Both are **generated** from `Provisioning.hpp` by `tools/gen_layouts.py`. CI
regenerates them and fails if anything changed, so the table a board gets and the
code that validates it cannot drift apart.

| File | Slots |
|---|---|
| `firmware/partitions/quadboot.csv` | 5 — MeshCore, Meshtastic, three reserved |
| `firmware/partitions/dualboot.csv` | 2 — nothing held back |

```
# Validate with: python tools/gate.py
```

## Adding a framework

1. Add a row to `kReservedFrameworks[]` in `firmware/src/SlotTable.cpp` — name,
   app label, filesystem label.
2. Add a sync word to `kSyncTable[]` in `firmware/src/ProtocolId.cpp` and a
   `Protocol` enumerator.
3. Widen the stride in `Provisioning.hpp` if 2 MB is not enough for the image.
4. `python tools/gen_layouts.py && python tools/gate.py`.

Steps 1 and 2 are one line each. That is the intended shape: adding a framework
should not mean touching the radio path.

## Alignment

Every app offset is 64 KB aligned. **This is not cosmetic.** The bootloader
refuses a misaligned app partition and reports nothing, so a board with one
simply appears dead with an empty serial log. The gate fails on a misaligned
slot, in the C++ validator, the Python validator, and the shipped-table test.
