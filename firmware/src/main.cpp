// SPDX-License-Identifier: MIT
//
// On-device bring-up, stage one.
//
// Deliberately dependency-free: this file and the three modules beside it
// compile with nothing but a C++17 compiler, which is the same property the host
// test gate relies on. The radio layer is *not* here yet, and this file does not
// pretend otherwise -- see the note at the bottom of setup().
//
// What it does do is the part worth doing before any RF is transmitted: prove
// out loud that the node's plan and its flash layout are coherent. Both checks
// below have caught real, silent, field-only failures, and both are far cheaper
// to find at boot than on a rooftop.

#include <Arduino.h>

#include <string>

#include "bridge/ChannelPlan.hpp"
#include "bridge/ProtocolId.hpp"
#include "bridge/SlotTable.hpp"

namespace {

// Region is a build flag. Default EU_868, because that is the one region where
// the two community defaults land on the same carrier and the whole
// architecture depends on it.
#ifndef BRIDGE_REGION
#define BRIDGE_REGION "EU_868"
#endif

// Overridable at build time, e.g. -DFREQUENCY_MHZ=906.875
#ifndef FREQUENCY_MHZ
#define FREQUENCY_MHZ 0.0f
#endif

constexpr std::uint32_t kFlashSizeBytes = 16u * 1024u * 1024u;

bridge::Region regionFromCode() {
  const bridge::RegionDefaults* d = bridge::regionDefaultsByCode(BRIDGE_REGION);
  return d->region;
}

void line(const char* s) {
  Serial.println(s);
}

void reportPlan() {
  const bridge::PlanReport r = bridge::resolvePlan(regionFromCode(), FREQUENCY_MHZ);

  line("");
  line("plan:");
  Serial.print("  ");
  Serial.println(bridge::describePlan(r));
  Serial.print("  status: ");
  Serial.println(r.detail);

  if (r.status == bridge::PlanStatus::FrequencyMismatch) {
    Serial.print("  the two community defaults are ");
    Serial.print(r.driftMHz, 3);
    Serial.println(" MHz apart");
    Serial.print("  retune the meshcore side to match, or this node hears one mesh plus noise: ");
    Serial.println(r.retune);
  }
  if (r.status == bridge::PlanStatus::OutOfBand) {
    Serial.print("  ");
    Serial.println(r.driftMHz, 3);
    Serial.println("  MHz is outside this region's band. That is a configuration error.");
  }
  if (r.status == bridge::PlanStatus::MeshCoreFreqUnknown) {
    Serial.println("  frequencies coincide but the MeshCore default here is unconfirmed.");
    Serial.println("  confirm both communities agree on this channel before deploying.");
  }

  Serial.print("  airtime cap: ");
  Serial.print(bridge::dutyCycleFor(regionFromCode()));
  line("%");
}

void reportFlash() {
  // The partition table baked into this image, so a node can be asked what it
  // thinks it was flashed with instead of the operator guessing from a filename.
  static const char kCsv[] =
      "# Partition table as compiled into this image\n"
      "bootloader,     app,  factory, 0x0,      0x7000,\n"
      "partition_tbl,  data, nvs,     0x8000,   0xC000,\n"
      "otadata,        data, otadata,0x14000,  0x2000,\n"
      "nvs,            data, nvs,     0x16000,  0xA000,\n"
      "ota_0,          app,  ota_0,   0x20000,  0x400000,\n"
      "fs_meshcore,    data, spiffs,  0x420000, 0x200000,\n"
      "ota_1,          app,  ota_1,   0x620000, 0x400000,\n"
      "fs_meshtastic,  data, littlefs,0xA20000, 0x200000,\n";

  bridge::TableReport pr;
  const bridge::SlotTable t =
      bridge::SlotTable::parse(std::string(kCsv), kFlashSizeBytes, &pr);

  line("");
  line("flash:");
  Serial.print("  ");
  Serial.println(t.describe());
  Serial.print("  layout: ");
  Serial.println(pr.ok() ? pr.detail : "PARSE FAILED");

  const bridge::TableReport v = t.validate();
  Serial.print("  geometry: ");
  Serial.println(v.ok() ? "ok" : v.detail);

  const bridge::TableReport f = t.validateFrameworks();
  Serial.print("  frameworks: ");
  Serial.println(f.ok() ? "isolated" : f.detail);
}

void reportIdentifier() {
  // Prove the identifier end to end on the target, using the same code the
  // radio path will call. A MeshCore public-channel frame and a Meshtastic
  // public-channel frame, each carrying the sync word it really arrives with.
  const std::uint8_t mc[] = {0x11, 0x01, 0x03, 0x7A, 0x9C, 0x2E, 0x51};
  const std::uint8_t mt[] = {0x95, 0x33, 0x16, 0xDE, 0xAD, 0xBE, 0xEF};

  line("");
  line("identifier:");
  const bridge::Identification a = bridge::identify(mc, sizeof(mc), 0x12, true);
  Serial.print("  MC frame -> ");
  Serial.print(bridge::protocolTag(a.protocol));
  Serial.print(" (");
  Serial.print(a.reason);
  line(")");

  const bridge::Identification b = bridge::identify(mt, sizeof(mt), 0x2B, true);
  Serial.print("  MT frame -> ");
  Serial.print(bridge::protocolTag(b.protocol));
  Serial.print(" (");
  Serial.print(b.reason);
  line(")");

  // The case that matters most in the field: a private Meshtastic channel
  // arrives wearing a sync word this build has never seen, and only the
  // plaintext MeshHeader magic saves it.
  const bridge::Identification c = bridge::identify(mt, sizeof(mt), 0x77, true);
  Serial.print("  private MT -> ");
  Serial.print(bridge::protocolTag(c.protocol));
  Serial.print(" (");
  Serial.print(c.reason);
  line(")");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) {
  }

  line("");
  line("meshcore-meshtastic-heltec-v4");

  reportPlan();
  reportFlash();
  reportIdentifier();

  line("");
  line("radio: not yet brought up. See docs/ARCHITECTURE.md for what is done");
  line("and what is not. This build transmits nothing.");

  // Deliberately no LoRa initialisation here yet. Bringing the radio up before
  // the plan above is known-coherent is how a node ends up keyed into one mesh
  // while its operator believes it serves two.
}

void loop() {
  // Nothing to do until the radio path exists. Left empty rather than filled
  // with a placeholder that pretends to be forwarding.
}
