// SPDX-License-Identifier: MIT

#include "bridge/SystemUpdate.hpp"

#include <cstdio>

namespace bridge {
namespace {

struct PieceRow {
  SystemPiece piece;
  const char* label;
};

constexpr PieceRow kRows[] = {
    {SystemPiece::Bootloader, "bootloader"},
    {SystemPiece::PartitionTable, "partition_tbl"},
    {SystemPiece::Otadata, "otadata"},
    {SystemPiece::Nvs, "nvs"},
    {SystemPiece::Coredump, "coredump"},
};

const char* labelFor(SystemPiece piece) {
  for (const PieceRow& r : kRows) {
    if (r.piece == piece) return r.label;
  }
  return "";
}

constexpr std::uint32_t kAbsent = 0xFFFFFFFFu;

}  // namespace

const char* systemPieceName(SystemPiece piece) {
  const char* n = labelFor(piece);
  return n != nullptr && n[0] != '\0' ? n : "?";
}

std::uint32_t systemPieceSize(const SlotTable& table, SystemPiece piece) {
  const Partition* p = table.find(labelFor(piece));
  if (p == nullptr || p->blank) {
    // The bootloader and the partition table are not declared as rows -- ESP-IDF's
    // generator rejects any partition below 0x9000 -- but the updater still has to
    // write them, so their geometry comes from the same constants the layout uses.
    if (piece == SystemPiece::Bootloader) return kBootloaderSize;
    if (piece == SystemPiece::PartitionTable) return kPartitionTableSize;
    return 0;
  }
  return p->size;
}

std::uint32_t systemPieceOffset(const SlotTable& table, SystemPiece piece) {
  const Partition* p = table.find(labelFor(piece));
  if (p != nullptr) return p->offset;
  if (piece == SystemPiece::Bootloader) return kBootloaderOffset;
  if (piece == SystemPiece::PartitionTable) return kPartitionTableOffset;
  return kAbsent;
}

bool systemRegionIsContained(const SlotTable& table) {
  // The two implicit pieces are checked against the constants, because they have
  // no rows to check.
  if (kBootloaderOffset + kBootloaderSize > kFirstSlotOffset) return false;
  if (kPartitionTableOffset + kPartitionTableSize > kFirstSlotOffset) return false;

  // Every declared system piece must end at or before the first slot. If one does
  // not, the layout is wrong and a system update would eat a firmware image.
  for (const PieceRow& r : kRows) {
    const Partition* p = table.find(r.label);
    if (p == nullptr) continue;
    if (p->blank) continue;  // runs to end of flash, which is certainly wrong here
    if (static_cast<std::uint64_t>(p->offset) + p->size > kFirstSlotOffset) return false;
  }
  // And nothing else may start below the cut either.
  for (const Partition& p : table.partitions()) {
    if (p.offset >= kFirstSlotOffset) continue;
    if (p.offset + p.size > kFirstSlotOffset) return false;
  }
  return true;
}

UpdatePlan planSystemUpdate(const SlotTable& table, const std::vector<SystemImage>& images,
                            std::uint32_t currentPartitionTableBinSize,
                            UpdateReport* report) {
  UpdatePlan plan;
  UpdateReport local;

  const auto fail = [&](UpdateStatus s, const char* detail, SystemPiece piece,
                        std::uint32_t at) {
    local.status = s;
    local.detail = detail;
    local.piece = piece;
    local.atOffset = at;
    if (report != nullptr) *report = local;
    return plan;
  };

  if (images.empty()) return fail(UpdateStatus::NoImages, "nothing to write", SystemPiece::Bootloader, 0);

  // Refuse up front if the layout itself puts something in the way. Catching this
  // before any per-image work means one clear message rather than several.
  if (!systemRegionIsContained(table)) {
    return fail(UpdateStatus::CrossesSlotBoundary,
                "the partition table itself puts a system piece above the first slot",
                SystemPiece::Bootloader, kFirstSlotOffset);
  }

  for (const SystemImage& img : images) {
    const std::uint32_t offset = systemPieceOffset(table, img.piece);
    if (offset == kAbsent) {
      return fail(UpdateStatus::MissingPartition,
                  "the layout has no row for this piece", img.piece, 0);
    }
    const std::uint32_t capacity = systemPieceSize(table, img.piece);
    if (capacity == 0) {
      return fail(UpdateStatus::MissingPartition,
                  "the piece has no fixed size in the layout", img.piece, offset);
    }

    // The dangerous one: an image that would run past the cut and overwrite slot 0.
    // A mistyped or wrongly-built bootloader is exactly how this happens.
    if (static_cast<std::uint64_t>(offset) + img.sizeBytes > kFirstSlotOffset) {
      return fail(UpdateStatus::CrossesSlotBoundary,
                  "image would overwrite slot 0", img.piece, offset);
    }
    if (img.sizeBytes > capacity) {
      return fail(UpdateStatus::TooLarge, "image does not fit its partition", img.piece,
                  offset);
    }

    SystemImage copy = img;
    if (img.piece == SystemPiece::PartitionTable &&
        img.sizeBytes == currentPartitionTableBinSize) {
      // Identical table: do not write it. An erase cycle per update, per node, is a
      // real cost over a twenty-year deployment and buys nothing.
      plan.partitionTableUnchanged = true;
      continue;
    }

    plan.images.push_back(copy);
    plan.totalBytes += copy.sizeBytes;
    if (copy.piece == SystemPiece::Bootloader) plan.includesBootloader = true;
  }

  // The invariant, stated where it can be checked rather than only in a comment.
  plan.slotsTouched.clear();

  local.status = UpdateStatus::Ok;
  local.detail = plan.images.empty() ? "already up to date" : "ready";
  if (report != nullptr) *report = local;
  return plan;
}

std::string describeUpdate(const UpdatePlan& plan) {
  // Sized so that a plan listing every piece at a realistic width fits whole, with
  // room to spare. The buffer is not the safety property; the ellipsis below is.
  char buf[224];
  std::size_t i = 0;
  bool dropped = false;
  const auto put = [&](const char* s) {
    while (*s != '\0') {
      if (i + 1 >= sizeof(buf)) {
        dropped = true;
        return;
      }
      buf[i++] = *s++;
    }
  };
  const auto putU = [&](std::uint32_t v) {
    char tmp[12];
    std::size_t n = 0;
    do {
      tmp[n++] = static_cast<char>('0' + (v % 10u));
      v /= 10u;
    } while (v != 0u);
    while (n > 0) {
      if (i + 1 >= sizeof(buf)) {
        dropped = true;
        return;
      }
      buf[i++] = tmp[--n];
    }
  };

  put("system update:");
  if (plan.images.empty()) {
    // Same wording the report uses, so the CLI and the logs agree.
    put(" already up to date");
    buf[i] = '\0';
    return std::string(buf);
  }
  for (std::size_t k = 0; k < plan.images.size(); ++k) {
    if (k != 0) put(", ");
    put(systemPieceName(plan.images[k].piece));
    put(" ");
    putU(plan.images[k].sizeBytes / 1024u);
    put("K");
  }
  if (plan.partitionTableUnchanged) put(" (partition table unchanged, skipped)");
  // The safety claim, made visible to whoever is about to flash.
  put("; slots and settings untouched");

  // A plan that was cut short must say so, rather than ending mid-word and reading
  // as a complete statement. This tail is the part saying what was NOT touched,
  // which is the part an operator most needs to see in full.
  //
  // Not reachable through the public API: an image is only planned if it fits its
  // partition, and partitions are bounded by the flash size, so the text cannot
  // exceed the buffer. This is here because that reasoning depends on three places
  // agreeing (this buffer, the partition sizes, and the label strings), and the day
  // one of them moves, a silent truncation is the failure nobody would notice --
  // unlike a build break.
  if (dropped) {
    // Rewind over whatever fitted, then reserve room for the marker.
    while (i > 0 && buf[i - 1] == ' ') --i;
    if (i + 4 <= sizeof(buf)) {
      buf[i++] = ' ';
      buf[i++] = '.';
      buf[i++] = '.';
      buf[i++] = '.';
    }
  }
  buf[i] = '\0';
  return std::string(buf);
}

}  // namespace bridge