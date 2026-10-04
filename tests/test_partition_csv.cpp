// SPDX-License-Identifier: MIT
//
// Validates the partition tables this repo actually ships.
//
// These read the CSV files off disk rather than a copy pasted into the test,
// because the artefact that matters is the one somebody flashes. A test against
// a copy would keep passing while the shipped table quietly rotted.

#include "bridge/SlotTable.hpp"
#include "harness.hpp"
#include "suites.hpp"

using namespace bridge;

namespace {
constexpr std::uint32_t k16Mb = 16u * 1024u * 1024u;
}  // namespace

void suite_partition_csv(const PartitionTableUnderTest* tables, int count) {
  harness::suite("shipped partition tables");

  REQUIRE(tables != nullptr);
  REQUIRE(count > 0);

  for (int i = 0; i < count; ++i) {
    const PartitionTableUnderTest& t = tables[i];
    CHECK_MSG(t.loaded, "could not read " + t.path);
    if (!t.loaded) continue;

    TableReport pr;
    const SlotTable table = SlotTable::parse(t.csv, k16Mb, &pr);
    CHECK_MSG(pr.ok(), t.path + ": parse failed: " + pr.detail);
    if (!pr.ok()) continue;

    const TableReport v = table.validate();
    CHECK_MSG(v.ok(), t.path + ": " + v.detail + " at " + v.label);

    const TableReport f = table.validateFrameworks();
    CHECK_MSG(f.ok(), t.path + ": " + f.detail + " (" + f.label + ")");

    // A table that exactly fills the chip is correct but leaves nothing for the
    // NVS growth, a bootloader update or a partition-table bump. Free space is
    // not wasted space.
    CHECK_MSG(table.freeBytes() > 0, t.path + ": no free flash left over");

    CHECK_MSG(table.highestByteUsed() <= k16Mb, t.path + ": overruns 16MB");

    // Every live framework has its own application slot and its own filesystem.
    for (std::size_t k = 0; k < kReservedFrameworkCount; ++k) {
      const FrameworkSlot& fs = kReservedFrameworks[k];
      const Partition* app = table.find(fs.appLabel);
      const Partition* fsPart = table.find(fs.fsLabel);

      if (fs.required) {
        CHECK_MSG(app != nullptr, t.path + ": missing app slot " + fs.appLabel);
        CHECK_MSG(fsPart != nullptr, t.path + ": missing fs slot " + fs.fsLabel);
      }
      if (app != nullptr) {
        CHECK_MSG(app->type == PartType::App, t.path + ": " + fs.appLabel + " is not an app slot");
        CHECK_MSG(app->size > 0, t.path + ": " + fs.appLabel + " has no size");
        // The silent-brick case. Caught here rather than on a rooftop.
        CHECK_MSG(app->offset % SlotTable::kAppAlignment == 0,
                  t.path + ": " + fs.appLabel + " is not 64KB aligned");
      }
      if (fsPart != nullptr) {
        CHECK_MSG(fsPart->type == PartType::Data, t.path + ": " + fs.fsLabel + " is not data");
      }
    }

    // The isolation guarantee, asserted on the real artefact.
    const Partition* mcFs = table.find("fs_meshcore");
    const Partition* mtFs = table.find("fs_meshtastic");
    if (mcFs != nullptr && mtFs != nullptr) {
      CHECK_MSG(mcFs->offset != mtFs->offset,
                t.path + ": MeshCore and Meshtastic share one filesystem");
      // Distinct offsets, so one firmware cannot format the other's settings away.
      // This is the property that matters and it is asserted above.
      //
      // Both subtypes are SPIFFS today, not LittleFS for Meshtastic, because the
      // partition generator inside the Arduino toolchain that builds this firmware
      // does not know the littlefs keyword -- it predates ESP-IDF 5.0. Converging
      // the type per framework is a change to make when the toolchain can build
      // it; asserting LittleFS here while the shipped table cannot say LittleFS
      // would be a test passing against a fiction.
      CHECK_MSG(mtFs->subType == mcFs->subType,
                t.path + ": the two frameworks must not be handed the same "
                         "filesystem type by accident -- either both SPIFFS or "
                         "distinct types, deliberately");
    }
  }
}
