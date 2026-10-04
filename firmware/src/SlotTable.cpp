// SPDX-License-Identifier: MIT

#include "bridge/SlotTable.hpp"

#include <cstdlib>

namespace bridge {
namespace {

std::string trim(const std::string& s) {
  std::size_t a = 0;
  std::size_t b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

std::string lower(const std::string& s) {
  std::string out = s;
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out[i] >= 'A' && out[i] <= 'Z') out[i] = static_cast<char>(out[i] - 'A' + 'a');
  }
  return out;
}

std::vector<std::string> splitFields(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : line) {
    if (c == ',') {
      out.push_back(trim(cur));
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(trim(cur));
  return out;
}

PartType parseType(const std::string& s) {
  return lower(s) == "app" ? PartType::App : PartType::Data;
}

PartSubType parseSubType(const std::string& s) {
  const std::string v = lower(s);
  if (v == "factory") return PartSubType::Factory;
  if (v == "ota_0") return PartSubType::Ota_0;
  if (v == "ota_1") return PartSubType::Ota_1;
  if (v == "ota_2") return PartSubType::Ota_2;
  if (v == "ota_3") return PartSubType::Ota_3;
  if (v == "nvs") return PartSubType::Nvs;
  if (v == "otadata") return PartSubType::Otadata;
  if (v == "spiffs") return PartSubType::Spiffs;
  if (v == "littlefs" || v == "lfs") return PartSubType::LittleFs;
  if (v == "coredump") return PartSubType::Coredump;
  if (v == "reserved") return PartSubType::Reserved;

  // ESP-IDF also writes subtypes numerically, and OTA slots are the case that
  // matters here: ota_0..ota_N are subtypes 16..16+N. A table naming ota_4 is
  // therefore not inventing anything, it is just written the numeric way.
  if (!v.empty() && v[0] >= '0' && v[0] <= '9') {
    const unsigned long n = std::strtoul(v.c_str(), nullptr, 10);
    if (n >= 16ul && n <= 16ul + 15ul) {
      const std::size_t idx = static_cast<std::size_t>(n - 16ul);
      static const PartSubType kOta[] = {
          PartSubType::Ota_0, PartSubType::Ota_1, PartSubType::Ota_2,
          PartSubType::Ota_3, PartSubType::Ota_4, PartSubType::Ota_5,
          PartSubType::Ota_6, PartSubType::Ota_7,
      };
      if (idx < sizeof(kOta) / sizeof(kOta[0])) return kOta[idx];
    }
  }
  return PartSubType::Unknown;
}

std::uint32_t alignUp(std::uint32_t v, std::uint32_t a) {
  if (a == 0) return v;
  const std::uint32_t rem = v % a;
  return rem == 0 ? v : v + (a - rem);
}

}  // namespace

// The reserved framework table now lives in the header as `inline constexpr`,
// which gives every translation unit the same single instance. Defining it here
// as well would be a second, external-linkage definition and a duplicate symbol.

bool parseSize(const std::string& text, std::uint32_t* out) {
  const std::string t = trim(text);
  if (t.empty() || out == nullptr) return false;

  // Hex, with or without 0x, optionally suffixed.
  std::size_t digitsEnd = t.size();
  std::uint32_t multiplier = 1;
  const char last = t[t.size() - 1];
  if (last == 'K' || last == 'k') {
    multiplier = 1024u;
    digitsEnd = t.size() - 1;
  } else if (last == 'M' || last == 'm') {
    multiplier = 1024u * 1024u;
    digitsEnd = t.size() - 1;
  }

  std::string digits = trim(t.substr(0, digitsEnd));
  if (digits.empty()) return false;

  const bool hex = digits.size() > 2 && digits[0] == '0' &&
                   (digits[1] == 'x' || digits[1] == 'X');
  const char* begin = digits.c_str() + (hex ? 2 : 0);

  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(begin, &end, hex ? 16 : 10);
  if (end == begin || *end != '\0') return false;

  const unsigned long long total = parsed * multiplier;
  if (total > 0xFFFFFFFFull) return false;
  *out = static_cast<std::uint32_t>(total);
  return true;
}

SlotTable SlotTable::parse(const std::string& csv, std::uint32_t flashSizeBytes,
                           TableReport* report) {
  SlotTable table;
  table.flashSizeBytes_ = flashSizeBytes;

  const auto fail = [&](TableStatus st, const char* detail) {
    if (report != nullptr) {
      report->status = st;
      report->detail = detail;
    }
    return table;
  };

  std::size_t pos = 0;
  std::uint32_t cursor = 0;
  bool sawAny = false;

  while (pos <= csv.size()) {
    const std::size_t nl = csv.find('\n', pos);
    const std::string raw = csv.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? csv.size() + 1 : nl + 1;

    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') continue;

    const std::vector<std::string> f = splitFields(line);
    if (f.size() < 5) return fail(TableStatus::ParseError, "fewer than five fields");

    Partition p;
    p.label = f[0];
    if (p.label.empty()) return fail(TableStatus::ParseError, "empty label");
    p.type = parseType(f[1]);
    p.subType = parseSubType(f[2]);

    // A blank offset means "pick up where the last one stopped", aligned.
    if (f[3].empty()) {
      p.offset = alignUp(cursor, kAppAlignment);
    } else if (!parseSize(f[3], &p.offset)) {
      return fail(TableStatus::ParseError, "bad offset");
    }

    // Size blank means "to end of flash". A trailing comma with an empty *flags*
    // field is the ordinary ESP-IDF way of writing a row with no flags, and must
    // not be mistaken for a blank size -- getting that backwards silently gives
    // every partition the whole remaining chip, which disables the overlap,
    // bounds and alignment checks all at once.
    if (f[4].empty()) {
      p.size = 0;
      p.blank = true;
    } else if (!parseSize(f[4], &p.size)) {
      return fail(TableStatus::ParseError, "bad size");
    }

    if (p.size != 0) cursor = p.offset + p.size;
    table.parts_.push_back(p);
    sawAny = true;
  }

  if (!sawAny) return fail(TableStatus::ParseError, "no partitions");
  if (report != nullptr) *report = TableReport{};
  return table;
}

std::uint32_t SlotTable::highestByteUsed() const {
  std::uint32_t high = 0;
  for (const Partition& p : parts_) {
    // A blank size means "runs to the end of flash", so it claims everything above
    // its offset. Counting it as ending where it starts would report a table full of
    // free space while one partition covers the whole chip.
    const std::uint32_t end =
        p.blank ? flashSizeBytes_ : p.offset + p.size;
    if (end > high) high = end;
  }
  return high;
}

std::uint32_t SlotTable::freeBytes() const {
  const std::uint32_t high = highestByteUsed();
  return flashSizeBytes_ > high ? flashSizeBytes_ - high : 0;
}

const Partition* SlotTable::find(const std::string& label) const {
  for (const Partition& p : parts_) {
    if (p.label == label) return &p;
  }
  return nullptr;
}

const Partition* SlotTable::find(const char* label) const {
  if (label == nullptr) return nullptr;
  for (const Partition& p : parts_) {
    if (p.label == label) return &p;
  }
  return nullptr;
}

TableReport SlotTable::validate() const {
  TableReport r;

  // Bounds first: an out-of-flash partition makes every other check noise.
  for (const Partition& p : parts_) {
    if (p.blank) continue;  // by definition ends at the end of flash
    const std::uint64_t end = static_cast<std::uint64_t>(p.offset) + p.size;
    if (end > flashSizeBytes_) {
      r.status = TableStatus::ExceedsFlash;
      r.detail = "partition runs past end of flash";
      r.label = p.label;
      r.atOffset = p.offset;
      return r;
    }
  }

  // Alignment: the bootloader refuses to boot a misaligned app partition, and
  // that refusal looks exactly like a dead board.
  for (const Partition& p : parts_) {
    if (p.type != PartType::App) continue;
    if (p.blank) continue;
    if (p.offset % kAppAlignment != 0) {
      r.status = TableStatus::Misaligned;
      r.detail = "app partition not 64KB aligned";
      r.label = p.label;
      r.atOffset = p.offset;
      return r;
    }
  }

  // Overlap. Sorted copy so the comparison is a single forward sweep.
  std::vector<const Partition*> sorted;
  sorted.reserve(parts_.size());
  for (const Partition& p : parts_) sorted.push_back(&p);
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    const Partition* key = sorted[i];
    std::size_t j = i;
    while (j > 0 && sorted[j - 1]->offset > key->offset) {
      sorted[j] = sorted[j - 1];
      --j;
    }
    sorted[j] = key;
  }
  for (std::size_t i = 1; i < sorted.size(); ++i) {
    const Partition* prev = sorted[i - 1];
    const Partition* cur = sorted[i];
    // A blank size runs to the end of flash, so it overlaps anything that starts
    // after it. Skipping such a row here would let an overlapping table validate
    // cleanly -- and a validator that passes a broken table is worse than no
    // validator, because it is trusted.
    const std::uint32_t prevEnd = prev->blank ? flashSizeBytes_ : prev->offset + prev->size;
    if (cur->offset < prevEnd) {
      r.status = TableStatus::Overlap;
      r.detail = "partitions claim the same bytes";
      r.label = cur->label;
      r.atOffset = cur->offset;
      return r;
    }
  }

  r.status = TableStatus::Ok;
  r.detail = "ok";
  return r;
}

TableReport SlotTable::validateFrameworks() const {
  TableReport r;

  for (std::size_t i = 0; i < kReservedFrameworkCount; ++i) {
    const FrameworkSlot& fs = kReservedFrameworks[i];

    const Partition* app = find(fs.appLabel);
    // A framework that is absent is legitimate: under progressive provisioning a
    // one-slot board runs MeshCore only. What must never happen is a framework
    // that is present and malformed, because that is what breaks whoever fills
    // the slot in later.
    if (app != nullptr && app->type != PartType::App) {
      r.status = TableStatus::MissingSlot;
      r.detail = "framework app partition is not type app";
      r.label = fs.appLabel;
      r.atOffset = app->offset;
      return r;
    }

    // The filesystem is the part that is easy to get wrong and expensive to get
    // wrong: a shared label lets one firmware format away the other's settings.
    const Partition* fsPart = find(fs.fsLabel);
    if (fsPart != nullptr && fsPart->type != PartType::Data) {
      r.status = TableStatus::MissingSlot;
      r.detail = "framework filesystem partition is not type data";
      r.label = fs.fsLabel;
      r.atOffset = fsPart->offset;
      return r;
    }
  }

  // Belt and braces: no label may serve two frameworks.
  for (std::size_t a = 0; a < kReservedFrameworkCount; ++a) {
    for (std::size_t b = a + 1; b < kReservedFrameworkCount; ++b) {
      if (kReservedFrameworks[a].fsLabel == kReservedFrameworks[b].fsLabel) {
        r.status = TableStatus::SharedFilesystem;
        r.detail = "two frameworks share one filesystem label";
        r.label = kReservedFrameworks[a].fsLabel;
        return r;
      }
    }
  }

  r.status = TableStatus::Ok;
  r.detail = "all frameworks isolated";
  return r;
}

const char* SlotTable::describe() const {
  // Same contract as ChannelPlan::describePlan(): a static buffer, single-consumer,
  // UI-task only. Copy the result out before calling again.
  static char line[96];
  std::size_t i = 0;
  const auto put = [&](const char* s) {
    while (*s != '\0' && i + 1 < sizeof(line)) line[i++] = *s++;
  };
  const auto putU = [&](std::uint32_t v) {
    char tmp[11];
    std::size_t n = 0;
    do {
      tmp[n++] = static_cast<char>('0' + (v % 10u));
      v /= 10u;
    } while (v != 0u);
    while (n > 0 && i + 1 < sizeof(line)) line[i++] = tmp[--n];
  };

  const std::uint32_t mb = flashSizeBytes_ / (1024u * 1024u);
  const std::uint32_t usedMb = highestByteUsed() / (1024u * 1024u);
  const std::uint32_t freeKb = freeBytes() / 1024u;

  putU(mb);
  put("MB flash, ");
  putU(usedMb);
  put("MB used, ");
  putU(freeKb);
  put("KB free");
  line[i] = '\0';
  return line;
}

}  // namespace bridge
