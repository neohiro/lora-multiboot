// SPDX-License-Identifier: MIT
//
// ChannelPlan tests.
//
// The load-bearing assertion in this whole project is that EU_868 resolves to a
// shared carrier. If that ever stops being true the architecture is dead, so it
// is asserted explicitly rather than left to be rediscovered in the field. The
// US_915 case matters just as much: it is the region where the idea does *not*
// work out of the box, and the code has to say so instead of trying.

#include "bridge/ChannelPlan.hpp"
#include "harness.hpp"

using namespace bridge;

namespace {

bool near(float a, float b, float tol) {
  const float d = a > b ? a - b : b - a;
  return d <= tol;
}

}  // namespace

void suite_channel_plan() {
  harness::suite("ChannelPlan");

  // --- the premise ----------------------------------------------------------

  {
    const RegionDefaults* eu = regionDefaults(Region::EU_868);
    CHECK(std::string(eu->code) == "EU_868");
    CHECK_MSG(near(eu->meshtasticFreqMHz, 869.525f, 0.001f), "Meshtastic EU_868 LongFast");
    CHECK_MSG(near(eu->meshCoreFreqMHz, 869.525f, 0.001f), "MeshCore default");
    CHECK_MSG(near(eu->meshtasticFreqMHz, eu->meshCoreFreqMHz, 0.001f),
              "THE PREMISE: both meshes land on one carrier in the EU");
    CHECK(eu->meshCoreFreqVerified);
    CHECK(near(eu->bandStartMHz, 869.40f, 0.001f));
    CHECK(near(eu->bandEndMHz, 869.65f, 0.001f));
  }

  // --- EU resolves clean ----------------------------------------------------

  {
    const PlanReport r = resolvePlan(Region::EU_868);
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(PlanStatus::Ok));
    CHECK(r.ok());
    CHECK_MSG(near(r.plan.frequencyMHz, 869.525f, 0.001f), "shared carrier");
    CHECK_MSG(r.plan.bandwidthKHz == 250.0f, "250 kHz, both stacks");
    CHECK_EQ(r.plan.spreadingFactor, 11);
    CHECK_EQ(r.plan.codingRateDenominator, 5);
    CHECK_EQ(r.plan.meshtasticSyncWord, 0x2B);
    CHECK_EQ(r.plan.meshCoreSyncWord, 0x12);
  }

  // --- compliance: the EU airtime budget is 10%, not MeshCore's stock 50% ---
  //
  // 869.4-869.65 MHz carries a 10% duty-cycle limit under the EU's
  // non-specific-SRD entry. MeshCore ships a 50% software default. A repeater
  // left on that default is not using a legal airtime budget.
  CHECK_EQ(dutyCycleFor(Region::EU_868), 10);

  // --- US_915: the premise does not hold, and the code must admit it --------

  {
    const PlanReport r = resolvePlan(Region::US_915);
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(PlanStatus::FrequencyMismatch));
    CHECK(!r.ok());
    // Meshtastic hashes LongFast to slot 20 = 906.875; MeshCore sits at 910.525.
    CHECK_MSG(near(r.driftMHz, 3.65f, 0.01f), "drift between the two defaults");
    // The radio takes Meshtastic's default, so it sits *below* MeshCore's
    // channel, and the side named is the one that has to come down to meet it.
    // Either side could move; this is just the deterministic default, and what
    // matters for the operator is that it is never silent.
    CHECK_MSG(std::string(r.retune) == "meshcore", "names the deviating side");
  }

  // --- an override is judged where it actually landed ------------------------

  {
    // Moving the radio onto the MeshCore US channel closes the drift for real,
    // not on paper.
    const PlanReport moved = resolvePlan(Region::US_915, 910.525f);
    CHECK_MSG(moved.status != PlanStatus::FrequencyMismatch,
              "override onto the MeshCore carrier resolves the mismatch");
    CHECK_MSG(moved.status == PlanStatus::MeshCoreFreqUnknown,
              "but the US MeshCore default stays flagged as unverified");

    const PlanReport bad = resolvePlan(Region::US_915, 915.0f);
    CHECK_EQ(static_cast<int>(bad.status), static_cast<int>(PlanStatus::FrequencyMismatch));
  }

  // --- out of band is a configuration error, not a preference ---------------

  {
    const PlanReport r = resolvePlan(Region::EU_868, 915.0f);
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(PlanStatus::OutOfBand));
    CHECK(!r.ok());
  }

  // --- a valid EU override still passes --------------------------------------

  {
    const PlanReport r = resolvePlan(Region::EU_868, 869.525f);
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(PlanStatus::Ok));
  }

  // --- Custom is operator-owned and unvalidated -----------------------------

  {
    const PlanReport r = resolvePlan(Region::Custom, 145.0f);
    CHECK_MSG(r.ok(), "custom regions are not second-guessed");
    CHECK(std::string(regionCode(Region::Custom)) == "CUSTOM");
  }

  // --- every built-in region stays internally consistent --------------------

  {
    const Region all[] = {Region::EU_868,  Region::US_915,   Region::ANZ_915,
                          Region::IN_865,  Region::KR_922,   Region::SG_923,
                          Region::Custom};
    for (Region r : all) {
      const RegionDefaults* d = regionDefaults(r);
      CHECK(d != nullptr);
      if (d == nullptr) continue;
      CHECK_MSG(d->bandEndMHz >= d->bandStartMHz, "band is not inverted");
      CHECK_MSG(d->dutyCyclePercent > 0 && d->dutyCyclePercent <= 100,
                "duty cycle is a sane percentage");
      CHECK_MSG(d->meshtasticFreqMHz >= d->bandStartMHz &&
                    d->meshtasticFreqMHz <= d->bandEndMHz,
                "Meshtastic default sits inside the region's own band");
    }
  }

  // --- lookup by code, including the bad-input path --------------------------

  {
    CHECK(std::string(regionDefaultsByCode("EU_868")->code) == "EU_868");
    // Unknown and null codes fall back to the EU rather than reading garbage.
    CHECK(std::string(regionDefaultsByCode("NOPE")->code) == "EU_868");
    CHECK(std::string(regionDefaultsByCode(nullptr)->code) == "EU_868");
  }

  // --- the status line ------------------------------------------------------

  {
    const PlanReport ok = resolvePlan(Region::EU_868);
    const std::string good = describePlan(ok);
    CHECK_MSG(good.find("shared OK") != std::string::npos, good);
    CHECK_MSG(good.find("869.525") != std::string::npos, good);
    CHECK_MSG(good.find("SF11") != std::string::npos, good);

    const PlanReport bad = resolvePlan(Region::US_915);
    const std::string line = describePlan(bad);
    CHECK_MSG(line.find("MISMATCH") != std::string::npos, line);
  }
}
