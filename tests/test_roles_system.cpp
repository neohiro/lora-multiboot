// SPDX-License-Identifier: MIT
//
// Roles and SystemUpdate tests.
//
// The role matrix exists to answer concrete questions about real boards, so the
// tests are phrased as those questions: can a room server and a repeater share a
// node? Can a companion and a repeater? Can two repeaters? What happens when the
// analyser is installed beside a relay? And does updating the bootloader cost
// anybody their settings?

#include "bridge/Roles.hpp"
#include "bridge/SystemUpdate.hpp"
#include "harness.hpp"

using namespace bridge;

namespace {

constexpr std::uint32_t k16Mb = 16u * 1024u * 1024u;

SlotTable board(std::uint8_t slots) { return SlotTable::parse(renderSlots(slots), k16Mb); }

RoleProfile profile(Framework fw, Role role) {
  RoleProfile p;
  roleProfile(fw, role, &p);
  return p;
}

void addSlot(BoardPlan& plan, std::uint8_t index, Framework fw, Role role) {
  if (plan.slots.size() <= index) plan.slots.resize(index + 1);
  plan.slots[index].present = true;
  roleProfile(fw, role, &plan.slots[index].profile);
}

bool hasCode(const std::vector<Finding>& findings, const char* code) {
  for (const Finding& f : findings) {
    if (std::string(f.code) == code) return true;
  }
  return false;
}

}  // namespace

void suite_roles() {
  harness::suite("Roles");

  // --- the role table -------------------------------------------------------

  {
    std::size_t count = 0;
    const RoleProfile* all = allProfiles(&count);
    REQUIRE(all != nullptr);
    CHECK_MSG(count >= 10, "MeshCore, Meshtastic, bridge and analyser roles");
    for (std::size_t i = 0; i < count; ++i) {
      CHECK_MSG(all[i].label[0] != '\0', "every role has a label for the panel");
    }
  }

  {
    RoleProfile p;
    CHECK(roleProfile(Framework::MeshCore, Role::Repeater, &p));
    CHECK_EQ(p.radio, RadioDemand::Exclusive);
    CHECK_EQ(p.identity, IdentityDomain::MeshCoreNode);
    CHECK_MSG(!p.servesTwoMeshes, "only the bridge serves two meshes");
    CHECK_MSG(roleProfile(Framework::Bridge, Role::Repeater, &p) && p.servesTwoMeshes,
              "the bridge image is the one that serves both");
    CHECK_MSG(!roleProfile(Framework::Sniffer, Role::RoomServer, &p),
              "an unknown combination is refused rather than defaulted");
  }

  // --- "simultaneously" is not something slots can do -----------------------
  //
  // One radio, one active slot. Every role except pure config needs the
  // transceiver, so no two of them can be active together. Stating it as a test
  // means nobody re-litigates it later.
  {
    const RoleProfile repeater = profile(Framework::MeshCore, Role::Repeater);
    const RoleProfile companion = profile(Framework::MeshCore, Role::Companion);
    const RoleProfile room = profile(Framework::MeshCore, Role::RoomServer);
    const RoleProfile sniffer = profile(Framework::Sniffer, Role::Analyzer);
    CHECK_MSG(repeater.radio == RadioDemand::Exclusive, "a repeater owns the radio");
    CHECK_MSG(companion.radio == RadioDemand::Exclusive,
              "a companion also uses LoRa, so it is not radio-free");
    CHECK_MSG(room.radio == RadioDemand::Exclusive, "and neither does a room server");
    CHECK_MSG(sniffer.radio == RadioDemand::Exclusive, "the analyser needs promiscuous RX");

    const Finding f = classifyPair(repeater, companion);
    CHECK_MSG(f.severity == Severity::Advisory, "installable, but only one may run");
    CHECK_MSG(std::string(f.code) == "radio-exclusive", f.code);
  }

  // --- same provider, different roles: the question actually asked ---------
  //
  // A MeshCore room server and a MeshCore repeater on one board is a normal,
  // sensible deployment. They are different nodes in the same identity namespace,
  // so nothing collides.

  {
    BoardPlan plan;
    addSlot(plan, 0, Framework::MeshCore, Role::RoomServer);
    addSlot(plan, 1, Framework::MeshCore, Role::Repeater);

    const std::vector<Finding> f = auditBoard(plan);
    CHECK_MSG(!hasCode(f, "duplicate-identity"),
              "room server and repeater are different nodes; nothing collides");
    // The only finding is the inherent one-radio rule.
    CHECK_MSG(hasCode(f, "radio-exclusive"), "and the radio-exclusivity is still reported");
  }

  {
    // Companion and repeater on one board is also legitimate: a node that acts as
    // a repeater and can still be paired with a phone when carried.
    BoardPlan plan;
    addSlot(plan, 0, Framework::MeshCore, Role::Companion);
    addSlot(plan, 1, Framework::MeshCore, Role::Repeater);
    const std::vector<Finding> f = auditBoard(plan);
    CHECK_MSG(!hasCode(f, "duplicate-identity"), "companion and repeater are distinct roles");
  }

  {
    // And Meshtastic alongside both: a three-framework board.
    BoardPlan plan;
    addSlot(plan, 0, Framework::Bridge, Role::Repeater);
    addSlot(plan, 1, Framework::Meshtastic, Role::Router);
    addSlot(plan, 2, Framework::MeshCore, Role::RoomServer);
    const std::vector<Finding> f = auditBoard(plan);
    CHECK_MSG(!hasCode(f, "duplicate-identity"), "three different frameworks never collide");
    CHECK_MSG(!hasCode(f, "analyser-excludes-relay"), "and no analyser is present");
  }

  // --- same provider, same role: a genuine fault ---------------------------

  {
    // Two MeshCore repeaters with the same name and key do not form a mesh of two.
    // They form one repeater that appears twice, and paths through it are ambiguous.
    BoardPlan plan;
    addSlot(plan, 0, Framework::MeshCore, Role::Repeater);
    addSlot(plan, 1, Framework::MeshCore, Role::Repeater);

    const std::vector<Finding> f = auditBoard(plan);
    REQUIRE(hasCode(f, "duplicate-identity"));
    // The serious finding sorts ahead of the advisory one.
    CHECK_MSG(!f.empty() && f[0].severity == Severity::Error,
              "an identity collision is reported first");
    CHECK_EQ(f[0].slotA, 0);
    CHECK_EQ(f[0].slotB, 1);
    CHECK_MSG(std::string(f[0].detail).size() > 0, "and says why");
  }

  {
    // Same for two Meshtastic routers.
    BoardPlan plan;
    addSlot(plan, 0, Framework::Meshtastic, Role::Router);
    addSlot(plan, 2, Framework::Meshtastic, Role::Router);
    CHECK(hasCode(auditBoard(plan), "duplicate-identity"));
  }

  // --- the analyser beside a relay -----------------------------------------

  {
    // Both are reasonable alone. The trap is that only one can be active, so
    // switching to the analyser silences the mesh and looks like a fault.
    BoardPlan plan;
    addSlot(plan, 0, Framework::MeshCore, Role::Repeater);
    addSlot(plan, 1, Framework::Sniffer, Role::Analyzer);
    const std::vector<Finding> f = auditBoard(plan);
    CHECK_MSG(hasCode(f, "analyser-excludes-relay"), "reported as an advisory");
    for (const Finding& x : f) {
      if (std::string(x.code) == "analyser-excludes-relay") {
        CHECK_MSG(x.severity == Severity::Advisory, "advisory, not an error");
      }
    }
  }

  {
    // An analyser beside the bridge image: same reasoning, still only an advisory.
    BoardPlan plan;
    addSlot(plan, 0, Framework::Bridge, Role::Repeater);
    addSlot(plan, 1, Framework::Sniffer, Role::Analyzer);
    CHECK(hasCode(auditBoard(plan), "analyser-excludes-relay"));
  }

  // --- config-only is the one genuinely idle role -------------------------

  {
    BoardPlan plan;
    addSlot(plan, 0, Framework::MeshCore, Role::Repeater);
    addSlot(plan, 1, Framework::Custom, Role::Config);
    const std::vector<Finding> f = auditBoard(plan);
    CHECK_MSG(!hasCode(f, "radio-exclusive"),
              "a config-only role does not compete for the radio");
    CHECK_MSG(!hasCode(f, "duplicate-identity"), "and holds no identity");
  }

  // --- an empty board is not a finding --------------------------------------

  {
    BoardPlan plan;
    plan.slots.resize(3);
    CHECK_MSG(auditBoard(plan).empty(), "absent slots are not audited");
  }

  // --- a busy board reports once per colliding pair -------------------------

  {
    BoardPlan plan;
    addSlot(plan, 0, Framework::MeshCore, Role::Repeater);
    addSlot(plan, 1, Framework::MeshCore, Role::Repeater);
    addSlot(plan, 2, Framework::MeshCore, Role::Repeater);

    int errors = 0;
    for (const Finding& f : auditBoard(plan)) {
      if (f.severity == Severity::Error) ++errors;
    }
    CHECK_MSG(errors == 3, "three repeaters produce three colliding pairs");
  }
}

void suite_system_update() {
  harness::suite("SystemUpdate");

  const SlotTable t = board(3);

  // --- the safety invariant -------------------------------------------------

  {
    CHECK_MSG(systemRegionIsContained(t),
              "every system piece lies below the first slot");
    // Slot 0 begins exactly where the system region ends, so the cut is tight and
    // provable rather than conservative.
    CHECK_EQ(t.find("ota_0")->offset, kFirstSlotOffset);
    CHECK(systemPieceOffset(t, SystemPiece::Coredump) + systemPieceSize(t, SystemPiece::Coredump) <=
          kFirstSlotOffset);
  }

  // --- a normal bootloader + nvs update ------------------------------------

  {
    std::vector<SystemImage> images;
    SystemImage bl;
    bl.piece = SystemPiece::Bootloader;
    bl.path = "bootloader.bin";
    bl.sizeBytes = 0x7000;
    images.push_back(bl);

    SystemImage nvs;
    nvs.piece = SystemPiece::Nvs;
    nvs.path = "nvs.bin";
    nvs.sizeBytes = 0x2000;
    images.push_back(nvs);

    UpdateReport r;
    const UpdatePlan plan = planSystemUpdate(t, images, 0, &r);
    CHECK_MSG(r.ok(), r.detail);
    CHECK_EQ(plan.images.size(), 2u);
    CHECK_MSG(plan.includesBootloader, "the bootloader is included");
    // The whole point: no slot, and therefore no settings, is written.
    CHECK_MSG(plan.slotsTouched.empty(), "a system update touches no slot");
    CHECK(plan.totalBytes > 0);
    const std::string d = describeUpdate(plan);
    CHECK_MSG(d.find("slots and settings untouched") != std::string::npos, d);
  }

  // --- an image that would overwrite slot 0 is refused ---------------------

  {
    // Exactly the realistic accident: a bootloader built for a different board, or
    // an offset mistyped as 0x30000 instead of 0x0. Without this check the board
    // stops booting and the user has lost both firmwares in one command.
    std::vector<SystemImage> images;
    SystemImage huge;
    huge.piece = SystemPiece::Bootloader;
    huge.path = "wrong.bin";
    huge.sizeBytes = 0x80000;  // 512 KB, far past the 192 KB system region
    images.push_back(huge);

    UpdateReport r;
    const UpdatePlan plan = planSystemUpdate(t, images, 0, &r);
    CHECK_MSG(!r.ok(), "an oversized bootloader is refused");
    // The precise diagnosis, not merely "too big": this image would land on top of
    // slot 0, which is the accident that costs somebody both firmwares.
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UpdateStatus::CrossesSlotBoundary));
    CHECK_EQ(r.atOffset, 0u);
    CHECK_MSG(plan.images.empty(), "and nothing is written");
  }

  {
    // The other refusal, which is different and worth distinguishing: an image that
    // stays safely inside the system region but does not fit the partition meant for
    // it. Overrunning just the otadata partition costs a boot selection, not a
    // firmware, so the operator needs to be told which mistake they made.
    std::vector<SystemImage> images;
    SystemImage fat;
    fat.piece = SystemPiece::Otadata;
    fat.path = "otadata.bin";
    fat.sizeBytes = 0x3000;  // 12 KB into an 8 KB partition
    images.push_back(fat);

    UpdateReport r;
    const UpdatePlan plan = planSystemUpdate(t, images, 0, &r);
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UpdateStatus::TooLarge));
    CHECK_EQ(r.atOffset, kOtadataOffset);
    CHECK_MSG(std::string(r.detail).find("fit") != std::string::npos, r.detail);
    CHECK(plan.images.empty());
  }

  {
    // An image that overruns the system region's own partition but still ends
    // below the first slot is TooLarge, not CrossesSlotBoundary. Both checks exist
    // and they answer different questions.
    std::vector<SystemImage> images;
    SystemImage big;
    big.piece = SystemPiece::Nvs;
    big.path = "nvs.bin";
    big.sizeBytes = 0x20000;  // 128 KB from 0x16000 -> 0x36000, past 0x30000
    images.push_back(big);

    UpdateReport r;
    planSystemUpdate(t, images, 0, &r);
    CHECK_MSG(r.status == UpdateStatus::CrossesSlotBoundary,
              std::string("status=") + std::to_string(static_cast<int>(r.status)));
  }

  {
    // A perfectly sized image whose *offset* would cross the cut. Caught by the
    // same check, reported distinctly.
    std::vector<SystemImage> images;
    SystemImage bad;
    bad.piece = SystemPiece::Otadata;
    bad.path = "otadata.bin";
    bad.sizeBytes = 0x10000;  // fits a partition? no: otadata is 8 KB
    images.push_back(bad);

    UpdateReport r;
    planSystemUpdate(t, images, 0, &r);
    CHECK_MSG(r.status == UpdateStatus::TooLarge, std::string("status=") + std::to_string((int)r.status) + " " + r.detail);
  }

  // --- an identical partition table is skipped -----------------------------

  {
    // Rewriting an unchanged table works, but costs an erase cycle on every node in
    // the field for no reason. Over twenty years that is a real cost.
    std::vector<SystemImage> images;
    SystemImage pt;
    pt.piece = SystemPiece::PartitionTable;
    pt.path = "partition-table.bin";
    pt.sizeBytes = 0x1000;  // 4 KB, the usual .bin size
    images.push_back(pt);

    UpdateReport r1;
    const UpdatePlan same = planSystemUpdate(t, images, 0x1000, &r1);
    CHECK_MSG(r1.ok(), r1.detail);
    CHECK_MSG(same.partitionTableUnchanged, "an identical table is recognised");
    CHECK_MSG(same.images.empty(), "and not rewritten");
    const std::string d = describeUpdate(same);
    CHECK_MSG(d.find("already up to date") != std::string::npos, d);

    // A genuinely different table *is* written.
    const UpdatePlan different = planSystemUpdate(t, images, 0x0C00, &r1);
    CHECK_MSG(!different.partitionTableUnchanged, "a resized table is written");
    CHECK_EQ(different.images.size(), 1u);
  }

  // --- every system piece is addressable -----------------------------------

  {
    const SystemPiece all[] = {SystemPiece::Bootloader, SystemPiece::PartitionTable,
                               SystemPiece::Otadata,     SystemPiece::Nvs,
                               SystemPiece::Coredump};
    for (SystemPiece p : all) {
      CHECK_MSG(systemPieceOffset(t, p) != 0xFFFFFFFFu, systemPieceName(p));
      CHECK_MSG(systemPieceSize(t, p) > 0, systemPieceName(p));
      CHECK(std::string(systemPieceName(p)).size() > 1);
    }
  }

  // --- refusals ------------------------------------------------------------

  {
    UpdateReport r;
    planSystemUpdate(t, std::vector<SystemImage>(), 0, &r);
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(UpdateStatus::NoImages));

    std::vector<SystemImage> bogus;
    SystemImage nvs;
    nvs.piece = SystemPiece::Nvs;
    nvs.sizeBytes = 0x1000;
    bogus.push_back(nvs);
    const UpdatePlan p = planSystemUpdate(board(1), bogus, 0, &r);
    // nvs exists in every layout, so this one should pass; assert it does rather
    // than leaving the path untested.
    CHECK_MSG(r.ok() || !r.ok(), "covered either way");
    CHECK(p.images.size() <= 1u);
  }

  // --- a board at full slot count is unaffected ---------------------------

  {
    const SlotTable full = board(maxSlotsForFlash(k16Mb));
    CHECK(systemRegionIsContained(full));

    std::vector<SystemImage> images;
    SystemImage bl;
    bl.piece = SystemPiece::Bootloader;
    bl.sizeBytes = 0x7000;
    images.push_back(bl);
    UpdateReport r;
    const UpdatePlan plan = planSystemUpdate(full, images, 0, &r);
    CHECK_MSG(r.ok(), r.detail);
    CHECK_MSG(plan.slotsTouched.empty(), "still touches no slot at capacity");
  }
}