// tests/shim/Arduino.cpp — see shim/Arduino.h.

#include "Arduino.h"

FakeSerial Serial;

namespace fake {

int      pinLevel[kMaxPins];
int      pinModeOf[kMaxPins];
int      analogCounts[kMaxPins];
uint32_t nowMs = 0;
std::vector<Event> events;
std::string serialLog;
bool captureSerial = false;

static uint64_t g_seq = 0;

void reset() {
  for (int i = 0; i < kMaxPins; ++i) {
    pinLevel[i] = -1;
    pinModeOf[i] = -1;
    // 0 counts = 0 V = an open 4-20 mA loop, which the level sensor treats
    // as a broken wire. Tests that want a reading set it explicitly.
    analogCounts[i] = 0;
  }
  nowMs = 0;
  events.clear();
  serialLog.clear();
  g_seq = 0;
}

size_t writesBeforeMode(uint8_t pin) {
  size_t count = 0;
  for (const Event& e : events) {
    if (e.pin != pin) continue;
    if (e.kind == EventKind::PIN_MODE) break;
    ++count;
  }
  return count;
}

size_t firstWriteIndex(uint8_t pin, uint8_t level) {
  for (size_t i = 0; i < events.size(); ++i) {
    const Event& e = events[i];
    if (e.kind == EventKind::DIGITAL_WRITE && e.pin == pin && e.value == level) {
      return i;
    }
  }
  return SIZE_MAX;
}

size_t lastWriteIndex(uint8_t pin, uint8_t level) {
  size_t found = SIZE_MAX;
  for (size_t i = 0; i < events.size(); ++i) {
    const Event& e = events[i];
    if (e.kind == EventKind::DIGITAL_WRITE && e.pin == pin && e.value == level) {
      found = i;
    }
  }
  return found;
}

static void record(EventKind kind, uint8_t pin, uint8_t value) {
  Event e;
  e.kind = kind;
  e.pin = pin;
  e.value = value;
  e.at = nowMs;
  e.seq = g_seq++;
  events.push_back(e);
}

}  // namespace fake

void pinMode(uint8_t pin, uint8_t mode) {
  if (pin < fake::kMaxPins) {
    fake::pinModeOf[pin] = mode;
  }
  fake::record(fake::EventKind::PIN_MODE, pin, mode);
}

void digitalWrite(uint8_t pin, uint8_t value) {
  if (pin < fake::kMaxPins) {
    fake::pinLevel[pin] = value;
  }
  fake::record(fake::EventKind::DIGITAL_WRITE, pin, value);
}

int digitalRead(uint8_t pin) {
  return (pin < fake::kMaxPins && fake::pinLevel[pin] > 0) ? HIGH : LOW;
}

uint32_t millis() { return fake::nowMs; }

void delay(uint32_t ms) { fake::nowMs += ms; }


int analogRead(uint8_t pin) {
  if (pin >= fake::kMaxPins) {
    return 0;
  }
  return fake::analogCounts[pin];
}

void analogReadResolution(int) {
  // The real core changes the ADC width here. Tests set fake::analogCounts
  // directly in whatever width they declare, so this is a no-op.
}
