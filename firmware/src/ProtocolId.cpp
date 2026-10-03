// SPDX-License-Identifier: MIT

#include "bridge/ProtocolId.hpp"

namespace bridge {
namespace {

// Reported by the radio when promiscuous capture is on and no real preamble
// byte was matched. Carries no information, so it is never allowed to vote.
constexpr std::uint8_t kWildcardSync = 0x00;

struct SyncEntry {
  std::uint8_t sync;
  Protocol protocol;
};

// Reserved protocol table. Adding a framework is a row here plus a protocol
// enumerator, which is the whole point of keeping the slots enumerated.
constexpr SyncEntry kSyncTable[] = {
    {0x12, Protocol::MeshCore},    // MeshCore default channel
    {0x2B, Protocol::Meshtastic},  // Meshtastic default (LongFast / public)
    {0x42, Protocol::Reticulum},   // Reticulum / RNode
    {0x34, Protocol::LoRaWan},     // LoRaWAN public sync word
};

constexpr std::size_t kSyncTableSize = sizeof(kSyncTable) / sizeof(kSyncTable[0]);

}  // namespace

bool isWildcardSync(std::uint8_t syncWord) { return syncWord == kWildcardSync; }

Protocol fromSyncWord(std::uint8_t syncWord) {
  if (isWildcardSync(syncWord)) return Protocol::Unknown;
  for (std::size_t i = 0; i < kSyncTableSize; ++i) {
    if (kSyncTable[i].sync == syncWord) return kSyncTable[i].protocol;
  }
  return Protocol::Unknown;
}

bool hasMeshHeaderMagic(const std::uint8_t* payload, std::size_t length) {
  if (payload == nullptr || length < 3) return false;
  return payload[0] == kMeshHeaderMagic0 && payload[1] == kMeshHeaderMagic1 &&
         payload[2] == kMeshHeaderMagic2;
}

Identification identify(const std::uint8_t* payload,
                        std::size_t length,
                        std::uint8_t syncWord,
                        bool hasSyncWord) {
  Identification id;
  id.syncWord = syncWord;

  // The radio may hand us a wildcard rather than a real byte. Record that
  // plainly, then treat the byte as absent so it cannot contribute evidence.
  if (hasSyncWord && isWildcardSync(syncWord)) {
    id.syncWordIsWildcard = true;
    hasSyncWord = false;
  }

  const Protocol fromSync = hasSyncWord ? fromSyncWord(syncWord) : Protocol::Unknown;
  const bool magic = hasMeshHeaderMagic(payload, length);

  // Both signals present: they must agree. Disagreement means a corrupted
  // capture, and the in-band magic read from the same buffer is the stronger
  // witness, so it wins.
  if (magic && fromSync != Protocol::Unknown) {
    id.protocol = Protocol::Meshtastic;
    id.evidence = Evidence::MeshHeaderMagic;
    id.corroborated = (fromSync == Protocol::Meshtastic);
    id.reason = id.corroborated ? "sync+magic agree" : "magic overrides sync";
    return id;
  }

  if (magic) {
    // No usable sync byte. This is the path that keeps *private* Meshtastic
    // channels working: they derive a per-channel sync word, so a private frame
    // arrives wearing a byte this table has never seen.
    id.protocol = Protocol::Meshtastic;
    id.evidence = Evidence::MeshHeaderMagic;
    id.reason = "magic only (private channel?)";
    return id;
  }

  if (fromSync != Protocol::Unknown) {
    id.protocol = fromSync;
    id.evidence = Evidence::SyncWord;
    id.reason = "sync word";
    return id;
  }

  id.protocol = Protocol::Unknown;
  id.evidence = Evidence::None;
  id.reason = hasSyncWord ? "unknown sync word" : "no sync word, no magic";
  return id;
}

const char* protocolTag(Protocol protocol) {
  switch (protocol) {
    case Protocol::MeshCore:
      return "MC";
    case Protocol::Meshtastic:
      return "MT";
    case Protocol::Reticulum:
      return "RT";
    case Protocol::LoRaWan:
      return "LW";
    case Protocol::Custom:
      return "CX";
    case Protocol::Unknown:
    default:
      return "??";
  }
}

const char* protocolName(Protocol protocol) {
  switch (protocol) {
    case Protocol::MeshCore:
      return "MeshCore";
    case Protocol::Meshtastic:
      return "Meshtastic";
    case Protocol::Reticulum:
      return "Reticulum";
    case Protocol::LoRaWan:
      return "LoRaWAN";
    case Protocol::Custom:
      return "Custom";
    case Protocol::Unknown:
    default:
      return "Unknown";
  }
}

}  // namespace bridge
