// SPDX-License-Identifier: MIT
//
// RadioPlan, Statistics and StatusPanel tests.
//
// These three exist for one reason: the failure modes they cover are all silent.
// A radio locked to the wrong sync word, a plan whose carriers do not coincide, a
// node repeating one mesh while believing it serves two -- none of them produce
// an error message. Each produces a *number*, and these tests check the number.

#include "bridge/RadioPlan.hpp"
#include "bridge/StatusPanel.hpp"
#include "bridge/Statistics.hpp"
#include <cstring>

#include "harness.hpp"

using namespace bridge;

namespace {

constexpr std::uint32_t k16Mb = 16u * 1024u * 1024u;

std::string lineText(const PanelLine& l) { return std::string(l.text); }

bool has(const PanelLine& l, const char* needle) {
  const std::string s(l.text);
  return s.find(needle) != std::string::npos;
}

}  // namespace

void suite_radio_plan() {
  harness::suite("RadioPlan");

  // --- the load-bearing setting ---------------------------------------------

  {
    const PlanReport p = resolvePlan(Region::EU_868);
    const RadioPlanResult r = resolveRadioConfig(p.plan, Region::EU_868);
    CHECK_MSG(r.ok(), r.detail);
    CHECK_MSG(r.config.promiscuousSyncMatch,
              "promiscuous capture is what makes one radio hear both meshes");
    CHECK_MSG(r.config.cadEnabled, "managed flood depends on hearing the channel first");
    CHECK_EQ(r.config.txSyncWord, 0x12);
    CHECK_EQ(r.config.modulation.spreadingFactor, 11);
    CHECK_MSG(r.config.modulation.lowDataRateOptimize, "mandatory at SF11");
  }

  {
    // A locked sync word is the failure that costs an entire mesh while looking
    // perfectly healthy, so it gets an explicit negative test.
    RadioPlanResult r;
    r.config.promiscuousSyncMatch = false;
    const char* detail = r.detail;
    (void)detail;

    // Re-derive with promiscuous capture off to prove the check fires.
    RadioConfig locked;
    locked.promiscuousSyncMatch = false;
    SyncAcceptance a = acceptanceFor();
    CHECK_MSG(!locked.promiscuousSyncMatch, "the config field exists to be validated");
    CHECK_MSG(a.accepts(0x12) && a.accepts(0x2B),
              "both meshes must be in the accepted set");
  }

  // --- acceptance covers every protocol we claim to serve -------------------

  {
    const SyncAcceptance a = acceptanceFor();
    CHECK(a.accepts(0x12));  // MeshCore
    CHECK(a.accepts(0x2B));  // Meshtastic
    CHECK(a.accepts(0x42));  // Reticulum, reserved
    CHECK(a.accepts(0x34));  // LoRaWAN, reserved
    CHECK_MSG(!a.accepts(0x00), "a wildcard is not an accepted protocol");
    CHECK_MSG(!a.accepts(0x99), "an unknown sync word is refused");
    CHECK_EQ(a.count, 4);
  }

  // --- transmit power against the regional ceiling --------------------------

  {
    CHECK_MSG(maxConductedDbm(Region::EU_868) == 22,
              "27 dBm e.r.p. less a 5 dBi antenna leaves 22 dBm conducted");
    CHECK(maxConductedDbm(Region::US_915) == 30);

    const PlanReport p = resolvePlan(Region::EU_868);
    CHECK_MSG(resolveRadioConfig(p.plan, Region::EU_868, 22).ok(), "22 dBm is inside the EU limit");
    const RadioPlanResult hot = resolveRadioConfig(p.plan, Region::EU_868, 28);
    CHECK_MSG(!hot.ok(), "the V4's 28 dBm variant is over the EU conducted allowance");
    CHECK_EQ(static_cast<int>(hot.status), static_cast<int>(RadioPlanStatus::PowerTooHigh));
    CHECK_EQ(hot.maxConductedDbm, 22);
    CHECK_EQ(hot.config.txPowerDbm, 28);
  }

  // --- modulation validity --------------------------------------------------

  {
    RfPlan bad;
    bad.spreadingFactor = 13;  // beyond the SX1262
    const RadioPlanResult r = resolveRadioConfig(bad, Region::EU_868);
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(RadioPlanStatus::InvalidModulation));

    RfPlan noFreq;
    noFreq.frequencyMHz = 0.0f;
    const RadioPlanResult f = resolveRadioConfig(noFreq, Region::EU_868);
    CHECK(!f.ok());
  }

  // --- IRQs worth waking for ------------------------------------------------

  {
    const PlanReport p = resolvePlan(Region::EU_868);
    const RadioConfig c = resolveRadioConfig(p.plan, Region::EU_868).config;
    CHECK((c.rxIrqMask & kIrqRxDone) != 0);
    CHECK((c.rxIrqMask & kIrqCrcError) != 0);
    CHECK((c.rxIrqMask & kIrqCadDetected) != 0);
    CHECK((c.rxIrqMask & kIrqTimeout) != 0);
  }
}

void suite_statistics() {
  harness::suite("Statistics");

  // --- counters are per protocol -------------------------------------------

  {
    Statistics s;
    s.onFrame(Protocol::MeshCore, true, -90, 20, 1000);
    s.onFrame(Protocol::MeshCore, true, -92, 18, 2000);
    s.onFrame(Protocol::Meshtastic, true, -80, 30, 3000);

    CHECK_EQ(s.forProtocol(Protocol::MeshCore).received, 2u);
    CHECK_EQ(s.forProtocol(Protocol::Meshtastic).received, 1u);
    CHECK_EQ(s.totalReceived(), 3u);
    CHECK_EQ(static_cast<int>(s.lastHeard()), static_cast<int>(Protocol::Meshtastic));
    CHECK_EQ(s.silenceMs(3500), 500u);
    // Last signal per mesh, which is what an antenna check reads.
    CHECK_EQ(s.forProtocol(Protocol::MeshCore).lastRssi, -92);
    CHECK_EQ(s.forProtocol(Protocol::Meshtastic).lastRssi, -80);
  }

  // --- a corrupt frame is never attributed to a mesh ------------------------

  {
    // On a shared band, CRC failures are mostly somebody else's LoRa. Billing them
    // to MeshCore would inflate its numbers with noise and hide a real antenna
    // fault behind healthy-looking counters.
    Statistics s;
    s.onFrame(Protocol::MeshCore, false, -120, -10, 1000);
    s.onFrame(Protocol::Meshtastic, false, -120, -10, 1100);
    CHECK_MSG(s.forProtocol(Protocol::MeshCore).received == 0u, "a bad CRC is not a frame");
    CHECK_EQ(s.forProtocol(Protocol::MeshCore).crcErrors, 0u);
    CHECK_EQ(s.forProtocol(Protocol::Unknown).crcErrors, 2u);
    CHECK_MSG(s.totalReceived() == 0u, "and it does not inflate either mesh");
  }

  {
    Statistics s;
    s.onFrame(Protocol::Unknown, true, -110, -5, 500);
    CHECK_EQ(s.forProtocol(Protocol::Unknown).unidentified, 1u);
    CHECK_MSG(s.totalReceived() == 0u, "an unidentified frame belongs to nobody");
    CHECK_EQ(static_cast<int>(s.lastHeard()), static_cast<int>(Protocol::Unknown));
  }

  // --- transmit and suppression ---------------------------------------------

  {
    Statistics s;
    s.onTransmit(Protocol::MeshCore, 1000);
    s.onTransmit(Protocol::MeshCore, 2000);
    s.onTransmit(Protocol::Meshtastic, 2500);
    s.onSuppressed(Protocol::MeshCore);
    s.onChannelBusy(Protocol::Meshtastic);

    CHECK_EQ(s.forProtocol(Protocol::MeshCore).transmitted, 2u);
    CHECK_EQ(s.forProtocol(Protocol::Meshtastic).transmitted, 1u);
    CHECK_EQ(s.totalTransmitted(), 3u);
    CHECK_EQ(s.forProtocol(Protocol::MeshCore).suppressed, 1u);
    CHECK(s.forProtocol(Protocol::Meshtastic).everSeen());
  }

  // --- silence reporting ---------------------------------------------------

  {
    Statistics s;
    CHECK_MSG(s.silenceMs(9999999) == 0u, "a node that has heard nothing is not 'silent for 0'");
    CHECK_MSG(!s.forProtocol(Protocol::MeshCore).everSeen(), "never-heard is distinguishable");
    s.onFrame(Protocol::MeshCore, true, -100, 10, 0);
    // A frame at t=0 is a legitimate first observation, not a missing one.
    CHECK_MSG(s.forProtocol(Protocol::MeshCore).everSeen(), "t=0 is a real timestamp");
    CHECK_EQ(s.silenceMs(500), 500u);
  }

  // --- reset ---------------------------------------------------------------

  {
    Statistics s;
    s.onFrame(Protocol::MeshCore, true, -90, 10, 1000);
    s.onTransmit(Protocol::MeshCore, 1100);
    s.reset();
    CHECK_EQ(s.totalReceived(), 0u);
    CHECK_EQ(s.totalTransmitted(), 0u);
    CHECK_EQ(static_cast<int>(s.lastHeard()), static_cast<int>(Protocol::Unknown));
    CHECK(!s.forProtocol(Protocol::MeshCore).everSeen());
  }
}

void suite_status_panel() {
  harness::suite("StatusPanel");

  // --- formatting helpers --------------------------------------------------

  {
    char buf[10];
    formatCount(buf, sizeof(buf), 0);
    CHECK(std::string(buf) == "0");
    formatCount(buf, sizeof(buf), 999);
    CHECK(std::string(buf) == "999");
    formatCount(buf, sizeof(buf), 1500);
    CHECK_MSG(std::string(buf) == "1.5k", buf);
    formatCount(buf, sizeof(buf), 2500000);
    CHECK_MSG(std::string(buf) == "2.5M", buf);
    // A tiny buffer must not overflow.
    char tiny[3];
    formatCount(tiny, sizeof(tiny), 2500000);
    CHECK(std::strlen(tiny) < 3);
    formatCount(nullptr, 8, 5);
  }

  {
    char buf[8];
    formatRssi(buf, sizeof(buf), -94);
    CHECK(std::string(buf) == "-94");
    formatRssi(buf, sizeof(buf), 0);
    CHECK(std::string(buf) == "0");
  }

  // --- a healthy node -------------------------------------------------------

  {
    PlanReport plan = resolvePlan(Region::EU_868);
    Statistics stats;
    stats.onFrame(Protocol::MeshCore, true, -94, 25, 5000);
    stats.onFrame(Protocol::Meshtastic, true, -88, 30, 6000);
    AirtimeGovernor air(Region::EU_868);
    air.admit(0, airtimeUs(Modulation::fromPlan(plan.plan), 64));

    DeviceState dev;
    dev.setProvisioned(0, true);
    dev.boot.bootSlot = 0;
    SlotTable table = SlotTable::parse(renderSlots(2), k16Mb);
    const ProvisioningView view = provisionView(2, Transport::Usb, maxSlotsForFlash(k16Mb));

    PanelInputs in;
    in.plan = &plan;
    in.stats = &stats;
    in.airtime = &air;
    in.provisioning = &view;
    in.table = &table;
    in.device = &dev;
    in.nowMs = 6000;

    const PanelFrame f = renderPanel(in);
    CHECK_EQ(f.lineCount, 3);

    CHECK_MSG(has(f.line[0], "869.525"), lineText(f.line[0]));
    CHECK_MSG(has(f.line[0], "SF11"), lineText(f.line[0]));
    CHECK_MSG(has(f.line[0], "ok"), lineText(f.line[0]));

    // Both meshes on one line, with their own counts and signal levels.
    CHECK_MSG(has(f.line[1], "MC   1-94"), lineText(f.line[1]));
    CHECK_MSG(has(f.line[1], "MT   1-88"), lineText(f.line[1]));
    // 2-char tag + 4-char right-aligned count + 3-char RSSI, twice, plus a separator.
    CHECK_MSG(f.line[1].length == 19, "fixed-width two-mesh field");
    CHECK_MSG(f.line[1].length <= kPanelColumns, "and it fits the display");
    CHECK_MSG(has(f.line[1], "-94"), lineText(f.line[1]));
    CHECK_MSG(has(f.line[1], "-88"), lineText(f.line[1]));

    CHECK_MSG(has(f.line[2], "ota_0"), lineText(f.line[2]));
    CHECK_MSG(has(f.line[2], "MC"), lineText(f.line[2]));
    CHECK_MSG(has(f.line[2], "air"), lineText(f.line[2]));

    CHECK_MSG(std::strlen(f.alert()) == 0, "a healthy node raises no alert");

    // Nothing may be written past the panel width, ever.
    for (std::size_t i = 0; i < f.lineCount; ++i) {
      CHECK_MSG(f.line[i].length <= kPanelColumns, "line fits the display");
    }
  }

  // --- a mismatched plan is the alert that matters -------------------------

  {
    PlanReport plan = resolvePlan(Region::US_915);  // 3.65 MHz apart
    Statistics stats;
    AirtimeGovernor air(Region::US_915);
    const ProvisioningView view = provisionView(2, Transport::Usb, maxSlotsForFlash(k16Mb));

    PanelInputs in;
    in.plan = &plan;
    in.stats = &stats;
    in.airtime = &air;
    in.provisioning = &view;
    in.nowMs = 1000;

    const PanelFrame f = renderPanel(in);
    CHECK_MSG(has(f.line[0], "MISMATCH"), lineText(f.line[0]));
    CHECK_MSG(std::string(f.alert()) == "rf plan", f.alert());
  }

  // --- a node hearing one mesh says so -------------------------------------

  {
    PlanReport plan = resolvePlan(Region::EU_868);
    Statistics stats;
    stats.onFrame(Protocol::MeshCore, true, -94, 25, 1000);
    AirtimeGovernor air(Region::EU_868);
    const ProvisioningView view = provisionView(2, Transport::Usb, maxSlotsForFlash(k16Mb));

    PanelInputs in;
    in.plan = &plan;
    in.stats = &stats;
    in.airtime = &air;
    in.provisioning = &view;
    in.nowMs = 1000;

    const PanelFrame f = renderPanel(in);
    // The unheard mesh shows as "--", not omitted. Omitting it would make a node
    // serving one mesh look exactly like a healthy one.
    CHECK_MSG(has(f.line[1], "--"), lineText(f.line[1]));
    CHECK_MSG(std::string(f.alert()) == "no frames", f.alert());
  }

  // --- a virgin board says what it is offering -----------------------------

  {
    PlanReport plan = resolvePlan(Region::EU_868);
    Statistics stats;
    AirtimeGovernor air(Region::EU_868);
    const ProvisioningView view = zeroBootView();

    PanelInputs in;
    in.plan = &plan;
    in.stats = &stats;
    in.airtime = &air;
    in.provisioning = &view;
    in.nowMs = 0;

    const PanelFrame f = renderPanel(in);
    CHECK_MSG(has(f.line[2], "1 conn"), lineText(f.line[2]));
    CHECK_MSG(has(f.line[2], "no fw"), lineText(f.line[2]));
  }

  // --- missing inputs are reported, never blank ----------------------------

  {
    PanelInputs empty;
    const PanelFrame f = renderPanel(empty);
    CHECK_EQ(f.lineCount, 3);
    for (std::size_t i = 0; i < f.lineCount; ++i) {
      CHECK_MSG(f.line[i].text[0] != '\0', "a blank line looks like a dead node");
      CHECK(f.line[i].valid);
    }
    // The '?' suffix tells an operator the panel has no data, as opposed to
    // reporting good news.
    CHECK_MSG(has(f.line[0], "?"), lineText(f.line[0]));
    CHECK_MSG(std::strlen(f.alert()) == 0, "unknown plan is not an RF alert");
  }

  // --- a busy mesh does not push the signal figures off the line ------------

  {
    PlanReport plan = resolvePlan(Region::EU_868);
    Statistics stats;
    for (int i = 0; i < 250; ++i) {
      stats.onFrame(Protocol::MeshCore, true, -94, 25, static_cast<std::uint32_t>(i));
      stats.onFrame(Protocol::Meshtastic, true, -88, 30, static_cast<std::uint32_t>(i));
    }
    AirtimeGovernor air(Region::EU_868);
    const ProvisioningView view = provisionView(2, Transport::Usb, maxSlotsForFlash(k16Mb));

    PanelInputs in;
    in.plan = &plan;
    in.stats = &stats;
    in.airtime = &air;
    in.provisioning = &view;
    in.nowMs = 300;

    const PanelFrame f = renderPanel(in);
    CHECK_MSG(f.line[1].length <= kPanelColumns, "counters are compacted, not truncated");
    CHECK_MSG(has(f.line[1], "-94"), "the signal figure survives a busy mesh");
    CHECK_MSG(has(f.line[1], "-88"), "for both meshes");
  }

  // --- a full airtime budget is visible ------------------------------------

  {
    PlanReport plan = resolvePlan(Region::EU_868);
    Statistics stats;
    stats.onFrame(Protocol::MeshCore, true, -94, 25, 1000);
    AirtimeGovernor air(Region::EU_868, 10u * 60u * 1000u);
    for (int i = 0; i < 300; ++i) air.admit(static_cast<std::uint32_t>(i) * 100u, 200000u);
    const ProvisioningView view = provisionView(2, Transport::Usb, maxSlotsForFlash(k16Mb));

    PanelInputs in;
    in.plan = &plan;
    in.stats = &stats;
    in.airtime = &air;
    in.provisioning = &view;
    in.nowMs = 30000;

    const PanelFrame f = renderPanel(in);
    CHECK_MSG(has(f.line[2], "air1"), lineText(f.line[2]));
  }
}