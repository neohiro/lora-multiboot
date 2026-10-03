// SPDX-License-Identifier: MIT
//
// ProtocolId tests.
//
// The cases that matter are not the happy path. They are the frames that arrive
// wearing a sync word this table has never seen -- which is every private
// Meshtastic channel, since those derive a per-channel sync word -- and the
// frames that arrive with no usable sync byte at all, which is what a radio in
// promiscuous capture mode does.

#include "bridge/ProtocolId.hpp"
#include "harness.hpp"

using namespace bridge;

namespace {

// Meshtastic leaves its MeshHeader magic in the clear; only what follows is
// encrypted. A realistic public-channel capture starts like this.
constexpr std::uint8_t kMeshHeader[] = {0x95, 0x33, 0x16, 0xDE, 0xAD, 0xBE, 0xEF,
                                        0x01, 0x02, 0x03, 0x04, 0x08};

// A MeshCore GRP_TXT on its public channel. The first byte is the destination
// / channel-hash byte, then type and flags, then ciphertext. Deliberately no
// MeshHeader magic, because a MeshCore frame must never be mistaken for a
// Meshtastic one.
constexpr std::uint8_t kMeshCorePkt[] = {0x11, 0x01, 0x03, 0x7A, 0x9C, 0x2E, 0x51,
                                         0x84, 0x33, 0xAF, 0x10, 0x62};

}  // namespace

void suite_protocol_id() {
  harness::suite("ProtocolId");

  // --- the one-byte fast path ------------------------------------------------

  {
    const Identification id =
        identify(kMeshCorePkt, sizeof(kMeshCorePkt), 0x12, true);
    CHECK_EQ(static_cast<int>(id.protocol), static_cast<int>(Protocol::MeshCore));
    CHECK_EQ(static_cast<int>(id.evidence), static_cast<int>(Evidence::SyncWord));
    CHECK(!id.corroborated);
    CHECK(!id.syncWordIsWildcard);
    CHECK_EQ(id.syncWord, 0x12);
  }

  CHECK_EQ(static_cast<int>(fromSyncWord(0x12)), static_cast<int>(Protocol::MeshCore));
  CHECK_EQ(static_cast<int>(fromSyncWord(0x2B)), static_cast<int>(Protocol::Meshtastic));
  CHECK_EQ(static_cast<int>(fromSyncWord(0x42)), static_cast<int>(Protocol::Reticulum));
  CHECK_EQ(static_cast<int>(fromSyncWord(0x34)), static_cast<int>(Protocol::LoRaWan));

  // --- two signals agreeing -------------------------------------------------

  {
    const Identification id =
        identify(kMeshHeader, sizeof(kMeshHeader), 0x2B, true);
    CHECK_EQ(static_cast<int>(id.protocol), static_cast<int>(Protocol::Meshtastic));
    CHECK_MSG(id.corroborated, "sync 0x2B and MeshHeader magic agree");
    CHECK_EQ(static_cast<int>(id.evidence), static_cast<int>(Evidence::MeshHeaderMagic));
  }

  // --- private Meshtastic: an unseen sync word, rescued by the magic ---------
  //
  // This is the case that justifies the fallback existing. A private channel
  // derives its own sync word, so a sync-first-only design would drop every
  // private-channel frame on the floor.
  {
    const Identification id =
        identify(kMeshHeader, sizeof(kMeshHeader), 0x77, true);
    CHECK_EQ(static_cast<int>(id.protocol), static_cast<int>(Protocol::Meshtastic));
    CHECK_EQ(static_cast<int>(id.evidence), static_cast<int>(Evidence::MeshHeaderMagic));
    CHECK(!id.corroborated);
    CHECK_EQ(static_cast<int>(fromSyncWord(0x77)), static_cast<int>(Protocol::Unknown));
  }

  // --- no usable sync byte at all (promiscuous capture) ---------------------

  {
    const Identification id =
        identify(kMeshHeader, sizeof(kMeshHeader), 0x00, true);
    CHECK_EQ(static_cast<int>(id.protocol), static_cast<int>(Protocol::Meshtastic));
    CHECK_MSG(id.syncWordIsWildcard, "0x00 is recorded as a wildcard");
    CHECK_EQ(static_cast<int>(id.evidence), static_cast<int>(Evidence::MeshHeaderMagic));
  }

  {
    const Identification id =
        identify(kMeshCorePkt, sizeof(kMeshCorePkt), 0x00, true);
    // A wildcard must never be allowed to vote, so a short MeshCore frame with
    // no magic is honestly Unknown rather than a guess.
    CHECK_EQ(static_cast<int>(id.protocol), static_cast<int>(Protocol::Unknown));
    CHECK_EQ(static_cast<int>(id.evidence), static_cast<int>(Evidence::None));
  }

  // --- contradiction: in-band magic outranks the preamble -------------------
  {
    const Identification id =
        identify(kMeshHeader, sizeof(kMeshHeader), 0x12, true);
    CHECK_MSG(id.protocol == Protocol::Meshtastic, "magic beats a sync word that says MeshCore");
    CHECK(!id.corroborated);
  }

  // --- unknown frames are left alone ----------------------------------------
  //
  // Somebody else's LoRa on a shared band is far more likely than ours. Guessing
  // here is how a bridge starts eating foreign traffic and getting the operator
  // shouted at by an unrelated community.

  {
    const std::uint8_t foreign[] = {0xC0, 0xFF, 0xEE, 0x11, 0x22};
    const Identification id = identify(foreign, sizeof(foreign), 0x55, true);
    CHECK_EQ(static_cast<int>(id.protocol), static_cast<int>(Protocol::Unknown));
    CHECK_EQ(static_cast<int>(id.evidence), static_cast<int>(Evidence::None));
  }

  // --- degenerate inputs -----------------------------------------------------

  {
    const Identification none = identify(nullptr, 0, 0x12, true);
    CHECK_EQ(static_cast<int>(none.protocol), static_cast<int>(Protocol::MeshCore));

    const Identification empty = identify(kMeshHeader, 0, 0x00, false);
    CHECK_EQ(static_cast<int>(empty.protocol), static_cast<int>(Protocol::Unknown));
  }

  {
    // Shorter than the magic: must not read past the end.
    const std::uint8_t tiny[] = {0x95, 0x33};
    CHECK(!hasMeshHeaderMagic(tiny, sizeof(tiny)));
    CHECK(!hasMeshHeaderMagic(nullptr, 99));
    const Identification id = identify(tiny, sizeof(tiny), 0x00, true);
    CHECK_EQ(static_cast<int>(id.protocol), static_cast<int>(Protocol::Unknown));
  }

  // --- display tags ----------------------------------------------------------

  CHECK(std::string(protocolTag(Protocol::MeshCore)) == "MC");
  CHECK(std::string(protocolTag(Protocol::Meshtastic)) == "MT");
  CHECK(std::string(protocolTag(Protocol::Unknown)) == "??");
  CHECK(std::string(protocolName(Protocol::Reticulum)) == "Reticulum");

  // Every outcome has to be explainable on a 128x64 OLED, so a reason string is
  // part of the contract rather than a debug extra.
  {
    const Identification mc = identify(kMeshCorePkt, sizeof(kMeshCorePkt), 0x12, true);
    const std::uint8_t foreign[] = {0xC0, 0xFF};
    const Identification other = identify(foreign, sizeof(foreign), 0x55, true);
    CHECK(mc.reason != nullptr && mc.reason[0] != '\0');
    CHECK(other.reason != nullptr && other.reason[0] != '\0');
  }
}
