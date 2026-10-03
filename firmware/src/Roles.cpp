// SPDX-License-Identifier: MIT

#include "bridge/Roles.hpp"

namespace bridge {
namespace {

// The role table. This is the single place that knows what a slot can be, so the
// menus, the CLI, the panel and the documentation cannot disagree about it.
//
// Ordering is deliberate: MeshCore first because this firmware derives from it and
// because slot 0 defaults to it.
constexpr RoleProfile kProfiles[] = {
    // MeshCore. Every one of these needs the radio exclusively: even a companion
    // carries traffic over LoRa as well as BLE, which is the point of it.
    {Framework::MeshCore, Role::Repeater, "MC repeater", RadioDemand::Exclusive,
     IdentityDomain::MeshCoreNode, true, false},
    {Framework::MeshCore, Role::Companion, "MC companion", RadioDemand::Exclusive,
     IdentityDomain::MeshCoreNode, true, false},
    {Framework::MeshCore, Role::RoomServer, "MC roomserver", RadioDemand::Exclusive,
     IdentityDomain::MeshCoreNode, true, false},
    {Framework::MeshCore, Role::RoomClient, "MC roomclient", RadioDemand::Exclusive,
     IdentityDomain::MeshCoreNode, true, false},
    {Framework::MeshCore, Role::Sensor, "MC sensor", RadioDemand::Exclusive,
     IdentityDomain::MeshCoreNode, true, false},

    // Meshtastic.
    {Framework::Meshtastic, Role::Router, "MT router", RadioDemand::Exclusive,
     IdentityDomain::MeshtasticNode, true, false},
    {Framework::Meshtastic, Role::Repeater, "MT repeater", RadioDemand::Exclusive,
     IdentityDomain::MeshtasticNode, true, false},
    {Framework::Meshtastic, Role::Client, "MT client", RadioDemand::Exclusive,
     IdentityDomain::MeshtasticNode, true, false},
    {Framework::Meshtastic, Role::Tracker, "MT tracker", RadioDemand::Exclusive,
     IdentityDomain::MeshtasticNode, true, false},
    {Framework::Meshtastic, Role::Sensor, "MT sensor", RadioDemand::Exclusive,
     IdentityDomain::MeshtasticNode, true, false},

    // The one role on this board that serves two meshes from one radio. It carries
    // both protocol stacks in one image, so it needs no second slot and no second
    // antenna.
    {Framework::Bridge, Role::Repeater, "BR both-meshes", RadioDemand::Exclusive,
     IdentityDomain::MeshCoreNode, true, true},

    // neohiro's analyser. Needs the receiver promiscuous, which is precisely what a
    // repeater must not have.
    {Framework::Sniffer, Role::Analyzer, "SN analyzer", RadioDemand::Exclusive,
     IdentityDomain::Analyzer, true, false},

    // Configuration-only. The one role that genuinely does not touch the radio, and
    // therefore the one role that genuinely could coexist with another if the
    // architecture ever grew a second core for it.
    {Framework::Custom, Role::Config, "CX config", RadioDemand::None,
     IdentityDomain::None, false, false},
};

constexpr std::size_t kProfileCount = sizeof(kProfiles) / sizeof(kProfiles[0]);

}  // namespace

const char* frameworkName(Framework framework) {
  switch (framework) {
    case Framework::MeshCore:
      return "MeshCore";
    case Framework::Meshtastic:
      return "Meshtastic";
    case Framework::Sniffer:
      return "sniffer";
    case Framework::Bridge:
      return "bridge";
    case Framework::Custom:
      return "custom";
  }
  return "?";
}

const char* roleName(Role role) {
  switch (role) {
    case Role::Repeater:
      return "repeater";
    case Role::Companion:
      return "companion";
    case Role::RoomServer:
      return "room server";
    case Role::RoomClient:
      return "room client";
    case Role::Router:
      return "router";
    case Role::Client:
      return "client";
    case Role::Sensor:
      return "sensor";
    case Role::Tracker:
      return "tracker";
    case Role::Analyzer:
      return "analyzer";
    case Role::Config:
      return "config";
  }
  return "?";
}

const char* roleShortLabel(Framework framework, Role role) {
  RoleProfile p;
  if (roleProfile(framework, role, &p)) return p.label;
  return "??";
}

bool roleProfile(Framework framework, Role role, RoleProfile* out) {
  for (std::size_t i = 0; i < kProfileCount; ++i) {
    if (kProfiles[i].framework == framework && kProfiles[i].role == role) {
      if (out != nullptr) *out = kProfiles[i];
      return true;
    }
  }
  return false;
}

const RoleProfile* allProfiles(std::size_t* count) {
  if (count != nullptr) *count = kProfileCount;
  return kProfiles;
}

Finding classifyPair(const RoleProfile& a, const RoleProfile& b) {
  Finding f;
  f.severity = Severity::Info;
  f.code = "ok";
  f.detail = "";

  // The rule that actually bites: two slots that would present the *same* node
  // identity. Two "MC repeater" slots with the same name and key do not form a mesh
  // of two, they form one repeater that appears twice, and any path through it is
  // ambiguous. Different roles in the same framework are different nodes and are
  // perfectly fine together.
  if (a.framework == b.framework && a.role == b.role && a.identity != IdentityDomain::None &&
      a.identity == b.identity) {
    f.severity = Severity::Error;
    f.code = "duplicate-identity";
    f.detail = "two slots would advertise the same node identity";
    return f;
  }

  // An analyser beside a relaying role is a trap rather than a fault: both are
  // reasonable on their own, but only one can be active and the other looks broken.
  const bool analyser = (a.identity == IdentityDomain::Analyzer) ||
                        (b.identity == IdentityDomain::Analyzer);
  const bool relay = (a.identity == IdentityDomain::MeshCoreNode) ||
                     (b.identity == IdentityDomain::MeshCoreNode) ||
                     (a.identity == IdentityDomain::MeshtasticNode) ||
                     (b.identity == IdentityDomain::MeshtasticNode);
  if (analyser && relay) {
    f.severity = Severity::Advisory;
    f.code = "analyser-excludes-relay";
    f.detail = "an analyser needs promiscuous RX, so it silences any relay slot";
    return f;
  }

  // One radio, one active role. Always true on this board and stated so nobody has
  // to rediscover it, but advisory rather than an error because it is inherent to
  // the hardware rather than to the layout.
  if (a.radio == RadioDemand::Exclusive || b.radio == RadioDemand::Exclusive) {
    const bool aIdle = a.radio == RadioDemand::None;
    const bool bIdle = b.radio == RadioDemand::None;
    if (!aIdle && !bIdle) {
      f.severity = Severity::Advisory;
      f.code = "radio-exclusive";
      f.detail = "only one slot can use the SX1262 at a time";
      return f;
    }
  }

  return f;
}

std::vector<Finding> auditBoard(const BoardPlan& plan) {
  std::vector<Finding> out;

  // Collect the present slots once, so the pairwise pass is simple and each slot is
  // named in the findings.
  struct Live {
    std::uint8_t index;
    RoleProfile profile;
  };
  std::vector<Live> live;
  for (std::size_t i = 0; i < plan.slots.size(); ++i) {
    if (!plan.slots[i].present) continue;
    Live l;
    l.index = static_cast<std::uint8_t>(i);
    l.profile = plan.slots[i].profile;
    live.push_back(l);
  }

  for (std::size_t i = 0; i < live.size(); ++i) {
    for (std::size_t j = i + 1; j < live.size(); ++j) {
      Finding f = classifyPair(live[i].profile, live[j].profile);
      if (f.severity == Severity::Info) continue;
      f.slotA = live[i].index;
      f.slotB = live[j].index;
      out.push_back(f);
    }
  }

  // Worst first, so a caller that shows only the first finding shows the important
  // one. Insertion sort over a handful of entries: simpler than pulling in an
  // algorithm for this.
  for (std::size_t i = 1; i < out.size(); ++i) {
    const Finding key = out[i];
    std::size_t k = i;
    while (k > 0 && out[k - 1].severity < key.severity) {
      out[k] = out[k - 1];
      --k;
    }
    out[k] = key;
  }
  return out;
}

}  // namespace bridge