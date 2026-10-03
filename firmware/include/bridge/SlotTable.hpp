// SPDX-License-Identifier: MIT
//
// SlotTable -- room for more than two frameworks, checked rather than hoped for.
//
// The 16 MB of external flash on a Heltec V4 comfortably holds two firmwares
// and their filesystems, which is why dual-boot is not a compromise here. It
// also holds three, four, or five. Reserving those slots *now*, before anyone
// writes a Reticulum or LoRaWAN image, is what keeps a later framework from
// needing a repartition that orphans somebody's settings.
//
// Two rules this module exists to enforce, both learned the hard way by the
// existing dual-boot attempts:
//
//   1. Every framework gets its own application slot. Two frameworks sharing an
//      app slot is not dual-boot, it is a coin toss.
//
//   2. Every framework gets its own *filesystem* slot, under its own label.
//      Meshtastic mounts LittleFS and MeshCore mounts SPIFFS. Handed the same
//      partition, each one formats what it finds on boot and the other's
//      settings die without a trace. Distinct labels mean neither ever sees
//      the other's filesystem, so neither can destroy it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bridge {

// ESP-IDF partition table types, plus the two this project cares about.
enum class PartType : std::uint8_t {
  App,
  Data,
};

// Subtypes we act on. Anything else is Data/unknown and only bounds-checked.
enum class PartSubType : std::uint8_t {
  Factory,
  Ota_0,
  Ota_1,
  Ota_2,
  Ota_3,
  Ota_4,
  Ota_5,
  Ota_6,
  Ota_7,
  Nvs,
  Otadata,
  Spiffs,
  LittleFs,
  Coredump,
  Reserved,
  Unknown,
};

struct Partition {
  std::string label;
  PartType type = PartType::Data;
  PartSubType subType = PartSubType::Unknown;
  std::uint32_t offset = 0;
  std::uint32_t size = 0;       // 0 means "to end of flash"
  bool blank = false;           // trailing comma in the CSV: take the remainder
};

enum class TableStatus : std::uint8_t {
  Ok = 0,
  ParseError,
  ExceedsFlash,      // a partition runs past the end of the device
  Overlap,           // two partitions claim the same bytes
  Misaligned,        // not on the required boundary
  MissingSlot,       // a framework has no app slot of its own
  SharedFilesystem,  // two frameworks point at one filesystem label
};

struct TableReport {
  TableStatus status = TableStatus::Ok;
  const char* detail = "";
  // Label of the offending partition where one applies.
  std::string label;
  // Byte offset where the trouble starts, for the CLI.
  std::uint32_t atOffset = 0;

  bool ok() const { return status == TableStatus::Ok; }
};

// A framework that gets its own reserved territory.
//
// `const char*` rather than std::string: this is a static table, and a static
// table of std::string allocates during static initialisation on a target with
// no heap to spare. It also makes the whole table `constexpr`, which gives it
// one instance per program instead of one per translation unit.
struct FrameworkSlot {
  const char* framework = "";   // "meshtastic", "meshcore", ...
  const char* appLabel = "";    // partition label holding its firmware
  const char* fsLabel = "";     // partition label holding its settings
  // True for the frameworks that ship today. Their slots must exist or the image
  // is broken.
  bool required = true;
  // True for a slot carved out for a framework that does not exist yet. Its
  // absence from a particular table is legitimate; its presence but malformed
  // is a bug, because that is the case that would bite whoever fills it in later.
  bool fsReservedButEmpty = false;
};

// Slots reserved on every Heltec V4 image this project produces. Adding a
// framework is one line here; the validator then insists it was given both an
// app slot and a filesystem before anything is allowed to ship.
//
// `inline constexpr` rather than `extern` plus a definition in the .cpp: an
// `extern` declaration in this header would give the definition external
// linkage, and a header-only inline variable has exactly one instance per
// program, which is what a table constant wants to be.
inline constexpr FrameworkSlot kReservedFrameworks[] = {
    {"meshtastic", "ota_1", "fs_meshtastic", true, false},
    {"meshcore", "ota_0", "fs_meshcore", true, false},
    {"reticulum", "ota_2", "fs_reticulum", false, true},
    {"lorawan", "ota_3", "fs_lorawan", false, true},
    {"custom", "ota_4", "fs_custom", false, true},
};

inline constexpr std::size_t kReservedFrameworkCount =
    sizeof(kReservedFrameworks) / sizeof(kReservedFrameworks[0]);

class SlotTable {
 public:
  // `flashSizeBytes` is the real device size. The Heltec V4 ships 16 MB, but
  // reading it from the chip rather than assuming is the difference between a
  // validation and a decoration.
  static SlotTable parse(const std::string& csv, std::uint32_t flashSizeBytes,
                         TableReport* report = nullptr);

  // Required alignment for an application partition on ESP32-S3.
  static constexpr std::uint32_t kAppAlignment = 0x10000;

  const std::vector<Partition>& partitions() const { return parts_; }
  std::uint32_t flashSizeBytes() const { return flashSizeBytes_; }

  // Highest byte any partition claims. Must not exceed flashSizeBytes().
  std::uint32_t highestByteUsed() const;

  // Bytes left unclaimed after every partition.
  std::uint32_t freeBytes() const;

  // Look up by label; nullptr when absent.
  const Partition* find(const std::string& label) const;
  const Partition* find(const char* label) const;

  TableReport validate() const;

  // Confirm every framework in kReservedFrameworks is represented.
  TableReport validateFrameworks() const;

  // Human summary for the OLED/CLI: reserved, used, free.
  const char* describe() const;

 private:
  std::vector<Partition> parts_;
  std::uint32_t flashSizeBytes_ = 0;
};

// Parse "0x1000", "4K", "1536K", "1M", "2M" into bytes. Returns false on
// garbage so the caller can report a real parse error instead of a zero.
bool parseSize(const std::string& text, std::uint32_t* out);

}  // namespace bridge
