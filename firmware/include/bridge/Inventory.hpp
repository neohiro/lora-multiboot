// SPDX-License-Identifier: MIT
//
// Slots in use, and the one that always stays free.
//
// Two things here that the earlier lifecycle work implied but did not enforce.
//
// **One slot is always free.** A board whose last free slot has been filled has no
// way to recover: `flash.py` cannot write a bad image back, the bootloader has no
// rescue path, and the owner needs a programmer and physical access to a node that
// is probably on a mast. So the final slot is reserved and cannot be provisioned,
// and `provision()` refuses it by name rather than by accident.
//
// It is called the **"Free <hardware> slot"** throughout, because that is the name
// an operator sees in a CLI or on a screen and it should say what it is for. A slot
// called `ota_4` tells nobody anything; one called "Free Heltec LoRa 32 V4 slot"
// tells the person holding a screwdriver exactly what it is.
//
// **Every installed slot is a live app.** Not a partition, not an offset: a thing
// with a name, a version, a role and a state that something on a connected machine
// can enumerate, read logs from, and eventually draw. `SlotInventory` is that view,
// and it serialises to one line per slot so a host tool needs no shared code and no
// special build.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bridge/Provisioning.hpp"
#include "bridge/Roles.hpp"
#include "bridge/SlotLifecycle.hpp"
#include "bridge/SlotTable.hpp"

namespace bridge {

// One slot as a live application.
struct LiveSlot {
  std::uint8_t index = 0;
  std::string name;         // "ota_1", or "Free Heltec LoRa 32 V4 slot"
  SlotState state = SlotState::Empty;
  Framework framework = Framework::Custom;
  Role role = Role::Config;
  bool purposeKnown = false;

  std::uint32_t offset = 0;
  std::uint32_t sizeBytes = 0;
  std::uint32_t fsOffset = 0;
  std::uint32_t fsSize = 0;

  bool bootsByDefault = false;
  bool isFreeSlot = false;

  // Fields a connected machine needs to show something useful.
  std::string firmwareVersion;   // from the image, once it reports one
  std::string lastLogLine;       // ring-buffer tail, for the host UI
  std::uint32_t lastSeenMs = 0;  // last time this slot proved it was alive
  std::uint8_t bootFailures = 0;
};

// The whole board as a set of live apps.
struct SlotInventory {
  std::vector<LiveSlot> slots;
  std::uint8_t activeSlot = 0xFF;
  std::uint8_t freeCount = 0;
  bool recoveryPossible = false;
};

SlotInventory inventory(const SlotTable& table, const DeviceState& state,
                        const char* hardware);

// One line per slot, for a serial log or a host tool that speaks nothing else.
//
// Deliberately not JSON: a node that cannot answer a request for firmware version
// must still be able to answer "what is installed", and a hand-rolled line format
// costs a fraction of the RAM and flash a JSON writer on a microcontroller would.
std::vector<std::string> inventoryLines(const SlotInventory& inv);

// A single summary line, e.g.
//   "4 slots: 2 live, 1 empty, 1 free -- recovery OK"
std::string inventorySummary(const SlotInventory& inv);

}  // namespace bridge