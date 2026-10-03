// SPDX-License-Identifier: MIT
//
// SharedContext -- one block of RAM that every slot reads.
//
// The question this answers: what does it actually cost to have several slots on
// one board, and how little of that has to be shared?
//
// The answer this project settled on: **adding a slot costs flash and zero shared
// RAM.** A slot is a firmware image plus its own settings partition. Neither needs
// its own copy of the radio plan, the airtime budget, the frame counters or the
// boot state, because there is only one radio, one plan and one airtime limit on
// the board.
//
// So all of that lives here, once, in a single trivially-copyable block:
//
//   - no dynamic allocation, so it can be placed in a fixed RAM section and
//     mapped at a known address by any slot that needs it
//   - plain data, so a slot can read it without needing this code linked in
//   - a compile-time size ceiling, asserted below, so "low memory profile" is a
//     property the build enforces rather than a claim in a document
//
// A slot reads this block. It does not own one. That is what keeps a five-slot
// board's RAM cost the same as a one-slot board's.
//
// The alternative -- each slot keeping its own plan and counters -- would multiply
// the fixed overhead by the slot count to store information that is, by
// construction, identical across them.

#pragma once

#include <cstddef>
#include <cstdint>

#include "bridge/Airtime.hpp"
#include "bridge/ChannelPlan.hpp"
#include "bridge/RadioPlan.hpp"
#include "bridge/SlotLifecycle.hpp"
#include "bridge/Statistics.hpp"

namespace bridge {

// Where this block lives. Chosen to be in internal SRAM on an ESP32-S3 and
// deliberately below any region a future SDK might claim for its own use.
inline constexpr std::uint32_t kSharedContextMagic = 0x4C4F5241;  // "LORA"

// The block.
//
// Every member is trivially copyable and free of owning pointers, which is what
// makes the whole thing trivially copyable and safe to share between images. The
// static_assert below is the point of the exercise: it turns "this fits in the
// RAM budget" from a comment into something that fails the build if it stops being
// true.
struct SharedContext {
  std::uint32_t magic = kSharedContextMagic;

  // Bumped whenever the layout changes, so a stale slot can detect that it is
  // looking at a block it does not understand rather than misreading it. With
  // several firmwares on one board, a layout mismatch is a realistic failure and
  // silently reading the wrong fields would be the worst possible handling of it.
  std::uint16_t version = 1;
  std::uint16_t sizeBytes = 0;

  // The resolved radio configuration. One plan, because one radio.
  RfPlan plan;
  RadioConfig radio;

  // The compliance budget. Shared, because the 10% limit applies to the antenna,
  // not to each slot individually. A board with five slots that each thought they
  // had the whole 10% would be five times over it.
  AirtimeGovernor airtime;

  // Counters. Shared so the panel can be drawn by whichever slot is running, and
  // so a slot switch does not lose the history.
  Statistics stats;

  // Which slots exist and which one boots.
  DeviceState device;
  BootRecord boot;

  std::uint8_t slotCount = 0;
  std::uint8_t activeSlot = 0xFF;
  std::uint32_t uptimeMs = 0;

  void stamp() { sizeBytes = static_cast<std::uint16_t>(sizeof(SharedContext)); }

  // A slot from an older or newer build must refuse rather than guess.
  bool compatible() const {
    return magic == kSharedContextMagic && version == 1 &&
           sizeBytes == sizeof(SharedContext);
  }
};

// The RAM budget for everything shared across all slots on one board.
//
// 1 KB is generous for what is in here and small enough that it is never the
// reason a node runs out. The static_assert below is the enforcement; this constant
// is the number to argue about if it ever fires.
constexpr std::size_t kSharedContextRamCeiling = 1024;

// Trivially copyable is the whole contract: a slot reads this block, and nothing
// here may own memory that outlives it or live on a heap.
static_assert(std::is_trivially_copyable<SharedContext>::value,
              "SharedContext must be trivially copyable so slots can share it without "
              "linking this code");
static_assert(sizeof(SharedContext) <= kSharedContextRamCeiling,
              "shared RAM budget exceeded");

}  // namespace bridge