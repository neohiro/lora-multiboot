// Minimal Arduino shim, used only to type-check firmware/src/main.cpp on the host.
//
// This is NOT an emulator. It exists because main.cpp is the real firmware entry
// point and it was in no build at all -- the host gate deliberately excludes it
// (it includes Arduino.h), which meant the file a user flashes first was the one
// file nobody ever compiled. Catching a typo there should not require a board.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Arduino exposes the radix as macros from Print.h. main.cpp uses HEX.
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

inline uint32_t millis() { return 0; }

class FakeSerial {
 public:
  void begin(unsigned long) {}
  void print(const char* s) { (void)s; }
  void print(char c) { (void)c; }
  void print(int v, int = DEC) { (void)v; }
  void print(long v, int = DEC) { (void)v; }
  void print(unsigned long v, int = DEC) { (void)v; }
  void print(unsigned long long v, int = DEC) { (void)v; }
  void print(unsigned int v, int = DEC) { (void)v; }
  void print(double v, int = DEC) { (void)v; }
  void print(float v, int = DEC) { (void)v; }
  void println() {}
  void println(const char* s) { (void)s; }
  void println(char c) { (void)c; }
  void println(int v, int = DEC) { (void)v; }
  void println(long v, int = DEC) { (void)v; }
  void println(unsigned long v, int = DEC) { (void)v; }
  void println(unsigned long long v, int = DEC) { (void)v; }
  void println(unsigned int v, int = DEC) { (void)v; }
  void println(double v, int = DEC) { (void)v; }
  void println(float v, int = DEC) { (void)v; }

  // Arduino's Stream exposes an implicit bool for "serial connected". main.cpp uses
  // `while (!Serial && ...)` and relies on that conversion.
  operator bool() const { return true; }
};

static FakeSerial Serial;