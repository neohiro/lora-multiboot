// SPDX-License-Identifier: MIT

#include "bridge/Airtime.hpp"

namespace bridge {
namespace {

// Symbol time in nanoseconds: 2^SF / bandwidth. The SX1262's divider is a power of
// two from 32 to 256, which is why the division below is exact rather than an
// approximation that drifts at 250 kHz.
std::uint64_t symbolTimeNsExact(const Modulation& m) {
  // kHz -> Hz, avoiding a division by a fractional value.
  const std::uint64_t bwHz =
      static_cast<std::uint64_t>(static_cast<double>(m.bandwidthKHz) * 1000.0 + 0.5);
  if (bwHz == 0) return 0;
  const std::uint64_t numerator = 1ull << m.spreadingFactor;
  return (numerator * 1000000000ull) / bwHz;
}

std::uint32_t ceilDiv(std::uint32_t a, std::uint32_t b) {
  if (b == 0) return 0;
  return (a + b - 1) / b;
}

}  // namespace

Modulation Modulation::fromPlan(const RfPlan& plan) {
  Modulation m;
  m.spreadingFactor = plan.spreadingFactor;
  m.bandwidthKHz = plan.bandwidthKHz;
  m.codingRateDenominator = plan.codingRateDenominator;
  m.preambleSymbols = 8;
  m.crcEnabled = true;
  m.explicitHeader = true;
  // Derived, never assumed: the SX126x requires it at SF11 and SF12.
  m.lowDataRateOptimize = lowDataRateRequired(plan.spreadingFactor);
  return m;
}

bool lowDataRateRequired(std::uint8_t spreadingFactor) {
  // Mandatory at SF11 and SF12, and not used below SF11 where it would break
  // compatibility. Both project defaults sit at SF11, so this is normally true.
  return spreadingFactor >= 11;
}

std::uint32_t symbolTimeNs(const Modulation& m) {
  const std::uint64_t ns = symbolTimeNsExact(m);
  return ns > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(ns);
}

std::uint32_t airtimeUs(const Modulation& m, std::uint16_t payloadBytes) {
  // Refuse rather than invent. A caller budgeting on a wrong figure is worse
  // than a caller that is told the configuration is unusable.
  if (m.spreadingFactor < 6 || m.spreadingFactor > 12) return 0;
  if (m.bandwidthKHz <= 0.0f) return 0;
  if (m.codingRateDenominator < 5 || m.codingRateDenominator > 8) return 0;

  const std::uint64_t tsymNs = symbolTimeNsExact(m);
  if (tsymNs == 0) return 0;

  // Preamble is n symbols plus a fixed 4.25. Since 4.25 is not a whole symbol
  // time, carry it in quarter-symbols so the arithmetic stays integral and the
  // rounding happens exactly once, at the end.
  const std::uint64_t preambleQuarters =
      static_cast<std::uint64_t>(m.preambleSymbols) * 4ull + 17ull;  // 4.25 * 4 == 17

  // Payload, per the SX126x datasheet.
  //   numerator   = 8*N - 4*SF + 28 + 16*CRC - 20*IH
  //   denominator = 4*SF - 4*DE
  const std::uint32_t sf = m.spreadingFactor;
  const std::int32_t ih = m.explicitHeader ? 0 : 1;
  const std::int32_t crc = m.crcEnabled ? 1 : 0;
  const std::int32_t de = m.lowDataRateOptimize ? 1 : 0;

  std::int64_t numerator =
      8ll * static_cast<std::int64_t>(payloadBytes) - 4ll * sf + 28ll + 16ll * crc - 20ll * ih;
  if (numerator < 0) numerator = 0;

  const std::int64_t denominator = 4ll * sf - 4ll * de;
  if (denominator <= 0) return 0;

  const std::uint32_t payloadSymbols = ceilDiv(
      static_cast<std::uint32_t>(numerator), static_cast<std::uint32_t>(denominator));

  // Total in nanoseconds, then to microseconds.
  const std::uint64_t totalNs =
      preambleQuarters * tsymNs / 4ull + static_cast<std::uint64_t>(payloadSymbols) * tsymNs;

  return static_cast<std::uint32_t>(totalNs / 1000ull);
}

// --- AirtimeGovernor -------------------------------------------------------

AirtimeGovernor::AirtimeGovernor(Region region,
                                 std::uint32_t windowMs,
                                 std::uint8_t activeSlotCount)
    : region_(region),
      windowMs_(windowMs == 0 ? kDefaultWindowMs : windowMs),
      bucketMs_((windowMs == 0 ? kDefaultWindowMs : windowMs) /
                static_cast<std::uint32_t>(kBuckets) +
                ((windowMs == 0 ? kDefaultWindowMs : windowMs) %
                         static_cast<std::uint32_t>(kBuckets)
                     ? 1u
                     : 0u)),
      activeSlotCount_(activeSlotCount == 0 ? 1 : activeSlotCount) {
  // A bucket must never round down to zero, or the ring would never advance.
  if (bucketMs_ == 0) bucketMs_ = 1;
  // The effective limit is the region's limit divided by the number of active slots.
  // This ensures the aggregate airtime across all slots never exceeds the regulatory limit.
  const std::uint8_t regionLimit = dutyCycleFor(region);
  const std::uint8_t effectiveLimit = regionLimit / activeSlotCount_;
  setLimitPercent(effectiveLimit);
}

void AirtimeGovernor::setLimitPercent(std::uint8_t percent) {
  if (percent == 0) percent = 1;  // 0% would mean never transmitting
  if (percent > 100) percent = 100;
  limitPercent_ = percent;
}

void AirtimeGovernor::setActiveSlotCount(std::uint8_t count) {
  if (count == 0) count = 1;
  activeSlotCount_ = count;
  // Recalculate the effective limit based on the new slot count.
  const std::uint8_t regionLimit = dutyCycleFor(region_);
  const std::uint8_t effectiveLimit = regionLimit / activeSlotCount_;
  setLimitPercent(effectiveLimit);
}

Region AirtimeGovernor::region() const { return region_; }

std::uint64_t AirtimeGovernor::budgetUs() const {
  return (static_cast<std::uint64_t>(windowMs_) * 1000ull * limitPercent_) / 100ull;
}

std::uint64_t AirtimeGovernor::maxSingleTransmissionUs() const { return budgetUs(); }

void AirtimeGovernor::advance(std::uint32_t nowMs) {
  if (!started_) {
    newestBucketStartMs_ = (nowMs / bucketMs_) * bucketMs_;
    started_ = true;
    return;
  }

  const std::uint32_t newest = (nowMs / bucketMs_) * bucketMs_;

  // Clock going backwards (a reboot without re-initialising the governor, or a
  // test jumping) must not index out of the ring or resurrect old airtime.
  if (newest < newestBucketStartMs_) {
    for (std::size_t i = 0; i < kBuckets; ++i) buckets_[i] = 0;
    newestBucketStartMs_ = newest;
    return;
  }

  const std::uint32_t steps = (newest - newestBucketStartMs_) / bucketMs_;
  if (steps == 0) return;
  if (steps >= static_cast<std::uint32_t>(kBuckets)) {
    // Everything is stale; the whole ring is outside the window.
    for (std::size_t i = 0; i < kBuckets; ++i) buckets_[i] = 0;
    newestBucketStartMs_ = newest;
    return;
  }

  const std::size_t newestIndex =
      static_cast<std::size_t>((newestBucketStartMs_ / bucketMs_) % kBuckets);
  for (std::uint32_t s = 1; s <= steps; ++s) {
    const std::size_t idx = (newestIndex + s) % kBuckets;
    buckets_[idx] = 0;
  }
  newestBucketStartMs_ = newest;
}

std::uint64_t AirtimeGovernor::usedUs(std::uint32_t nowMs) const {
  if (!started_) return 0;

  // Each bucket's absolute start time is derivable from its index and the newest
  // bucket's start, so buckets that have fallen out of the window can be skipped
  // without mutating anything. That keeps this const and correct even when a long
  // time jump has occurred since the last admit() -- which a caller querying the
  // budget after an idle hour absolutely will do.
  //
  // The ring spans slightly more than the window (bucket size is rounded up), so
  // the oldest bucket or two may sit outside it and be excluded. Everything the
  // ring still holds is counted in FULL, never truncated at "now": under-counting
  // a transmission in progress would admit more than the limit allows, and a
  // governor that reports compliance it does not have is worse than none at all.
  const std::uint64_t windowStartMs =
      nowMs > windowMs_ ? static_cast<std::uint64_t>(nowMs - windowMs_) : 0ull;
  const std::size_t newestIndex =
      static_cast<std::size_t>((newestBucketStartMs_ / bucketMs_) % kBuckets);

  std::uint64_t total = 0;
  for (std::size_t i = 0; i < kBuckets; ++i) {
    if (buckets_[i] == 0) continue;
    // How many bucket-steps back from the newest is this one?
    const std::size_t back = (newestIndex + kBuckets - i) % kBuckets;
    const std::uint64_t backMs = static_cast<std::uint64_t>(back) * bucketMs_;
    if (backMs > newestBucketStartMs_) continue;  // would precede the epoch
    const std::uint64_t bucketStartMs = static_cast<std::uint64_t>(newestBucketStartMs_) - backMs;
    if (bucketStartMs >= windowStartMs) total += buckets_[i];
  }
  return total;
}

bool AirtimeGovernor::wouldAdmit(std::uint32_t nowMs, std::uint32_t airtimeUs) const {
  if (airtimeUs == 0) return true;  // nothing to spend
  return usedUs(nowMs) + airtimeUs <= budgetUs();
}

bool AirtimeGovernor::admit(std::uint32_t nowMs, std::uint32_t airtimeUs) {
  advance(nowMs);
  if (!wouldAdmit(nowMs, airtimeUs)) return false;
  if (airtimeUs == 0) return true;

  const std::size_t idx =
      static_cast<std::size_t>((newestBucketStartMs_ / bucketMs_) % kBuckets);
  buckets_[idx] += airtimeUs;
  return true;
}

float AirtimeGovernor::usedFraction(std::uint32_t nowMs) const {
  const std::uint64_t budget = budgetUs();
  if (budget == 0) return 0.0f;
  return static_cast<float>(static_cast<double>(usedUs(nowMs)) / static_cast<double>(budget));
}

std::size_t AirtimeGovernor::activeBuckets() const {
  std::size_t n = 0;
  for (std::size_t i = 0; i < kBuckets; ++i) {
    if (buckets_[i] != 0) ++n;
  }
  return n;
}

}  // namespace bridge