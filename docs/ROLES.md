# Roles

What a slot on a Heltec V4 can be, and which combinations of slots are actually
worth deploying.

The authoritative list lives in code, not here — `kProfiles[]` in
`firmware/src/Roles.cpp` — because the panel, the CLI and this document must never
disagree about what is possible. This file explains the reasoning.

## The one thing to be clear about

**Slots are exclusive. One runs at a time, and the running slot owns the single SX1262.**

That is not a shortcoming of this implementation. It is what one radio is. Two
images cannot both drive the transceiver, and every role in the table below needs
the radio: a "companion" carries traffic over LoRa as well as BLE, which is
precisely what makes it useful, and the analyser needs the receiver promiscuous.

So five slots means five **installed, switchable** frameworks with their settings
preserved. Not five running ones.

The one thing that *does* serve both meshes simultaneously is the **bridge image**,
which is a single firmware containing both protocol stacks. That is the whole point
of the one-byte identifier in [PROTOCOL-ID.md](PROTOCOL-ID.md). Slots and the
bridge are not alternatives to each other — the bridge is one slot that happens to
serve two meshes.

## What a slot can be

| Framework | Role | Label | Radio | Identity |
|---|---|---|---|---|
| MeshCore | Repeater | `MC repeater` | exclusive | MeshCore node |
| MeshCore | Companion | `MC companion` | exclusive | MeshCore node |
| MeshCore | Room Server | `MC roomserver` | exclusive | MeshCore node |
| MeshCore | Room Client | `MC roomclient` | exclusive | MeshCore node |
| MeshCore | Sensor | `MC sensor` | exclusive | MeshCore node |
| Meshtastic | Router | `MT router` | exclusive | Meshtastic node |
| Meshtastic | Repeater | `MT repeater` | exclusive | Meshtastic node |
| Meshtastic | Client | `MT client` | exclusive | Meshtastic node |
| Meshtastic | Tracker | `MT tracker` | exclusive | Meshtastic node |
| Meshtastic | Sensor | `MT sensor` | exclusive | Meshtastic node |
| bridge | Repeater | `BR both-meshes` | exclusive | MeshCore node, **serves two meshes** |
| sniffer | Analyzer | `SN analyzer` | exclusive | analyser |
| custom | Config | `CX config` | **none** | none |

`CX config` is the only role that does not touch the radio. It is the one entry
where a second concurrent role would be genuinely possible, if the architecture ever
grew a second core to run it on.

## Which combinations are valid

`auditBoard()` answers this, and the tests in `tests/test_roles_system.cpp` pin
each case. There are three answers: fine, advisory, error.

### Fine — same provider, different roles

A MeshCore **room server** and a MeshCore **repeater** on one board is a normal,
sensible deployment. They are different nodes in the same identity namespace, so
nothing collides. The only finding is the inherent one-radio rule.

Same for a **companion** plus a **repeater**: a node that relays for the mesh and
can still be paired with a phone when it is carried somewhere.

A three-framework board — bridge image, Meshtastic router, MeshCore room server —
is equally fine. Different frameworks never collide, because their identity
namespaces are separate.

### Error — same provider, same role

Two MeshCore repeaters with the same name and key do **not** form a mesh of two.
They form one repeater that appears twice, and any path through it is ambiguous.
The same applies to two Meshtastic routers.

This is reported as an error rather than an advisory, and it is not enforced
automatically: during a migration an operator may genuinely want the old node
still installed. The tool's job is to say "these two will collide", not to forbid
it.

### Advisory — the analyser beside a relay

The analyser needs the receiver promiscuous, which is exactly what a repeater must
not have. Both slots are individually reasonable, but only one can be active, so
switching to the analyser silences the mesh and looks like a fault.

Reported as an advisory because it is a deployment choice rather than a mistake.

## A realistic five-slot board

| Slot | Purpose | Why |
|---|---|---|
| 0 | `BR both-meshes` | Serves Meshtastic and MeshCore from one radio |
| 1 | `MT router` | Standard Meshtastic presence for its own ecosystem |
| 2 | `MC roomserver` | Group-chat infrastructure on the MeshCore mesh |
| 3 | `SN analyzer` | `neohiro/lora-sniffer` for diagnosing the above |
| 4 | spare | Room for a framework that does not exist yet |

Audited, this reports one advisory — the analyser excludes the relays — and no
errors. Which is the correct answer: it is a coherent board with one real caveat
worth knowing before somebody switches to slot 3 and wonders why the mesh went
quiet.

A variant worth considering is dropping slot 0. The bridge image already serves
Meshtastic, so a dedicated Meshtastic router is only worth a slot if you want
Meshtastic's WiFi/MQTT bridging, which the bridge image does not do.