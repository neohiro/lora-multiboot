// SPDX-License-Identifier: MIT
//
// Slot lifecycle tests.
//
// The promise under test is the one that is easiest to break and most expensive
// to get wrong: **an operation on one slot writes nothing else.** Every
// operation returns the set of slots it touched, so that is checked as a value
// rather than asserted in a comment.
//
// The second promise is that the bootloader is never a candidate. There is always
// exactly one way back to a working board.

#include "bridge/SlotLifecycle.hpp"
#include "harness.hpp"

using namespace bridge;

namespace {

constexpr std::uint32_t k16Mb = 16u * 1024u * 1024u;
constexpr std::uint32_t kImage = 0x180000;  // 1.5 MB, comfortably inside a 2 MB slot

SlotTable tableWith(std::uint8_t slots) {
  return SlotTable::parse(renderSlots(slots), k16Mb);
}

// A board with MeshCore on slot 0 and Meshtastic on slot 1, both provisioned.
DeviceState twoLive() {
  DeviceState s;
  s.setProvisioned(0, true);
  s.setProvisioned(1, true);
  s.boot.bootSlot = 0;
  s.boot.otaSeq = 2;
  return s;
}

}  // namespace

void suite_slot_lifecycle() {
  harness::suite("SlotLifecycle");

  // --- the reserved free slot -----------------------------------------------
  //
  // The last slot in the table can never be provisioned. A board whose last free
  // slot has been filled has no obvious target to write a recovery image into, and
  // the person who discovers that is standing on a ladder in the rain.
  {
    const SlotTable one = tableWith(1);
    CHECK_MSG(provisionableSlots(one) == 0, "a one-slot table is entirely reserve");
    CHECK(!slotIsProvisionable(one, 0));

    const SlotTable two = tableWith(2);
    CHECK_MSG(provisionableSlots(two) == 1, "two slots yield one usable");
    CHECK(slotIsProvisionable(two, 0));
    CHECK(!slotIsProvisionable(two, 1));

    const SlotTable five = tableWith(5);
    CHECK_MSG(provisionableSlots(five) == 4, "five slots yield four usable");
    for (std::uint8_t i = 0; i < 4; ++i) CHECK(slotIsProvisionable(five, i));
    CHECK_MSG(!slotIsProvisionable(five, 4), "the last one is reserve");

    // The name says what it is for, because that is what an operator reads.
    const std::string n = freeSlotName("Heltec LoRa 32 V4");
    CHECK_MSG(n.find("Free") == 0, n);
    CHECK_MSG(n.find("slot") != std::string::npos, n);
    CHECK(n.find("Heltec") != std::string::npos);
    // A missing hardware name must not produce a broken string.
    CHECK(freeSlotName(nullptr).size() > 0);
    CHECK(freeSlotName("").size() > 0);
  }

  {
    // Provisioning the reserve is refused by name, and the reason is the reason.
    const SlotTable t = tableWith(2);
    DeviceState blank;
    const SlotOpResult r = provision(t, blank, 1, kImage);
    CHECK_MSG(!r.ok, "the reserved slot cannot be filled");
    CHECK_MSG(std::string(r.detail).find("reserved free slot") != std::string::npos, r.detail);
    CHECK_MSG(r.touchedOnly(0xFF), "a refused provision writes nothing");
    CHECK_MSG(!r.state.isProvisioned(1), "and changes no state");
  }

  {
    // Filling every usable slot must still leave the reserve free.
    const SlotTable t = tableWith(5);
    DeviceState s;
    for (std::uint8_t i = 0; i < provisionableSlots(t); ++i) {
      const SlotOpResult r = provision(t, s, i, kImage);
      REQUIRE(r.ok);
      s = r.state;
    }
    CHECK_MSG(freeSlots(t, s) == 1, "exactly one free slot always remains");
    CHECK_MSG(!s.isProvisioned(4), "and it is the reserved one");
    // And the board can still be written to.
    CHECK_MSG(slotIsProvisionable(t, 0), "the reserve is the only place left to write");
  }

  // --- the board always has a way back ---------------------------------------

  {
    for (std::uint8_t n = 1; n <= maxSlotsForFlash(k16Mb); ++n) {
      CHECK_MSG(bootloaderAlwaysReachable(tableWith(n)),
                "bootloader reachable at " + std::to_string(n) + " slots");
    }
  }

  // --- provision: the first firmware on a blank board becomes the boot target

  {
    // A 2-slot table: slot 0 is usable, slot 1 is the reserve.
    const SlotTable t = tableWith(2);
    DeviceState blank;
    const SlotOpResult r = provision(t, blank, 0, kImage);
    REQUIRE(r.ok);
    CHECK_MSG(r.touchedOnly(0), "provisioning writes exactly one slot");
    CHECK(r.state.isProvisioned(0));
    CHECK_EQ(activeSlot(r.state), 0);
    CHECK_MSG(r.state.boot.otaSeq > 0, "boot sequence advanced");
    CHECK_EQ(r.state.provisionedCount(), 1);
  }

  {
    // A second framework must not steal the boot target from a working board.
    // Four slots, so slot 2 is usable and slot 3 is the reserve.
    const SlotTable t = tableWith(4);
    const DeviceState s = twoLive();
    const SlotOpResult r = provision(t, s, 2, kImage);
    REQUIRE(r.ok);
    CHECK_MSG(r.touchedOnly(2), "provisioning slot 2 writes only slot 2");
    CHECK_EQ(activeSlot(r.state), 0);
    CHECK(r.state.isProvisioned(0));
    CHECK(r.state.isProvisioned(1));
    CHECK(r.state.isProvisioned(2));
  }

  // --- refusals: an image that does not fit would be silently truncated -----

  {
    const SlotTable t = tableWith(4);
    const DeviceState s = twoLive();
    CHECK(!provision(t, s, 2, 0).ok);
    CHECK(!provision(t, s, 2, kSlotAppBytes + 1).ok);
    // Exactly filling the slot is allowed.
    CHECK(provision(t, s, 2, kSlotAppBytes).ok);
    // Provisioning over live firmware must be an explicit reflash.
    CHECK(!provision(t, s, 1, kImage).ok);
    CHECK(!provision(t, s, 9, kImage).ok);
  }

  // --- reflash keeps settings, which is the point of separating it from erase

  {
    const SlotTable t = tableWith(2);
    const DeviceState s = twoLive();
    const SlotOpResult r = reflash(t, s, 1, kImage);
    REQUIRE(r.ok);
    CHECK_MSG(r.touchedOnly(1), "reflashing slot 1 writes only slot 1");
    // The filesystem partition is not written, so channel keys survive a firmware
    // repair. Losing the node's identity on the mesh is not what "fix the image"
    // should mean.
    CHECK_MSG(std::string(r.detail).find("settings kept") != std::string::npos, r.detail);
    CHECK(r.state.isProvisioned(0));
    CHECK(r.state.isProvisioned(1));

    CHECK(!reflash(t, s, 0, 0).ok);
  }

  // --- eraseApp: clears firmware, keeps the partition row -------------------

  {
    const SlotTable t = tableWith(2);
    const DeviceState s = twoLive();
    const SlotOpResult r = eraseApp(t, s, 1);
    REQUIRE(r.ok);
    CHECK_MSG(r.touchedOnly(1), "erasing slot 1 writes only slot 1");
    CHECK(!r.state.isProvisioned(1));
    CHECK_MSG(r.state.isProvisioned(0), "slot 0 is untouched");
    // The row stays: the slot is Empty, not gone. The address does not move and
  // the flash is simply not used.
    CHECK_MSG(r.table.find("ota_1") != nullptr, "the slot row survives an erase");
    CHECK_MSG(r.table.find("fs_meshtastic") != nullptr, "and so does its settings partition");
    const SlotStatus st = slotStatus(r.table, r.state, 1);
    CHECK_EQ(static_cast<int>(st.state), static_cast<int>(SlotState::Empty));

    // Erasing twice is a mistake worth reporting, not a silent no-op.
    CHECK(!eraseApp(t, r.state, 1).ok);
  }

  {
    // eraseSlot is the destructive one. The device records the *intent* -- the
    // actual settings wipe is flash I/O the tool performs, because firmware cannot
    // sanely erase the filesystem partition it is running out of.
    const SlotTable t = tableWith(4);
    const DeviceState s = twoLive();
    const SlotOpResult r = eraseSlot(t, s, 1);
    REQUIRE(r.ok);
    CHECK_MSG(r.touchedOnly(1), "erasing slot 1 writes only slot 1");
    CHECK_MSG(std::string(r.detail).find("settings wipe requested") != std::string::npos,
              r.detail);
    // And it is otherwise identical to eraseApp, which is the honest position.
    CHECK_MSG(!r.state.isProvisioned(1), "the firmware is gone either way");
    CHECK(r.state.isProvisioned(0));
  }

  // --- erasing the boot target must not leave the board bootless -----------

  {
    const SlotTable t = tableWith(2);
    const DeviceState s = twoLive();
    const SlotOpResult r = eraseApp(t, s, 0);
    REQUIRE(r.ok);
    CHECK_MSG(activeSlot(r.state) != 0xFF, "boot record never points at an erased slot");
    // It falls back to a slot that actually holds something.
    const std::uint8_t now = activeSlot(r.state);
    CHECK_MSG(now == 0xFF || r.state.isProvisioned(now), "boot target holds firmware");
    CHECK_EQ(now, 1);
  }

  {
    // Erasing the only firmware leaves nowhere to boot. That is reported, and it
    // is the exact situation the bootloader exists for.
    const SlotTable t = tableWith(1);
    DeviceState only;
    only.setProvisioned(0, true);
    only.boot.bootSlot = 0;
    const SlotOpResult r = eraseApp(t, only, 0);
    REQUIRE(r.ok);
    CHECK_EQ(activeSlot(r.state), 0xFF);
    CHECK_MSG(bootloaderAlwaysReachable(r.table), "the bootloader is still there");
  }

  // --- retire: only the top, and only when empty ---------------------------

  {
    const SlotTable t = tableWith(3);
    DeviceState s;
    s.setProvisioned(2, true);
    // Retire a middle slot: refused, and the reason given is the actual reason.
    const SlotOpResult bad = retireSlot(t, s, 1);
    CHECK(!bad.ok);
    CHECK_MSG(std::string(bad.detail).find("highest") != std::string::npos, bad.detail);
    CHECK_MSG(bad.touchedOnly(0xFF), "a refused operation writes nothing");

    // Retire a provisioned slot: refused, erase first.
    CHECK(!retireSlot(t, s, 2).ok);

    // Erase, then retire the top: allowed.
    const SlotOpResult erased = eraseApp(t, s, 2);
    REQUIRE(erased.ok);
    const SlotOpResult retired = retireSlot(erased.table, erased.state, 2);
    REQUIRE(retired.ok);
    CHECK_EQ(provisionedSlots(retired.table), 2);
    CHECK_MSG(retired.table.find("ota_2") == nullptr, "the row is gone");
    CHECK_MSG(retired.table.find("fs_reticulum") == nullptr, "and its filesystem with it");
    CHECK_MSG(retired.table.validate().ok(), retired.table.validate().detail);
  }

  // --- retirement does not move anything that survives ---------------------

  {
    const SlotTable t = tableWith(4);
    DeviceState s;
    s.setProvisioned(0, true);
    s.setProvisioned(1, true);
    s.setProvisioned(3, true);
    const SlotOpResult erased = eraseApp(t, s, 3);
    REQUIRE(erased.ok);
    const SlotOpResult retired = retireSlot(erased.table, erased.state, 3);
    REQUIRE(retired.ok);

    for (std::uint8_t i = 0; i < 3; ++i) {
      char lbl[24];
      std::snprintf(lbl, sizeof(lbl), "ota_%u", static_cast<unsigned>(i));
      const Partition* before = t.find(lbl);
      const Partition* after = retired.table.find(lbl);
      REQUIRE(after != nullptr);
      CHECK_MSG(after->offset == before->offset, "retirement must not move a live slot");
      CHECK_MSG(after->size == before->size, "retirement must not resize a live slot");
    }
  }

  // --- setBootSlot writes no firmware at all -------------------------------

  {
    const SlotTable t = tableWith(2);
    const DeviceState s = twoLive();
    const SlotOpResult r = setBootSlot(t, s, 1);
    REQUIRE(r.ok);
    CHECK_MSG(r.touchedOnly(0xFF), "choosing a boot target writes no slot");
    CHECK_EQ(activeSlot(r.state), 1);
    CHECK_EQ(r.state.boot.rollbackSlot, 0);
    // Cannot point the bootloader at an empty slot.
    DeviceState one = twoLive();
    one.setProvisioned(1, false);
    CHECK(!setBootSlot(t, one, 1).ok);
  }

  // --- ensureRoom opens the next slot without writing anything ------------

  {
    const SlotTable t = tableWith(1);
    DeviceState s;
    s.setProvisioned(0, true);
    const SlotOpResult r = ensureRoom(t, s, k16Mb);
    REQUIRE(r.ok);
    CHECK_MSG(r.touchedOnly(0xFF), "extending the table writes no slot");
    CHECK_EQ(provisionedSlots(r.table), 2);
    CHECK(std::string(r.detail).find("opened") != std::string::npos);

    // Already full: reported, not an error.
    const SlotTable full = tableWith(maxSlotsForFlash(k16Mb));
    const SlotOpResult none = ensureRoom(full, s, k16Mb);
    CHECK(none.ok);
    CHECK_MSG(std::string(none.detail).find("capacity") != std::string::npos, none.detail);
  }

  // --- an 8 MB board gets fewer slots from the same code -------------------

  {
    // An 8 MB board gets fewer slots from the same code, with no special case
    // anywhere: two, against five for the 16 MB V4.
    CHECK_EQ(maxSlotsForFlash(8u * 1024u * 1024u), 2);
    CHECK_EQ(maxSlotsForFlash(k16Mb), 5);

    const SlotTable t = tableWith(1);
    DeviceState s;
    s.setProvisioned(0, true);

    const SlotOpResult grown = ensureRoom(t, s, 8u * 1024u * 1024u);
    REQUIRE(grown.ok);
    CHECK_EQ(provisionedSlots(grown.table), 2);

    // And then it is genuinely full, which is reported rather than guessed.
    const SlotOpResult full = ensureRoom(grown.table, s, 8u * 1024u * 1024u);
    CHECK(full.ok);
    CHECK_MSG(std::string(full.detail).find("capacity") != std::string::npos, full.detail);
    CHECK_MSG(full.touchedOnly(0xFF), "a full board writes nothing");
  }

  // --- a full lifecycle leaves the board coherent --------------------------

  {
    // A board with room to actually provision: two usable slots plus the reserve.
    SlotTable t = tableWith(3);
    DeviceState s;

    // Provision, grow, provision, reflash, erase, retire, regrow.
    SlotOpResult r = provision(t, s, 0, kImage);
    REQUIRE(r.ok);
    t = r.table;
    s = r.state;

    r = ensureRoom(t, s, k16Mb);
    REQUIRE(r.ok);
    t = r.table;

    r = provision(t, s, 1, kImage);
    REQUIRE(r.ok);
    t = r.table;
    s = r.state;
    CHECK_EQ(s.provisionedCount(), 2);

    r = reflash(t, s, 1, kImage);
    REQUIRE(r.ok);
    s = r.state;

    r = eraseApp(t, s, 1);
    REQUIRE(r.ok);
    t = r.table;
    s = r.state;

    // Retirement only ever applies to the top slot: reclaiming a hole in the middle
    // would mean moving everything above it, which is the operation that destroys
    // live firmware. So the table shrinks from the top and the reserve follows it
    // down.
    const std::uint8_t before = provisionedSlots(t);
    r = retireSlot(t, s, static_cast<std::uint8_t>(before - 1));
    REQUIRE(r.ok);
    t = r.table;
    s = r.state;
    CHECK_MSG(provisionedSlots(t) == before - 1, "the table shrank by one");
    CHECK_MSG(freeSlots(t, s) >= 1, "a free slot always remains");

    r = ensureRoom(t, s, k16Mb);
    REQUIRE(r.ok);
    t = r.table;

    CHECK_MSG(t.validate().ok(), t.validate().detail);
    CHECK_MSG(t.validateFrameworks().ok(), t.validateFrameworks().detail);
    CHECK_MSG(bootloaderAlwaysReachable(t), "still recoverable at the end");
    // The surviving slot never moved.
    CHECK_EQ(t.find("ota_0")->offset, slotOffset(0));
    CHECK(s.isProvisioned(0));
  }

  // --- display ------------------------------------------------------------

  {
    const SlotTable t = tableWith(3);
    const DeviceState s = twoLive();
    const std::vector<SlotStatus> all = allSlots(t, s);
    CHECK_EQ(all.size(), 3);
    CHECK_EQ(static_cast<int>(all[0].state), static_cast<int>(SlotState::Provisioned));
    CHECK_EQ(static_cast<int>(all[2].state), static_cast<int>(SlotState::Empty));
    CHECK(all[0].bootsByDefault);
    CHECK_EQ(static_cast<int>(all[0].state), static_cast<int>(SlotState::Provisioned));
    CHECK(std::string(slotStateName(SlotState::Empty)) == "empty");
  }
}