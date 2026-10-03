// SPDX-License-Identifier: MIT

#include "bridge/Inventory.hpp"

#include <cstdio>

namespace bridge {
namespace {

std::string appLabel(std::uint8_t index) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "ota_%u", static_cast<unsigned>(index));
  return std::string(buf);
}

}  // namespace

SlotInventory inventory(const SlotTable& table, const DeviceState& state,
                        const char* hardware) {
  SlotInventory inv;
  const std::uint8_t declared = provisionedSlots(table);
  inv.activeSlot = activeSlot(state);
  inv.freeCount = freeSlots(table, state);

  for (std::uint8_t i = 0; i < declared; ++i) {
    LiveSlot s;
    s.index = i;

    const bool free = (i + kReservedFreeSlots >= declared) && !state.isProvisioned(i);
    s.isFreeSlot = free;
    s.name = free ? freeSlotName(hardware) : appLabel(i);

    const Partition* app = table.find(appLabel(i));
    const Partition* fs = table.find(slotFsLabel(i));
    if (app != nullptr) {
      s.offset = app->offset;
      s.sizeBytes = app->size;
    }
    if (fs != nullptr) {
      s.fsOffset = fs->offset;
      s.fsSize = fs->size;
    }

    s.state = state.isProvisioned(i) ? SlotState::Provisioned : SlotState::Empty;
    s.bootsByDefault = inv.activeSlot == i;

    // Which application a slot holds is device state, not geometry: the partition
    // table knows the sizes and the offsets, and nothing about what is installed.
    // Until a slot reports its own identity, the framework is simply unknown -- and
    // saying "unknown" is far better than guessing, which is the same principle the
    // identifier follows on the air.
    RoleProfile role;
    if (free) {
      s.framework = Framework::Custom;
      s.role = Role::Config;
      s.purposeKnown = false;
    } else {
      s.framework = Framework::Custom;
      s.role = Role::Config;
      s.purposeKnown = false;
    }
    (void)role;

    inv.slots.push_back(s);
  }

  // Recovery is possible when something is still free to write into. A board with
  // every slot filled has no path back that does not involve a bench.
  inv.recoveryPossible = false;
  for (std::uint8_t i = 0; i < declared; ++i) {
    if (slotIsProvisionable(table, i) && !state.isProvisioned(i)) {
      inv.recoveryPossible = true;
      break;
    }
  }
  return inv;
}

std::vector<std::string> inventoryLines(const SlotInventory& inv) {
  std::vector<std::string> out;
  out.reserve(inv.slots.size() + 1);

  for (const LiveSlot& s : inv.slots) {
    char line[224];
    const char* state = s.state == SlotState::Provisioned ? "live" : "empty";
    const RoleProfile* known = nullptr;
    RoleProfile tmp;
    if (s.purposeKnown && roleProfile(s.framework, s.role, &tmp)) known = &tmp;

    std::snprintf(line, sizeof(line),
                  "slot=%u name=%s state=%s role=%s off=0x%X size=%u fs=0x%X fs_size=%u "
                  "boot=%u free=%u ver=%s seen=%u fails=%u",
                  static_cast<unsigned>(s.index), s.name.c_str(), state,
                  known != nullptr ? known->label : "unknown", s.offset,
                  static_cast<unsigned>(s.sizeBytes), s.fsOffset,
                  static_cast<unsigned>(s.fsSize), s.bootsByDefault ? 1u : 0u,
                  s.isFreeSlot ? 1u : 0u,
                  s.firmwareVersion.empty() ? "-" : s.firmwareVersion.c_str(),
                  static_cast<unsigned>(s.lastSeenMs),
                  static_cast<unsigned>(s.bootFailures));
    out.push_back(std::string(line));
  }

  char head[128];
  std::snprintf(head, sizeof(head), "slots=%u active=%u free=%u recovery=%s",
                static_cast<unsigned>(inv.slots.size()),
                static_cast<unsigned>(inv.activeSlot),
                static_cast<unsigned>(inv.freeCount), inv.recoveryPossible ? "yes" : "no");
  out.insert(out.begin(), std::string(head));
  return out;
}

std::string inventorySummary(const SlotInventory& inv) {
  std::uint8_t live = 0;
  std::uint8_t empty = 0;
  for (const LiveSlot& s : inv.slots) {
    if (s.state == SlotState::Provisioned) ++live;
    else ++empty;
  }
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%u slots: %u live, %u empty, %u free -- recovery %s",
                static_cast<unsigned>(inv.slots.size()), static_cast<unsigned>(live),
                static_cast<unsigned>(empty), static_cast<unsigned>(inv.freeCount),
                inv.recoveryPossible ? "OK" : "IMPOSSIBLE");
  return std::string(buf);
}

}  // namespace bridge