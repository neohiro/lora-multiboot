// SPDX-License-Identifier: MIT

#include "bridge/StatusPanel.hpp"

#include <cstdio>
#include <cstring>

namespace bridge {
namespace {

// Append without ever writing past the line's capacity. Every write goes through
// here, so no individual call site can be the one that overflows.
void append(PanelLine& l, const char* s) {
  std::size_t i = l.length;
  while (*s != '\0' && i < kPanelColumns) l.text[i++] = *s++;
  l.text[i] = '\0';
  l.length = i;
  l.valid = true;
}

void appendChar(PanelLine& l, char c) {
  const char buf[2] = {c, '\0'};
  append(l, buf);
}

// Append `count` spaces. A count, not a target length: padding an already
// populated line "to" a width silently does nothing once the line is longer than
// it, which is exactly how a column layout collapses without anyone noticing.
void pad(PanelLine& l, std::size_t count) {
  for (std::size_t i = 0; i < count && l.length < kPanelColumns; ++i) appendChar(l, ' ');
}

// Right-align `text` into a field of exactly `width`, truncating from the left
  // if it does not fit. Left truncation keeps the most significant end of a number
  // or a word, which is the end anybody reads.
void field(PanelLine& l, const char* text, std::size_t width) {
  const std::size_t n = std::strlen(text);
  if (n >= width) {
    append(l, text + (n - width));
  } else {
    pad(l, width - n);
    append(l, text);
  }
}

void appendUint(PanelLine& l, std::uint32_t v) {
  char buf[11];
  std::size_t n = 0;
  do {
    buf[n++] = static_cast<char>('0' + (v % 10u));
    v /= 10u;
  } while (v != 0u);
  while (n > 0) {
    const char one[2] = {buf[--n], '\0'};
    append(l, one);
  }
}

}  // namespace

void formatCount(char* out, std::size_t cap, std::uint32_t value) {
  if (out == nullptr || cap == 0) return;
  out[0] = '\0';
  if (cap < 6) return;

  // Compact form. A mesh doing thousands of frames an hour would otherwise push
  // the signal figures off the end of the line, and signal strength is what
  // tells an operator whether the antenna is still working.
  if (value >= 1000000u) {
    std::snprintf(out, cap, "%.1fM", static_cast<double>(value) / 1000000.0);
  } else if (value >= 10000u) {
    std::snprintf(out, cap, "%.0fk", static_cast<double>(value) / 1000.0);
  } else if (value >= 1000u) {
    std::snprintf(out, cap, "%.1fk", static_cast<double>(value) / 1000.0);
  } else {
    std::snprintf(out, cap, "%u", static_cast<unsigned>(value));
  }
}

void formatRssi(char* out, std::size_t cap, std::int16_t rssi) {
  if (out == nullptr || cap == 0) return;
  out[0] = '\0';
  if (cap < 5) return;
  std::snprintf(out, cap, "%d", static_cast<int>(rssi));
}

PanelLine renderPlanLine(const PanelInputs& in) {
  PanelLine l;
  if (in.plan == nullptr) {
    append(l, "plan ?");
    return l;
  }

  const char* verdict = "ok";
  switch (in.plan->status) {
    case PlanStatus::Ok:
      break;
    case PlanStatus::FrequencyMismatch:
      verdict = "MISMATCH";
      break;
    case PlanStatus::OutOfBand:
      verdict = "OFFBAND";
      break;
    case PlanStatus::MeshCoreFreqUnknown:
      verdict = "unconf";
      break;
  }

  // Frequency in millihertz, so the decimal point is placed by integer
  // arithmetic and the real value is shown rather than whatever it defaults to.
  std::uint32_t mhz1000 = 0;
  if (in.plan->plan.frequencyMHz > 0.0f) {
    mhz1000 = static_cast<std::uint32_t>(in.plan->plan.frequencyMHz * 1000.0f + 0.5f);
  }

  char freq[16];
  std::snprintf(freq, sizeof(freq), "%u.%03u", static_cast<unsigned>(mhz1000 / 1000u),
                static_cast<unsigned>(mhz1000 % 1000u));

  if (in.plan->status == PlanStatus::Ok) {
    append(l, freq);
    append(l, " SF");
    appendUint(l, in.plan->plan.spreadingFactor);
    append(l, "  ok");
  } else {
    // The verdict comes FIRST when there is one. A fixed-width line truncates the
    // right-hand end, and a node reporting "MISMATC" because the frequency ate
    // the width is worse than a node reporting nothing.
    append(l, verdict);
    appendChar(l, ' ');
    append(l, freq);
  }
  return l;
}

PanelLine renderMeshLine(const PanelInputs& in) {
  PanelLine l;
  if (in.stats == nullptr) {
    append(l, "mesh ?");
    return l;
  }

  // Both live meshes, side by side, always, in fixed 10-character fields so the
  // line cannot reflow as numbers change. A mesh that has never been heard shows
  // "--", because "not there" and "not displayed" must never look the same: a node
  // quietly serving one mesh is precisely the failure this panel exists to reveal.
  //
  //   MC |  250 | -94   MT |  250 | -88
  //      2   +  4    +  3     + 1    + 2  + 4  + 3  = 21 columns exactly
  const Protocol all[2] = {Protocol::MeshCore, Protocol::Meshtastic};
  for (std::size_t i = 0; i < 2; ++i) {
    const ProtocolStats& s = in.stats->forProtocol(all[i]);
    if (i != 0) appendChar(l, ' ');
    append(l, protocolTag(all[i]));

    char count[10];
    formatCount(count, sizeof(count), s.received);
    field(l, count, 4);

    char rssi[8];
    if (s.everSeen()) {
      formatRssi(rssi, sizeof(rssi), s.lastRssi);
    } else {
      std::snprintf(rssi, sizeof(rssi), "--");
    }
    field(l, rssi, 3);
  }
  return l;
}

PanelLine renderSlotLine(const PanelInputs& in) {
  PanelLine l;
  if (in.provisioning == nullptr) {
    append(l, "slot ?");
    return l;
  }

  if (in.provisioning->state == ProvisionState::ZeroBoot) {
    append(l, "no fw  1 conn");
    return l;
  }

  // The active slot, what is in it, and how much of the airtime budget is gone.
  std::uint8_t active = 0xFF;
  if (in.device != nullptr) active = activeSlot(*in.device);
  if (active != 0xFF) {
    append(l, "ota_");
    appendUint(l, active);
    appendChar(l, ' ');
    const char* fw = defaultFrameworkForSlot(active);
    if (fw != nullptr && fw[0] != '\0') {
      append(l, fw[0] == 'm' && fw[1] == 'e' ? "MC" : "MT");
    }
  } else {
    append(l, "no fw");
  }

  if (in.airtime != nullptr) {
    append(l, "  air");
    appendUint(l, static_cast<std::uint32_t>(in.airtime->usedFraction(in.nowMs) * 100.0f));
    appendChar(l, '%');
  }
  return l;
}

PanelFrame renderPanel(const PanelInputs& in) {
  PanelFrame f;
  f.line[0] = renderPlanLine(in);
  f.line[1] = renderMeshLine(in);
  f.line[2] = renderSlotLine(in);
  f.lineCount = 3;
  return f;
}

namespace {

bool contains(const char* haystack, const char* needle) {
  if (needle[0] == '\0') return false;
  for (const char* p = haystack; *p != '\0'; ++p) {
    const char* h = p;
    const char* n = needle;
    while (*n != '\0' && *h == *n) {
      ++h;
      ++n;
    }
    if (*n == '\0') return true;
  }
  return false;
}

}  // namespace

const char* PanelFrame::alert() const {
  // Missing data outranks everything: a panel that cannot draw is worse than one
  // reporting bad news, because it looks identical to a healthy node.
  for (std::size_t i = 0; i < lineCount; ++i) {
    if (!line[i].valid) return "no data";
  }

  // Then the RF plan, which is the failure that silently costs an entire mesh.
  for (std::size_t i = 0; i < lineCount; ++i) {
    if (contains(line[i].text, "MISMATCH") || contains(line[i].text, "OFFBAND")) {
      return "rf plan";
    }
  }

  // Then a node that has never heard anything, which usually means an antenna
  // problem rather than a mesh problem.
  if (contains(line[1].text, "--")) return "no frames";

  return "";
}

}  // namespace bridge