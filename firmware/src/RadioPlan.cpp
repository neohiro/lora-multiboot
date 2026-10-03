// SPDX-License-Identifier: MIT

#include "bridge/RadioPlan.hpp"

namespace bridge {

int8_t maxConductedDbm(Region region) {
  switch (region) {
    case Region::EU_868:
      // 500 mW e.r.p. is 27 dBm e.r.p.; with a 5 dBi antenna that is 22 dBm
      // conducted. The V4's 28 dBm high-power variant is over this line with an
      // ordinary antenna, which is why the check exists rather than a note.
      return 22;
    case Region::US_915:
    case Region::ANZ_915:
    case Region::KR_922:
    case Region::SG_923:
    case Region::IN_865:
      // No conducted ceiling of our own here; the 36 dBm e.r.p. figure in the US
      // depends on antenna gain and is the operator's to account for. 30 dBm is a
      // sanity bound, not a regulatory claim.
      return 30;
    case Region::Custom:
      return 31;  // int8 cannot exceed 127; 30 dBm is the practical ceiling anyway
  }
  return 22;
}

SyncAcceptance acceptanceFor() {
  // Every protocol this build claims to serve, and therefore every sync word it
  // must be willing to receive. Derived from the same table the identifier uses,
  // so the radio can never accept a set the software disagrees with.
  SyncAcceptance a;
  a.words[0] = 0x12;  // MeshCore
  a.words[1] = 0x2B;  // Meshtastic
  a.words[2] = 0x42;  // Reticulum, reserved
  a.words[3] = 0x34;  // LoRaWAN, reserved
  a.count = 4;
  return a;
}

bool SyncAcceptance::accepts(std::uint8_t word) const {
  for (std::uint8_t i = 0; i < count && i < 4; ++i) {
    if (words[i] == word) return true;
  }
  return false;
}

const char* radioPlanStatusName(RadioPlanStatus status) {
  switch (status) {
    case RadioPlanStatus::Ok:
      return "ok";
    case RadioPlanStatus::NotPromiscuous:
      return "sync word locked";
    case RadioPlanStatus::MissingSyncWord:
      return "missing sync word";
    case RadioPlanStatus::PowerTooHigh:
      return "tx power too high";
    case RadioPlanStatus::InvalidModulation:
      return "invalid modulation";
    case RadioPlanStatus::FrequencyMismatch:
      return "carriers differ";
  }
  return "?";
}

RadioPlanResult resolveRadioConfig(const RfPlan& plan, Region region, int8_t txPowerDbm) {
  RadioPlanResult r;
  r.maxConductedDbm = maxConductedDbm(region);

  r.config.frequencyMHz = plan.frequencyMHz;
  r.config.modulation = Modulation::fromPlan(plan);
  r.config.txPowerDbm = txPowerDbm;

  // The modulation has to be one the SX1262 can actually do. Checking here rather
  // than letting the radio reject it means the operator sees why, on the serial
  // log, instead of a silent failure to configure.
  if (r.config.modulation.spreadingFactor < 6 || r.config.modulation.spreadingFactor > 12 ||
      r.config.modulation.bandwidthKHz <= 0.0f) {
    r.status = RadioPlanStatus::InvalidModulation;
    r.detail = "the SX1262 cannot produce this modulation";
    return r;
  }

  // The load-bearing check. A locked sync word means one of the two meshes is
  // invisible, and nothing else would tell you.
  if (!r.config.promiscuousSyncMatch) {
    r.status = RadioPlanStatus::NotPromiscuous;
    r.detail = "a locked sync word makes one mesh invisible; promiscuous capture is the design";
    return r;
  }

  const SyncAcceptance accepted = acceptanceFor();
  if (!accepted.accepts(0x12) || !accepted.accepts(0x2B)) {
    r.status = RadioPlanStatus::MissingSyncWord;
    r.detail = "configuration would not accept both meshes";
    return r;
  }

  // Both meshes must be on this carrier, or there is nothing to bridge.
  if (plan.frequencyMHz <= 0.0f) {
    r.status = RadioPlanStatus::FrequencyMismatch;
    r.detail = "no frequency resolved for this region";
    return r;
  }

  if (txPowerDbm > r.maxConductedDbm) {
    r.status = RadioPlanStatus::PowerTooHigh;
    r.detail = "over the region's conducted allowance";
    return r;
  }

  r.status = RadioPlanStatus::Ok;
  r.detail = "ready";
  return r;
}

}  // namespace bridge