// SPDX-License-Identifier: MIT
//
// Roles -- what a slot can be, and which combinations of slots are actually valid.
//
// This exists because "a repeater for both" was too narrow a description of the
// hardware, and a board that can hold five slots can be asked for combinations
// that are perfectly flashable and operationally nonsense.
//
// The honest position on "simultaneously", stated once here so it does not get
// re-argued later:
//
//   **Slots are exclusive. One runs at a time, and the running slot owns the one
//   SX1262.**
//
// That is not a limitation of this implementation, it is what one radio is. Two
// images cannot both drive the transceiver at once, and every role in the table
// below needs the radio -- a "companion" carries traffic over LoRa as well as BLE,
// and a sniffer needs the receiver in promiscuous mode. So five slots mean five
// *installed, switchable* frameworks with their settings preserved, not five
// running ones.
//
// The thing that genuinely does run both meshes at once is the bridge firmware in
// slot 0: a single image containing both protocol stacks, which is the entire point
// of the one-byte identifier in ProtocolId.hpp. Slots and the bridge are not
// alternatives; the bridge is one slot that happens to serve two meshes.
//
// What this module therefore checks is different and still worth automating:
// whether a *set* of installed slots is a sane thing to deploy. The two failure
// modes that matter are not crashes, they are silent operational faults:
//
//   - Two slots advertising the same node identity. Two "repeaters" with the same
//     name and key do not form a mesh of two; they form one repeater that appears
//     twice, and paths through it are ambiguous.
//   - A sniffer installed beside a repeater, which is fine until someone switches
//     to the sniffer and wonders why the mesh stopped relaying.
//
// Both are reported rather than silently permitted, because both are discovered in
// production otherwise.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bridge/SlotTable.hpp"

namespace bridge {

// Who provides the firmware in a slot.
enum class Framework : std::uint8_t {
  MeshCore = 0,
  Meshtastic,
  Sniffer,   // neohiro's own LoRa analyser
  Bridge,    // this project's shared-radio image: both meshes, one radio
  Custom,
};

// What the firmware does.
enum class Role : std::uint8_t {
  Repeater = 0,
  Companion,
  RoomServer,
  RoomClient,
  Router,
  Client,
  Sensor,
  Tracker,
  Analyzer,   // sniffer
  Config,     // configuration-only, no mesh traffic
};

// How hard the role needs the radio.
enum class RadioDemand : std::uint8_t {
  Exclusive = 0,  // cannot share a radio with another running role
  None,           // never touches the transceiver
};

// Whether this role holds a node identity, and in which namespace. Two roles in the
// same framework and identity namespace that are the *same role* would collide;
// different roles in the same namespace are different nodes and are fine.
enum class IdentityDomain : std::uint8_t {
  None = 0,
  MeshCoreNode,
  MeshtasticNode,
  Analyzer,
};

const char* frameworkName(Framework framework);
const char* roleName(Role role);
const char* roleShortLabel(Framework framework, Role role);

// A slot's purpose.
struct RoleProfile {
  Framework framework = Framework::MeshCore;
  Role role = Role::Repeater;

  const char* label = "";    // "MC repeater", for the panel and the CLI
  RadioDemand radio = RadioDemand::Exclusive;
  IdentityDomain identity = IdentityDomain::MeshCoreNode;
  bool needsAntenna = true;

  // The bridge image is the one role that serves two meshes from one radio, because
  // it carries both protocol stacks. Nothing else on this board does.
  bool servesTwoMeshes = false;
};

// Look a role up. Returns false when the combination is not one this build knows,
// which is treated as a configuration error rather than defaulted.
bool roleProfile(Framework framework, Role role, RoleProfile* out);

// Every profile this build knows, for menus and documentation.
const RoleProfile* allProfiles(std::size_t* count);

// How serious a finding is.
enum class Severity : std::uint8_t {
  Info = 0,
  Advisory,
  Error,  // refused
};

// One problem with a proposed board layout.
struct Finding {
  Severity severity = Severity::Info;
  const char* code = "";
  const char* detail = "";
  std::uint8_t slotA = 0xFF;
  std::uint8_t slotB = 0xFF;
};

// Audit a proposed set of slots.
//
// `slots[i]` is the purpose of slot i; entries with framework Custom and role
// Config, or marked absent via `present`, are ignored.
struct BoardPlan {
  struct Entry {
    bool present = false;
    RoleProfile profile;
  };
  std::vector<Entry> slots;
};

// Validate a board. Returns every finding, worst first.
//
// Deliberately does not mutate or refuse: the caller decides. A board with two
// identical repeaters is a thing an operator may genuinely want during a migration,
// and the tool's job is to say "these two will collide" rather than to forbid it.
std::vector<Finding> auditBoard(const BoardPlan& plan);

// Convenience: is this pair a problem at all?
Finding classifyPair(const RoleProfile& a, const RoleProfile& b);

}  // namespace bridge