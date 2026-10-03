// SPDX-License-Identifier: MIT
//
// Slot lifecycle -- deleting and reflashing without touching anything else.
//
// Growth is the easy direction. The dangerous one is removal, because the whole
// safety argument rests on the table being append-only. The question "what if the
// operator wants one of these gone?" has to have an answer that does not
// invalidate that argument, or the argument is only true until somebody needs it.
//
// The answer has three parts:
//
//   1. **Erasing a slot's firmware is not deleting the slot.** Clearing a slot's
//      app and marking it unprovisioned leaves its partition row exactly where it
//      was. Nothing moves, nothing shrinks, and the freed space is simply not
//      used. This is the operation anybody actually wants 95% of the time -- a
//      bad flash, a firmware to replace, a node being repurposed -- and it is
//      completely safe.
//
//   2. **Erasing settings is a separate, explicit choice.** A slot's filesystem
//      survives an app erase by default, because "the firmware is broken" and
//      "throw away the channel keys and node database" are different requests.
//      Conflating them is how a user reflashes to recover from a bad image and
//      silently loses their mesh identity.
//
//   3. **Only the highest-numbered slot can be *retired*.** Reclaiming a slot in
//      the middle means moving every slot above it down into its place, which is
//      precisely the operation that would overwrite live firmware. So retirement
//      is allowed at the top and nowhere else, and `retireSlot()` refuses
//      otherwise with an explanation rather than doing the dangerous thing
//      quietly. Freed space is reclaimed by retiring trailing slots.
//
// The bootloader is below every slot and is never a candidate for any of this.
// There is always exactly one way back to a working board, and it does not
// depend on any slot being provisioned.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bridge/Provisioning.hpp"
#include "bridge/SlotTable.hpp"

namespace bridge {

// What a slot is for right now.
//
// `Empty` and `Retired` sound alike and are not the same: an Empty slot is
// available for provisioning and still holds a row in the table, while a Retired
// slot has had its row removed and its flash handed back.
enum class SlotState : std::uint8_t {
  Empty = 0,
  Provisioned,
  Retired,
};

const char* slotStateName(SlotState state);

// Mirrors the ESP-IDF `otadata` struct: a sequence number and the selected slot.
// The sequence number is what lets the bootloader reject an image that fails to
// boot and fall back, which is the whole reason the field exists.
struct BootRecord {
  std::uint32_t otaSeq = 0;
  std::uint8_t bootSlot = 0;
  std::uint8_t rollbackSlot = 0xFF;  // 0xFF = none

  // ESP-IDF writes 0xFFFFFFFF over a record once it has been consumed, so a
  // rollback target must never be 0xFF and bootSlot must never be 0xFF.
  bool valid() const { return bootSlot != 0xFF; }
};

// The mutable half of the board's configuration, kept in NVS rather than in the
// partition table.
//
// This split is deliberate. The partition table is geometry and is append-only;
// which slots happen to hold firmware is runtime state. Keeping them apart is
// what lets a slot be erased and rewritten without ever rewriting the table.
struct DeviceState {
  BootRecord boot;
  // Bit i set means slot i holds a firmware image that has not been erased.
  std::uint16_t provisionedMask = 0;

  bool isProvisioned(std::uint8_t index) const {
    if (index >= 16) return false;
    return (provisionedMask & (1u << index)) != 0;
  }
  void setProvisioned(std::uint8_t index, bool on) {
    if (index >= 16) return;
    if (on) {
      provisionedMask = static_cast<std::uint16_t>(provisionedMask | (1u << index));
    } else {
      provisionedMask = static_cast<std::uint16_t>(provisionedMask & ~(1u << index));
    }
  }
  std::uint8_t provisionedCount() const;
};

// State of one slot, as reported to the OLED and the CLI.
struct SlotStatus {
  std::uint8_t index = 0;
  SlotState state = SlotState::Empty;
  std::uint32_t offset = 0;
  std::uint32_t appSize = 0;
  std::uint32_t fsOffset = 0;
  std::uint32_t fsSize = 0;
  const char* framework = "";
  bool bootsByDefault = false;
};

// The result of a lifecycle operation.
//
// `touched` is the important field. Every operation that writes anything lists
// exactly which slots it wrote, so "deleting one slot leaves the others intact"
// is a checkable property of the return value rather than a claim in a comment.
struct SlotOpResult {
  bool ok = false;
  const char* detail = "";

  SlotTable table;       // changed only by retire
  DeviceState state;     // changed by every operation
  std::vector<std::uint8_t> touched;

  // Confirm the operation wrote nothing it should not have. `expected` is the
  // one slot that was supposed to change, or 0xFF for "none".
  bool touchedOnly(std::uint8_t expected) const;
};

SlotStatus slotStatus(const SlotTable& table, const DeviceState& state, std::uint8_t index);

// Every slot the table declares, for display.
std::vector<SlotStatus> allSlots(const SlotTable& table, const DeviceState& state);

// The slot the bootloader will enter next.
std::uint8_t activeSlot(const DeviceState& state);

// --- the reserved free slot -----------------------------------------------
//
// Every board reserves its final slot. One is enough: its whole job is to be the
// thing you can write when everything else is wrong. A board whose last free slot
// has been filled has no recovery path that does not involve a bench and physical
// access to a node that is probably on a mast.

// The name shown for it. Says what it is for rather than where it lives, because
// this string is what somebody reads while holding a screwdriver.
std::string freeSlotName(const char* hardware);

constexpr std::uint8_t kReservedFreeSlots = 1;

// True when this slot index may be provisioned at all. The highest slot in the
// table never may, so `provision()` refuses it by name rather than by accident.
bool slotIsProvisionable(const SlotTable& table, std::uint8_t index);

// The provisionable range: the table's slots minus the reserved one.
std::uint8_t provisionableSlots(const SlotTable& table);

// How many free slots remain. A correctly-configured board reports at least 1.
std::uint8_t freeSlots(const SlotTable& table, const DeviceState& state);

// --- operations ------------------------------------------------------------

// Write firmware into an empty slot. The next slot the operator should use.
SlotOpResult provision(const SlotTable& table, const DeviceState& state, std::uint8_t index,
                       std::uint32_t imageBytes);

// Overwrite the firmware in an already-provisioned slot. Settings are untouched.
//
// This is the common field repair: a node with a bad image is reflashed and comes
// back with its channel keys and node database intact, because a firmware problem
// is not a reason to forget the node's identity on the mesh.
SlotOpResult reflash(const SlotTable& table, const DeviceState& state, std::uint8_t index,
                     std::uint32_t imageBytes);

// Erase a slot's app, leaving its filesystem alone.
SlotOpResult eraseApp(const SlotTable& table, const DeviceState& state, std::uint8_t index);

// Erase a slot's app *and* its filesystem. Separate from eraseApp() on purpose.
SlotOpResult eraseSlot(const SlotTable& table, const DeviceState& state, std::uint8_t index);

// Remove a slot's rows from the table, handing its flash back.
//
// Refuses unless the index is the highest slot the table declares. That refusal
// is the design: compacting a gap would have to move every slot above it, which is
// precisely the operation that overwrites live firmware.
SlotOpResult retireSlot(const SlotTable& table, const DeviceState& state, std::uint8_t index);

// --- reclaiming the flash of a middle slot ---------------------------------
//
// This is the question that actually gets asked: "what happens if I uninstall
// something from the middle?" The answer is that the flash is not lost, and the
// fix is not compaction.
//
// # Why compaction is the wrong answer
//
// Removing a middle slot's rows from the partition table is itself completely safe --
// the rows of every other slot stay at their existing offsets and sizes, and
// nothing is moved or rewritten. That part is easy.
//
// What is *not* possible is putting a new numbered slot in the hole. Slot n's
// address is arithmetic, `kFirstSlotOffset + n * stride`, precisely so that
// growing the table can never relocate a partition holding live firmware. Fill a
// middle hole with a numbered slot and that guarantee is gone: the next append
// would compute an address from a count that no longer describes the layout, and
// the first user to be caught by that writes firmware over somebody's mesh.
//
// So a reclaimed slot's space is not re-numbered. It is re-purposed.
//
// # What the space becomes
//
// An **OTA staging region**, declared as a `reserved` partition covering the hole.
// That is precisely what a multi-slot bootloader needs and had nowhere to put: a
// place to receive the *next* image for a slot before switching to it. Reclaiming
// a slot you have finished with therefore pays for in-place updates of the slots
// you kept -- without any of them moving, and without needing a USB cable on a
// board that is on a mast.
//
// This is the same argument as reserving the free slot, pointed at a different
// problem: space that cannot be re-numbered is still space, and the useful thing to
// do with it is make the rest of the system need less of everything else.

constexpr const char* kOtaStagingLabel = "ota_stage";

struct ReclaimResult {
  bool ok = false;
  const char* detail = "";
  std::uint8_t index = 0;

  std::uint32_t freedOffset = 0;
  std::uint32_t freedBytes = 0;
  std::uint32_t totalFreeBytes = 0;

  SlotTable table;  // rebuilt with the slot gone and the hole declared as staging
};

// Reclaim an erased slot's flash as OTA staging space.
//
// Refused when the slot still holds firmware (which would destroy it), and when
// the slot is the reserved free one (which must stay empty and addressable).
ReclaimResult reclaimSlot(const SlotTable& table, const DeviceState& state,
                          std::uint8_t index);

// The staging region, or nullptr when nothing has been reclaimed. Its offset and
// size are what the flasher writes a new image into.
const Partition* otaStagingRegion(const SlotTable& table);

// Choose which slot the bootloader enters.
SlotOpResult setBootSlot(const SlotTable& table, const DeviceState& state, std::uint8_t index);

// Extend the table by one slot when there is room for one that is not declared.
SlotOpResult ensureRoom(const SlotTable& table, const DeviceState& state,
                        std::uint32_t flashSizeBytes);

// The bootloader is below every slot and is never retired, erased or moved, so
// there is always a way back. Returns false only if the geometry itself is wrong.
bool bootloaderAlwaysReachable(const SlotTable& table);

}  // namespace bridge