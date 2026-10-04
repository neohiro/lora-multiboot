// SPDX-License-Identifier: MIT
//
// On-device bring-up.
//
// Deliberately dependency-free beyond Arduino: every decision this file reports is
// made by the portable modules beside it, which compile with nothing but a C++17
// compiler and are exercised on a laptop by the same gate that runs in CI.
//
// The job here is not to control a radio yet. It is to make the node prove, out
// loud, that everything it is about to depend on is actually true: the RF plan, the
// flash layout, the isolation between its frameworks, the airtime budget, and the
// airtime it has spent. Every one of those has a failure mode that is otherwise
// silent -- a node that repeats happily while serving one mesh, or that is quietly
// over its duty-cycle limit, is not something anybody notices until it is a
// regulatory problem or a support ticket.

#include <Arduino.h>

#include <string>

#include "bridge/Airtime.hpp"
#include "bridge/Inventory.hpp"
#include "bridge/RadioProfiles.hpp"
#include "bridge/ChannelPlan.hpp"
#include "bridge/ProtocolId.hpp"
#include "bridge/Provisioning.hpp"
#include "bridge/RadioPlan.hpp"
#include "bridge/Roles.hpp"
#include "bridge/SlotLifecycle.hpp"
#include "bridge/SlotTable.hpp"
#include "bridge/Statistics.hpp"
#include "bridge/StatusPanel.hpp"
#include "bridge/SystemUpdate.hpp"

namespace {

// Region is a build flag. Default EU_868, because that is the one region where the
// two community defaults land on the same carrier and the whole architecture
// depends on it.
#ifndef BRIDGE_REGION
#define BRIDGE_REGION "EU_868"
#endif

// 0 means "use the region's default frequency".
#ifndef FREQUENCY_MHZ
#define FREQUENCY_MHZ 0.0f
#endif

#ifndef TX_POWER_DBM
#define TX_POWER_DBM 22
#endif

// Read from the chip at runtime rather than assumed. The Heltec V4 ships 16 MB of
// external flash, but a V3 does not, and a five-slot layout on an 8 MB board
// fails in a way that looks like a bad download rather than a wrong assumption.
#ifndef FLASH_SIZE_BYTES
#define FLASH_SIZE_BYTES (16u * 1024u * 1024u)
#endif

constexpr std::uint32_t kFlashSize = FLASH_SIZE_BYTES;

bridge::Region region() {
  return bridge::regionDefaultsByCode(BRIDGE_REGION)->region;
}

void rule() { Serial.println("--------------------------------------------------"); }

// The partition table this image was built against, kept in sync with
// firmware/partitions/quadboot.csv by tools/gen_layouts.py. Deliberately embedded
// rather than read from flash, so a node can always report what it believes even
// when the flash it was told to trust has been rewritten underneath it.
//
// The bootloader and the partition table are absent because ESP-IDF's generator
// rejects any declared partition below 0x9000; they exist, and
// SystemUpdate knows their geometry as constants.
const char* kCompiledLayout =
    "nvs,            data, nvs,     0x9000,   0xA000,\n"
    "otadata,        data, ota,     0x13000,  0x2000,\n"
    "coredump,       data, coredump,0x15000,  0x10000,\n"
    "ota_0,          app,  ota_0,   0x30000,  0x200000,\n"
    "fs_meshcore,    data, spiffs,  0x230000, 0x100000,\n"
    "ota_1,          app,  ota_1,   0x330000, 0x200000,\n"
    "fs_meshtastic,  data, spiffs,  0x530000, 0x100000,\n"
    "ota_2,          app,  ota_2,   0x630000, 0x200000,\n"
    "fs_reticulum,   data, spiffs,  0x830000, 0x100000,\n"
    "ota_3,          app,  ota_3,   0x930000, 0x200000,\n"
    "fs_lorawan,     data, spiffs,  0xB30000, 0x100000,\n"
    "ota_4,          app,  ota_4,   0xC30000, 0x200000,\n"
    "fs_custom,      data, spiffs,  0xE30000, 0x100000,\n";

void reportPlan(const bridge::PlanReport& plan) {
  Serial.println();
  Serial.println("RF plan");
  Serial.print("  ");
  Serial.println(bridge::describePlan(plan));
  Serial.print("  status:  ");
  Serial.println(plan.detail);

  switch (plan.status) {
    case bridge::PlanStatus::FrequencyMismatch: {
      Serial.print("  the two defaults are ");
      Serial.print(plan.driftMHz, 3);
      Serial.println(" MHz apart");
      Serial.print("  retune the ");
      Serial.print(plan.retune);
      Serial.println(" side to match, or this node hears one mesh plus noise");
      break;
    }
    case bridge::PlanStatus::OutOfBand:
      Serial.print("  ");
      Serial.print(plan.driftMHz, 3);
      Serial.println(" MHz is outside this region's band: a configuration error");
      break;
    case bridge::PlanStatus::MeshCoreFreqUnknown:
      Serial.println("  carriers coincide but the MeshCore default is unconfirmed here");
      Serial.println("  confirm both communities use this channel before deploying");
      break;
    case bridge::PlanStatus::Ok:
      Serial.println("  shared carrier: one radio, both meshes");
      break;
  }

  Serial.print("  airtime cap: ");
  Serial.print(bridge::dutyCycleFor(region()));
  Serial.println("%  (MeshCore's stock 50% is not a legal budget here)");
}

void reportRadio(const bridge::RadioPlanResult& radio) {
  Serial.println();
  Serial.println("Radio");
  Serial.print("  config:      ");
  Serial.println(bridge::radioPlanStatusName(radio.status));
  Serial.print("  promiscuous: ");
  Serial.println(radio.config.promiscuousSyncMatch ? "yes (required)" : "NO - one mesh would be invisible");
  Serial.print("  sync words:  ");
  const bridge::SyncAcceptance acc = bridge::acceptanceFor();
  Serial.print("0x");
  Serial.print(acc.words[0], HEX);
  Serial.print(" 0x");
  Serial.print(acc.words[1], HEX);
  Serial.println(" ...");
  Serial.print("  tx power:    ");
  Serial.print(radio.config.txPowerDbm);
  Serial.print(" dBm  (region allows ");
  Serial.print(radio.maxConductedDbm);
  Serial.println(")");

  bridge::Modulation m = radio.config.modulation;
  bridge::RfPlan mp;
  mp.frequencyMHz = radio.config.frequencyMHz;
  mp.bandwidthKHz = m.bandwidthKHz;
  mp.spreadingFactor = m.spreadingFactor;
  mp.codingRateDenominator = m.codingRateDenominator;
  const bridge::Modulation mod = bridge::Modulation::fromPlan(mp);
  Serial.print("  airtime:     ");
  Serial.print(static_cast<float>(bridge::airtimeUs(mod, 64)) / 1000.0f, 2);
  Serial.println(" ms for a 64-byte frame");
}

void reportFlash(const bridge::SlotTable& table) {
  Serial.println();
  Serial.println("Flash");
  Serial.print("  ");
  Serial.println(table.describe());
  Serial.print("  geometry:    ");
  Serial.println(table.validate().detail);
  Serial.print("  frameworks:  ");
  Serial.println(table.validateFrameworks().ok() ? "isolated" : table.validateFrameworks().detail);
  Serial.print("  system zone: ");
  Serial.println(bridge::systemRegionIsContained(table) ? "self-contained"
                                                         : "NOT CONTAINED - updates unsafe");
  Serial.print("  slots:       ");
  Serial.print(bridge::maxSlotsForFlash(kFlashSize));
  Serial.println(" fit in this flash");
}

void reportSlots(const bridge::SlotTable& table, bridge::DeviceState& state) {
  Serial.println();
  Serial.println("Slots");
  const std::uint8_t declared = bridge::provisionedSlots(table);
  for (std::uint8_t i = 0; i < declared; ++i) {
    const bridge::SlotStatus s = bridge::slotStatus(table, state, i);
    Serial.print("  ota_");
    Serial.print(i);
    Serial.print("  ");
    Serial.print(s.state == bridge::SlotState::Provisioned ? "provisioned" : "empty    ");
    Serial.print("  0x");
    Serial.print(s.offset, HEX);
    Serial.print("  settings 0x");
    Serial.print(s.fsOffset, HEX);
    Serial.print("  ");
    Serial.println(s.bootsByDefault ? "<- boots" : "");
  }
}

void reportRoles() {
  // The role catalogue. How many of these actually fit is a flash question, answered
  // by reportSlots(); how many are *valid* together is answered by auditBoard().
  Serial.println();
  Serial.println("Roles");
  std::size_t count = 0;
  const bridge::RoleProfile* all = bridge::allProfiles(&count);
  for (std::size_t i = 0; i < count; ++i) {
    Serial.print("  ");
    Serial.print(all[i].label);
    Serial.print(all[i].servesTwoMeshes ? "   (serves both meshes)" : "");
    Serial.println("");
  }
  Serial.print("  slots are exclusive: one runs at a time and owns the SX1262.");
  Serial.println("");
}

void reportInventory(const bridge::SlotTable& table, const bridge::DeviceState& state) {
  const bridge::SlotInventory inv = bridge::inventory(table, state, "Heltec LoRa 32 V4");

  Serial.println();
  Serial.println("Installed slots");
  // One line per slot, in the format a connected host tool parses. Printed here so
  // the same view a UI would show is visible over a serial cable.
  const std::vector<std::string> lines = bridge::inventoryLines(inv);
  for (const std::string& l : lines) Serial.println(l.c_str());
  Serial.print("  ");
  Serial.println(bridge::inventorySummary(inv).c_str());

  if (!inv.recoveryPossible) {
    Serial.println("  WARNING: every slot is filled. The board has no obvious");
    Serial.println("  recovery target. Keep the reserved free slot empty.");
  }
}

void reportMasterRx(const std::vector<bridge::TxProfile>& installed) {
  Serial.println();
  Serial.println("RX master");
  const bridge::ReconcileReport r = bridge::reconcile(installed);
  Serial.print("  ");
  if (r.hasMaster) {
    Serial.print(r.master.frequencyMHz, 3);
    Serial.print(" MHz  BW ");
    Serial.print(r.master.bandwidthKHz, 0);
    Serial.print(" kHz  SF");
    Serial.print(r.master.spreadingFactor);
    Serial.print("  4/");
    Serial.print(r.master.codingRateDenominator);
    Serial.print("  preamble ");
    Serial.print(r.master.preambleSymbols);
  } else {
    Serial.print("none -- ");
    Serial.print(r.detail);
  }
  Serial.println("");
  Serial.print("  profiles heard: ");
  Serial.print(r.plan.profilesHeard);
  Serial.print(" of ");
  Serial.print(installed.size());
  Serial.print("   (");
  Serial.print(r.detail);
  Serial.println(")");
}

void reportIdentifier() {
  Serial.println();
  Serial.println("Identifier");
  const std::uint8_t mc[] = {0x11, 0x01, 0x03, 0x7A, 0x9C, 0x2E, 0x51};
  const std::uint8_t mt[] = {0x95, 0x33, 0x16, 0xDE, 0xAD, 0xBE, 0xEF};

  const bridge::Identification a = bridge::identify(mc, sizeof(mc), 0x12, true);
  Serial.print("  MC frame -> ");
  Serial.print(bridge::protocolTag(a.protocol));
  Serial.print("   (");
  Serial.print(a.reason);
  Serial.println(")");

  const bridge::Identification b = bridge::identify(mt, sizeof(mt), 0x2B, true);
  Serial.print("  MT frame -> ");
  Serial.print(bridge::protocolTag(b.protocol));
  Serial.print("   (");
  Serial.print(b.reason);
  Serial.println(")");

  // The case that matters most in the field: a private Meshtastic channel arrives
  // wearing a sync word this build has never seen, and only the plaintext
  // MeshHeader magic saves it.
  const bridge::Identification c = bridge::identify(mt, sizeof(mt), 0x77, true);
  Serial.print("  private MT -> ");
  Serial.print(bridge::protocolTag(c.protocol));
  Serial.print("   (");
  Serial.print(c.reason);
  Serial.println(")");

  const std::uint8_t foreign[] = {0xC0, 0xFF, 0xEE};
  const bridge::Identification d = bridge::identify(foreign, sizeof(foreign), 0x55, true);
  Serial.print("  foreign LoRa -> ");
  Serial.print(bridge::protocolTag(d.protocol));
  Serial.print("   (");
  Serial.print(d.reason);
  Serial.println(")");
}

void reportPanel(const bridge::PanelFrame& panel) {
  Serial.println();
  Serial.println("Panel");
  for (std::uint8_t i = 0; i < panel.lineCount; ++i) {
    Serial.print("  |");
    Serial.print(panel.line[i].text);
    Serial.println("|");
  }
  const char* alert = panel.alert();
  if (alert[0] != '\0') {
    Serial.print("  alert: ");
    Serial.println(alert);
  }
}

}  // namespace

bridge::PlanReport g_plan;
bridge::RadioPlanResult g_radio;
bridge::SlotTable g_table;
bridge::DeviceState g_state;
bridge::AirtimeGovernor g_airtime{region()};
bridge::Statistics g_stats;

void setup() {
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) {
  }

  Serial.println();
  Serial.println("lora-multiboot");
  Serial.print("region ");
  Serial.print(BRIDGE_REGION);
  Serial.println("  -- radio not yet brought up, nothing is transmitted");
  rule();

  bridge::TableReport parseReport;
  g_table = bridge::SlotTable::parse(std::string(kCompiledLayout), kFlashSize, &parseReport);

  // A blank board has nothing provisioned, so this reports the zero-boot view:
  // one connection, offering the first slot as though it were the only option.
  g_state.boot.bootSlot = 0xFF;
  const std::uint8_t provisioned = 0;
  const bridge::ProvisioningView view =
      bridge::provisionView(provisioned, bridge::Transport::Usb, bridge::maxSlotsForFlash(kFlashSize));
  for (const bridge::Endpoint& e : view.endpoints) {
    Serial.print("provisioning: ");
    Serial.print(e.label);
    Serial.print("  ");
    Serial.print(e.framework[0] != '\0' ? e.framework : "(reserved)");
    Serial.println("");
  }

  g_plan = bridge::resolvePlan(region(), FREQUENCY_MHZ);
  g_radio = bridge::resolveRadioConfig(g_plan.plan, region(), TX_POWER_DBM);

  reportPlan(g_plan);
  rule();
  reportRadio(g_radio);
  rule();
  reportFlash(g_table);
  rule();
  reportSlots(g_table, g_state);
  rule();
  reportRoles();
  rule();
  reportInventory(g_table, g_state);
  rule();
  // Every installed framework brings its own TX profile; the master RX settings are
  // reconciled from all of them so the board can hear every mesh it claims to serve.
  reportMasterRx(std::vector<bridge::TxProfile>());
  rule();
  reportIdentifier();
  rule();

  bridge::PanelInputs in;
  in.plan = &g_plan;
  in.radio = &g_radio;
  in.stats = &g_stats;
  in.airtime = &g_airtime;
  in.provisioning = &view;
  in.table = &g_table;
  in.device = &g_state;
  in.nowMs = millis();
  reportPanel(bridge::renderPanel(in));

  rule();
  Serial.println("This build transmits nothing. See docs/ARCHITECTURE.md for what");
  Serial.println("is done and what is not.");

  // Deliberately no LoRa initialisation here yet. Bringing the radio up before the
  // plan above is known-coherent is how a node ends up keyed into one mesh while
  // its operator believes it serves two.
}

void loop() {
  // Refresh the panel periodically so the airtime figure moves. The radio path
  // will replace this with real frame accounting.
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 2000) return;
  last = now;

  bridge::PanelInputs in;
  in.plan = &g_plan;
  in.radio = &g_radio;
  in.stats = &g_stats;
  in.airtime = &g_airtime;
  in.table = &g_table;
  in.device = &g_state;
  in.nowMs = now;
  reportPanel(bridge::renderPanel(in));
}