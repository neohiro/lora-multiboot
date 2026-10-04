// SPDX-License-Identifier: MIT
//
// Airtime -- the compliance-critical piece.
//
// A repeater is a transmitter. Transmitters are regulated by airtime, and the
// arithmetic has to be right or the node is unlawful rather than merely
// impolite. Two things are therefore computed here rather than guessed:
//
//   1. How long a packet actually occupies the channel, from the LoRa airtime
//      formula. Needed because the duty-cycle budget is spent in airtime, and
//      airtime depends on payload length, spreading factor, coding rate and the
//      low-data-rate optimisation flag. Meshtastic and MeshCore payloads differ in
//      length, so an estimate is not good enough -- a node that budgets on an
//      estimate is a node that eventually exceeds the limit.
//
//   2. Whether the next transmission fits inside the region's duty-cycle limit.
//
// The EU default is 10%, not MeshCore's stock 50%. That is not a stylistic
// choice: 869.4-869.65 MHz carries a 10% duty-cycle limit under the EU's
// non-specific SRD entry, and firmware offering a higher figure proves capability
// rather than permission. See docs/RF-PLAN.md.

#pragma once

#include <cstddef>
#include <cstdint>

#include "bridge/ChannelPlan.hpp"

namespace bridge {

// Enough LoRa modulation detail to compute airtime exactly as the SX1262 does it.
struct Modulation {
  std::uint8_t spreadingFactor = 11;
  float bandwidthKHz = 250.0f;
  // Coding rate denominator: 5 for 4/5, 6 for 4/6, and so on.
  std::uint8_t codingRateDenominator = 5;
  std::uint8_t preambleSymbols = 8;
  bool crcEnabled = true;
  bool explicitHeader = true;  // Meshtastic and MeshCore both send one
  // The low-data-rate optimisation is mandatory for SF11 and SF12 on the
  // bandwidths in use, and it shortens the symbol count. Forgetting it
  // underestimates airtime by roughly 8%, which is exactly the kind of error that
  // only shows up on a compliance audit.
  bool lowDataRateOptimize = true;

  static Modulation fromPlan(const RfPlan& plan);
};

// Airtime of one packet in microseconds.
//
// Returns 0 for an unusable configuration rather than a large number: a caller
// budgeting on a bogus figure would deny every transmission, whereas a caller
// told "0, unknown" can decide to refuse rather than transmit.
std::uint32_t airtimeUs(const Modulation& m, std::uint16_t payloadBytes);

// The modulation's own on-air symbol time, in nanoseconds. Exposed because it is
// the value most likely to be got wrong and is worth asserting directly.
std::uint32_t symbolTimeNs(const Modulation& m);

// True when the low-data-rate optimisation must be enabled, per the SX126x rules:
// mandatory for SF11 and SF12, and never used below SF11.
bool lowDataRateRequired(std::uint8_t spreadingFactor);

// One transmission the governor has accounted for, kept for the record rather
// than for the arithmetic: see the note on AirtimeGovernor for why the budget is
// not computed from a list of these.
struct AirtimeEvent {
  std::uint32_t startMs = 0;
  std::uint32_t airtimeUs = 0;
};

// Sliding-window duty-cycle governor.
//
// The rule it enforces: over any window, total airtime must not exceed the
// region's percentage of that window.
//
// The window is divided into fixed time buckets rather than keeping a list of
// transmissions. That is not an optimisation, it is a correctness requirement.
// 10% of an hour is six minutes of airtime, which at one 1 ms packet per second
// is 360,000 events -- so any per-event history is either unbounded or, once
// bounded, silently forgets the recent past and admits more than the limit
// allows. Buckets cost a fixed 512 bytes and are indifferent to packet rate.
//
// Buckets are also conservative by construction: the live window is slightly
// shorter than the bucket span, so the sum can only over-count, never under-count.
// Over-counting makes the node transmit slightly less than it may. Under-counting
// would make it unlawful while reporting compliance, which is the failure that
// actually matters.
//
// When multiple slots share the single antenna, the region's duty-cycle limit
// (10% for EU_868) is divided equally among the active slots. If N slots are
// active, each slot gets (region_limit / N) of the total airtime budget. This
// compensates for the shared radiotime so that the aggregate airtime across all
// slots never exceeds the regulatory limit.
class AirtimeGovernor {
 public:
  // `windowMs` is the averaging period. An hour is the usual regulatory reading of
  // "duty cycle"; a shorter window is stricter and therefore safer, and is exposed
  // so an operator can choose.
  static constexpr std::uint32_t kDefaultWindowMs = 60u * 60u * 1000u;

  // 32 buckets over a one-hour window is a 112 s bucket. Because airtime is spread
  // rather than clumped, the worst-case over-count is about 1/32 of the budget --
  // a few percent, in the safe direction. Halving this from 64 saves 256 bytes of
  // RAM that every slot in the chain would otherwise be paying for.
  static constexpr std::size_t kBuckets = 32;

  // Bucket size for the default window. Precomputed as a constant so a
  // default-constructed governor is fully valid -- which it must be, because the
  // shared RAM block in SharedContext.hpp embeds one and has to be default
  // constructible to stay a plain data structure.
  static constexpr std::uint32_t kDefaultBucketMs =
      (kDefaultWindowMs / static_cast<std::uint32_t>(kBuckets)) +
      ((kDefaultWindowMs % static_cast<std::uint32_t>(kBuckets)) ? 1u : 0u);

  // Defaults to the EU: the strictest cap in the table, and the region this
  // project is built around. Defaulting to the *lenient* option would be the
  // wrong direction to be wrong in.
  AirtimeGovernor() = default;

  // `activeSlotCount` is the number of slots currently sharing the antenna.
  // The effective duty-cycle limit is region_limit / activeSlotCount.
  // A value of 0 or 1 means no division (single slot).
  explicit AirtimeGovernor(Region region,
                           std::uint32_t windowMs = kDefaultWindowMs,
                           std::uint8_t activeSlotCount = 1);

  // Ask whether a transmission of this airtime may proceed at this time.
  //
  // Pure: it does not record anything. Call admit() to actually spend the budget.
  bool wouldAdmit(std::uint32_t nowMs, std::uint32_t airtimeUs) const;

  // Spend the budget. Returns false and changes nothing when it would breach the
  // limit, so a refused transmission costs nothing and can simply be retried.
  bool admit(std::uint32_t nowMs, std::uint32_t airtimeUs);

  // Airtime spent inside the current window, in microseconds.
  std::uint64_t usedUs(std::uint32_t nowMs) const;

  // Fraction of the window's budget spent so far, 0.0 upward. Can exceed 1.0 only
  // if airtime was spent without the governor, which is why it is reported rather
  // than clamped: a number above 1.0 is a fact worth seeing.
  float usedFraction(std::uint32_t nowMs) const;

  // The largest single transmission the budget could ever allow, which is the
  // fastest useful answer to "is my payload too big to send at all".
  std::uint64_t maxSingleTransmissionUs() const;

  // The budget for the whole window, in microseconds.
  std::uint64_t budgetUs() const;

  std::uint32_t windowMs() const { return windowMs_; }
  std::uint32_t bucketMs() const { return bucketMs_; }

  // The cap in force, as a percentage (region limit divided by activeSlotCount).
  std::uint8_t limitPercent() const { return limitPercent_; }

  // The number of active slots sharing the antenna.
  std::uint8_t activeSlotCount() const { return activeSlotCount_; }

  // Update the active slot count and recalculate the effective limit.
  void setActiveSlotCount(std::uint8_t count);

  // The region whose cap is being enforced.
  Region region() const;

  // One behaviour worth knowing before deploying a node that runs for months.
  //
  // `nowMs` is expected to be a millisecond uptime counter, which on ESP32 wraps
  // every ~49.7 days. A backwards jump is treated as "the clock restarted": every
  // bucket is cleared, so the window refills. The consequence is one budget refill
  // per wrap, not a continuous over-send.
  //
  // That is the right trade for a reboot (where the counter really does restart, and
  // the board is genuinely a new session) and a negligible one for a wrap: an
  // airtime-free window is worth at most a few seconds of transmission once every 50
  // days, against a 10% ceiling that is otherwise never exceeded. Feeding a true
  // monotonic 64-bit counter instead would remove the case entirely, at the cost of
  // every caller having to own one.
  static constexpr std::uint32_t kMillisWrapMs = 0xFFFFFFFFu;

  // Buckets currently holding airtime, for diagnostics.
  std::size_t activeBuckets() const;

  void setLimitPercent(std::uint8_t percent);

 private:
  // Zero every bucket that has fallen out of the window. Non-const.
  void advance(std::uint32_t nowMs);

  Region region_ = Region::EU_868;
  std::uint32_t windowMs_ = kDefaultWindowMs;
  std::uint32_t bucketMs_ = kDefaultBucketMs;
  std::uint8_t limitPercent_ = dutyCycleFor(Region::EU_868);
  std::uint8_t activeSlotCount_ = 1;

  std::uint64_t buckets_[kBuckets] = {};
  // Wall-clock time of the newest bucket's start, and whether anything is live.
  std::uint32_t newestBucketStartMs_ = 0;
  bool started_ = false;
};

}  // namespace bridge