// SPDX-License-Identifier: MIT
//
// StatusPanel -- what the operator actually sees.
//
// This is the "great UX" requirement, and it is mostly restraint. A node on a
// rooftop has exactly one screen and nobody is standing next to it, so the display
// has one job: make the question "is this thing working, and for which mesh?" a
// glance rather than an investigation.
//
// Three lines, 128x64, no scrolling, no menus:
//
//     MC 869.525 SF11  ok        <- the shared carrier, or why it is not
//     MC  128  -94   MT  64      <- frames heard per mesh, last RSSI
//     ota_0 MC  air 4%           <- active slot, state, airtime spent
//
// The design rule throughout: a line that cannot be drawn because something went
// wrong must say so in the space it had. Silently omitting the second mesh because
// there is no room is how a node serving one mesh looks identical to a healthy
// one.

#pragma once

#include <cstdint>

#include "bridge/Airtime.hpp"
#include "bridge/ChannelPlan.hpp"
#include "bridge/Provisioning.hpp"
#include "bridge/RadioPlan.hpp"
#include "bridge/SlotLifecycle.hpp"
#include "bridge/Statistics.hpp"

namespace bridge {

// Fixed line width. The OLED is 128 px; at the usual 6x8 font that is 21
// characters, and the renderer never writes past this.
constexpr std::size_t kPanelColumns = 21;

// Everything the panel draws from. Passed as one struct so a screen refresh is a
// single value copy rather than a sequence of calls that could each be interleaved
// with an update and show a half-old, half-new state.
struct PanelInputs {
  const PlanReport* plan = nullptr;
  const RadioPlanResult* radio = nullptr;
  const Statistics* stats = nullptr;
  const AirtimeGovernor* airtime = nullptr;
  const ProvisioningView* provisioning = nullptr;
  const SlotTable* table = nullptr;
  const DeviceState* device = nullptr;
  std::uint32_t nowMs = 0;
};

// One rendered line, NUL-terminated, never longer than kPanelColumns.
struct PanelLine {
  char text[kPanelColumns + 1] = {0};
  std::size_t length = 0;
  bool valid = false;
};

struct PanelFrame {
  PanelLine line[3];
  std::uint8_t lineCount = 0;

  // The most urgent thing worth saying, or "" when everything is fine. Used for a
  // single-colour LED or a BLE notification, and by tests.
  const char* alert() const;
};

// Render. Never fails: a missing input yields a line saying so, because a blank
// panel is indistinguishable from a broken node.
PanelFrame renderPanel(const PanelInputs& in);

// The individual rows, exposed so they can be tested and reused on the CLI.
PanelLine renderPlanLine(const PanelInputs& in);
PanelLine renderMeshLine(const PanelInputs& in);
PanelLine renderSlotLine(const PanelInputs& in);

// Compact form of a counter: 1234 -> "1.2k", so a busy mesh does not push the
// signal strength off the end of the line.
void formatCount(char* out, std::size_t cap, std::uint32_t value);

// Compact RSSI: -94 -> "-94", a sentinel for never-heard is left to the caller.
void formatRssi(char* out, std::size_t cap, std::int16_t rssi);

}  // namespace bridge