// SPDX-License-Identifier: MIT
//
// Statistics -- per-protocol counters, so "both meshes are alive" is a fact on
// the display rather than an assumption.
//
// The reason this exists in the same breath as the identifier is that the
// identifier's failure mode is *silent*. A radio locked to the wrong sync word,
// or a plan whose carriers do not coincide, produces a node that repeats happily
// while serving one mesh. Counters per protocol are the cheapest possible way to
// make that visible: one number per mesh, watched by whoever owns the node.
//
// Counters are deliberately plain integers with saturating increments. A counter
// that wraps to a small number after 2^32 frames would be worse than useless, and
// `unsigned` overflow is undefined, so it is handled explicitly.

#pragma once

#include <cstdint>

#include "bridge/ProtocolId.hpp"

namespace bridge {

// One protocol's counters.
struct ProtocolStats {
  std::uint32_t received = 0;      // frames delivered, CRC-valid
  std::uint32_t transmitted = 0;   // frames sent
  std::uint32_t crcErrors = 0;     // heard but corrupt: exactly the shared-band noise
  std::uint32_t unidentified = 0;  // heard but unattributable, normally somebody else's LoRa
  std::uint32_t suppressed = 0;    // not repeated, by policy: duplicate, hop limit, seen before

  std::int16_t lastRssi = 0;       // dBm
  std::int8_t lastSnr = 0;         // quarter-dB, as the SX126x reports it
  std::uint32_t lastSeenMs = 0;

  // Explicit rather than inferred from lastSeenMs, because a frame at t=0 is a
  // real observation and a zero timestamp would report it as "never heard" -- so a
  // node whose first packet arrives in the first millisecond of boot looks dead.
  bool seen = false;

  bool everSeen() const { return seen; }
};

// Runtime counters across every protocol, plus the radio-wide ones.
class Statistics {
 public:
  // `nowMs` is a millisecond uptime counter. Zero is a legitimate value for the
  // first call, which is why "never seen" is tracked by an explicit flag rather
  // than by a zero timestamp.
  void onFrame(Protocol protocol, bool crcOk, std::int16_t rssi, std::int8_t snr,
               std::uint32_t nowMs);
  void onTransmit(Protocol protocol, std::uint32_t nowMs);
  void onSuppressed(Protocol protocol);
  void onChannelBusy(Protocol protocol);
  void onUnidentified();

  const ProtocolStats& forProtocol(Protocol protocol) const;
  std::uint32_t totalReceived() const;
  std::uint32_t totalTransmitted() const;

  // Milliseconds since anything at all was heard, or 0 if the air has been
  // silent since boot. This is the single most useful field when diagnosing a node
  // that "is up but not working".
  std::uint32_t silenceMs(std::uint32_t nowMs) const;

  // Protocol that has been heard most recently, or Unknown if none.
  Protocol lastHeard() const;

  void reset();

 private:
  static std::size_t indexOf(Protocol protocol);
  static std::uint32_t saturate(std::uint32_t v);

  ProtocolStats stats_[5];  // one per Protocol enumerator
  std::uint32_t channelBusy_[5] = {};
  bool anyHeard_ = false;
  std::uint32_t lastHeardMs_ = 0;
  Protocol lastHeard_ = Protocol::Unknown;
};

}  // namespace bridge