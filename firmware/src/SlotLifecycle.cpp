// SPDX-License-Identifier: MIT

#include "bridge/SlotLifecycle.hpp"

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
  const Partition* b = table.find("bootloader");
  if (b == nullptr) return false;
  if (b->size == 0) return false;
  // Below every slot, and not overlapping the first one.
  if (b->offset + b->size > kFirstSlotOffset) return false;
  for (const Partition& p : table.partitions()) {
    if (p.label == "bootloader") continue;
    if (p.type != PartType::App) continue;
    if (p.offset < b->offset + b->size && b->offset < p.offset + p.size) return false;
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
  // The filesystem goes too. Deliberately opt-in, because it is unrecoverable and
  // almost never what someone means by "the firmware is broken".
  r.detail = "app and settings erased";
  return r;
}

SlotOpResult retireSlot(const SlotTable& table, const DeviceState& state, std::uint8_t index) {
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