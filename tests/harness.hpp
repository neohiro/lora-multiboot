// SPDX-License-Identifier: MIT
//
// A test harness with no dependencies. The point of this project is that the
// portable logic can be exercised on a laptop with nothing but a C++ compiler,
// so pulling in a framework would undercut the reason it works that way.

#pragma once

#include <cstdio>
#include <string>

namespace harness {

// Inline variables rather than statics inside inline functions: one instance per
// program, no duplicate-symbol trouble when the header lands in several
// translation units.
inline int gChecks = 0;
inline int gFailures = 0;

inline int& checks() { return gChecks; }

inline int& failures() { return gFailures; }

inline void suite(const char* name) { std::printf("\n-- %s\n", name); }

inline void report(bool ok, const char* expr, const char* file, int line,
                   const std::string& extra) {
  ++gChecks;
  if (ok) return;
  ++gFailures;
  if (extra.empty()) {
    std::printf("   FAIL %s:%d  %s\n", file, line, expr);
  } else {
    std::printf("   FAIL %s:%d  %s  -> %s\n", file, line, expr, extra.c_str());
  }
}

}  // namespace harness

#define CHECK(expr) harness::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__, std::string())

#define CHECK_MSG(expr, msg) \
  harness::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__, std::string(msg))

#define CHECK_EQ(a, b) harness::report((a) == (b), #a " == " #b, __FILE__, __LINE__, std::string())

// Aborts the current suite when a precondition fails. Used where continuing
// would produce a cascade of meaningless failures out of one real problem.
#define REQUIRE(expr)                                                    \
  do {                                                                   \
    if (!static_cast<bool>(expr)) {                                      \
      harness::report(false, #expr, __FILE__, __LINE__, "precondition"); \
      return;                                                            \
    }                                                                    \
  } while (0)

#define REQUIRE_MSG(expr, msg)                                                  \
  do {                                                                          \
    if (!static_cast<bool>(expr)) {                                             \
      harness::report(false, #expr, __FILE__, __LINE__, std::string(msg));      \
      return;                                                                   \
    }                                                                           \
  } while (0)
