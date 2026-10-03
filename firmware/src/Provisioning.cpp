// SPDX-License-Identifier: MIT

#include "bridge/Provisioning.hpp"

#include <cstdio>

namespace bridge {
namespace {

struct FsSpec {
  const char* label;
  bool littlefs;
};

// Per-slot filesystem identity. Slot 0 is MeshCore (SPIFFS) and slot 1 is
// Meshtastic (LittleFS); everything past that is space for a framework that does
// not exist yet, so it is named but has no opinion about its filesystem type
// until whoever writes it decides.
constexpr FsSpec kFs[] = {
    {"fs_meshcore", false}, {"fs_meshtastic", true}, {"fs_reticulum", false},
    {"fs_lorawan", false},  {"fs_custom", false},
};
constexpr std::size_t kFsCount = sizeof(kFs) / sizeof(kFs[0]);

const FsSpec& fsFor(std::uint8_t index) {
  return kFs[index < kFsCount ? index : kFsCount - 1];
}

std::string row(const char* label, const char* type, const char* subtype,
                std::uint32_t offset, std::uint32_t size) {
  char buf[160];
  // All three commas are load-bearing. ESP-IDF wants
  // name,type,subtype,offset,size,flags, and dropping the comma after the name
  // silently glues the name and the type into one field: the row still parses,
  // but as a data partition with a nonsense label, and every count derived from
  // it comes out zero.
  std::snprintf(buf, sizeof(buf), "%-15s, %-5s, %-9s, 0x%X, 0x%X,\n", label, type, subtype,
                offset, size);
  return std::string(buf);
}

}  // namespace

const char* transportName(Transport transport) {
  switch (transport) {
    case Transport::Usb:
      return "usb";
    case Transport::Ble:
      return "ble";
    case Transport::Wifi:
      return "wifi";
  }
  return "?";
}

const char* provisionStateName(ProvisionState state) {
  switch (state) {
    case ProvisionState::ZeroBoot:
      return "zero-boot";
    case ProvisionState::Provisioning:
      return "provisioning";
    case ProvisionState::Full:
      return "full";
  }
  return "?";
}

std::uint8_t maxSlotsForFlash(std::uint32_t flashSizeBytes) {
  // Whole strides only: slot n occupies [first + n*stride, first + (n+1)*stride),
  // so a slot that would overhang the end of flash is not a slot.
  if (flashSizeBytes <= kFirstSlotOffset) return 0;
  const std::uint32_t usable = flashSizeBytes - kFirstSlotOffset;
  std::uint8_t n = static_cast<std::uint8_t>(usable / kSlotStrideBytes);
  return n > kMaxSlots ? kMaxSlots : n;
}

std::uint32_t slotOffset(std::uint8_t index) {
  return kFirstSlotOffset + static_cast<std::uint32_t>(index) * kSlotStrideBytes;
}

std::uint32_t slotFsOffset(std::uint8_t index) { return slotOffset(index) + kSlotAppBytes; }

const char* defaultFrameworkForSlot(std::uint8_t index) {
  if (index == 0) return "meshcore";
  if (index == 1) return "meshtastic";
  return "";
}

const char* slotFsLabel(std::uint8_t index) { return fsFor(index).label; }

bool slotFsIsLittleFs(std::uint8_t index) { return fsFor(index).littlefs; }

std::uint8_t nextSlotIndex(std::uint8_t provisionedCount) { return provisionedCount; }

ProvisioningView provisionView(std::uint8_t provisionedCount, Transport transport,
                               std::uint8_t maxSlots) {
  ProvisioningView v;
  v.provisionedCount = provisionedCount;
  v.maxSlots = maxSlots;

  if (provisionedCount == 0) {
    // A board with nothing on it offers exactly one connection and behaves as
    // though a single slot were the only option, because right now it is. No
    // second endpoint appears, and none is implied: the operator has no choice
    // to make and nothing to understand before their first flash.
    v.state = ProvisionState::ZeroBoot;
    v.offeredCount = 1;
    Endpoint e;
    e.transport = transport;
    e.slot = 0;
    e.framework = defaultFrameworkForSlot(0);
    e.label = "MC  ota_0";
    v.endpoints.push_back(e);
    return v;
  }

  if (provisionedCount >= maxSlots) {
    v.state = ProvisionState::Full;
    v.offeredCount = 0;
    return v;
  }

  v.state = ProvisionState::Provisioning;
  // Two: the slot just provisioned and the slot now opening. A deployed board is
  // multi-slot because a community genuinely runs two frameworks on one antenna,
  // not five, so those are the two an operator can act on. Every slot stays
  // addressable by index for anyone who wants the rest.
  const std::uint8_t headroom = maxSlots - provisionedCount;
  v.offeredCount = headroom > 1 ? 2 : headroom;

  const std::uint8_t indices[2] = {
      static_cast<std::uint8_t>(provisionedCount - 1),
      provisionedCount,
  };

  for (std::uint8_t i = 0; i < v.offeredCount; ++i) {
    const std::uint8_t idx = indices[i];
    Endpoint e;
    e.transport = transport;
    e.slot = idx;
    e.framework = defaultFrameworkForSlot(idx);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "ota_%u", static_cast<unsigned>(idx));
    e.label = buf;
    v.endpoints.push_back(e);
  }
  return v;
}

ProvisioningView zeroBootView(Transport transport) {
  return provisionView(0, transport);
}

std::uint8_t provisionedSlots(const SlotTable& table) {
  // Counts *slots*, not "app partitions". The bootloader is also an app
  // partition and lives at offset 0, so a naive count is permanently one too
  // high -- which would push every "next slot" index forward and, in a loop that
  // grows until it is full, never terminate. Only OTA-style slots count.
  std::uint8_t n = 0;
  for (const Partition& p : table.partitions()) {
    if (p.type != PartType::App) continue;
    if (p.subType == PartSubType::Factory) continue;
    if (p.blank) continue;
    ++n;
  }
  return n;
}

std::string renderSlots(std::uint8_t slots, std::uint32_t flashSizeBytes) {
  std::string csv =
      "# Append-only slot layout. Slot n is at a fixed address whether there are\n"
      "# one slots or sixteen, so growing this table can never move a partition\n"
      "# that already holds firmware.\n"
      "# Validate: python tools/gate.py\n";
  csv += row("bootloader", "app ", "factory", kBootloaderOffset, kBootloaderSize);
  csv += row("partition_tbl", "data", "nvs", kPartitionTableOffset, kPartitionTableSize);
  csv += row("otadata", "data", "otadata", kOtadataOffset, kOtadataSize);
  csv += row("nvs", "data", "nvs", kNvsOffset, kNvsSize);
  // Emitted here rather than at the end because its offset is below the slots.
  // The validator sorts by offset regardless, but a table a human can read
  // top-to-bottom is a table that gets reviewed correctly.
  csv += row("coredump", "data", "coredump", kCoredumpOffset, kCoredumpSize);

  for (std::uint8_t i = 0; i < slots; ++i) {
    char lbl[24];
    char sub[16];
    std::snprintf(lbl, sizeof(lbl), "ota_%u", static_cast<unsigned>(i));
    std::snprintf(sub, sizeof(sub), "ota_%u", static_cast<unsigned>(i));
    csv += row(lbl, "app ", sub, slotOffset(i), kSlotAppBytes);
    csv += row(slotFsLabel(i), "data", slotFsIsLittleFs(i) ? "littlefs" : "spiffs",
               slotFsOffset(i), kSlotFsBytes);
  }

  (void)flashSizeBytes;
  return csv;
}

SlotTable growTable(const SlotTable& existing, std::uint32_t flashSizeBytes,
                    TableReport* report) {
  TableReport local;

  const std::uint8_t current = provisionedSlots(existing);
  const std::uint8_t next = nextSlotIndex(current);
  const std::uint8_t room = maxSlotsForFlash(flashSizeBytes);

  if (next >= kMaxSlots || next >= room) {
    local.status = TableStatus::MissingSlot;
    local.detail = "no room for another slot on this flash";
    if (report) *report = local;
    return SlotTable();
  }

  // Rebuild from the slot count rather than editing the previous table. Because
  // slot n's address is arithmetic and independent of the count, this cannot
  // move anything that already exists -- which is the entire safety argument
  // for rewriting a partition table in place, and the reason it is stated as a
  // rebuild instead of an insert.
  TableReport pr;
  SlotTable out = SlotTable::parse(renderSlots(static_cast<std::uint8_t>(next + 1), flashSizeBytes),
                                   flashSizeBytes, &pr);

  // Prove the invariant rather than assert it in a comment: every app slot that
  // existed before must still exist, at the same offset and the same size.
  for (std::uint8_t i = 0; i < current; ++i) {
    char lbl[24];
    std::snprintf(lbl, sizeof(lbl), "ota_%u", static_cast<unsigned>(i));
    const Partition* before = existing.find(lbl);
    const Partition* after = out.find(lbl);
    if (before == nullptr || after == nullptr || before->offset != after->offset ||
        before->size != after->size) {
      local.status = TableStatus::Overlap;
      local.detail = "growth would relocate an existing slot";
      local.label = lbl;
      if (report) *report = local;
      return SlotTable();
    }
  }

  local.status = TableStatus::Ok;
  local.detail = "grown";
  if (report) *report = local;
  return out;
}

std::string renderCsv(const SlotTable& table) { return renderSlots(provisionedSlots(table), 0); }

}  // namespace bridge
