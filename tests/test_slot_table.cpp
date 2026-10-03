// SPDX-License-Identifier: MIT
//
// SlotTable tests.
//
// Two failure modes are worth more than the happy path here, because both
// present on the bench as an inexplicably dead board: an app partition that is
// not 64KB aligned, and two partitions claiming the same bytes. Neither throws
// an error at build time, so both have to be caught by parsing the table.

#include "bridge/SlotTable.hpp"
#include "harness.hpp"

using namespace bridge;

namespace {

constexpr std::uint32_t k16Mb = 16u * 1024u * 1024u;

const char* kGood =
    "# name,   type, subtype,  offset,   size,     flags\n"
    "bootloader,  app,  factory,  0x0,      0x6000,\n"
    "partition_tbl, data, nvs,     0x8000,   0xC000,\n"
    "otadata,      data,  otadata, 0x14000,  0x2000,\n"
    "ota_0,        app,  ota_0,    0x20000,  0x440000,\n"
    "ota_1,        app,  ota_1,    0x460000, 0x440000,\n"
    "fs_meshcore,  data, spiffs,   0x8A0000, 0x200000,\n"
    "fs_meshtastic,data, littlefs,0xAA0000,0x200000,\n"
    "coredump,     data, coredump, 0xCA0000, 0x10000,\n";

}  // namespace

void suite_slot_table() {
  harness::suite("SlotTable");

  // --- size parsing ---------------------------------------------------------

  {
    std::uint32_t v = 0;
    CHECK(parseSize("0x10000", &v) && v == 0x10000);
    CHECK(parseSize("4K", &v) && v == 4096);
    CHECK(parseSize("1536K", &v) && v == 1536u * 1024u);
    CHECK(parseSize("1M", &v) && v == 1024u * 1024u);
    CHECK(parseSize("2M", &v) && v == 2u * 1024u * 1024u);
    CHECK(parseSize("4096", &v) && v == 4096);

    CHECK(!parseSize("", &v));
    CHECK(!parseSize("nonsense", &v));
    CHECK(!parseSize("12X", &v));
    CHECK(!parseSize("0x", &v));
    CHECK(!parseSize("4K", nullptr));
  }

  // --- parsing a good table -------------------------------------------------

  {
    TableReport pr;
    const SlotTable t = SlotTable::parse(kGood, k16Mb, &pr);
    CHECK_EQ(static_cast<int>(pr.status), static_cast<int>(TableStatus::Ok));
    CHECK_MSG(t.partitions().size() == 8, "every row parsed");

    const Partition* ota0 = t.find("ota_0");
    REQUIRE(ota0 != nullptr);
    CHECK_EQ(ota0->offset, 0x20000u);
    CHECK_EQ(ota0->size, 0x440000u);
    CHECK(ota0->type == PartType::App);

    const Partition* lfs = t.find("fs_meshtastic");
    REQUIRE(lfs != nullptr);
    CHECK(lfs->subType == PartSubType::LittleFs);
    CHECK(lfs->type == PartType::Data);

    CHECK(t.find("does_not_exist") == nullptr);
    CHECK(t.flashSizeBytes() == k16Mb);
    CHECK(t.freeBytes() > 0);
  }

  // --- the good table validates ---------------------------------------------

  {
    const SlotTable t = SlotTable::parse(kGood, k16Mb);
    const TableReport v = t.validate();
    CHECK_MSG(v.ok(), v.detail);
  }

  // --- numeric OTA subtypes, which is how ESP-IDF writes ota_4 --------------

  {
    const char* csv =
        "ota_0, app, 16, 0x10000, 0x100000,\n"
        "ota_4, app, 20, 0x110000, 0x100000,\n";
    const SlotTable t = SlotTable::parse(csv, k16Mb);
    const Partition* ota4 = t.find("ota_4");
    REQUIRE(ota4 != nullptr);
    CHECK_EQ(static_cast<int>(ota4->subType), static_cast<int>(PartSubType::Ota_4));
  }

  // --- overlap: two rows claiming the same bytes ----------------------------

  {
    const char* csv =
        "a, app,  factory, 0x10000, 0x100000,\n"
        "b, app,  ota_0,   0x10000, 0x100000,\n";  // same offset on purpose
    const SlotTable t = SlotTable::parse(csv, k16Mb);
    const TableReport v = t.validate();
    CHECK_EQ(static_cast<int>(v.status), static_cast<int>(TableStatus::Overlap));
    CHECK_MSG(std::string(v.label) == "b", "the later row is named");
  }

  // --- partial overlap also caught ------------------------------------------
  //
  // `a` ends at 0x110000 and `b` starts at 0x100000, which is inside `a` while
  // still landing on a 64 KB boundary -- otherwise this would trip the alignment
  // check first and prove nothing about overlap.
  {
    const char* csv =
        "a, app, factory, 0x10000, 0x100000,\n"
        "b, app, ota_0,   0x100000, 0x100000,\n";
    const SlotTable t = SlotTable::parse(csv, k16Mb);
    const TableReport v = t.validate();
    CHECK_EQ(static_cast<int>(v.status), static_cast<int>(TableStatus::Overlap));
    CHECK(std::string(v.label) == "b");
  }

  // --- a zero size written literally is not "the rest of the chip" -----------
  //
  // The bootloader legitimately lives at offset 0, and an explicit 0x0 must not
  // be mistaken for the ESP-IDF blank-size shorthand that runs to end of flash.
  {
    const char* csv =
        "bootloader, app, factory, 0x0,    0x7000,\n"
        "tiny,       app, ota_0,   0x10000, 0x0,\n";
    const SlotTable t = SlotTable::parse(csv, k16Mb);
    const Partition* tiny = t.find("tiny");
    REQUIRE(tiny != nullptr);
    CHECK_MSG(!tiny->blank, "explicit 0x0 is a size, not a blank");
    CHECK_EQ(tiny->size, 0u);
    CHECK_MSG(t.validate().ok(), t.validate().detail);
  }

  // --- running past the end of flash ----------------------------------------

  {
    const char* csv = "a, app, factory, 0x10000, 0x2000000,\n";  // 32MB on a 16MB board
    const SlotTable t = SlotTable::parse(csv, k16Mb);
    const TableReport v = t.validate();
    CHECK_EQ(static_cast<int>(v.status), static_cast<int>(TableStatus::ExceedsFlash));
    CHECK(std::string(v.label) == "a");
  }

  // --- misaligned app partition: the silent-brick case ----------------------

  {
    // 0x11000 is not a multiple of 64KB. The bootloader will refuse this and the
    // board will look simply dead, with nothing on the serial log.
    const char* csv = "a, app, factory, 0x11000, 0x100000,\n";
    const SlotTable t = SlotTable::parse(csv, k16Mb);
    const TableReport v = t.validate();
    CHECK_EQ(static_cast<int>(v.status), static_cast<int>(TableStatus::Misaligned));
    CHECK_EQ(v.atOffset, 0x11000u);
  }

  {
    // Data partitions have no such alignment requirement.
    const char* csv = "d, data, nvs, 0x11000, 0x1000,\n";
    const SlotTable t = SlotTable::parse(csv, k16Mb);
    CHECK_MSG(t.validate().ok(), "a misaligned data partition is fine");
  }

  // --- parse errors are reported, not swallowed -----------------------------

  {
    TableReport pr;
    SlotTable::parse("only,three,fields\n", k16Mb, &pr);
    CHECK_EQ(static_cast<int>(pr.status), static_cast<int>(TableStatus::ParseError));

    TableReport pr2;
    SlotTable::parse("# just a comment\n", k16Mb, &pr2);
    CHECK_EQ(static_cast<int>(pr2.status), static_cast<int>(TableStatus::ParseError));

    TableReport pr3;
    SlotTable::parse("a, app, factory, 0xZ000, 0x1000,\n", k16Mb, &pr3);
    CHECK_EQ(static_cast<int>(pr3.status), static_cast<int>(TableStatus::ParseError));
  }

  // --- comments and blank lines are skipped ---------------------------------

  {
    const char* csv =
        "\n"
        "# a comment\n"
        "\n"
        "a, app, factory, 0x10000, 0x100000,\n"
        "# trailing comment\n";
    TableReport pr;
    const SlotTable t = SlotTable::parse(csv, k16Mb, &pr);
    CHECK_EQ(static_cast<int>(pr.status), static_cast<int>(TableStatus::Ok));
    CHECK_EQ(t.partitions().size(), 1u);
  }

  // --- framework isolation --------------------------------------------------

  {
    // kGood carries both live frameworks with isolated filesystems, so it passes
    // even though it reserves nothing for future ones.
    const SlotTable t = SlotTable::parse(kGood, k16Mb);
    const TableReport f = t.validateFrameworks();
    CHECK_MSG(f.ok(), f.detail);
  }

  {
    // A lean two-framework table: the dual-boot image. Reserving nothing for the
    // future is legitimate, so this must pass.
    const char* csv =
        "ota_0,         app,  ota_0, 0x20000,  0x400000,\n"
        "ota_1,         app,  ota_1, 0x420000, 0x400000,\n"
        "fs_meshcore,   data, spiffs,0x820000, 0x200000,\n"
        "fs_meshtastic, data, littlefs,0xA20000,0x200000,\n";
    const SlotTable t = SlotTable::parse(csv, k16Mb);
    CHECK_MSG(t.validate().ok(), t.validate().detail);
    CHECK_MSG(t.validateFrameworks().ok(), t.validateFrameworks().detail);
  }

  {
    // A reserved slot that is present but declared as the wrong type is a bug,
    // not a free pass: it is what would break whoever tries to fill it in later.
    const char* csv =
        "ota_0,         app,  ota_0, 0x20000, 0x400000,\n"
        "ota_1,         app,  ota_1, 0x420000,0x400000,\n"
        "fs_meshcore,   data, spiffs,0x820000,0x200000,\n"
        "fs_meshtastic, data, littlefs,0xA20000,0x200000,\n"
        "ota_2,         data, ota_2, 0xC20000,0x200000,\n";  // app slot declared data
    const SlotTable t = SlotTable::parse(csv, k16Mb);
    const TableReport f = t.validateFrameworks();
    CHECK_EQ(static_cast<int>(f.status), static_cast<int>(TableStatus::MissingSlot));
  }

  {
    // Two frameworks sharing one filesystem label is the mistake that quietly
    // destroys settings: each firmware formats what it finds on boot.
    std::string csv =
        "ota_0, app, ota_0, 0x10000, 0x400000,\n"
        "ota_1, app, ota_1, 0x410000, 0x400000,\n"
        "ota_2, app, ota_2, 0x810000, 0x400000,\n"
        "ota_3, app, ota_3, 0xC10000, 0x300000,\n"
        "ota_4, app, ota_4, 0xF10000, 0x300000,\n";
    for (std::size_t i = 0; i < kReservedFrameworkCount; ++i) {
      csv += kReservedFrameworks[i].fsLabel;
      csv += ", data, spiffs, 0x1000000, 0x100000,\n";
    }
    const SlotTable t = SlotTable::parse(csv, k16Mb);
    const TableReport f = t.validateFrameworks();
    CHECK_MSG(f.ok(), f.detail);
    // Distinct labels are what makes the isolation real.
    for (std::size_t i = 0; i < kReservedFrameworkCount; ++i) {
      for (std::size_t j = i + 1; j < kReservedFrameworkCount; ++j) {
        CHECK_MSG(kReservedFrameworks[i].fsLabel != kReservedFrameworks[j].fsLabel,
                  "no two frameworks share a filesystem label");
      }
    }
  }

  {
    // A table with only one slot is a legitimate state: under progressive
    // provisioning that board runs MeshCore alone and offers one connection.
    // Requiring Meshtastic's slot here would make the first successful
    // provisioning look like a broken table.
    const char* csv = "ota_0, app, ota_0, 0x10000, 0x400000,\n";
    const SlotTable t = SlotTable::parse(csv, k16Mb);
    const TableReport f = t.validateFrameworks();
    CHECK_MSG(f.ok(), "one framework alone is isolated and fine");
  }

  // --- describe -------------------------------------------------------------

  {
    const SlotTable t = SlotTable::parse(kGood, k16Mb);
    const std::string d = t.describe();
    CHECK_MSG(d.find("16MB") != std::string::npos, d);
    CHECK_MSG(d.find("free") != std::string::npos, d);
  }

  // --- at least two frameworks are reserved, and more ------------------------

  CHECK_MSG(kReservedFrameworkCount >= 2, "dual-boot is the floor");
  CHECK_MSG(kReservedFrameworkCount > 2, "slots are held for future frameworks");
}
