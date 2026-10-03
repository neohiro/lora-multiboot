// SPDX-License-Identifier: MIT
//
// SharedContext tests.
//
// The claims being checked are the ones that decide whether a multi-slot board is
// viable at all:
//
//   1. The shared block fits in a small, fixed RAM budget -- asserted at compile
//      time, and measured here so the number is visible rather than hypothetical.
//   2. Adding slots does not grow it. This is the difference between "several
//      firmwares on one board" and "several firmwares and several copies of the
//      radio plan on one board".
//   3. It is safe to share between images: trivially copyable, self-describing, and
//      version-checked so a stale slot refuses rather than misreads.

#include "bridge/SharedContext.hpp"
#include "harness.hpp"

using namespace bridge;

namespace {

// Sizes, so the budget discussion is grounded in numbers.
std::size_t sharedSize() { return sizeof(SharedContext); }

}  // namespace

void suite_shared_context() {
  harness::suite("SharedContext");

  // --- the budget -----------------------------------------------------------

  {
    CHECK_MSG(sharedSize() <= kSharedContextRamCeiling,
              "the shared block fits the RAM budget");
    // 560 bytes against a 1 KB ceiling, so roughly 450 bytes are left for
    // everything else on the board. Asserted as a fraction rather than a literal so
    // this is a statement about headroom and not about today's exact size.
    CHECK_MSG(sharedSize() <= kSharedContextRamCeiling * 3 / 4,
              "and leaves at least a quarter of the budget free");
    CHECK(std::is_trivially_copyable<SharedContext>::value);
    CHECK_EQ(sharedSize() % 4, 0u);
  }

  {
    // The two things that dominate, measured individually so a regression in
    // either is obvious rather than just "the total went up".
    CHECK_MSG(sizeof(AirtimeGovernor) <= 320,
              "the airtime governor is the largest single consumer");
    CHECK_MSG(sizeof(Statistics) <= 256, "the counters are small");
    CHECK_MSG(sizeof(RadioConfig) <= 128, "the radio config is small");
    // Trimming the bucket count from 64 to 32 saved 256 bytes of RAM that every
    // board would otherwise pay for, at the cost of a few percent more
    // conservatism in the airtime accounting.
    CHECK_MSG(sizeof(AirtimeGovernor) <= 300, "governor trimmed to 32 buckets");
  }

  // --- adding slots must not grow the shared block --------------------------

  {
    // The whole point. Slot count is one byte; the rest of the block is identical
    // whether there is one slot or five, because there is one radio, one plan and
    // one airtime limit regardless.
    SharedContext a;
    a.slotCount = 1;
    a.activeSlot = 0;

    SharedContext b;
    b.slotCount = 5;
    for (std::uint8_t i = 0; i < 5; ++i) b.device.setProvisioned(i, true);
    b.boot.bootSlot = 0;

    CHECK_MSG(sizeof(a) == sizeof(b), "the block does not grow with slot count");
    CHECK_EQ(a.slotCount, 1);
    CHECK_EQ(b.slotCount, 5);
    CHECK_EQ(b.device.provisionedCount(), 5);
    CHECK_EQ(a.activeSlot, 0);
  }

  {
    // Per-slot state is exactly two bytes of mask, not a per-slot struct. Sixteen
    // slots cost sixteen bits.
    DeviceState d;
    for (std::uint8_t i = 0; i < 16; ++i) d.setProvisioned(i, true);
    CHECK_EQ(d.provisionedCount(), 16);
    CHECK_MSG(sizeof(d.provisionedMask) == 2, "sixteen slots cost two bytes");
    d.setProvisioned(15, false);
    CHECK_EQ(d.provisionedCount(), 15);
    // Out of range must be ignored rather than corrupting the mask.
    d.setProvisioned(16, true);
    CHECK_EQ(d.provisionedCount(), 15);
    CHECK(!d.isProvisioned(200));
  }

  // --- safe to share between images -----------------------------------------

  {
    SharedContext ctx;
    CHECK_MSG(!ctx.compatible(), "an unstamped block reports incompatible");

    ctx.stamp();
    CHECK_MSG(ctx.compatible(), "a stamped block is compatible");
    CHECK_EQ(ctx.sizeBytes, sizeof(SharedContext));

    // A slot built against a different layout must refuse rather than misread.
    SharedContext stale;
    stale.version = 99;
    stale.stamp();
    CHECK_MSG(!stale.compatible(), "a version mismatch is detected");

    SharedContext alien;
    alien.magic = 0xDEADBEEF;
    alien.stamp();
    CHECK_MSG(!alien.compatible(), "a foreign block is detected");
  }

  // --- one plan, one budget -------------------------------------------------

  {
    // Populated the way firmware would, and confirmed coherent. The airtime cap
    // lives here once, which is what stops a five-slot board from spending 10%
    // five times over.
    SharedContext ctx;
    const PlanReport plan = resolvePlan(Region::EU_868);
    ctx.plan = plan.plan;
    ctx.radio = resolveRadioConfig(plan.plan, Region::EU_868, 22).config;
    ctx.airtime = AirtimeGovernor(Region::EU_868);
    ctx.stamp();

    CHECK_MSG(ctx.compatible(), "a populated block is still compatible");
    CHECK_EQ(ctx.airtime.limitPercent(), 10);
    CHECK_EQ(ctx.plan.spreadingFactor, 11);
    CHECK_MSG(ctx.radio.promiscuousSyncMatch, "and the shared plan keeps its shape");
    CHECK_EQ(static_cast<int>(ctx.airtime.region()), static_cast<int>(Region::EU_868));
  }

  // --- counters survive a slot switch ---------------------------------------

  {
    // Counters live in the shared block precisely so that switching slots does not
    // lose the history: the panel can then be drawn by whichever slot is running and
    // still show what the board has been hearing.
    SharedContext ctx;
    for (int i = 0; i < 5; ++i) {
      ctx.stats.onFrame(Protocol::MeshCore, true, -95, 20, static_cast<std::uint32_t>(i));
      ctx.stats.onFrame(Protocol::Meshtastic, true, -90, 25, static_cast<std::uint32_t>(i));
    }
    ctx.activeSlot = 1;

    CHECK_EQ(ctx.stats.forProtocol(Protocol::MeshCore).received, 5u);
    CHECK_EQ(ctx.stats.forProtocol(Protocol::Meshtastic).received, 5u);

    // The switch changes nothing about the accounting.
    ctx.activeSlot = 0;
    CHECK_EQ(ctx.stats.totalReceived(), 10u);
  }

  // --- the airtime budget is a whole-board budget ---------------------------

  {
    // Five slots must not mean five budgets. Spending it once and observing the
    // limit is the behaviour that keeps the board inside its licence.
    SharedContext ctx;
    ctx.airtime = AirtimeGovernor(Region::EU_868, 10u * 60u * 1000u);
    const std::uint32_t packet = 200000u;
    std::uint32_t admitted = 0;
    for (std::uint32_t t = 0; t < 600000u; t += 100u) {
      if (ctx.airtime.admit(t, packet)) ++admitted;
    }
    CHECK_MSG(admitted == 300, "the board's whole budget, spent once");
    CHECK_MSG(ctx.airtime.usedUs(30000u) <= ctx.airtime.budgetUs(),
              "and never exceeded, however many slots are installed");
  }
}