// tests/shim/Arduino.h — host-side stand-in for the Arduino core.
//
// Provides just enough of the Arduino API for pump_controller.cpp,
// http_protocol.cpp and api_handler.cpp to compile and run natively, plus
// instrumentation so tests can assert on the exact electrical levels driven
// onto each pin and on the ORDER in which they were driven.
//
// This file is never compiled into firmware.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#define HIGH 0x1
#define LOW  0x0

#define INPUT        0x0
#define OUTPUT       0x1
#define INPUT_PULLUP 0x2

// Analog pin numbers. The real core maps A0.. to board-specific indices; the
// only property the firmware depends on is that they are distinct from the
// digital pins it drives, so 14+ mirrors the classic Arduino numbering.
#define A0 14
#define A1 15
#define A2 16
#define A3 17
#define A4 18
#define A5 19

// On real hardware F() moves the literal to flash. On the host it is identity.
#define F(x) (x)

typedef uint8_t byte;
typedef bool boolean;

namespace fake {

constexpr int kMaxPins = 32;

enum class EventKind : uint8_t { PIN_MODE, DIGITAL_WRITE };

struct Event {
  EventKind kind;
  uint8_t   pin;
  uint8_t   value;    // level for DIGITAL_WRITE, mode for PIN_MODE
  uint32_t  at;       // value of fake::nowMs when it happened
  uint64_t  seq;      // strictly increasing, for order assertions
};

// Raw ADC counts a test wants analogRead() to return for a pin. Defaults to
// 0, which for a 4-20 mA loop reads as an open circuit -- the safe default.
extern int      analogCounts[kMaxPins];

extern int      pinLevel[kMaxPins];   // -1 until first digitalWrite
extern int      pinModeOf[kMaxPins];  // -1 until first pinMode
extern uint32_t nowMs;
extern std::vector<Event> events;
extern std::string serialLog;
extern bool captureSerial;

void reset();

// Number of digitalWrite calls that happened on `pin` before its first
// pinMode(OUTPUT). Used to prove outputs are pre-driven to a safe level.
size_t writesBeforeMode(uint8_t pin);

// Index of the first event matching a predicate, or SIZE_MAX.
size_t firstWriteIndex(uint8_t pin, uint8_t level);
size_t lastWriteIndex(uint8_t pin, uint8_t level);

}  // namespace fake

void     pinMode(uint8_t pin, uint8_t mode);
void     digitalWrite(uint8_t pin, uint8_t value);
int      digitalRead(uint8_t pin);
int      analogRead(uint8_t pin);
void     analogReadResolution(int bits);
uint32_t millis();
void     delay(uint32_t ms);

// ---------------------------------------------------------------------------
// Minimal Serial
// ---------------------------------------------------------------------------

class FakeSerial {
 public:
  void begin(unsigned long) {}
  explicit operator bool() const { return true; }

  void print(const char* s) { emit(s); }
  void print(char c) { char b[2] = {c, 0}; emit(b); }
  void print(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); emit(b); }
  void print(unsigned v) { char b[16]; snprintf(b, sizeof(b), "%u", v); emit(b); }
  void print(long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); emit(b); }
  void print(unsigned long v) { char b[24]; snprintf(b, sizeof(b), "%lu", v); emit(b); }

  void println() { emit("\n"); }
  void println(const char* s) { emit(s); emit("\n"); }
  void println(int v) { print(v); emit("\n"); }
  void println(unsigned v) { print(v); emit("\n"); }
  void println(long v) { print(v); emit("\n"); }
  void println(unsigned long v) { print(v); emit("\n"); }

 private:
  void emit(const char* s) {
    if (fake::captureSerial) {
      fake::serialLog += s;
    }
  }
};

extern FakeSerial Serial;
