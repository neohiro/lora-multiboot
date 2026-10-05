// SPDX-License-Identifier: MIT
//
// Provisioning -- one board, growing from nothing, without ever being refitted.
//
// The problem this solves. A fixed multi-slot partition table has to choose, at
// manufacture, how many frameworks a board will ever run. Choose too few and a
// later framework needs a repartition, which costs every existing user their
// settings and a trip to the rooftop. Choose too many and every board carries
// dead partitions forever, which is what reserving five slots up front amounts
// to.
//
// The alternative is to let the layout grow as the board is commissioned, and to
// make growth safe enough that nobody has to think about it for the next twenty
// years. That is this module.
//
// Three rules make it maintenance-free:
//
//   1. **Append-only.** A slot, once allocated, never moves and never changes
//      size. Growth only ever appends. This is what makes rewriting the
//      partition table safe: a table that only grows can be rewritten in place,
//      while any scheme that relocates a slot would orphan live firmware.
//
//   2. **A fixed grid.** Slot n starts at a 64 KB-aligned offset computed purely
//      from n. Position is arithmetic, not bookkeeping, so nothing has to be
//      remembered across reboots, upgrades or a factory reset of the table
//      itself. Grid misalignment is also what makes a bootloader refuse to boot,
//      so the rule is not negotiable.
//
//   3. **Transport-independent.** USB, BLE and WiFi are interchangeable labels on
//      the same provisioning service. Adding a fourth transport is an
//      enumeration entry, not a redesign.
//
// On "one antenna feed": this needs no antenna hardware whatsoever, and that is
// structural rather than lucky. Only the running slot ever drives the SX1262, and
// a slot that is not running is storage, not a radio. Every slot shares the one
// radio and the one antenna because at most one is ever executing, so growing
// the slot count adds no RF path, no mux, no switch, and nothing to keep in
// calibration.

// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bridge/SlotTable.hpp"

namespace bridge {

// How a provisioning session is reached. All three present the same service and
// the same state machine; only the transport differs.
enum class Transport : std::uint8_t {
  Usb,
  Ble,
  Wifi,
};

const char* transportName(Transport transport);

// The three states a board can be in, derived rather than stored wherever
// possible so a fresh board with a blank flash lands in a defined place instead
// of an undefined one.
enum class ProvisionState : std::uint8_t {
  // Nothing valid anywhere. The flash has never been provisioned, or has been
  // erased. This is where a brand new board arrives and where a factory-reset
  // board returns to.
  ZeroBoot = 0,
  // At least one slot holds firmware, and the next slot is open and waiting.
  Provisioning,
  // Every slot in the table holds firmware. Nothing further can be added without
  // a repartition, which is a deliberate decision rather than a surprise.
  Full,
};

const char* provisionStateName(ProvisionState state);

// Slot geometry. One uniform stride for every slot, so slot n's address is
// arithmetic rather than bookkeeping.
//
// Each slot is app-then-filesystem, both fixed size. Uniformity is what makes
// growth safe: with a ragged layout, where a slot's address depends on which
// other slots exist, growing the table can move a live partition. With a fixed
// stride, slot n is at the same address whether there are one slots or sixteen,
// so a table rewrite is provably append-only and provably cannot orphan firmware.
//
// 2 MB of app fits the ~1.2 MB images both projects produce with room to grow,
// and 1 MB of filesystem is ample for channel keys and a node database.
constexpr std::uint32_t kSlotStrideBytes = 0x300000;
constexpr std::uint32_t kSlotAppBytes = 0x200000;
constexpr std::uint32_t kSlotFsBytes = 0x100000;

// The region below the first slot.
//
// Only the three rows that are genuinely *partitions* live here. The bootloader
// and the partition table are NOT declared as rows, and this is not a style
// choice -- it is a hard requirement of ESP-IDF's generator.
//
//   gen_esp32part.py starts the first declared partition at
//   CONFIG_PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE = 0x8000 + 0x1000 = 0x9000,
//   and rejects any row below that. It also skips only rows whose *type* is
//   `bootloader` or `partition_table`, so declaring the bootloader as an `app` row
//   at 0x0 is rejected outright.
//
// The bootloader is written separately (esptool `--flash-image ... 0x1000`) and the
// table is the generator's own output. Declaring them as partitions produced a
// table that passed every check here and was refused by the real tool.
//
// Offsets and sizes are still known, because the system layer has to be updatable:
// SystemUpdate falls back to these constants when the table has no row for the
// piece.
constexpr std::uint32_t kBootloaderOffset = 0x0;
constexpr std::uint32_t kPartitionTableOffset = 0x8000;
constexpr std::uint32_t kPartitionTableSize = 0x1000;
constexpr std::uint32_t kBootloaderSize = kPartitionTableOffset - 0x1000;  // 0x7000

// First declared partition. 0x9000 is the floor ESP-IDF enforces.
constexpr std::uint32_t kNvsOffset = 0x9000;
constexpr std::uint32_t kNvsSize = 0xA000;
constexpr std::uint32_t kOtadataOffset = 0x13000;
constexpr std::uint32_t kOtadataSize = 0x2000;
constexpr std::uint32_t kCoredumpOffset = 0x15000;
constexpr std::uint32_t kCoredumpSize = 0x10000;

// Where slot 0 begins. Above the whole system region and 64 KB aligned.
constexpr std::uint32_t kFirstSlotOffset = 0x30000;

// A board that must never be bricked by growth.
constexpr std::uint32_t kDefaultFlashBytes = 16u * 1024u * 1024u;

// Hard ceiling on slot count, well above what any flash here can hold. It exists
// so the index arithmetic cannot run away, not to limit ambition. The real limit
// is always maxSlotsForFlash().
constexpr std::uint8_t kMaxSlots = 16;

// How many slots this much flash can hold, derived rather than declared.
//
// A slot must fit *entirely*, so this is the number of whole strides between the
// first slot and the end of flash. Deriving it means an 8 MB V3 is handled by the
// same code as a 16 MB V4 instead of by whoever remembered to change a constant.
std::uint8_t maxSlotsForFlash(std::uint32_t flashSizeBytes = kDefaultFlashBytes);

// Offset of slot `index`. Pure arithmetic: no table, no state, no bookkeeping.
std::uint32_t slotOffset(std::uint8_t index);

// Filesystem offset for slot `index`.
std::uint32_t slotFsOffset(std::uint8_t index);

// The framework a newly opened slot should be provisioned with.
//
// The first slot is MeshCore, deliberately: this is the image that knows it
// shares a radio, so the radio is in the hands of firmware that can reason about
// both meshes from the first boot rather than after a later reflash. Meshtastic
// is stock firmware and needs no such awareness, which is why it opens second.
const char* defaultFrameworkForSlot(std::uint8_t index);

// Filesystem label and type for slot `index`.
const char* slotFsLabel(std::uint8_t index);
bool slotFsIsLittleFs(std::uint8_t index);

// Which slot is offered next, given how many are already provisioned.
//
// Deliberately `count`, never `count - 1` and never a stored "next" pointer: a
// derived value cannot drift out of step with reality after an interrupted
// flash, a factory reset or a table that was rewritten by an older bootloader.
std::uint8_t nextSlotIndex(std::uint8_t provisionedCount);

// One connection offered to the operator.
struct Endpoint {
  Transport transport = Transport::Usb;
  std::uint8_t slot = 0;
  const char* framework = "";
  const char* label = "";  // e.g. "MC  ota_0"
};

// What a board presents, in every state.
//
// This is the heart of the brief: a board with nothing on it offers exactly one
// connection and behaves as though one slot were the only option, because at that
// moment it is. Once that slot is provisioned the next boot offers two, and so on
// up to the cap. The operator connects to whichever slot they mean to service;
// each is independently reachable and none of them can see the others.
struct ProvisioningView {
  ProvisionState state = ProvisionState::ZeroBoot;
  std::uint8_t provisionedCount = 0;
  std::uint8_t offeredCount = 0;
  std::vector<Endpoint> endpoints;
  std::uint8_t maxSlots = kMaxSlots;
};

ProvisioningView provisionView(std::uint8_t provisionedCount,
                               Transport transport = Transport::Usb,
                               std::uint8_t maxSlots = kMaxSlots);

// Convenience: the single connection a virgin board offers.
ProvisioningView zeroBootView(Transport transport = Transport::Usb);

// Grow a partition table by one slot, append-only.
//
// `existing` is the table as it stands. Returns the new table. Guarantees, and
// these are the whole point:
//
//   - every pre-existing partition keeps its exact offset and size;
//   - the new app partition is 64 KB aligned;
//   - nothing runs past the end of the flash;
//   - the table is only ever added to.
//
// A table that cannot grow without moving something is refused, not repaired.
// An earlier scheme that relocated slots would work until the day a user's
// firmware was written over by a repartition, and that day is exactly the day
// somebody is standing on a ladder.
SlotTable growTable(const SlotTable& existing, std::uint32_t flashSizeBytes,
                    TableReport* report = nullptr);

// Render a layout of exactly `slots` slots as ESP-IDF partition CSV.
//
 // This is the canonical form: slot n's rows depend only on n, so the CSV for
 // four slots contains the three-slot CSV verbatim plus one more slot. That is
 // what makes the table safe to rewrite in place.
std::string renderSlots(std::uint8_t slots);

// Render a parsed table back to CSV.
std::string renderCsv(const SlotTable& table);

// Count how many app slots a table currently declares. This is the one number
// that drives the whole state machine.
std::uint8_t provisionedSlots(const SlotTable& table);

}  // namespace bridge
