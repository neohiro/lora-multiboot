// SPDX-License-Identifier: MIT

#include "bridge/SlotLifecycle.hpp"

#include "bridge/Provisioning.hpp"

#include <cstdio>

namespace bridge {
namespace {

std::string appLabel(std::uint8_t index) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "ota_%u", static_cast<unsigned>(index));
  return std::string(buf);
}

SlotOpResult failure(const SlotTable& table, const DeviceState& state, const char* detail) {
  SlotOpResult r;
  r.ok = false;
  r.detail = detail;
  r.table = table;
  r.state = state;
  return r;
}

}  // namespace


std::string freeSlotName(const char* hardware) {
  // Named for what it is for, not for where it lives. This string is what somebody
  // reads while holding a screwdriver, so it says Free and it says what the board
  // is.
  return std::string("Free ") +
         (hardware != nullptr && hardware[0] != '\0' ? hardware : "LoRa") + " slot";
}

std::uint8_t provisionableSlots(const SlotTable& table) {
  const std::uint8_t declared = provisionedSlots(table);
  return declared > kReservedFreeSlots
             ? static_cast<std::uint8_t>(declared - kReservedFreeSlots)
             : 0;
}

bool slotIsProvisionable(const SlotTable& table, std::uint8_t index) {
  const std::uint8_t declared = provisionedSlots(table);
  if (declared == 0 || index >= declared) return false;
  // The last slot in the table is the recovery slot and stays empty.
  return index + kReservedFreeSlots < declared;
}

std::uint8_t freeSlots(const SlotTable& table, const DeviceState& state) {
  const std::uint8_t declared = provisionedSlots(table);
  std::uint8_t n = 0;
  for (std::uint8_t i = 0; i < declared; ++i) {
    if (!state.isProvisioned(i)) ++n;
  }
  return n;
}

const char* slotStateName(SlotState state) {
  switch (state) {
    case SlotState::Empty:
      return "empty";
    case SlotState::Provisioned:
      return "provisioned";
    case SlotState::Retired:
      return "retired";
  }
  return "?";
}

std::uint8_t DeviceState::provisionedCount() const {
  std::uint8_t n = 0;
  for (std::uint8_t i = 0; i < 16; ++i) {
    if ((provisionedMask & (1u << i)) != 0) ++n;
  }
  return n;
}

bool SlotOpResult::touchedOnly(std::uint8_t expected) const {
  if (expected == 0xFF) return touched.empty();
  return touched.size() == 1 && touched[0] == expected;
}

std::uint8_t activeSlot(const DeviceState& state) {
  return state.boot.valid() ? state.boot.bootSlot : 0xFF;
}

SlotStatus slotStatus(const SlotTable& table, const DeviceState& state, std::uint8_t index) {
  SlotStatus s;
  s.index = index;
  s.state = SlotState::Retired;
  s.framework = defaultFrameworkForSlot(index);

  const Partition* app = table.find(appLabel(index));
  const Partition* fs = table.find(slotFsLabel(index));
  if (app == nullptr) return s;  // no row: retired

  s.state = state.isProvisioned(index) ? SlotState::Provisioned : SlotState::Empty;
  s.offset = app->offset;
  s.appSize = app->size;
  s.bootsByDefault = activeSlot(state) == index;
  if (fs != nullptr) {
    s.fsOffset = fs->offset;
    s.fsSize = fs->size;
  }
  return s;
}

std::vector<SlotStatus> allSlots(const SlotTable& table, const DeviceState& state) {
  std::vector<SlotStatus> out;
  for (std::uint8_t i = 0; i < maxSlotsForFlash(); ++i) {
    const SlotStatus s = slotStatus(table, state, i);
    if (s.state == SlotState::Retired) continue;
    out.push_back(s);
  }
  return out;
}

bool bootloaderAlwaysReachable(const SlotTable& table) {
  // The bootloader is not a declared partition -- ESP-IDF's generator rejects any
  // row below 0x9000, and the bootloader lives at 0x0. It is written separately by
  // esptool, so its geometry comes from the same constants the layout and the
  // updater use.
  if (kBootloaderSize == 0) return false;
  if (kBootloaderOffset + kBootloaderSize > kFirstSlotOffset) return false;

  // And nothing that does exist in the table may overlap it.
  const std::uint64_t bootEnd = kBootloaderOffset + kBootloaderSize;
  for (const Partition& p : table.partitions()) {
    if (p.offset < bootEnd && kBootloaderOffset < p.offset + p.size) return false;
  }
  return true;
}

// --- operations ------------------------------------------------------------

SlotOpResult provision(const SlotTable& table, const DeviceState& state, std::uint8_t index,
                       std::uint32_t imageBytes) {
  const Partition* app = table.find(appLabel(index));
  if (app == nullptr) {
    return failure(table, state, "no such slot in the partition table");
  }
  // The final slot is the recovery slot and stays empty on purpose. A board whose
  // last free slot has been filled has no way back: the flasher cannot write a fixed
  // image, the bootloader has no rescue path, and the owner needs physical access to
  // a node that is probably on a mast.
  if (!slotIsProvisionable(table, index)) {
    return failure(table, state,
                   "this is the reserved free slot; it stays empty so the board can "
                   "always be recovered");
  }
  if (state.isProvisioned(index)) {
    return failure(table, state, "slot already holds firmware; use reflash");
  }
  // An image that does not fit would be silently truncated by the programmer, and
  // a truncated app is an unbootable slot that still looks provisioned.
  if (imageBytes == 0 || imageBytes > app->size) {
    return failure(table, state, "image does not fit this slot");
  }

  SlotOpResult r;
  r.ok = true;
  r.detail = "provisioned";
  r.table = table;
  r.state = state;
  r.state.setProvisioned(index, true);
  r.state.boot.otaSeq += 1;
  // The first firmware on a blank board becomes the default boot target, because
  // otherwise the board has nothing to boot into.
  if (!r.state.boot.valid()) {
    r.state.boot.bootSlot = index;
    r.state.boot.rollbackSlot = 0xFF;
  }
  r.touched.push_back(index);
  return r;
}

SlotOpResult reflash(const SlotTable& table, const DeviceState& state, std::uint8_t index,
                     std::uint32_t imageBytes) {
  const Partition* app = table.find(appLabel(index));
  if (app == nullptr) {
    return failure(table, state, "no such slot in the partition table");
  }
  if (!state.isProvisioned(index)) {
    return failure(table, state, "slot is empty; provision it instead");
  }
  if (imageBytes == 0 || imageBytes > app->size) {
    return failure(table, state, "image does not fit this slot");
  }

  SlotOpResult r;
  r.ok = true;
  // Settings survive on purpose. A bad firmware is not a reason to forget the
  // node's channel keys, and the filesystem partition is simply not written.
  r.detail = "reflashed, settings kept";
  r.table = table;
  r.state = state;
  r.state.boot.otaSeq += 1;
  r.touched.push_back(index);
  return r;
}

SlotOpResult eraseApp(const SlotTable& table, const DeviceState& state, std::uint8_t index) {
  if (table.find(appLabel(index)) == nullptr) {
    return failure(table, state, "no such slot in the partition table");
  }
  if (!state.isProvisioned(index)) {
    return failure(table, state, "slot is already empty");
  }

  SlotOpResult r;
  r.ok = true;
  r.detail = "app erased, settings kept";
  r.table = table;
  r.state = state;
  r.state.setProvisioned(index, false);

  // Never leave the boot record pointing at a slot with nothing in it.
  if (r.state.boot.bootSlot == index) {
    r.state.boot.rollbackSlot = index;
    // Fall back to the lowest provisioned slot, or the same one marked empty so
    // the operator lands somewhere real rather than in a boot loop.
    std::uint8_t next = 0xFF;
    for (std::uint8_t i = 0; i < 16; ++i) {
      if (r.state.isProvisioned(i)) {
        next = i;
        break;
      }
    }
    r.state.boot.bootSlot = next;
  }
  r.touched.push_back(index);
  return r;
}

SlotOpResult eraseSlot(const SlotTable& table, const DeviceState& state, std::uint8_t index) {
  SlotOpResult r = eraseApp(table, state, index);
  if (!r.ok) return r;
  // The settings wipe is performed by the flasher (`flash.py app N
  // --erase-settings`), not by the device: erasing a filesystem partition is flash
  // I/O, and firmware cannot sanely do it to the partition it is running out of. So
  // the device state after this call is the same as eraseApp -- what differs is the
  // operator's stated intent, which is recorded here so a UI can show that the
  // identity is going as well as the image.
  r.detail = "app erased; settings wipe requested";
  return r;
}

SlotOpResult retireSlot(const SlotTable& table, const DeviceState& state, std::uint8_t index) {
  // Checked before anything else, because it applies regardless of which slot is
  // named and is the more actionable of the two answers. Consistent with
  // growTable(), which refuses for the same reason: the geometry is rebuilt from a
  // slot count and cannot carry a staging region with it, so retiring here would
  // silently destroy an OTA area somebody is relying on.
  if (otaStagingRegion(table) != nullptr) {
    return failure(table, state,
                   "an OTA staging region is in use; reclaim it explicitly rather than "
                   "losing it to a table rebuild");
  }

  const Partition* app = table.find(appLabel(index));
  if (app == nullptr) {
    return failure(table, state, "no such slot to retire");
  }

  // The rule that keeps the append-only guarantee intact. Compacting a hole would
  // mean moving every slot above it down into the gap, overwriting live firmware.
  const std::uint8_t top = provisionedSlots(table);
  if (index + 1 != top) {
    return failure(table, state,
                   "only the highest slot can be retired; retiring a lower one would "
                   "move every slot above it");
  }
  if (state.isProvisioned(index)) {
    return failure(table, state, "slot still holds firmware; erase it first");
  }

  // Rebuild with one fewer slot. Same arithmetic, so every surviving row keeps its
  // exact offset and size.
  SlotOpResult r;
  r.ok = true;
  r.detail = "retired";
  r.state = state;
  r.table = SlotTable::parse(renderSlots(top - 1), table.flashSizeBytes());
  r.touched.push_back(index);
  return r;
}

const Partition* otaStagingRegion(const SlotTable& table) { return table.find(kOtaStagingLabel); }

ReclaimResult reclaimSlot(const SlotTable& table, const DeviceState& state,
                          std::uint8_t index) {
  ReclaimResult r;
  r.index = index;

  const auto refuse = [&](const char* why) {
    r.ok = false;
    r.detail = why;
    r.table = table;
    return r;
  };

  const Partition* app = table.find(appLabel(index));
  if (app == nullptr) return refuse("no such slot to reclaim");

  // Erasing and reclaiming are separate requests, and conflating them is how
  // somebody removes a framework and loses the node's identity at the same time.
  if (state.isProvisioned(index)) {
    return refuse("slot still holds firmware; erase it first, deliberately");
  }

  const Partition* fs = table.find(slotFsLabel(index));
  if (fs != nullptr && fs->size != 0) {
    // The slot is not provisioned (checked above), but the filesystem partition
    // exists and has size. This is the normal case for a slot that was erased
    // with --erase (firmware erased, settings kept). We allow reclaiming it.
    // The hole will be declared as an OTA staging region.
  }

  // If a staging region already exists it would collide with this hole, so the
  // operation is refused rather than silently producing an overlapping table. Checked
  // before the reserve rule, because it is the more specific of the two answers.
  if (otaStagingRegion(table) != nullptr) {
    return refuse("an OTA staging region already exists; retire the top slot instead");
  }

  // The reserve has to stay addressable and empty. Reclaiming it would leave a board
  // with no free slot at all, which is the situation everything here exists to avoid.
  const std::uint8_t declared = provisionedSlots(table);
  if (index + kReservedFreeSlots >= declared) {
    return refuse("this is the reserved free slot; it must stay available");
  }

  const std::uint32_t fsOffset = fs != nullptr ? fs->offset : app->offset + app->size;
  const std::uint32_t fsSize = fs != nullptr ? fs->size : 0;
  const std::uint32_t holeStart = app->offset;
  const std::uint32_t holeEnd = fs != nullptr ? fsOffset + fsSize : app->offset + app->size;

  // Rebuild the table with this slot's rows dropped and the hole declared as a
  // staging partition. Every other slot's rows are emitted unchanged, which is the
  // whole safety argument: there is no arithmetic here that could move anything.
  //
  // The system rows come from the same constants Provisioning.hpp owns, not from
  // literals written out here. Restating them meant a moved system region produced
  // a reclaimed table that disagreed with the shipped layout and with every other
  // subsystem -- a table that parsed, validated, and was wrong.
  //
  // The bootloader and partition table are absent for a different reason: ESP-IDF's
  // generator rejects any declared partition below 0x9000, so they are not rows at
  // all. SystemUpdate knows their geometry as constants.
  std::string csv;
  {
    char sys[160];
    std::snprintf(sys, sizeof(sys),
                  "%-14s, data, %-9s, 0x%X, 0x%X,\n"
                  "%-14s, data, %-9s, 0x%X, 0x%X,\n"
                  "%-14s, data, %-9s, 0x%X, 0x%X,\n",
                  "nvs", "nvs", kNvsOffset, kNvsSize,
                  "otadata", "ota", kOtadataOffset, kOtadataSize,
                  "coredump", "coredump", kCoredumpOffset, kCoredumpSize);
    csv += sys;
  }

  for (std::uint8_t i = 0; i < declared; ++i) {
    if (i == index) continue;  // the hole
    const Partition* a = table.find(appLabel(i));
    const Partition* f = table.find(slotFsLabel(i));
    if (a != nullptr) {
      char row[96];
      std::snprintf(row, sizeof(row), "%-14s, app , %-9s, 0x%X, 0x%X,\n", a->label.c_str(),
                    a->subType == PartSubType::Factory   ? "factory "
                    : a->subType == PartSubType::Ota_0    ? "ota_0   "
                    : a->subType == PartSubType::Ota_1    ? "ota_1   "
                    : a->subType == PartSubType::Ota_2    ? "ota_2   "
                    : a->subType == PartSubType::Ota_3    ? "ota_3   "
                    : a->subType == PartSubType::Ota_4    ? "ota_4   "
                                                            : "ota_0   ",
                    a->offset, a->size);
      csv += row;
    }
    if (f != nullptr) {
      char row[96];
      std::snprintf(row, sizeof(row), "%-14s, data, %-9s, 0x%X, 0x%X,\n", f->label.c_str(),
                    f->subType == PartSubType::LittleFs ? "littlefs" : "spiffs  ",
                    f->offset, f->size);
      csv += row;
    }
  }

  // The reclaimed hole, held for OTA staging.
  //
  // Deliberately `undefined` (0x06) rather than a filesystem type. This region is
  // written with esptool and never mounted, so claiming SPIFFS or LittleFS would
  // assert a format nothing here implements. It is also the honest reading: a data
  // partition whose purpose is deliberately unspecified by the filesystem layer.
  // "reserved" is not an ESP-IDF subtype name and the real generator rejects it.
  char stage[96];
  std::snprintf(stage, sizeof(stage), "%-14s, data, undefined, 0x%X, 0x%X,\n",
                kOtaStagingLabel, holeStart, holeEnd - holeStart);
  csv += stage;

  TableReport pr;
  const SlotTable rebuilt = SlotTable::parse(csv, table.flashSizeBytes(), &pr);
  if (!pr.ok()) return refuse("the rebuilt table did not parse");
  const TableReport v = rebuilt.validate();
  if (!v.ok()) return refuse("the rebuilt table would be invalid");

  // Prove the invariant instead of asserting it in a comment: every surviving slot
  // must still be at exactly the offset and size it had.
  for (std::uint8_t i = 0; i < declared; ++i) {
    if (i == index) continue;
    const Partition* before = table.find(appLabel(i));
    const Partition* after = rebuilt.find(appLabel(i));
    if (before == nullptr || after == nullptr || before->offset != after->offset ||
        before->size != after->size) {
      return refuse("rebuilding would relocate a surviving slot");
    }
  }

  // The same argument for the system region. This used to be taken on trust: the
  // rows were written as literals above, nothing compared them to what came in,
  // and a mismatch here is a node whose settings partition has quietly moved.
  for (std::uint8_t s = 0; s < 3; ++s) {
    const char* label = s == 0 ? "nvs" : (s == 1 ? "otadata" : "coredump");
    const Partition* before = table.find(label);
    const Partition* after = rebuilt.find(label);
    if (before == nullptr) continue;  // not in the input table; nothing to preserve
    if (after == nullptr || before->offset != after->offset ||
        before->size != after->size) {
      return refuse("rebuilding would move a system partition");
    }
  }

  r.ok = true;
  r.detail = "reclaimed as OTA staging space";
  r.freedOffset = holeStart;
  r.freedBytes = holeEnd - holeStart;
  r.table = rebuilt;
  r.totalFreeBytes = rebuilt.freeBytes();
  return r;
}

SlotOpResult setBootSlot(const SlotTable& table, const DeviceState& state, std::uint8_t index) {
  if (table.find(appLabel(index)) == nullptr) {
    return failure(table, state, "no such slot in the partition table");
  }
  if (!state.isProvisioned(index)) {
    return failure(table, state, "cannot boot a slot with no firmware in it");
  }

  SlotOpResult r;
  r.ok = true;
  r.detail = "boot slot set";
  r.table = table;
  r.state = state;
  r.state.boot.rollbackSlot = state.boot.bootSlot;
  r.state.boot.bootSlot = index;
  r.state.boot.otaSeq += 1;
  // Selecting a slot is not a write to that slot's flash, so nothing is touched.
  return r;
}

SlotOpResult ensureRoom(const SlotTable& table, const DeviceState& state,
                        std::uint32_t flashSizeBytes) {
  const std::uint8_t declared = provisionedSlots(table);
  const std::uint8_t room = maxSlotsForFlash(flashSizeBytes);

  SlotOpResult r;
  r.ok = true;
  r.detail = "at capacity";
  r.table = table;
  r.state = state;

  if (declared >= room) {
    r.detail = "at capacity";
    return r;
  }
  // Another slot exists in the hardware but not yet in the table: grow.
  TableReport gr;
  const SlotTable grown = growTable(table, flashSizeBytes, &gr);
  if (!gr.ok()) {
    r.ok = false;
    r.detail = gr.detail;
    return r;
  }
  r.table = grown;
  r.detail = "opened the next slot";
  // Extending the table writes no slot's firmware.
  return r;
}

}  // namespace bridge