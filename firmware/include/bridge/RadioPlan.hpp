// SPDX-License-Identifier: MIT
//
// RadioPlan -- the SX1262 configuration this project requires, expressed as
// policy that can be tested without a radio.
//
// This exists because there is one setting that silently decides whether the
// whole project works, and it is not an obvious one:
//
//   **The radio must not lock its sync word to a single value.**
//
// In normal packet mode the SX1262 strips the sync word and reports a packet only
// if it matches the configured one. A radio locked to MeshCore's 0x12 is blind to
// Meshtastic; a radio locked to 0x2B is blind to MeshCore. Both then look
// perfectly healthy -- they receive, they repeat, they answer their own mesh --
// while the other mesh is simply absent. That is the most expensive possible
// failure for this architecture and it produces no error anywhere.
//
// So promiscuous capture is not a tuning choice here, it is the mechanism the
// design depends on, and `resolveRadioConfig()` refuses to produce a
// configuration that lacks it.
//
// Everything else in this file is the ordinary detail that still has to be right:
// preamble length, explicit header, the low-data-rate optimisation, and a
// transmit power that respects the region.

#pragma once

#include <cstddef>
#include <cstdint>

#include "bridge/Airtime.hpp"
#include "bridge/ChannelPlan.hpp"
#include "bridge/ProtocolId.hpp"

namespace bridge {

// SX126x IRQ flags, named for what this project uses them for. Values are the
// datasheet's, not invented.
enum RadioIrq : std::uint16_t {
  kIrqTxDone = 1u << 0,
  kIrqRxDone = 1u << 1,
  kIrqCrcError = 1u << 3,
  kIrqCadDone = 1u << 10,
  kIrqCadDetected = 1u << 11,
  kIrqTimeout = 1u << 14,
};

// A complete, reviewable radio configuration.
struct RadioConfig {
  float frequencyMHz = 869.525f;
  Modulation modulation;

  // Must be true. See the note at the top of this file.
  bool promiscuousSyncMatch = true;
  // The sync word used when *transmitting*. Receiving is promiscuous, but a
  // transmitted frame still needs one, and it is the bridge's own identity on the
  // air. Set to MeshCore's by default because this firmware is MeshCore-derived.
  std::uint8_t txSyncWord = 0x12;

  // Channel activity detection. Meshtastic's managed-flood model depends on
  // hearing the channel before transmitting, so CAD is not optional.
  bool cadEnabled = true;
  std::uint8_t cadSymbols = 2;

  int8_t txPowerDbm = 22;

  // Everything worth waking the CPU for.
  std::uint16_t rxIrqMask = kIrqRxDone | kIrqCrcError | kIrqTimeout | kIrqCadDone |
                            kIrqCadDetected | kIrqTxDone;

  // Front-end module. The V4.2 uses GC1109, the V4.3 a KCT8103L with a switchable
  // LNA; both are handled upstream in firmware and neither is decided here.
  bool femLnaEnabled = true;
};

enum class RadioPlanStatus : std::uint8_t {
  Ok = 0,
  NotPromiscuous,   // fatal for this architecture
  MissingSyncWord,  // a protocol we claim to serve is not in the accepted set
  PowerTooHigh,     // exceeds the region's conducted allowance
  InvalidModulation,
  FrequencyMismatch,  // the two meshes do not share this carrier
};

struct RadioPlanResult {
  RadioPlanStatus status = RadioPlanStatus::Ok;
  RadioConfig config;
  const char* detail = "";
  // For PowerTooHigh: what the region allows, and what was asked for.
  int8_t maxConductedDbm = 0;

  bool ok() const { return status == RadioPlanStatus::Ok; }
};

// Conducted TX power ceiling for a region, in dBm.
//
// EU 869.4-869.65 MHz permits up to 500 mW e.r.p. under the harmonised table,
// which is 27 dBm e.r.p. With a 5 dBi antenna that leaves 22 dBm conducted. The
// V4's high-power variant can produce 28 dBm, which is over that line with an
// ordinary antenna -- a real and easy mistake to make on a node that advertises
// "28 dBm".
int8_t maxConductedDbm(Region region);

// The sync words this configuration will accept, which is what promiscuous
// capture means in practice.
struct SyncAcceptance {
  std::uint8_t words[4] = {0, 0, 0, 0};
  std::uint8_t count = 0;
  bool accepts(std::uint8_t word) const;
};

SyncAcceptance acceptanceFor();

// Build a configuration from a resolved plan, validating the things that would
// otherwise fail silently on a rooftop.
RadioPlanResult resolveRadioConfig(const RfPlan& plan, Region region,
                                   int8_t txPowerDbm = 22);

const char* radioPlanStatusName(RadioPlanStatus status);

}  // namespace bridge