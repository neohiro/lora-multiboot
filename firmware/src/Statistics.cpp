// SPDX-License-Identifier: MIT

#include "bridge/Statistics.hpp"

namespace bridge {

std::size_t Statistics::indexOf(Protocol protocol) {
  switch (protocol) {
    case Protocol::MeshCore:
      return 1;
    case Protocol::Meshtastic:
      return 2;
    case Protocol::Reticulum:
      return 3;
    case Protocol::LoRaWan:
      return 4;
    case Protocol::Custom:
      return 5;
    case Protocol::Unknown:
    default:
      return 0;
  }
}

std::uint32_t Statistics::saturate(std::uint32_t v) {
  // Wrapping a "frames heard" counter to zero after 2^32 would be worse than not
  // having it, and unsigned overflow is undefined behaviour, so the increment is
  // handled explicitly instead of being left to the compiler.
  return v == 0xFFFFFFFFu ? v : v + 1u;
}

void Statistics::onFrame(Protocol protocol, bool crcOk, std::int16_t rssi, std::int8_t snr,
                         std::uint32_t nowMs) {
  if (!crcOk) {
    // Counted as a CRC error only, and deliberately NOT also as "unidentified".
    //
    // Those are two different things an operator acts on differently. A rising
    // crcErrors is an antenna or channel problem: our own frames are arriving
    // damaged. A rising unidentified means somebody else's LoRa, which is normal on
    // a shared band and not a fault. Counting a corrupt frame as both hides the
    // first behind the second, so the diagnostic gets lost in the noise.
    ++stats_[0].crcErrors;
    return;
  }

  if (protocol == Protocol::Unknown) {
    // Counted once, by the same helper used for corrupt frames.
    onUnidentified();
    return;
  }

  ProtocolStats& s = stats_[indexOf(protocol)];
  s.received = saturate(s.received);
  s.lastRssi = rssi;
  s.lastSnr = snr;
  s.lastSeenMs = nowMs;
  s.seen = true;
  anyHeard_ = true;
  lastHeardMs_ = nowMs;
  lastHeard_ = protocol;
}

void Statistics::onTransmit(Protocol protocol, std::uint32_t nowMs) {
  if (protocol == Protocol::Unknown) return;
  ProtocolStats& s = stats_[indexOf(protocol)];
  s.transmitted = saturate(s.transmitted);
  s.lastSeenMs = nowMs;
  s.seen = true;
  anyHeard_ = true;
  lastHeardMs_ = nowMs;
  lastHeard_ = protocol;
}

void Statistics::onSuppressed(Protocol protocol) {
  if (protocol == Protocol::Unknown) {
    stats_[0].suppressed = saturate(stats_[0].suppressed);
    return;
  }
  ProtocolStats& s = stats_[indexOf(protocol)];
  s.suppressed = saturate(s.suppressed);
}

void Statistics::onChannelBusy(Protocol protocol) {
  if (protocol == Protocol::Unknown) return;
  channelBusy_[indexOf(protocol)] = saturate(channelBusy_[indexOf(protocol)]);
}

void Statistics::onUnidentified() { stats_[0].unidentified = saturate(stats_[0].unidentified); }

const ProtocolStats& Statistics::forProtocol(Protocol protocol) const {
  return stats_[indexOf(protocol)];
}

std::uint32_t Statistics::totalReceived() const {
  std::uint32_t total = 0;
  for (std::size_t i = 1; i < 5; ++i) {
    // Saturating rather than wrapping: a total that has wrapped is a lie.
    total = (total > 0xFFFFFFFFu - stats_[i].received) ? 0xFFFFFFFFu
                                                        : total + stats_[i].received;
  }
  return total;
}

std::uint32_t Statistics::totalTransmitted() const {
  std::uint32_t total = 0;
  for (std::size_t i = 1; i < 5; ++i) {
    total = (total > 0xFFFFFFFFu - stats_[i].transmitted) ? 0xFFFFFFFFu
                                                          : total + stats_[i].transmitted;
  }
  return total;
}

std::uint32_t Statistics::silenceMs(std::uint32_t nowMs) const {
  if (!anyHeard_) return 0;
  // Unsigned subtraction, which is correct across the 49-day millisecond rollover.
  return nowMs - lastHeardMs_;
}

Protocol Statistics::lastHeard() const { return lastHeard_; }

void Statistics::reset() {
  for (std::size_t i = 0; i < 5; ++i) stats_[i] = ProtocolStats();
  for (std::size_t i = 0; i < 5; ++i) channelBusy_[i] = 0;
  anyHeard_ = false;
  lastHeardMs_ = 0;
  lastHeard_ = Protocol::Unknown;
}

}  // namespace bridge