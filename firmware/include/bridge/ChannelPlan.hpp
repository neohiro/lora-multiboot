// SPDX-License-Identifier: MIT
//
// ChannelPlan -- the reason this whole idea is possible, stated as code.
//
// Meshtastic EU_868 LongFast and the MeshCore default channel are not merely
// close on the Heltec V4's single SX1262. They are the *same* RF plan:
//
//     frequency  869.525 MHz   (both)
//     bandwidth  250 kHz       (both)
//     SF         11            (both)
//     coding     4/5           (both)
//     sync word  0x2B vs 0x12  (the only difference)
//
// One radio, one antenna, continuous receive, and the two meshes separated by a
// single byte in the preamble. That is why no time-slicing is required and why
// the bridge does not have to give up airtime to serve both.
//
// The corollary is uncomfortable and is enforced here rather than discovered in
// the field: outside the EU those two defaults *do not coincide*. This module
// exists to say so loudly, at build time, instead of letting a half-configured
// node sit on a hilltop quietly hearing one mesh.

#pragma once

#include <cstddef>
#include <cstdint>

namespace bridge {

enum class Region : std::uint8_t {
  EU_868 = 0,
  US_915,
  ANZ_915,
  IN_865,
  KR_922,
  SG_923,
  Custom,
};

struct RegionDefaults {
  Region region;
  const char* code;

  // Slot range a Meshtastic modem preset can hash into. For the EU this is the
  // 869.40-869.65 MHz sub-band that carries the higher ERP and the 10%
  // duty-cycle allowance, which is narrower than the 863-870 MHz SRD envelope.
  float bandStartMHz;
  float bandEndMHz;

  // LongFast default slot centre for this region.
  float meshtasticFreqMHz;

  // MeshCore community default for this region.
  float meshCoreFreqMHz;

  // Whether meshCoreFreqMHz is a verified published default or a placeholder
  // the operator must confirm. Per-region, because the EU figure is confirmed
  // and most others are not, and a single global flag would either disable the
  // check where it is needed or pretend to a confidence we do not have.
  bool meshCoreFreqVerified;

  // Airtime cap this region expects, in percent. The EU figure is the 10%
  // duty-cycle limit that applies to 869.4-869.65 MHz. US has no general
  // duty-cycle cap, so the cap there is a courtesy bound, not a licence term.
  std::uint8_t dutyCyclePercent;
};

// Name of the region for logs and the OLED.
const RegionDefaults* regionDefaults(Region region);
const RegionDefaults* regionDefaultsByCode(const char* code);

// Name of the region for logs and the OLED.
const char* regionCode(Region region);

// Shared LoRa modulation. Both meshes must agree on all five fields to hear
// each other; sync word is the only one they deliberately do not share.
struct RfPlan {
  float frequencyMHz = 869.525f;
  float bandwidthKHz = 250.0f;
  std::uint8_t spreadingFactor = 11;
  // Coding rate 4/5 is den=5. 4/8 is the odd one out in Meshtastic's table and
  // is represented as den=8.
  std::uint8_t codingRateDenominator = 5;
  std::uint8_t meshtasticSyncWord = 0x2B;
  std::uint8_t meshCoreSyncWord = 0x12;
};

// Everything the bridge needs to tell the operator whether a node is coherent.
enum class PlanStatus : std::uint8_t {
  Ok = 0,
  FrequencyMismatch,   // the two meshes would land on different carriers
  OutOfBand,           // the chosen frequency sits outside the region
  MeshCoreFreqUnknown, // no verified MeshCore default for this region
};

struct PlanReport {
  PlanStatus status = PlanStatus::Ok;
  RfPlan plan;

  // Set when status == FrequencyMismatch: the drift that has to be retuned away.
  float driftMHz = 0.0f;
  // Which side has to move to close the drift.
  const char* retune = "";
  // The single-word reason, for a one-line status display.
  const char* detail = "";

  bool ok() const { return status == PlanStatus::Ok; }
};

// Resolve the shared plan for a region.
//
// `overrideFreqMHz` <= 0 means "use whatever the region resolves to". A
// non-positive override does not suppress an out-of-band finding: asking for a
// frequency outside the region is a configuration error, not a preference.
PlanReport resolvePlan(Region region, float overrideFreqMHz = 0.0f);

// One-line human summary, e.g.
//   "EU_868 869.525MHz SF11/250kHz 4/5 shared OK (airtime<=10%)"
const char* describePlan(const PlanReport& report);

// Airtime cap the bridge must enforce for a resolved plan.
std::uint8_t dutyCycleFor(Region region);

}  // namespace bridge
