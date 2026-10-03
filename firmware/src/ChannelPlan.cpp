// SPDX-License-Identifier: MIT

#include "bridge/ChannelPlan.hpp"

#include <cstring>

namespace bridge {
namespace {

// Frequency agreement tolerance. The SX1262 synthesises from a 32 MHz xtal with
// a fractional-N divider; both meshes' radios sit on the same reference, so the
// real requirement is that the *plans* agree, not the crystals. 1 kHz is far
// tighter than any crystal drift over temperature and far looser than float
// comparison noise, which is exactly the margin wanted here.
constexpr float kFreqToleranceMHz = 0.001f;

constexpr RegionDefaults kRegions[] = {
    // EU_868: the one region where the two community defaults coincide.
    // MeshCore's published default is 869.525 MHz, which is also Meshtastic
    // EU_868 LongFast slot 1. 10% is the regulatory duty-cycle limit for
    // 869.4-869.65 MHz -- NOT MeshCore's stock 50% software default, which is
    // not a legal airtime budget here.
    {Region::EU_868, "EU_868", 869.40f, 869.65f, 869.525f, 869.525f, true, 10},

    // US_915: Meshtastic hashes LongFast to slot 20 = 906.875 MHz on a factory
    // reset. The MeshCore community default is a different channel entirely.
    // Expect FrequencyMismatch and retune one side deliberately.
    {Region::US_915, "US_915", 902.0f, 928.0f, 906.875f, 910.525f, false, 100},

    {Region::ANZ_915, "ANZ_915", 915.0f, 928.0f, 919.875f, 910.525f, false, 100},
    {Region::IN_865, "IN_865", 865.0f, 867.0f, 865.875f, 865.875f, false, 100},
    {Region::KR_922, "KR_922", 920.0f, 923.0f, 922.875f, 922.875f, false, 100},
    {Region::SG_923, "SG_923", 917.0f, 925.0f, 917.875f, 917.875f, false, 100},

    // Custom carries no opinion. An operator who knows their licence has set it;
    // the bridge must not pretend to validate that.
    {Region::Custom, "CUSTOM", 0.0f, 0.0f, 0.0f, 0.0f, true, 100},
};

constexpr std::size_t kRegionCount = sizeof(kRegions) / sizeof(kRegions[0]);

// The EU is the reference case and never moves. Spelled out separately so no
// table edit can quietly change the region this whole project depends on.
constexpr RegionDefaults kEuFallback = {Region::EU_868, "EU_868", 869.40f, 869.65f,
                                        869.525f, 869.525f, true, 10};

const RegionDefaults* findByCode(const char* code) {
  if (code == nullptr) return nullptr;
  for (std::size_t i = 0; i < kRegionCount; ++i) {
    if (std::strcmp(kRegions[i].code, code) == 0) return &kRegions[i];
  }
  return nullptr;
}

bool inBand(const RegionDefaults& d, float mhz) {
  if (d.region == Region::Custom) return true;
  return mhz >= d.bandStartMHz && mhz <= d.bandEndMHz;
}

}  // namespace

const RegionDefaults* regionDefaults(Region region) {
  if (region == Region::EU_868) return &kEuFallback;  // no exceptions, ever
  for (std::size_t i = 0; i < kRegionCount; ++i) {
    if (kRegions[i].region == region) return &kRegions[i];
  }
  return &kEuFallback;
}

const RegionDefaults* regionDefaultsByCode(const char* code) {
  const RegionDefaults* d = findByCode(code);
  return d != nullptr ? d : &kEuFallback;
}

const char* regionCode(Region region) { return regionDefaults(region)->code; }

std::uint8_t dutyCycleFor(Region region) { return regionDefaults(region)->dutyCyclePercent; }

PlanReport resolvePlan(Region region, float overrideFreqMHz) {
  const RegionDefaults* d = regionDefaults(region);

  PlanReport r;
  r.plan.frequencyMHz =
      overrideFreqMHz > 0.0f ? overrideFreqMHz : d->meshtasticFreqMHz;

  if (region == Region::Custom) {
    // No band, no published defaults, no opinion. Whatever was asked for is
    // what it gets, and the caller owns the paperwork.
    r.status = PlanStatus::Ok;
    r.detail = "custom plan, operator-owned";
    return r;
  }

  if (!inBand(*d, r.plan.frequencyMHz)) {
    r.status = PlanStatus::OutOfBand;
    r.driftMHz = r.plan.frequencyMHz;
    r.retune = "frequency";
    r.detail = "frequency outside region band";
    return r;
  }

  // The single most important check in this file. Two meshes on one radio are
  // only one mesh plus noise unless their carriers are the same carrier.
  //
  // Compare the *effective* frequency against the MeshCore one, not the
  // Meshtastic default against it: an operator who has deliberately moved the
  // radio needs the verdict on where they actually put it.
  const float drift = r.plan.frequencyMHz - d->meshCoreFreqMHz;
  if (drift > kFreqToleranceMHz || drift < -kFreqToleranceMHz) {
    r.status = PlanStatus::FrequencyMismatch;
    r.driftMHz = drift < 0.0f ? -drift : drift;
    // Name the side that has to move onto the radio: if the radio sits below the
    // MeshCore channel then MeshCore comes down to meet it, and vice versa.
    // Either side is a legitimate choice by the operator; naming one keeps the
    // finding actionable instead of merely alarming.
    r.retune = (drift < 0.0f) ? "meshcore" : "meshtastic";
    r.detail = "community defaults on different carriers";
    return r;
  }

  // Frequencies agree, so the MeshCore figure only counts if it was verified
  // rather than assumed. Where it was assumed, say so instead of implying the
  // two meshes are known to line up.
  if (!d->meshCoreFreqVerified) {
    r.status = PlanStatus::MeshCoreFreqUnknown;
    r.detail = "matched, but MeshCore default unverified for region";
    return r;
  }

  r.status = PlanStatus::Ok;
  r.detail = "shared carrier";
  return r;
}

const char* describePlan(const PlanReport& report) {
  // The OLED and the CLI both want one short line and no allocation, so this
  // formats into a static buffer and hands back a stable pointer. Not
  // thread-safe by design: it is a status-rendering path, called from the UI
  // task only, and a second concurrent caller would get a torn line rather than
  // a crash. Anything that needs the value twice should copy it out first.
  static char line[128];
  std::size_t i = 0;

  const auto put = [&](const char* s) {
    while (*s != '\0' && i + 1 < sizeof(line)) line[i++] = *s++;
  };
  // Unsigned decimal, any magnitude, no printf on the target.
  const auto putU = [&](std::uint32_t v) {
    char tmp[11];
    std::size_t n = 0;
    do {
      tmp[n++] = static_cast<char>('0' + (v % 10u));
      v /= 10u;
    } while (v != 0u);
    while (n > 0 && i + 1 < sizeof(line)) line[i++] = tmp[--n];
  };

  const char* verdict = "shared OK";
  switch (report.status) {
    case PlanStatus::Ok:
      verdict = "shared OK";
      break;
    case PlanStatus::FrequencyMismatch:
      verdict = "MISMATCH";
      break;
    case PlanStatus::OutOfBand:
      verdict = "OUT OF BAND";
      break;
    case PlanStatus::MeshCoreFreqUnknown:
      verdict = "shared?";
      break;
  }

  // Frequency in millihertz, so the decimal point is placed by integer
  // arithmetic and never by a float subtraction that could round the wrong way.
  const float f = report.plan.frequencyMHz;
  float shifted = f * 1000.0f;
  if (shifted < 0.0f) shifted = -shifted;  // frequencies are positive by construction
  std::uint32_t mhz = static_cast<std::uint32_t>(shifted + 0.5f);
  const std::uint32_t whole = mhz / 1000u;
  const std::uint32_t frac = mhz % 1000u;

  putU(whole);
  put(".");
  putU(frac / 100u);
  putU((frac / 10u) % 10u);
  putU(frac % 10u);
  put("MHz SF");
  putU(report.plan.spreadingFactor);
  put("/");
  putU(static_cast<std::uint32_t>(report.plan.bandwidthKHz));
  put("kHz 4/");
  putU(report.plan.codingRateDenominator);
  put(" ");
  put(verdict);
  line[i] = '\0';
  return line;
}

}  // namespace bridge
