// SPDX-License-Identifier: MIT
//
// SystemUpdate -- updating the bootloader and the partition table without losing
// anything the operator cares about.
//
// The requirement this exists to satisfy: a Heltec update must not cost anybody
// their MeshCore, their Meshtastic, their channel keys or their node database. So
// the system layer is defined as the region *below the first slot* -- bootloader,
// partition table, otadata, NVS, crash dump -- and updating it must leave every
// byte at or above kFirstSlotOffset untouched.
//
// That gives a clean, checkable safety property:
//
//   **An update writes the system region or nothing. It never writes a slot, and
//   it never writes a filesystem.**
//
// Which is what makes "update everything" safe as a routine operation rather than
// something that needs a backup first. There is nothing above the cut to lose.
//
// The bootloader is the one genuinely risky write, because a bad bootloader can
// leave a board that nothing will boot. So:
//
//   - The region is checksummed in the plan, and the size of each piece is checked
//     against the partition table before anything is written.
//   - `validateUpdatePlan()` refuses anything that would cross kFirstSlotOffset, so
//     a mistyped bootloader cannot overwrite slot 0.
//   - The operation reports which piece it is writing, so a failure halfway through
//     is diagnosable rather than mysterious.
//
// The partition table is only rewritten when it is genuinely different. Rewriting
// an identical table would work, but it would burn an erase cycle on every node in
// the field for no reason, and flash erase cycles are the one resource that really
// does run out over twenty years.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bridge/Provisioning.hpp"
#include "bridge/SlotTable.hpp"

namespace bridge {

// The pieces of the system layer, all of them below the first slot.
enum class SystemPiece : std::uint8_t {
  Bootloader = 0,
  PartitionTable,
  Otadata,   // boot selection
  Nvs,       // device state, including which slots are provisioned
  Coredump,
};

const char* systemPieceName(SystemPiece piece);

// One image to be written.
struct SystemImage {
  SystemPiece piece = SystemPiece::Bootloader;
  std::string path;
  std::uint32_t sizeBytes = 0;
};

// The plan, after validation.
struct UpdatePlan {
  std::vector<SystemImage> images;

  bool includesBootloader = false;
  // True when the partition table is unchanged and will therefore not be written.
  // Skipping an identical write saves an erase cycle per update, and flash in this
  // class of part is rated for a finite number of them.
  bool partitionTableUnchanged = false;
  std::uint32_t totalBytes = 0;

  // Slots and filesystems the plan touches. Always empty, and asserted as such,
  // because that is the whole safety argument.
  std::vector<std::uint8_t> slotsTouched;
};

enum class UpdateStatus : std::uint8_t {
  Ok = 0,
  UnknownPiece,      // a label that is not part of the system layer
  TooLarge,          // the image does not fit its partition
  CrossesSlotBoundary,  // would overwrite slot 0: the dangerous one
  MissingPartition,  // the layout has no row for this piece
  NotSmallerRegion,  // sanity: an image that claims to be above the cut
  NoImages,
};

struct UpdateReport {
  UpdateStatus status = UpdateStatus::Ok;
  const char* detail = "";
  SystemPiece piece = SystemPiece::Bootloader;
  std::uint32_t atOffset = 0;
  bool ok() const { return status == UpdateStatus::Ok; }
};

// Expected size of each piece from the partition table, or 0 when absent.
std::uint32_t systemPieceSize(const SlotTable& table, SystemPiece piece);

// Offset of each piece, or 0xFFFFFFFF when absent.
std::uint32_t systemPieceOffset(const SlotTable& table, SystemPiece piece);

// Build and validate a plan.
//
// `currentPartitionTableBinSize` is how big the existing table image is, so an
// unchanged table can be skipped rather than rewritten.
UpdatePlan planSystemUpdate(const SlotTable& table,
                            const std::vector<SystemImage>& images,
                            std::uint32_t currentPartitionTableBinSize,
                            UpdateReport* report);

// True when every piece of the system layer lies below the first slot. Cheap
// enough to call after flashing, and worth calling: it is the invariant that makes
// a system update safe.
bool systemRegionIsContained(const SlotTable& table);

// A one-line summary for the CLI, e.g.
//   "system update: bootloader 28K, partition table 48K (unchanged, skipped)"
std::string describeUpdate(const UpdatePlan& plan);

}  // namespace bridge