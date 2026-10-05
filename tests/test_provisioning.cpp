// SPDX-License-Identifier: MIT
//
// Provisioning tests.
//
// Two things are being promised to whoever deploys this, and both are tested
// here rather than described in a document and hoped for:
//
//   1. A virgin board offers exactly one connection, and behaves as though a
//      single slot were the only option.
//   2. Growth never relocates anything that already holds firmware.
//
// The second is the one that matters. A repartition that moved a live slot would
// destroy a user's firmware, and it would do so on the day they least expect it.

#include "bridge/Provisioning.hpp"
#include "harness.hpp"

using namespace bridge;

namespace {

constexpr std::uint32_t k16Mb = 16u * 1024u * 1024u;
constexpr std::uint32_t k8Mb = 8u * 1024u * 1024u;

TableReport parseOk(const std::string& csv, std::uint32_t flash) {
  TableReport pr;
  SlotTable::parse(csv, flash, &pr);
  return pr;
}

}  // namespace

void suite_provisioning() {
  harness::suite("Provisioning");

  // --- zero boot: one connection, one option --------------------------------
  //
  // The first thing a new owner does must not require understanding slots.

  {
    const ProvisioningView v = zeroBootView();
    CHECK_EQ(static_cast<int>(v.state), static_cast<int>(ProvisionState::ZeroBoot));
    CHECK_EQ(v.provisionedCount, 0);
    CHECK_MSG(v.offeredCount == 1, "exactly one connection on a virgin board");
    REQUIRE(v.endpoints.size() == 1);
    CHECK_EQ(v.endpoints[0].slot, 0);
    CHECK_EQ(std::string(v.endpoints[0].framework), std::string("meshcore"));
    CHECK_EQ(v.endpoints[0].transport, Transport::Usb);
  }

  // --- growth is progressive -------------------------------------------------

  {
    const ProvisioningView v1 = provisionView(1);
    CHECK_EQ(static_cast<int>(v1.state), static_cast<int>(ProvisionState::Provisioning));
    CHECK_MSG(v1.offeredCount == 2, "after the first slot, two connections");
    REQUIRE(v1.endpoints.size() == 2);
    CHECK_EQ(v1.endpoints[0].slot, 0);
    CHECK_EQ(v1.endpoints[1].slot, 1);
    CHECK_EQ(std::string(v1.endpoints[1].framework), std::string("meshtastic"));
  }

  {
    const ProvisioningView v2 = provisionView(2);
    CHECK_EQ(v2.offeredCount, 2);
    REQUIRE(v2.endpoints.size() == 2);
    CHECK_EQ(v2.endpoints[0].slot, 1);
    CHECK_EQ(v2.endpoints[1].slot, 2);
    // A slot with no framework behind it yet must not pretend to have one.
    CHECK_EQ(std::string(v2.endpoints[1].framework), std::string(""));
  }

  // --- transport independence ------------------------------------------------
  //
  // USB, BLE and WiFi are labels on one service. Nothing about the state machine
  // may depend on which one is being used, or adding a transport becomes a redesign.

  {
    const Transport all[] = {Transport::Usb, Transport::Ble, Transport::Wifi};
    for (Transport t : all) {
      const ProvisioningView v = provisionView(2, t);
      CHECK_EQ(static_cast<int>(v.state), static_cast<int>(ProvisionState::Provisioning));
      CHECK_EQ(v.offeredCount, 2);
      REQUIRE(v.endpoints.size() == 2);
      for (const Endpoint& e : v.endpoints) CHECK_EQ(e.transport, t);
    }
    // A virgin board offers exactly one on every transport.
    for (Transport t : all) {
      CHECK_EQ(zeroBootView(t).offeredCount, 1);
    }
    CHECK(std::string(transportName(Transport::Ble)) == "ble");
  }

  // --- full: no phantom endpoints -------------------------------------------

  {
    const std::uint8_t room = maxSlotsForFlash(k16Mb);
    const ProvisioningView v = provisionView(room, Transport::Usb, room);
    CHECK_EQ(static_cast<int>(v.state), static_cast<int>(ProvisionState::Full));
    CHECK_MSG(v.offeredCount == 0, "nothing to offer when there is nowhere to put it");
  }

  // A board at capacity must not invent endpoints, whatever ceiling it is given.
  {
    const ProvisioningView v = provisionView(3, Transport::Usb, 3);
    CHECK_EQ(static_cast<int>(v.state), static_cast<int>(ProvisionState::Full));
    CHECK_EQ(v.offeredCount, 0);
  }

  // --- slot addressing is arithmetic ----------------------------------------

  {
    CHECK_EQ(slotOffset(0), kFirstSlotOffset);
    CHECK_EQ(slotOffset(1), slotOffset(0) + kSlotStrideBytes);
    CHECK_EQ(slotOffset(2), slotOffset(0) + 2u * kSlotStrideBytes);
    // The alignment the bootloader silently demands.
    CHECK_EQ(slotOffset(0) % SlotTable::kAppAlignment, 0u);
    CHECK_EQ(slotOffset(3) % SlotTable::kAppAlignment, 0u);
    CHECK_EQ(slotFsOffset(0), slotOffset(0) + kSlotAppBytes);
  }

  // --- capacity is derived, not declared ------------------------------------

  {
    const std::uint8_t big = maxSlotsForFlash(k16Mb);
    const std::uint8_t small = maxSlotsForFlash(k8Mb);
    CHECK_MSG(big >= 4, "16MB holds four slots");
    CHECK_MSG(small < big, "an 8MB board reports fewer slots from the same code");
    CHECK(small >= 2);
    // Whatever it reports, it must actually fit.
    CHECK(slotOffset(static_cast<std::uint8_t>(big - 1)) + kSlotStrideBytes <= k16Mb);
  }

  // --- rendering a one-slot board -------------------------------------------

  {
    const std::string csv = renderSlots(1);
    CHECK_MSG(parseOk(csv, k16Mb).ok(), "one-slot table parses");
    const SlotTable t = SlotTable::parse(csv, k16Mb);
    CHECK_MSG(t.validate().ok(), t.validate().detail);
    CHECK_EQ(provisionedSlots(t), 1);
    // The two live frameworks each get an app slot and their own filesystem.
    CHECK_MSG(t.validateFrameworks().ok(), t.validateFrameworks().detail);
    const Partition* ota0 = t.find("ota_0");
    const Partition* mcFs = t.find("fs_meshcore");
    REQUIRE(ota0 != nullptr);
    REQUIRE_MSG(mcFs != nullptr, "slot 0 brings MeshCore's filesystem with it");
    CHECK_EQ(ota0->offset, kFirstSlotOffset);
    CHECK_EQ(ota0->size, kSlotAppBytes);
    // MeshCore mounts SPIFFS. Handing it LittleFS, or the reverse, makes each
    // firmware format the other's filesystem on boot and lose its settings.
    CHECK(mcFs->subType == PartSubType::Spiffs);

    // Meshtastic is genuinely absent from a one-slot board, and that is correct:
    // it arrives with slot 1. Dereferencing a partition that is not there is a
    // crash rather than a failed assertion, which is a poor way to learn this.
    CHECK_MSG(t.find("ota_1") == nullptr, "slot 1 does not exist yet");
    CHECK_MSG(t.find("fs_meshtastic") == nullptr, "and neither does its filesystem");
  }

  // --- a two-slot board pairs each framework with its own filesystem -------

  {
    const SlotTable t = SlotTable::parse(renderSlots(2), k16Mb);
    const Partition* mcFs = t.find("fs_meshcore");
    const Partition* mtFs = t.find("fs_meshtastic");
    REQUIRE(mcFs != nullptr);
    REQUIRE(mtFs != nullptr);
    // SPIFFS for both: the partition generator inside the Arduino toolchain
    // predates ESP-IDF 5.0 and rejects the littlefs keyword, so LittleFS is a
    // table this toolchain cannot build. The separation that actually protects
    // anybody's settings is the offset, asserted on the next line.
    CHECK_MSG(mcFs->subType == PartSubType::Spiffs, "MeshCore -> SPIFFS");
    CHECK_MSG(mtFs->subType == PartSubType::Spiffs, "Meshtastic -> SPIFFS");
    CHECK_MSG(mcFs->offset != mtFs->offset, "separate partitions, or one wipes the other");
  }

  // --- growth is append-only: THE invariant ---------------------------------
  //
  // Walk the whole way up. At every step, every slot that already existed must
  // still exist, byte for byte.

  {
    TableReport gr;
    SlotTable t = SlotTable::parse(renderSlots(1), k16Mb);
    REQUIRE(parseOk(renderSlots(1), k16Mb).ok());

    std::uint32_t previous[8];
    std::uint32_t previousSize[8];
    for (int step = 1; step < 5; ++step) {
      const std::uint8_t before = provisionedSlots(t);

      // Record every existing slot before growing.
      for (std::uint8_t i = 0; i < before; ++i) {
        char lbl[24];
        std::snprintf(lbl, sizeof(lbl), "ota_%u", static_cast<unsigned>(i));
        const Partition* p = t.find(lbl);
        REQUIRE(p != nullptr);
        previous[i] = p->offset;
        previousSize[i] = p->size;
      }

      const SlotTable grown = growTable(t, k16Mb, &gr);
      CHECK_MSG(gr.ok(), gr.detail);
      if (!gr.ok()) return;
      CHECK_MSG(grown.validate().ok(), grown.validate().detail);
      CHECK_EQ(provisionedSlots(grown), static_cast<std::uint8_t>(before + 1));

      for (std::uint8_t i = 0; i < before; ++i) {
        char lbl[24];
        std::snprintf(lbl, sizeof(lbl), "ota_%u", static_cast<unsigned>(i));
        const Partition* p = grown.find(lbl);
        REQUIRE(p != nullptr);
        CHECK_MSG(p->offset == previous[i], "growth must not move a live slot");
        CHECK_MSG(p->size == previousSize[i], "growth must not resize a live slot");
      }
      t = grown;
    }
  }

  // --- growth stops at the hardware, not at a wish ----------------------

  {
    const std::uint8_t room = maxSlotsForFlash(k16Mb);
    std::uint8_t count = 1;
    TableReport gr;
    SlotTable t = SlotTable::parse(renderSlots(1), k16Mb);

    // Bounded on purpose. If growth ever stopped incrementing the slot count this
    // loop would otherwise run until the process died, and a crash reports far
    // less than a failed assertion with a name attached.
    for (int guard = 0; guard < 64; ++guard) {
      if (provisionedSlots(t) >= room) break;
      const SlotTable grown = growTable(t, k16Mb, &gr);
      REQUIRE(gr.ok());
      const std::uint8_t after = provisionedSlots(grown);
      REQUIRE_MSG(after == provisionedSlots(t) + 1, "growth must add exactly one slot");
      t = grown;
      ++count;
    }
    CHECK_EQ(count, room);
    CHECK_MSG(t.validate().ok(), "final table is still coherent");
    CHECK_MSG(t.validateFrameworks().ok(), "final table is still isolated");
  }

  // --- growth refuses rather than overrunning ------------------------------

  {
    const std::uint8_t room = maxSlotsForFlash(k16Mb);
    const SlotTable full = SlotTable::parse(renderSlots(room), k16Mb);
    TableReport gr;
    const SlotTable grown = growTable(full, k16Mb, &gr);
    CHECK_MSG(!gr.ok(), "no room means no growth");
    CHECK_EQ(provisionedSlots(grown), 0);
  }

  // --- every intermediate table is flashable ------------------------------

  {
    for (std::uint8_t n = 1; n <= maxSlotsForFlash(k16Mb); ++n) {
      const std::string csv = renderSlots(n);
      TableReport pr;
      const SlotTable t = SlotTable::parse(csv, k16Mb, &pr);
      CHECK_MSG(pr.ok(), "slot " + std::to_string(n) + ": " + pr.detail);
      const TableReport v = t.validate();
      CHECK_MSG(v.ok(), "slot " + std::to_string(n) + ": " + v.detail);
      CHECK_MSG(t.validateFrameworks().ok(), "slot " + std::to_string(n) + ": isolation");
      CHECK_MSG(t.freeBytes() > 0 || n == maxSlotsForFlash(k16Mb),
                "slot " + std::to_string(n) + ": leaves flash for later growth");
    }
  }
}
