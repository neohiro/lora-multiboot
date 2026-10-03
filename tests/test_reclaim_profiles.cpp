// SPDX-License-Identifier: MIT
//
// Reclaiming the flash of a middle slot.
//
// The question this answers, because it is the one that actually gets asked:
//
//   "I want to uninstall an app from the middle. Do I lose memory, and do I break
//    the board?"
//
// Neither. The flash is not lost, and nothing moves. What the space *cannot* do is
// become a new numbered slot -- that would break the arithmetic addressing which is
// the only reason growth is safe in the first place. So it becomes OTA staging
// space instead, which is what a multi-slot bootloader needed and had nowhere to
// put.

#include "bridge/Inventory.hpp"
#include "bridge/RadioProfiles.hpp"
#include "bridge/RadioPlan.hpp"
#include "bridge/SlotLifecycle.hpp"
#include "harness.hpp"

using namespace bridge;

namespace {

constexpr std::uint32_t k16Mb = 16u * 1024u * 1024u;
constexpr std::uint32_t kImage = 0x180000;
constexpr const char* kHw = "Heltec LoRa 32 V4";

SlotTable board(std::uint8_t slots) { return SlotTable::parse(renderSlots(slots), k16Mb); }

// Fill slots 0..n-1 and leave the rest empty.
DeviceState withFirmware(std::uint8_t count) {
  DeviceState s;
  for (std::uint8_t i = 0; i < count; ++i) s.setProvisioned(i, true);
  s.boot.bootSlot = 0;
  return s;
}

}  // namespace

void suite_reclaim() {
  harness::suite("Reclaim");

  // --- reclaiming a middle slot is safe and loses nothing ------------------

  {
    // Slots 0 and 1 hold firmware, slot 2 is erased and is in the middle of the
    // table, slot 3 is empty, slot 4 is the reserve.
    const SlotTable t = board(5);
    DeviceState s = withFirmware(2);

    const ReclaimResult r = reclaimSlot(t, s, 2);
    REQUIRE(r.ok);
    CHECK_MSG(r.freedBytes > 0, "the space is accounted for, not vanished");
    CHECK_EQ(r.freedOffset, t.find("ota_2")->offset);

    // The critical invariant: nothing that survives has moved.
    for (const char* label : {"ota_0", "ota_1", "ota_3", "ota_4"}) {
      const Partition* before = t.find(label);
      const Partition* after = r.table.find(label);
      REQUIRE(after != nullptr);
      CHECK_MSG(after->offset == before->offset, label);
      CHECK_MSG(after->size == before->size, label);
    }

    // The geometry still holds, and the hole is now usable space.
    CHECK_MSG(r.table.validate().ok(), r.table.validate().detail);
    CHECK_MSG(r.table.validateFrameworks().ok(), "surviving frameworks stay isolated");
    const Partition* stage = otaStagingRegion(r.table);
    REQUIRE(stage != nullptr);
    CHECK_EQ(stage->offset, r.freedOffset);
    CHECK_EQ(stage->size, r.freedBytes);
    CHECK_MSG(stage->subType == PartSubType::Reserved, "declared as held-back space");

    // And the two live firmwares' settings are still exactly where they were.
    CHECK(r.table.find("fs_meshcore") != nullptr);
    CHECK(r.table.find("fs_meshtastic") != nullptr);
  }

  // --- the reclaim is refused when it would actually lose something --------

  {
    const SlotTable t = board(5);
    const DeviceState s = withFirmware(3);  // slot 2 is live
    const ReclaimResult r = reclaimSlot(t, s, 2);
    CHECK_MSG(!r.ok, "a slot holding firmware is never reclaimed silently");
    CHECK_MSG(std::string(r.detail).find("erase") != std::string::npos, r.detail);
  }

  {
    // Erasing first is a separate, deliberate act. That separation is the point:
    // conflating "remove this framework" with "forget this node's identity" is how
    // somebody loses a channel key by accident.
    const SlotTable t = board(5);
    DeviceState s = withFirmware(3);
    const SlotOpResult erased = eraseApp(t, s, 2);
    REQUIRE(erased.ok);
    CHECK_MSG(!erased.state.isProvisioned(2), "firmware gone, settings kept");
    const ReclaimResult r = reclaimSlot(erased.table, erased.state, 2);
    CHECK_MSG(r.ok, "and now the space can be reclaimed");
  }

  {
    // The reserve is never reclaimed: a board with no free slot is the situation
    // everything here exists to prevent.
    const SlotTable t = board(5);
    const DeviceState s = withFirmware(2);
    const ReclaimResult r = reclaimSlot(t, s, 4);
    CHECK_MSG(!r.ok, "the reserved free slot stays available");
    CHECK_MSG(std::string(r.detail).find("reserved") != std::string::npos, r.detail);
  }

  {
    // Only one staging region can exist, since a second hole would have to be
    // somewhere that is either taken or would overlap.
    const SlotTable t = board(5);
    const DeviceState s = withFirmware(1);
    const ReclaimResult a = reclaimSlot(t, s, 2);
    REQUIRE(a.ok);
    const ReclaimResult b = reclaimSlot(a.table, s, 3);
    CHECK_MSG(!b.ok, "a second reclaim is refused rather than overlapping");
    CHECK_MSG(std::string(b.detail).find("already exists") != std::string::npos, b.detail);
  }

  // --- the reclaimed space really is usable --------------------------------

  {
    const SlotTable t = board(5);
    const DeviceState s = withFirmware(1);
    const ReclaimResult r = reclaimSlot(t, s, 3);
    REQUIRE(r.ok);
    const Partition* stage = otaStagingRegion(r.table);

    // Enough for a real firmware image, which is the entire point.
    CHECK_MSG(stage->size >= kImage, "big enough to stage an actual image");
    // Outside every surviving slot.
    for (const char* label : {"ota_0", "ota_1", "ota_2", "ota_4"}) {
      const Partition* p = r.table.find(label);
      REQUIRE(p != nullptr);
      CHECK_MSG(stage->offset >= p->offset + p->size || stage->offset + stage->size <= p->offset,
                "staging does not overlap a surviving slot");
    }
  }

  // --- growing afterwards must not collide with the reclaimed hole ----------

  {
    // This is the trap. Slot addressing is arithmetic, so growing appends at
    // kFirstSlotOffset + n*stride. If that arithmetic ever produced an address
    // inside a reclaimed hole, the next append would write over it.
    const SlotTable t = board(5);
    const DeviceState s = withFirmware(1);
    const ReclaimResult r = reclaimSlot(t, s, 3);
    REQUIRE(r.ok);

    const Partition* stage = otaStagingRegion(r.table);
    REQUIRE(stage != nullptr);

    // The hole *is* slot 3's old address -- that is the point of reclaiming it. What
    // must not happen is any other slot's arithmetic address landing inside it.
    for (std::uint8_t n = 0; n <= 8; ++n) {
      if (n == 3) continue;  // the reclaimed one, by definition
      const std::uint32_t off = slotOffset(n);
      const bool overlaps =
          off < stage->offset + stage->size && stage->offset < off + kSlotAppBytes;
      CHECK_MSG(!overlaps, "no other arithmetic slot address lands in the hole");
    }

    // Growing afterwards must be refused, not emitted as an overlapping table: growth
    // is arithmetic and would put a numbered slot straight back on top of the
    // staging region. Deferring growth is a decision an operator can make; an invalid
    // partition table is a brick.
    TableReport gr;
    const SlotTable grown = growTable(r.table, k16Mb, &gr);
    CHECK_MSG(!gr.ok(), "growth is refused while staging space is held");
    CHECK_MSG(std::string(gr.detail).find("staging") != std::string::npos, gr.detail);
    CHECK_MSG(grown.partitions().empty(), "and nothing invalid is returned");
  }

  // --- the free-slot guarantee survives a reclaim --------------------------

  {
    const SlotTable t = board(5);
    DeviceState s = withFirmware(2);
    const ReclaimResult r = reclaimSlot(t, s, 2);
    REQUIRE(r.ok);
    CHECK_MSG(!s.isProvisioned(2), "still erased");
    // The reserve at the top is untouched by a reclaim in the middle.
    CHECK_MSG(!s.isProvisioned(4), "the reserve is still free");
    CHECK_MSG(bootloaderAlwaysReachable(r.table), "and the bootloader is still there");
  }
}

void suite_radio_profiles() {
  harness::suite("RadioProfiles");

  // --- the default profile is a complete working configuration -------------

  {
    const TxProfile d = defaultTxProfile();
    CHECK_MSG(d.frequencyMHz == 869.525f, "the shared carrier");
    CHECK_EQ(d.spreadingFactor, 11);
    CHECK(d.bandwidthKHz == 250.0f);
    CHECK_MSG(!d.setsOwnSettings, "the board default configures nothing");
    CHECK_MSG(d.reconfigureMs == 0, "and is already in place");

    // A slot that configures nothing still transmits legally and intelligibly.
    CHECK(d.txPowerDbm <= maxConductedDbm(Region::EU_868));
  }

  // --- each framework carries its own identity ------------------------------

  {
    TxProfile mc;
    TxProfile mt;
    CHECK(txProfileFor(Framework::MeshCore, Role::Repeater, &mc));
    CHECK(txProfileFor(Framework::Meshtastic, Role::Router, &mt));
    CHECK_MSG(mc.syncWord == 0x12, "MeshCore identity on the air");
    CHECK_MSG(mt.syncWord == 0x2B, "Meshtastic identity on the air");
    CHECK_MSG(mc.setsOwnSettings && mt.setsOwnSettings, "both configure themselves");
    // And they share a carrier, which is the premise.
    CHECK_MSG(mc.frequencyMHz == mt.frequencyMHz, "both share the carrier");

    TxProfile unknown;
    CHECK_MSG(!txProfileFor(Framework::Custom, Role::Tracker, &unknown),
              "an unknown combination is refused, not guessed");
  }

  // --- "the most powerful RX setting masters the weaker ones" ---------------
  //
  // Bandwidth and preamble have compatible supersets. Spreading factor and coding
  // rate do not, and pretending otherwise would produce a receiver that hears
  // nothing.

  {
    TxProfile wide;
    TxProfile narrow;
    txProfileFor(Framework::MeshCore, Role::Repeater, &wide);
    txProfileFor(Framework::MeshCore, Role::RoomServer, &narrow);
    wide.bandwidthKHz = 500.0f;
    narrow.bandwidthKHz = 125.0f;

    CHECK_MSG(dominates(RxSettings(wide), narrow),
              "a 500 kHz receiver hears a 125 kHz frame");
    CHECK_MSG(!dominates(RxSettings(narrow), wide),
              "but a 125 kHz receiver does not hear a 500 kHz frame");

    // Preamble: longer masters shorter.
    TxProfile longPre;
    TxProfile shortPre;
    txProfileFor(Framework::MeshCore, Role::Repeater, &longPre);
    txProfileFor(Framework::MeshCore, Role::RoomServer, &shortPre);
    longPre.preambleSymbols = 12;
    shortPre.preambleSymbols = 8;
    CHECK(dominates(RxSettings(longPre), shortPre));
    CHECK(!dominates(RxSettings(shortPre), longPre));
  }

  {
    // Spreading factor is not masterable in either direction. This is the whole
    // reason "use the most powerful setting" has a limit.
    TxProfile fast;
    TxProfile slow;
    txProfileFor(Framework::MeshCore, Role::Repeater, &fast);
    txProfileFor(Framework::MeshCore, Role::RoomServer, &slow);
    fast.spreadingFactor = 9;
    slow.spreadingFactor = 11;
    CHECK_MSG(!dominates(RxSettings(slow), fast), "SF11 cannot decode SF9");
    CHECK_MSG(!dominates(RxSettings(fast), slow), "and SF9 cannot decode SF11");

    TxProfile strong;
    TxProfile weak;
    txProfileFor(Framework::MeshCore, Role::Repeater, &strong);
    txProfileFor(Framework::MeshCore, Role::RoomServer, &weak);
    strong.codingRateDenominator = 5;
    weak.codingRateDenominator = 8;
    CHECK_MSG(!dominates(RxSettings(strong), weak), "coding rate must match too");
  }

  // --- the master configuration -------------------------------------------

  {
    // Two frameworks, same carrier, different sync words: one master hears both.
    // This is the common case and the premise of the project.
    std::vector<TxProfile> v;
    TxProfile mc;
    TxProfile mt;
    txProfileFor(Framework::MeshCore, Role::Repeater, &mc);
    txProfileFor(Framework::Meshtastic, Role::Router, &mt);
    v.push_back(mc);
    v.push_back(mt);

    const ReconcileReport r = reconcile(v);
    CHECK_MSG(r.hasMaster, "one master configuration exists");
    CHECK(r.singleConfigCoversAll);
    CHECK_EQ(r.plan.profilesHeard, 2u);
    CHECK_MSG(r.plan.promiscuous, "accepting two sync words means promiscuous");
    CHECK(r.plan.accepts(0x12));
    CHECK(r.plan.accepts(0x2B));
    CHECK_MSG(!r.plan.accepts(0x42), "and not something nobody installed");
  }

  {
    // Different bandwidths merge by widening -- no time-slicing needed.
    std::vector<TxProfile> v;
    TxProfile a;
    TxProfile b;
    txProfileFor(Framework::MeshCore, Role::Repeater, &a);
    txProfileFor(Framework::Custom, Role::Config, &b);
    v.push_back(a);
    v.push_back(b);

    ReconcileReport r = reconcile(v);
    if (!r.hasMaster) {
      // LoRaWAN is not in the role table, so this is really testing the generic path.
      CHECK_MSG(r.singleConfigCoversAll, "same carrier and modulation merge");
    } else {
      CHECK_MSG(r.plan.bandwidthKHz >= a.bandwidthKHz, "widened to the widest");
      CHECK_EQ(r.plan.profilesHeard, 2u);
    }
  }

  {
    // Different spreading factors: no master, and an honest report of why.
    std::vector<TxProfile> v;
    TxProfile a;
    TxProfile b;
    txProfileFor(Framework::MeshCore, Role::Repeater, &a);
    txProfileFor(Framework::Meshtastic, Role::Router, &b);
    b.spreadingFactor = 9;
    v.push_back(a);
    v.push_back(b);

    const ReconcileReport r = reconcile(v);
    CHECK_MSG(!r.hasMaster, "no master exists across spreading factors");
    CHECK(!r.singleConfigCoversAll);
    CHECK_EQ(static_cast<int>(r.divergence), static_cast<int>(RxDivergence::SpreadingFactor));
    CHECK_MSG(r.plan.profilesHeard == 0,
              "and it reports zero rather than appearing to hear something");
    CHECK(std::string(r.detail).find("time-slicing") != std::string::npos);
  }

  {
    // Carriers too far apart for one receiver.
    std::vector<TxProfile> v;
    TxProfile a;
    TxProfile b;
    txProfileFor(Framework::MeshCore, Role::Repeater, &a);
    txProfileFor(Framework::Meshtastic, Role::Router, &b);
    b.frequencyMHz = 903.0f;
    v.push_back(a);
    v.push_back(b);
    const ReconcileReport r = reconcile(v);
    CHECK_MSG(!r.hasMaster, "3.5 MHz apart needs two receivers or slicing");
    CHECK_MSG(std::string(r.detail).find("time-slicing") != std::string::npos, r.detail);
  }

  {
    // Carriers close enough for a wide receiver to span: that merges, and the
    // report says what it cost.
    std::vector<TxProfile> v;
    TxProfile a;
    TxProfile b;
    txProfileFor(Framework::MeshCore, Role::Repeater, &a);
    txProfileFor(Framework::Meshtastic, Role::Router, &b);
    b.frequencyMHz = 869.60f;
    v.push_back(a);
    v.push_back(b);
    const ReconcileReport r = reconcile(v);
    REQUIRE(r.hasMaster);
    CHECK_MSG(r.carrierSpanKHz > 0.0f, "there was a span to cover");
    CHECK_MSG(r.plan.bandwidthKHz > a.bandwidthKHz, "so the receiver was widened");
    // And the master genuinely dominates both.
    CHECK(dominates(r.master, a));
    CHECK(dominates(r.master, b));
  }

  {
    // The blank board is a valid state, not a special case.
    const ReconcileReport r = reconcile(std::vector<TxProfile>());
    CHECK(r.hasMaster);
    CHECK(r.plan.frequencyMHz == 869.525f);
    CHECK(r.plan.profilesHeard == 0);
    CHECK_MSG(std::string(r.detail).find("defaults") != std::string::npos, r.detail);
  }

  // --- switching ------------------------------------------------------------

  {
    TxProfile mc;
    TxProfile mt;
    txProfileFor(Framework::MeshCore, Role::Repeater, &mc);
    txProfileFor(Framework::Meshtastic, Role::Router, &mt);

    const SwitchDecision same = decideSwitch(mc, mc, 60000);
    CHECK_MSG(!same.shouldSwitch, "no pointless reconfiguration");
    CHECK_EQ(same.costMs, 0);

    const SwitchDecision mid = decideSwitch(mc, mt, 60000, true);
    CHECK_MSG(!mid.shouldSwitch, "never reconfigure mid-transmission");
    CHECK(std::string(mid.reason).find("in progress") != std::string::npos);

    const SwitchDecision broke = decideSwitch(mc, mt, 60000, false);
    CHECK_MSG(broke.shouldSwitch, "otherwise switch");
    CHECK_MSG(broke.costMs > 0, "and it costs deafness, which is stated");

    // With almost no budget left, the switch is not worth making.
    const SwitchDecision poor = decideSwitch(mc, mt, 1, false);
    CHECK_MSG(!poor.shouldSwitch, "deferred when there is nothing to transmit anyway");
  }
}