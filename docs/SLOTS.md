# Slots: the reserve, and getting space back

Two questions that come up the moment a board holds more than one framework, both
answered here because both have a non-obvious right answer.

## One slot is always free

Every board reserves its **final slot**. It is called the **Free \<hardware\> slot**
— "Free Heltec LoRa 32 V4 slot" — because that is the name an operator reads while
holding a screwdriver, and it should say what it is for. A slot called `ota_4` tells
nobody anything.

It cannot be provisioned. `provision()` refuses it by name:

```
this is the reserved free slot; it stays empty so the board can always be recovered
```

The reason is mundane and worth stating plainly: a board whose last free slot has
been filled has no obvious place to write a fixed image, no obvious place to stage
an update, and no way for the flasher to tell a blank board from a full one. The
person who discovers that is standing on a ladder.

So the usable count is **N−1**:

| Layout | Slots | Usable | Always free |
|---|---|---|---|
| `quadboot.csv` | 5 | 4 | `ota_4` |
| `dualboot.csv` | 3 | 2 | `ota_2` |

`dualboot.csv` carries three slots rather than two precisely so that it still
delivers the two firmwares its name promises.

## Uninstalling something from the middle

> *"I want to remove an app from slot 2. Do I lose memory, and do I break the
> board?"*

**Neither.** And the reason is worth understanding, because the obvious fix is the
wrong one.

### Compaction is the wrong answer

The tempting fix is to close the gap: move slot 3 down into slot 2's space, move
slot 4 down into slot 3's, and so on. That reclaims everything and is exactly how a
repartition destroys somebody's firmware. It would also break the one invariant
this project depends on.

Slot *n*'s address is arithmetic — `0x30000 + n × 0x300000` — precisely so that
growing the table can **never** relocate a partition holding live firmware. That
property is what makes "add a framework" a safe routine operation rather than
something that needs a backup. Fill a middle hole with a numbered slot and the
guarantee is gone: the next append computes an address from a count that no longer
describes the layout, and the first person caught by that writes firmware over a
running mesh.

So a reclaimed slot does **not** become slot *n* again.

### What it becomes instead

An **OTA staging region**: a `reserved` partition covering exactly the freed hole.
That is what a multi-slot bootloader needs and had nowhere to put — somewhere to
receive the *next* image for a slot before switching to it.

So reclaiming a framework you have finished with pays for in-place updates of the
frameworks you kept, without any of them moving, and without a USB cable on a board
that is on a mast.

```bash
# 1. erase the firmware, keeping its settings, deliberately
python tools/flash.py app 2 --erase

# 2. hand the 3 MB back to the system as staging space
#    (a firmware CLI call: reclaim slot 2)
```

### What reclaim refuses, and why

| Refused | Reason |
|---|---|
| A slot that still holds firmware | Erasing and reclaiming are separate acts. Conflating "remove this framework" with "forget this node's identity" is how somebody loses a channel key by accident. |
| The reserved free slot | A board with no free slot is the situation all of this exists to prevent. |
| A second reclaim | Only one staging region can exist; a second hole would overlap. Retire the top slot instead. |

### The one real trade

**Growth is refused once a staging region exists.** Growth rebuilds geometry
arithmetically and would put a numbered slot straight back on top of the hole, so
`growTable()` returns an error rather than an overlapping table.

That is a deliberate trade and the right way round: deferring growth is a decision
an operator can make on their own schedule, whereas a partition table that overlaps
is a brick. Retiring the top slot clears the way and the staging region can then be
reclaimed in its place.

## Seeing every slot as a live app

A partition is not an application. `SlotInventory` presents each slot as a thing
with a name, a state, a role and a purpose:

```
slots=5 active=0 free=3 recovery=yes
slot=0 name=ota_0 state=live role=MC repeater off=0x30000 size=2097152 fs=0x230000 ...
slot=4 name=Free Heltec LoRa 32 V4 slot state=empty role=unknown ...
```

It is a deliberately hand-rolled line format, not JSON. A node that cannot answer a
request for its firmware version must still be able to answer "what is installed",
and a JSON writer on a microcontroller costs real flash and RAM for a format a host
tool can read with two lines of `split()`.

Fields a connected machine will want, and which are reserved now so adding them is
not a breaking change: `firmwareVersion`, `lastLogLine`, `lastSeenMs` and
`bootFailures`. The host-side UI and log collection are the obvious next step; the
wire format is settled so they can be built against it.

Which framework a slot holds is **device state, not geometry**. The partition table
knows sizes and offsets and nothing about what is installed, so until a slot reports
its own identity the role reads `unknown`. Guessing would be the same mistake the
air identifier exists to avoid.

## One radio still

All of the above is about storage. It does not change the fundamental constraint,
which is restated in [ROLES.md](ROLES.md): **one slot runs at a time and owns the
SX1262.** Five installed slots are five switchable frameworks with settings
preserved, not five running ones. The thing that serves both meshes simultaneously
is the bridge image, because it carries both protocol stacks in one image.