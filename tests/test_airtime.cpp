// SPDX-License-Identifier: MIT
//
// Airtime tests.
//
// The airtime formula is checked against hand-computed reference values, not just
// for internal consistency. A duty-cycle governor fed a wrong airtime figure is
// a node that is unlawful while reporting that it is compliant, which is worse
// than having no governor at all.
//
// The enforcement tests then check the property that actually matters: no
// sequence of transmissions can push airtime past the cap.

#include "bridge/Airtime.hpp"
#include "harness.hpp"

using namespace bridge;

namespace {

// SF11, 250 kHz, 4/5, 8-symbol preamble, CRC on, explicit header, LDRO on.
// This is the shared default plan for both meshes.
Modulation sharedDefault() {
  const RfPlan plan;
  return Modulation::fromPlan(plan);
}

}  // namespace

void suite_airtime() {
  harness::suite("Airtime");

  // --- the modulation derived from the plan -------------------------------

  {
    const Modulation m = sharedDefault();
    CHECK_EQ(m.spreadingFactor, 11);
    CHECK(m.bandwidthKHz == 250.0f);
    CHECK_EQ(m.codingRateDenominator, 5);
    // LDRO is mandatory at SF11 and forgetting it underestimates airtime by
    // about 8%, so it is derived rather than left to the caller.
    CHECK_MSG(m.lowDataRateOptimize, "SF11 requires the low-data-rate optimisation");
  }

  {
    // At SF9 it must be off, or the packet is malformed rather than merely slow.
    RfPlan p;
    p.spreadingFactor = 9;
    const Modulation m = Modulation::fromPlan(p);
    CHECK(!m.lowDataRateOptimize);
    CHECK(lowDataRateRequired(11));
    CHECK(lowDataRateRequired(12));
    CHECK(!lowDataRateRequired(9));
    CHECK(!lowDataRateRequired(7));
  }

  // --- symbol time ---------------------------------------------------------

  {
    // SF11 at 250 kHz: 2^11 / 250000 = 8192 us = 8192000 ns.
    const Modulation m = sharedDefault();
    CHECK_MSG(symbolTimeNs(m) == 8192000u, "2^11 / 250kHz");

    Modulation sf7;
    sf7.spreadingFactor = 7;
    sf7.bandwidthKHz = 125.0f;
    // 2^7 / 125000 = 1024 us.
    CHECK_MSG(symbolTimeNs(sf7) == 1024000u, "2^7 / 125kHz");

    Modulation sf12;
    sf12.spreadingFactor = 12;
    sf12.bandwidthKHz = 125.0f;
    CHECK_MSG(symbolTimeNs(sf12) == 32768000u, "2^12 / 125kHz");
  }

  // --- airtime against hand-computed values --------------------------------
  //
  // Shared default, SF11/250kHz/4-5, LDRO on, CRC on, explicit header.
  //   Tpreamble = (8 + 4.25) * 8192 us = 100352 us
  //   numerator   = 8*N - 4*11 + 28 + 16 = 8*N
  //   denominator = 4*11 - 4*1          = 40
  //   Tpayload    = ceil(8*N / 40) * 8192 us

  {
    const Modulation m = sharedDefault();
    const std::uint32_t sym = 8192u;

    // Preamble is (8 + 4.25) symbols, carried in quarter-symbols so the
    // fractional part never rounds: (8*4 + 17) * sym / 4.
    const std::uint32_t preambleUs = (8u * 4u + 17u) * sym / 4u;
    CHECK_MSG(preambleUs == 100352u, "reference preamble: 12.25 * 8192us");
    //   numerator   = 8*N - 4*11 + 28 + 16 = 8*N
    //   denominator = 4*11 - 4*1          = 40
    //   Tpayload    = ceil(8*N / 40) * 8192 us
    CHECK_MSG(airtimeUs(m, 12) == 124928u, "SF11 12 bytes");
    // N=32: ceil(256/40)=7 symbols -> 7*8192 = 57344; total = 157696
    CHECK_MSG(airtimeUs(m, 32) == 157696u, "SF11 32 bytes");
    // N=64: ceil(512/40)=13 symbols -> 13*8192 = 106496; total = 206848
    CHECK_MSG(airtimeUs(m, 64) == 206848u, "SF11 64 bytes, a realistic Meshtastic frame");
  }

  {
    // Airtime must be monotonic in payload length. Any non-monotonicity means
    // the ceiling is being applied in the wrong place, and the governor would
    // then reject a longer packet while admitting a shorter one.
    const Modulation m = sharedDefault();
    std::uint32_t previous = 0;
    for (std::uint16_t n = 0; n <= 255; n += 1) {
      const std::uint32_t a = airtimeUs(m, n);
      CHECK_MSG(a >= previous, "airtime must not decrease with payload length");
      previous = a;
    }
  }

  // --- LDRO genuinely changes the answer -----------------------------------

  {
    // The low-data-rate optimisation *lengthens* airtime: DE makes the payload
    // divisor smaller (4*SF - 4), so more payload symbols are needed.
    //
    // That direction matters. Leaving LDRO off is the dangerous mistake, because
    // it makes the computed airtime shorter than the truth and the governor then
    // believes it has budget it does not have. Hence the assertion is that the
    // correctly-configured figure is the larger one.
    Modulation on = sharedDefault();
    Modulation off = on;
    off.lowDataRateOptimize = false;
    const std::uint32_t a = airtimeUs(on, 64);
    const std::uint32_t b = airtimeUs(off, 64);
    CHECK_MSG(a > b, "LDRO on is the longer, correct figure for SF11");
    // 13 symbols with LDRO, 12 without: one symbol, 8192 us.
    CHECK_MSG(a - b == 8192u, "exactly one symbol of difference");
  }

  // --- degenerate configurations are refused, not invented -----------------

  {
    Modulation bad;
    bad.spreadingFactor = 5;  // below the SX1262's minimum of SF6
    CHECK_EQ(airtimeUs(bad, 32), 0u);

    Modulation badBw = sharedDefault();
    badBw.bandwidthKHz = 0.0f;
    CHECK_EQ(airtimeUs(badBw, 32), 0u);

    Modulation badCr = sharedDefault();
    badCr.codingRateDenominator = 9;
    CHECK_EQ(airtimeUs(badCr, 32), 0u);

    // 255 bytes is the largest Meshtastic can produce, so the upper end of the
    // practical range must compute without overflow.
    CHECK(airtimeUs(sharedDefault(), 255) > 0u);
  }

  // --- the governor's default limit is the legal one -----------------------

  {
    const AirtimeGovernor eu(Region::EU_868);
    CHECK_MSG(eu.limitPercent() == 10,
              "the EU limit at 869.525 MHz is 10%, not MeshCore's stock 50%");
    CHECK_EQ(static_cast<int>(eu.region()), static_cast<int>(Region::EU_868));

    const AirtimeGovernor us(Region::US_915);
    CHECK_MSG(us.limitPercent() == 100, "the US has no general duty-cycle cap");
  }

  {
    // A zero window must not become a divide-by-zero or a node that never
    // transmits.
    AirtimeGovernor zero(Region::EU_868, 0);
    CHECK(zero.windowMs() == AirtimeGovernor::kDefaultWindowMs);
    AirtimeGovernor none(Region::EU_868);
    none.setLimitPercent(0);
    CHECK_MSG(none.limitPercent() == 1, "a 0% cap would mean never transmitting");
    none.setLimitPercent(200);
    CHECK_EQ(none.limitPercent(), 100);
  }

  // --- enforcement: the budget is respected --------------------------------

  {
    // A 10-minute window at 10% is 60 seconds of airtime.
    AirtimeGovernor g(Region::EU_868, 10u * 60u * 1000u);
    CHECK_EQ(g.limitPercent(), 10);
    CHECK(g.maxSingleTransmissionUs() == 60000000ull);
    CHECK(g.bucketMs() > 0);

    // A 200 ms packet is 1/300th of the budget, so 300 fit -- and the 301st must
    // not.
    const std::uint32_t packet = 200000u;
    std::uint32_t admitted = 0;
    for (std::uint32_t i = 0; i < 1000; ++i) {
      if (g.admit(i * 100u, packet)) ++admitted;  // one every 100 ms
    }
    CHECK_MSG(admitted == 300, "300 packets fit in a 60s budget at 200ms each");
    CHECK_MSG(g.usedUs(30000u) <= g.budgetUs(), "the budget was never exceeded");
    CHECK_MSG(!g.wouldAdmit(100000u, packet), "and the next one is refused");
    CHECK_MSG(g.admit(100000u, packet) == false, "a refused send changes nothing");
  }

  {
    // A refused transmission must not spend budget, or a chatty node would
    // throttle itself into permanent silence.
    AirtimeGovernor g(Region::EU_868, 10u * 60u * 1000u);
    const std::uint32_t big = 200000u;
    for (std::uint32_t i = 0; i < 300; ++i) g.admit(i * 100u, big);
    const std::uint64_t used = g.usedUs(30000u);
    for (int i = 0; i < 50; ++i) {
      CHECK(!g.admit(30000u, big));
    }
    CHECK_MSG(g.usedUs(30000u) == used, "refused sends leave the budget untouched");
  }

  {
    // The budget recovers as the window slides. A node that hit its cap an hour
    // ago must be able to transmit again, or it goes silent forever.
    AirtimeGovernor g(Region::EU_868, 10u * 60u * 1000u);
    const std::uint32_t packet = 200000u;
    for (std::uint32_t i = 0; i < 300; ++i) g.admit(i * 100u, packet);
    CHECK(!g.wouldAdmit(30000u, packet));
    // One window later, everything has aged out.
    CHECK_MSG(g.wouldAdmit(30000u + 11u * 60u * 1000u, packet),
              "budget recovers once the window has passed");
    CHECK_MSG(g.usedUs(30000u + 11u * 60u * 1000u) == 0ull, "and the old airtime is gone");
  }

  {
    // usedFraction is the number an operator reads. It must never report a
    // comfortable value while the cap is actually being exceeded.
    AirtimeGovernor g(Region::EU_868, 10u * 60u * 1000u);
    const std::uint32_t packet = 200000u;
    for (std::uint32_t i = 0; i < 300; ++i) g.admit(i * 100u, packet);
    const float f = g.usedFraction(30000u);
    CHECK_MSG(f > 0.9f && f < 1.05f, "the window is essentially full");
  }

  // --- zero airtime is free -------------------------------------------------

  {
    AirtimeGovernor g(Region::EU_868, 10u * 60u * 1000u);
    CHECK_MSG(g.wouldAdmit(0, 0), "nothing to spend means nothing to refuse");
    CHECK(g.admit(0, 0));
    CHECK_EQ(g.usedUs(0), 0ull);
  }

  // --- high packet rate must not lose history ------------------------------
  //
  // This is the case that rules out per-event accounting. A 1 ms packet every
  // millisecond is a million events inside the window, so a bounded event list
  // would forget the recent past and admit without limit. Buckets do not care
  // how many packets arrive.
  {
    AirtimeGovernor g(Region::EU_868, 60u * 1000u);  // 1-minute window, 10% -> 6s
    const std::uint32_t tiny = 1000u;                // 1 ms
    std::uint32_t admitted = 0;
    for (std::uint32_t t = 0; t < 60u * 1000u; t += 1u) {
      if (g.admit(t, tiny)) ++admitted;
    }
    CHECK_MSG(admitted > 0, "some packets fit");
    CHECK_MSG(g.usedUs(60u * 1000u) <= g.budgetUs(), "the cap holds under load");
    // 6 s of budget at 1 ms per packet is at most ~6000, and never more.
    CHECK_MSG(admitted <= 7000u, "and no more than the budget allows");
  }

  {
    // A huge jump in time must not resurrect old airtime or index out of range.
    AirtimeGovernor g(Region::EU_868, 60u * 60u * 1000u);
    g.admit(0, 500000u);
    CHECK(g.usedUs(0) == 500000ull);
    // Far beyond the window in one step.
    CHECK_MSG(g.wouldAdmit(24u * 60u * 60u * 1000u, 1000u), "a day later, budget is fresh");
    CHECK_MSG(g.usedUs(24u * 60u * 60u * 1000u) == 0ull, "and the old airtime is gone");
  }

  {
    // Time going backwards must not corrupt anything. A node that restarts its
    // millisecond clock without re-initialising the governor would otherwise
    // index the ring the wrong way.
    AirtimeGovernor g(Region::EU_868, 60u * 1000u);
    g.admit(50000u, 100000u);
    g.admit(10u, 100000u);  // backwards
    CHECK_MSG(g.usedUs(10u) <= g.budgetUs(), "still within budget after a clock step back");
    CHECK(g.activeBuckets() <= AirtimeGovernor::kBuckets);
  }

  {
    // A window shorter than the bucket count still yields a usable bucket size
    // rather than dividing by zero.
    AirtimeGovernor tiny(Region::EU_868, 10u);
    CHECK_MSG(tiny.bucketMs() >= 1u, "bucket size never rounds to zero");
    CHECK(tiny.admit(0, 100u));
  }

  // --- a realistic SF11 frame fits inside a sane budget --------------------

  {
    const Modulation m = sharedDefault();
    const std::uint32_t airtime = airtimeUs(m, 64);
    AirtimeGovernor g(Region::EU_868, 60u * 60u * 1000u);  // 1 hour, 10% -> 6 min
    // At one 64-byte frame every 30 s that is 120 frames/hour = 24.8 s of
    // airtime, comfortably inside 360 s.
    for (std::uint32_t t = 0; t < 60u * 60u * 1000u; t += 30u * 1000u) {
      CHECK_MSG(g.admit(t, airtime), "a frame every 30s is well inside a 10% budget");
    }
    CHECK_MSG(g.usedFraction(60u * 60u * 1000u) < 0.1f, "and the budget confirms it");
  }
}