// SPDX-License-Identifier: MIT
//
// ProtocolId -- "one small identifier, read once".
//
// Meshtastic and MeshCore share one RF plan on the Heltec V4's single SX1262
// (see ChannelPlan.hpp). What separates a frame on the air is the LoRa sync
// word: 0x2B for Meshtastic, 0x12 for MeshCore. That is a one-byte read taken
// from the preamble, before any decryption, key lookup or parsing happens.
//
// The rule this module exists to enforce: decide which mesh a frame belongs to
// using the cheapest signal available, and never make the decision depend on
// something the radio has already thrown away.

#pragma once

#include <cstddef>
#include <cstdint>

namespace bridge {

// Protocol slots this bridge knows how to name. Slots are reserved, not
// implemented: Reticulum is here so the identifier table has a stable shape
// for the next framework and adding one is a table entry, not a refactor.
enum class Protocol : std::uint8_t {
  Unknown = 0,
  MeshCore,
  Meshtastic,
  Reticulum,
  LoRaWan,
  Custom,
};

// Which signal actually produced the answer. Reported on the OLED and over the
// CLI, because "why did this frame get labelled MeshCore" is the first question
// anyone asks when a mesh misbehaves.
enum class Evidence : std::uint8_t {
  None = 0,
  SyncWord,         // LoRa PHY preamble discriminator -- the fast path
  MeshHeaderMagic,  // plaintext 0x95 0x33 0x16 MeshHeader
};

// Meshtastic leaves its MeshHeader magic in the clear; only the payload that
// follows is encrypted. Three bytes, unambiguous, and readable even when the
// radio gave us no sync word at all.
inline constexpr std::uint8_t kMeshHeaderMagic0 = 0x95;
inline constexpr std::uint8_t kMeshHeaderMagic1 = 0x33;
inline constexpr std::uint8_t kMeshHeaderMagic2 = 0x16;

struct Identification {
  Protocol protocol = Protocol::Unknown;
  Evidence evidence = Evidence::None;
  std::uint8_t syncWord = 0x00;

  // True when a second, independent signal agreed with the first. A single
  // sync-word hit is a guess the radio made for us; two agreeing signals is
  // evidence. Forwarding is allowed on one, but the operator can be told when
  // it was only ever one.
  bool corroborated = false;

  // True when the radio handed us a wildcard rather than a real sync byte --
  // promiscuous capture, or a permissive sync mask. A wildcard carries no
  // information, so it must never be allowed to vote.
  bool syncWordIsWildcard = false;

  // Short human-readable cause, for the status line. Never longer than the
  // OLED can show.
  const char* reason = "unidentified";
};

// Cheap, allocation-free protocol tag. Kept as free functions rather than a
// class so the same code compiles for the ESP32-S3 target and for the host
// tests, which are the same translation units by construction.

// True when the sync byte carries no discriminating information.
bool isWildcardSync(std::uint8_t syncWord);

// Map one sync byte to a protocol. Unknown bytes map to Protocol::Unknown
// rather than defaulting to a mesh: an unrecognised frame on a shared
// frequency is far more likely to be somebody else's LoRa than ours, and
// guessing here is how a bridge starts eating foreign traffic.
Protocol fromSyncWord(std::uint8_t syncWord);

// Read the plaintext MeshHeader magic, if present.
bool hasMeshHeaderMagic(const std::uint8_t* payload, std::size_t length);

// Full decision. `hasSyncWord` is false when the radio could not report one
// (stripped in packet mode, or unsupported by the RadioLib path in use).
//
// Precedence is deliberate:
//   1. A real sync byte wins. It is the cheapest signal and the only one
//      available for frames too short to carry a MeshHeader.
//   2. Otherwise fall back to the MeshHeader magic.
//   3. Otherwise Unknown.
//
// A sync word that names one mesh but is contradicted by a MeshHeader magic
// naming the other is treated as a corrupted capture: the magic wins, because
// it is in-band and was read from the same buffer as the payload.
Identification identify(const std::uint8_t* payload,
                        std::size_t length,
                        std::uint8_t syncWord,
                        bool hasSyncWord);

// Fixed-width label ("MC", "MT", "RT", "LW", "??") for the OLED and CLI.
const char* protocolTag(Protocol protocol);

// Full name, for logs and the CLI.
const char* protocolName(Protocol protocol);

}  // namespace bridge
