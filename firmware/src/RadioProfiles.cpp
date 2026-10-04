// SPDX-License-Identifier: MIT

#include "bridge/RadioProfiles.hpp"

namespace bridge {
namespace {

// Frequency agreement tolerance. Both meshes' radios sit on the same reference, so
// the requirement is that the plans agree, not that the crystals do.
constexpr float kFreqToleranceKHz = 0.001f;

bool nearFreq(float a, float b) {
  const float d = a > b ? a - b : b - a;
  return d * 1000.0f <= kFreqToleranceKHz;
}

}  // namespace

TxProfile defaultTxProfile() {
  // The shared default both meshes use, which is also the most defensible thing to
  // transmit with when nobody has configured anything: it is the plan both
  // ecosystems actually ship.
  TxProfile p;
  p.framework = Framework::Bridge;
  p.role = Role::Repeater;
  p.frequencyMHz = 869.525f;
  p.bandwidthKHz = 250.0f;
  p.spreadingFactor = 11;
  p.codingRateDenominator = 5;
  p.preambleSymbols = 8;
  p.syncWord = 0x12;
  p.txPowerDbm = 22;
  p.setsOwnSettings = false;  // the board's own default; nothing overrides it
  p.reconfigureMs = 0;         // already there
  p.label = "board default";
  return p;
}

bool txProfileFor(Framework framework, Role role, TxProfile* out) {
  RoleProfile roleDef;
  if (!roleProfile(framework, role, &roleDef)) return false;

  // Every role starts from the shared default, because on a board built around a
  // shared carrier that is the correct plan. What differs per role is the sync word,
  // which is the identity a framework puts on the air.
  TxProfile p = defaultTxProfile();
  p.framework = framework;
  p.role = role;
  p.label = roleDef.label;
  p.setsOwnSettings = true;
  p.reconfigureMs = 3;

  switch (framework) {
    case Framework::MeshCore:
      p.syncWord = 0x12;
      break;
    case Framework::Meshtastic:
      p.syncWord = 0x2B;
      break;
    case Framework::Sniffer:
      p.syncWord = 0x00;  // an analyser wants everything
      break;
    case Framework::Bridge:
      // Transmits as MeshCore: the firmware this derives from, and the identity it
      // holds on the MeshCore mesh.
      p.syncWord = 0x12;
      break;
    case Framework::Custom:
      p.syncWord = 0x00;
      // A custom application may configure nothing at all, in which case the board
      // defaults stand. Supported, not merely tolerated.
      p.setsOwnSettings = false;
      break;
  }

  if (out != nullptr) *out = p;
  return true;
}

const std::vector<TxProfile>& allTxProfiles() {
  static const std::vector<TxProfile> kAll = [] {
    std::vector<TxProfile> v;
    v.push_back(defaultTxProfile());
    std::size_t count = 0;
    const RoleProfile* roles = allProfiles(&count);
    for (std::size_t i = 0; i < count; ++i) {
      TxProfile p;
      if (txProfileFor(roles[i].framework, roles[i].role, &p)) v.push_back(p);
    }
    return v;
  }();
  return kAll;
}

bool RxPlan::accepts(std::uint8_t syncWord) const {
  // A promiscuous plan with no specific word accepts anything, which is what an
  // analyser needs and what no other role should ask for.
  if (promiscuous && syncWordCount == 0) return true;
  for (std::uint8_t i = 0; i < syncWordCount; ++i) {
    if (syncWords[i] == syncWord) return true;
  }
  return false;
}

float bandwidthToSpan(float aMHz, float bMHz, float profileBandwidthKHz) {
  // The receiver must be wide enough to contain both signals. Add the carrier
  // separation to the widest of the two profiles, with a small margin so a signal
  // sitting at the far edge is not clipped by the receiver's filter.
  const float spanKHz = (aMHz > bMHz ? aMHz - bMHz : bMHz - aMHz) * 1000.0f;
  const float widest = profileBandwidthKHz;
  return spanKHz + widest + widest * 0.05f;
}

RxDivergence divergenceBetween(const TxProfile& a, const TxProfile& b) {
  // Order matters: the unmergeable settings are checked first, because widening the
  // receiver cannot rescue a mismatched spreading factor and reporting "merged"
  // would be a lie.
  if (a.spreadingFactor != b.spreadingFactor) return RxDivergence::SpreadingFactor;
  if (a.codingRateDenominator != b.codingRateDenominator) return RxDivergence::CodingRate;

  const float spanKHz = (a.frequencyMHz > b.frequencyMHz ? a.frequencyMHz - b.frequencyMHz
                                                          : b.frequencyMHz - a.frequencyMHz) *
                        1000.0f;
  const float widest = a.bandwidthKHz > b.bandwidthKHz ? a.bandwidthKHz : b.bandwidthKHz;

  if (spanKHz > 0.0f) {
    // Different carriers can still be merged if one receiver can physically be wide
    // enough to contain both. That is a question about bandwidth, not arithmetic.
    const float needed = bandwidthToSpan(a.frequencyMHz, b.frequencyMHz, widest);
    // Beyond the SX1262's widest setting, there is no merge left to find.
    if (needed <= 1000.0f) return RxDivergence::FrequencySpan;
    return RxDivergence::FrequencySpan;  // reported; reconcile() decides feasibility
  }

  if (a.bandwidthKHz != b.bandwidthKHz) return RxDivergence::BandwidthOnly;
  if (a.preambleSymbols != b.preambleSymbols) return RxDivergence::PreambleOnly;
  return RxDivergence::None;
}

bool dominates(const RxSettings& master, const TxProfile& other) {
  // Modulation is not masterable in either direction. A receiver cannot be made
  // cleverer by choosing a better setting, only by matching.
  if (master.spreadingFactor != other.spreadingFactor) return false;
  if (master.codingRateDenominator != other.codingRateDenominator) return false;

  // Preamble: longer is safe, shorter is not.
  if (master.preambleSymbols < other.preambleSymbols) return false;

  // Bandwidth: wider decodes narrower signals; narrower cannot.
  if (master.bandwidthKHz < other.bandwidthKHz) return false;

  // The signal has to land inside the receiver's window.
  const float halfKHz = master.bandwidthKHz / 2.0f;
  const float offsetKHz = (master.frequencyMHz > other.frequencyMHz
                               ? master.frequencyMHz - other.frequencyMHz
                               : other.frequencyMHz - master.frequencyMHz) *
                          1000.0f;
  if (offsetKHz > halfKHz) return false;

  return true;
}

bool leastUpperBound(const TxProfile* profiles, std::size_t count, RxSettings* out,
                     RxDivergence* whyNot) {
  if (profiles == nullptr || count == 0 || out == nullptr) return false;

  // Modulation: must be common to all, or nothing merges.
  RxSettings lub;
  lub.frequencyMHz = profiles[0].frequencyMHz;
  lub.bandwidthKHz = profiles[0].bandwidthKHz;
  lub.spreadingFactor = profiles[0].spreadingFactor;
  lub.codingRateDenominator = profiles[0].codingRateDenominator;
  lub.preambleSymbols = profiles[0].preambleSymbols;

  float lowestHz = profiles[0].frequencyMHz * 1000.0f;
  float highestHz = lowestHz;
  float widestKHz = profiles[0].bandwidthKHz;
  std::uint8_t longestPreamble = profiles[0].preambleSymbols;

  for (std::size_t i = 0; i < count; ++i) {
    const TxProfile& p = profiles[i];

    // Unmasterable first, so a genuine mismatch is never papered over by a wider
    // receiver.
    if (p.spreadingFactor != lub.spreadingFactor) {
      if (whyNot != nullptr) *whyNot = RxDivergence::SpreadingFactor;
      return false;
    }
    if (p.codingRateDenominator != lub.codingRateDenominator) {
      if (whyNot != nullptr) *whyNot = RxDivergence::CodingRate;
      return false;
    }

    const float hz = p.frequencyMHz * 1000.0f;
    if (hz < lowestHz) lowestHz = hz;
    if (hz > highestHz) highestHz = hz;
    if (p.bandwidthKHz > widestKHz) widestKHz = p.bandwidthKHz;
    if (p.preambleSymbols > longestPreamble) longestPreamble = p.preambleSymbols;
  }

  // The master is centred on the span and widened just enough to hold the widest
  // profile and both carriers. Narrowing any further would stop hearing something.
  const float spanKHz = highestHz - lowestHz;
  float neededKHz = spanKHz + widestKHz + widestKHz * 0.05f;

  if (neededKHz > kMaxReceiverBandwidthKHz) {
    if (whyNot != nullptr) *whyNot = RxDivergence::FrequencySpan;
    return false;
  }

  lub.frequencyMHz = ((lowestHz + highestHz) / 2.0f) / 1000.0f;
  lub.bandwidthKHz = neededKHz;
  lub.preambleSymbols = longestPreamble;

  // Verify the relation rather than trusting the derivation. A mistake here would
  // produce a master that silently fails to hear one of the profiles it was built
  // from, which is precisely the failure this module exists to prevent.
  for (std::size_t i = 0; i < count; ++i) {
    if (!dominates(lub, profiles[i])) {
      if (whyNot != nullptr) {
        *whyNot = divergenceBetween(profiles[0], profiles[i]);
      }
      return false;
    }
  }

  if (out != nullptr) *out = lub;
  if (whyNot != nullptr) *whyNot = RxDivergence::None;
  return true;
}

ReconcileReport reconcile(const std::vector<TxProfile>& profiles) {
  ReconcileReport r;

  // The blank board: no profiles at all. The default plan is a complete working
  // configuration, so "nothing installed" is a valid state, not a special case.
  if (profiles.empty()) {
    const TxProfile d = defaultTxProfile();
    r.plan.frequencyMHz = d.frequencyMHz;
    r.plan.bandwidthKHz = d.bandwidthKHz;
    r.plan.spreadingFactor = d.spreadingFactor;
    r.plan.codingRateDenominator = d.codingRateDenominator;
    r.plan.preambleSymbols = d.preambleSymbols;
    r.plan.promiscuous = false;
    r.plan.syncWords[0] = d.syncWord;
    r.plan.syncWordCount = 1;
    r.plan.profilesHeard = 0;
    r.distinctConfigs = 1;
    r.singleConfigCoversAll = true;
    // The board default is itself a master: it is the least upper bound of nothing,
    // and a complete working configuration. A blank board is a valid state.
    r.hasMaster = true;
    r.master.frequencyMHz = d.frequencyMHz;
    r.master.bandwidthKHz = d.bandwidthKHz;
    r.master.spreadingFactor = d.spreadingFactor;
    r.master.codingRateDenominator = d.codingRateDenominator;
    r.master.preambleSymbols = d.preambleSymbols;
    r.detail = "no profiles installed; board defaults in force";
    return r;
  }

  // One pass to gather what the report needs: the carrier span, the widest bandwidth,
  // the longest preamble, and every sync word that has to be accepted.
  //
  // Deliberately NOT a second opinion on mergeability. An earlier version also
  // ranked pairwise divergences here, which meant two independent computations of
  // the same question -- one here, one in leastUpperBound() -- free to disagree,
  // with the more confident-looking answer winning. The merge decision has exactly
  // one authority, below.
  float centreHz = profiles[0].frequencyMHz * 1000.0f;
  float lowestHz = centreHz;
  float highestHz = centreHz;
  float widestKHz = profiles[0].bandwidthKHz;
  std::uint8_t longestPreamble = profiles[0].preambleSymbols;

  for (std::size_t i = 0; i < profiles.size(); ++i) {
    const TxProfile& p = profiles[i];
    const float hz = p.frequencyMHz * 1000.0f;
    if (hz < lowestHz) lowestHz = hz;
    if (hz > highestHz) highestHz = hz;
    if (p.bandwidthKHz > widestKHz) widestKHz = p.bandwidthKHz;
    if (p.preambleSymbols > longestPreamble) longestPreamble = p.preambleSymbols;

    // Sync words are collected regardless of whether a merge exists: each
    // configuration still needs its own word, and the operator wants to see the
    // full set either way.
    if (p.syncWord == 0x00) {
      r.plan.promiscuous = true;
      continue;
    }
    bool present = false;
    for (std::uint8_t k = 0; k < r.plan.syncWordCount; ++k) {
      if (r.plan.syncWords[k] == p.syncWord) present = true;
    }
    if (!present && r.plan.syncWordCount < 8) {
      r.plan.syncWords[r.plan.syncWordCount++] = p.syncWord;
    }
  }

  const float spanKHz = highestHz - lowestHz;
  r.carrierSpanKHz = spanKHz;
  r.requiredBandwidthKHz = bandwidthToSpan(lowestHz / 1000.0f, highestHz / 1000.0f, widestKHz);

  // More than one sync word can only be accepted by loosening the match, which is
  // exactly the promiscuous capture the whole design depends on.
  if (r.plan.syncWordCount > 1) r.plan.promiscuous = true;

  // The master: the least upper bound of everything installed. This is the "most
  // powerful" setting that masters the weaker profiles. If one does not exist, say so
  // and report zero profiles heard rather than shipping a configuration that sounds
  // plausible and hears nothing.
  RxDivergence why = RxDivergence::None;
  RxSettings master;
  if (leastUpperBound(profiles.data(), profiles.size(), &master, &why)) {
    r.hasMaster = true;
    r.master = master;
    r.plan.frequencyMHz = master.frequencyMHz;
    r.plan.bandwidthKHz = master.bandwidthKHz;
    r.plan.spreadingFactor = master.spreadingFactor;
    r.plan.codingRateDenominator = master.codingRateDenominator;
    r.plan.preambleSymbols = master.preambleSymbols;
    r.plan.profilesHeard = profiles.size();
    r.distinctConfigs = 1;
    r.singleConfigCoversAll = true;
    r.requiredBandwidthKHz = master.bandwidthKHz;

    if (spanKHz > 0.0f) {
      r.detail = "master widened to span every installed carrier";
    } else if (widestKHz > profiles[0].bandwidthKHz) {
      r.detail = "master widened to the widest installed profile";
    } else if (r.plan.promiscuous) {
      r.detail = "one master hears every profile (promiscuous sync)";
    } else {
      r.detail = "one master hears every profile";
    }
    return r;
  }

  // No master exists. This is the honest answer for mismatched spreading factors,
  // mismatched coding rates, or carriers further apart than the radio can span.
  r.hasMaster = false;
  r.divergence = why;
  r.distinctConfigs = profiles.size();
  r.singleConfigCoversAll = false;
  r.plan.profilesHeard = 0;
  // Fall back to the first profile's own settings, so the radio has something
  // coherent to run while the operator is told the truth.
  r.plan.frequencyMHz = profiles[0].frequencyMHz;
  r.plan.bandwidthKHz = widestKHz;
  r.plan.spreadingFactor = profiles[0].spreadingFactor;
  r.plan.codingRateDenominator = profiles[0].codingRateDenominator;
  r.plan.preambleSymbols = longestPreamble;

  switch (r.divergence) {
    case RxDivergence::SpreadingFactor:
      r.detail = "spreading factors differ: no master exists, time-slicing required";
      break;
    case RxDivergence::CodingRate:
      r.detail = "coding rates differ: no master exists, time-slicing required";
      break;
    default:
      r.detail = "carriers too far apart for one receiver: time-slicing required";
      break;
  }
  return r;
}

SwitchDecision decideSwitch(const TxProfile& current, const TxProfile& desired,
                           std::uint32_t airtimeRemainingMs, bool midTransmission) {
  SwitchDecision d;

  if (nearFreq(current.frequencyMHz, desired.frequencyMHz) &&
      nearFreq(current.bandwidthKHz, desired.bandwidthKHz) &&
      current.spreadingFactor == desired.spreadingFactor &&
      current.codingRateDenominator == desired.codingRateDenominator &&
      current.preambleSymbols == desired.preambleSymbols &&
      current.syncWord == desired.syncWord && current.txPowerDbm == desired.txPowerDbm) {
    d.shouldSwitch = false;
    d.costMs = 0;
    d.reason = "already on this profile";
    return d;
  }

  d.costMs = desired.reconfigureMs;

  if (midTransmission) {
    // Switching mid-packet truncates the frame being sent, and the sender would
    // simply never be heard again.
    d.shouldSwitch = false;
    d.reason = "deferred: a transmission is in progress";
    return d;
  }

  // Reconfiguration costs no airtime but costs deafness, and it is time the node is
  // not listening on a shared band. When the budget is nearly spent, there is little
  // worth transmitting anyway, so the switch buys nothing.
  if (airtimeRemainingMs < desired.reconfigureMs) {
    d.shouldSwitch = false;
    d.reason = "deferred: airtime budget too small to be worth the deafness";
    return d;
  }

  d.shouldSwitch = true;
  d.reason = "switching";
  return d;
}

}  // namespace bridge