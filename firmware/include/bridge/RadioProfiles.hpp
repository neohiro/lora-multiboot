// SPDX-License-Identifier: MIT
//
// RadioProfiles -- per-framework TX settings, and one reconciled RX plan.
//
// Different firmware on different slots wants different radio settings. MeshCore
// has a sync word and a channel; Meshtastic has a sync word and a channel; LoRaWAN
// has another; a bespoke application may set nothing and expect sane defaults. On
// one board with one radio those wants have to be reconciled, and the reconciliation
// has to be physically honest rather than merely plausible.
//
// # Why settings are not "averaged"
//
// Averaging is the obvious thing to do and it is wrong in two different ways, so it
// is worth being precise about which is which.
//
// **1. Most settings have no average. They must match exactly.**
//
// Spreading factor is the clearest case: SF11 is 2048 chips per symbol, SF9 is 512.
// There is no value between them that demodulates either. The same is true of
// frequency -- a receiver at 869.525 MHz hears nothing of a 869.4 MHz frame, and no
// midpoint helps -- and, less obviously, of coding rate. LoRa's forward error
// correction rate is a link parameter the demodulator needs in order to interpret the
// bit stream; configure it differently and the frame does not decode, no matter how
// close the average is. CRC, implicit/explicit header and the low-data-rate
// optimisation are the same story.
//
// Any scheme that produces a blended value for these produces a receiver that hears
// nothing.
//
// **2. Two settings *do* have a compatible superset, and that is the real answer.**
//
// This is the part worth having, because it means boards with genuinely different
// firmware often still need only one receiver configuration.
//
// - **Bandwidth is a superset.** A receiver configured wider than the transmitted
//   signal decodes it perfectly well, because the whole signal fits inside the
//   receiver's window. Narrowing below the signal does not. So the correct merge of
//   several bandwidths is their **maximum**, and this is not a compromise -- it is
//   strictly more capable. A receiver at 250 kHz decodes a 125 kHz frame as a matter
//   of course. (The cost is sensitivity: more of the band means more noise admitted,
//   which is the tradeoff to state rather than hide.)
//
// - **Preamble is a superset.** A receiver needs at least as many preamble symbols
//   as the transmitter sent. Longer is fine; it costs only airtime. So the merge is
//   the **maximum**, and 8 is the value both ecosystems use, so it never bites.
//
// **3. Frequency has a third option, which is bandwidth.**
//
// Profiles on genuinely different carriers cannot be merged. But "close enough" is a
// question about the receiver's bandwidth, not about the arithmetic mean. Two
// channels 200 kHz apart *can* be heard by one receiver at BW500, centred between
// them -- and that is worth reporting, because it turns an apparent
// time-slicing-forever situation into a single configuration.
//
// So the three outcomes are:
//
//   | profiles differ in | outcome |
//   |---|---|
//   | bandwidth only | merged, one config, receiver widened |
//   | preamble only | merged, one config, longer preamble |
//   | frequency, within half a receiver bandwidth of each other | merged, one config, widened to span |
//   | spreading factor or coding rate | **time-slicing. No merge exists.** |
//   | frequency, further apart than the receiver can span | **time-slicing** |
//
// The common case on this board is the first row, because every profile shares a
// carrier and only the sync word differs -- which is the premise the whole project
// rests on.
//
// # None of it is mandatory
//
// A special-purpose slot application, future-bound and knowing what it is doing,
// must be able to boot and transmit without ever configuring the radio. So a profile
// may declare `setsOwnSettings = false`, and the board defaults are a complete,
// working, legal configuration on their own. Firmware that wants nothing need do
// nothing, and `reconcile()` treats an empty set of profiles as a valid state rather
// than a special case.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "bridge/ChannelPlan.hpp"
#include "bridge/Roles.hpp"

namespace bridge {

// What actually differs between two profiles, and whether it can be merged.
enum class RxDivergence : std::uint8_t {
  None = 0,
  BandwidthOnly,      // merged by widening the receiver
  PreambleOnly,       // merged by taking the longer preamble
  FrequencySpan,      // merged by widening enough to cover both carriers
  SpreadingFactor,    // no merge exists
  CodingRate,         // no merge exists
};

// The radio settings one firmware role transmits with.
struct TxProfile {
  Framework framework = Framework::MeshCore;
  Role role = Role::Repeater;

  float frequencyMHz = 869.525f;
  float bandwidthKHz = 250.0f;
  std::uint8_t spreadingFactor = 11;
  std::uint8_t codingRateDenominator = 5;
  std::uint8_t preambleSymbols = 8;
  std::uint8_t syncWord = 0x12;
  int8_t txPowerDbm = 22;

  // False for a special application that relies entirely on the board's defaults.
  // Such a profile still has to be *heard* -- it still participates in
  // reconciliation -- it simply does not insist on being configured.
  bool setsOwnSettings = true;

  // What it costs to switch the radio to this profile, in milliseconds: leave
  // receive, reconfigure, re-enter receive. This is why per-message switching is a
  // decision rather than a free action.
  std::uint16_t reconfigureMs = 3;

  const char* label = "";
};

// The receive-relevant settings, separated from a profile's identity and power so
// that two profiles can be compared on what actually matters for hearing them.
struct RxSettings {
  float frequencyMHz = 869.525f;
  float bandwidthKHz = 250.0f;
  std::uint8_t spreadingFactor = 11;
  std::uint8_t codingRateDenominator = 5;
  std::uint8_t preambleSymbols = 8;

  RxSettings() = default;
  RxSettings(const TxProfile& p)
      : frequencyMHz(p.frequencyMHz),
        bandwidthKHz(p.bandwidthKHz),
        spreadingFactor(p.spreadingFactor),
        codingRateDenominator(p.codingRateDenominator),
        preambleSymbols(p.preambleSymbols) {}
};

// Can a receiver running `master` hear a transmitter configured like `other`?
//
// This is the relation the whole multi-slot design leans on, so it is stated
// precisely rather than approximated by comparing a few fields:
//
//   bandwidth  a receiver wider than the signal decodes it; narrower does not.
//              So the master dominates when its bandwidth is at least the other's.
//   preamble   a receiver needs at least as many preamble symbols as were sent.
//              Longer is fine. So the master dominates when its preamble is not
//              shorter.
//   frequency  the signal must fall inside the receiver's window, so every carrier
//              must lie within half a receiver bandwidth of the centre.
//   spreading  MUST match. SF11 is 2048 chips per symbol and SF9 is 512; there is
//   factor     no setting that decodes both. Not masterable, in either direction.
//   coding     MUST match. The demodulator needs it to interpret the bit stream.
//   rate       Not masterable.
//
// So "the most powerful RX setting" is the *widest bandwidth with the longest
// preamble*, and it masters every profile that shares its carrier and modulation.
// It cannot master a different spreading factor, and claiming otherwise would
// produce a receiver that hears nothing.
bool dominates(const RxSettings& master, const TxProfile& other);

// The narrowest receive configuration that hears every one of `profiles` -- the
// least upper bound, which is the point at which widening stops buying anything.
//
// False when no such configuration exists, with `whyNot` set to the reason. There
// is no partial answer worth returning: a configuration that hears some of the
// installed firmware is worse than an honest failure, because it looks healthy.
bool leastUpperBound(const TxProfile* profiles, std::size_t count, RxSettings* out,
                     RxDivergence* whyNot);

// The widest bandwidth the SX1262 can be configured for, in kHz. Above this a merge
// stops being a question of arithmetic and becomes a question of hardware.
constexpr float kMaxReceiverBandwidthKHz = 1000.0f;

// The receive configuration in force. One radio, so one at a time -- but one can
// serve many profiles, because bandwidth and preamble have compatible supersets.
struct RxPlan {
  float frequencyMHz = 869.525f;
  float bandwidthKHz = 250.0f;
  std::uint8_t spreadingFactor = 11;
  std::uint8_t codingRateDenominator = 5;
  std::uint8_t preambleSymbols = 8;

  // Promiscuous when more than one sync word has to be accepted, which is what lets
  // a single receive configuration hear two different frameworks.
  bool promiscuous = false;
  std::uint8_t syncWords[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  std::uint8_t syncWordCount = 0;

  // How many of the supplied profiles this single configuration serves.
  std::size_t profilesHeard = 0;

  bool accepts(std::uint8_t syncWord) const;
};

struct ReconcileReport {
  RxPlan plan;

  // The master configuration: the least upper bound of every installed profile, when
  // one exists. This is the "most powerful" setting that masters the weaker ones.
  RxSettings master;
  bool hasMaster = false;

  // How many distinct receive configurations are needed to hear everything
  // installed. One is the good outcome; more means the receiver must be time-sliced.
  std::size_t distinctConfigs = 1;
  bool singleConfigCoversAll = true;

  // What had to give, and why. `None` when one configuration sufficed.
  RxDivergence divergence = RxDivergence::None;
  const char* detail = "";
  // For a frequency span: how far apart the carriers were, and the bandwidth the
  // receiver needed in order to cover both.
  float carrierSpanKHz = 0.0f;
  float requiredBandwidthKHz = 0.0f;
};

// The default TX settings: a complete, working configuration in its own right, so a
// slot that configures nothing still transmits legally and intelligibly.
TxProfile defaultTxProfile();

// Look up the profile for a framework/role pair. False when the combination is not
// one this build knows, in which case the caller should use defaultTxProfile()
// rather than guessing.
bool txProfileFor(Framework framework, Role role, TxProfile* out);

const std::vector<TxProfile>& allTxProfiles();

// Reconcile a set of installed profiles into a receive plan.
//
// `profiles` may be empty: that is the blank board, and the default plan is a
// complete working configuration, so it is a valid state rather than a special case.
ReconcileReport reconcile(const std::vector<TxProfile>& profiles);

// Why two profiles cannot share one receive configuration.
RxDivergence divergenceBetween(const TxProfile& a, const TxProfile& b);

// The bandwidth a receiver must have in order to cover both of two carriers.
float bandwidthToSpan(float aMHz, float bMHz, float profileBandwidthKHz);

// --- switching --------------------------------------------------------------

// Whether to reconfigure the radio now, or defer.
//
// Deferring is not a micro-optimisation. Reconfiguring means the receiver is deaf
// for the duration, so a node that switched on every message would spend a
// measurable share of its life not listening -- and on a shared band, not listening
// means missing the beginnings of other people's frames.
struct SwitchDecision {
  bool shouldSwitch = true;
  std::uint16_t costMs = 0;
  const char* reason = "";
};

SwitchDecision decideSwitch(const TxProfile& current, const TxProfile& desired,
                           std::uint32_t airtimeRemainingMs, bool midTransmission = false);

}  // namespace bridge