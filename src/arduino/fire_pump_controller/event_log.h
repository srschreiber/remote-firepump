// event_log.h — fixed-size in-RAM event ring, drained by the Raspberry Pi.
//
// WHY THIS EXISTS
//
// /v1/status is a snapshot: it says what is true now and can never say what
// happened. The questions that actually matter after an incident are all
// historical -- why did it fault, did the starter release before the kill
// grounded, how long did the crank really take, did the controller reset.
// Serial answers those, but nobody is plugged into a USB port at the pump.
//
// WHY PULL, NOT PUSH
//
// The Arduino never initiates a network transaction here. The Pi drains the
// ring with GET /v1/log?since=<seq> on its own schedule.
//
// That is deliberate. Every WiFiS3 call is an AT exchange with a 10 second
// modem timeout, against a ~5.6 second watchdog. If the device pushed log
// lines as they occurred, a log emitted during CRANKING would become a new
// way to stall the main loop mid-crank. Appending to this ring is a handful
// of stores and cannot block, so it is safe to call from anywhere -- the
// relay layer, the safety enforcer, an interrupt-free tick.
//
// The failure mode is also better: a slow or dead Pi just means older entries
// are overwritten and `dropped` counts them. Nothing back-pressures into the
// safety loop.
//
// Entries are stored as fixed-width records of small integers, not formatted
// strings. Twelve bytes each instead of eighty, and the Pi expands the codes
// into text where there is a real disk and a real log format.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

// What happened. Deliberately coarse: the detail field carries the specifics.
enum class LogEvent : uint8_t {
  BOOT = 0,        // detail: reason (currently always 0 = power-on/reset)
  STATE_CHANGE,    // detail: previous PumpState
  RELAY,           // detail: LogRelay, flags bit0 = new level (1 = active)
  COMMAND,         // detail: CommandType, flags bit0 = accepted
  FAULT,           // detail: FaultCode
  SAFETY,          // detail: LogSafety -- an interlock refused or forced
  WATER,           // detail: 1 = available, 0 = lost
  NET,             // detail: LogNet
};

enum class LogRelay : uint8_t { STARTER = 0, CHOKE, KILL, VALVE };

enum class LogSafety : uint8_t {
  STARTER_REFUSED_KILL = 0,   // starter asked for while the kill was asserted
  STARTER_REFUSED_UNPRIMED,   // starter asked for before the prime completed
  STARTER_FORCED_OFF,         // MAX_CRANK_MS backstop fired
  CHOKE_FORCED_OFF,           // MAX_CHOKE_MS backstop fired
  VALVE_CLOSE_REFUSED,        // asked to shut the intake on a running engine
  VALVE_REOPENED,             // intake found shut with the engine turning
};

enum class LogNet : uint8_t {
  ASSOCIATING = 0, ASSOCIATED, GOT_ADDRESS, LINK_LOST, DHCP_TIMEOUT,
};

// 12 bytes. 128 of these is 1.5 KB -- about 7% of the free RAM on this board.
struct LogEntry {
  uint32_t seq;       // monotonic since boot; the Pi's cursor
  uint32_t uptimeMs;  // millis() at the moment it happened
  uint8_t  event;     // LogEvent
  uint8_t  state;     // PumpState at the time
  uint8_t  detail;    // event-specific, see above
  uint8_t  flags;     // bit0 event-specific; bits 4-7 relay snapshot
};

static_assert(sizeof(LogEntry) == 12, "LogEntry must stay 12 bytes");

// Relay snapshot packed into the top nibble of `flags`, so every entry
// carries the full output picture without a separate record.
constexpr uint8_t LOG_FLAG_BIT       = 0x01;
constexpr uint8_t LOG_RELAY_STARTER  = 0x10;
constexpr uint8_t LOG_RELAY_CHOKE    = 0x20;
constexpr uint8_t LOG_RELAY_KILL     = 0x40;
constexpr uint8_t LOG_RELAY_VALVE    = 0x80;

class EventLog {
 public:
  void clear();

  // Appends one entry. Never blocks, never allocates. Overwrites the oldest
  // entry when full and counts the loss.
  void add(uint32_t uptimeMs, LogEvent event, uint8_t state, uint8_t detail,
           uint8_t flags);

  // Copies up to `max` entries with seq >= `since` into `out`, oldest first.
  // Returns how many were written. `outNextSeq` receives the seq the caller
  // should ask for next time.
  size_t drain(uint32_t since, LogEntry* out, size_t max,
               uint32_t& outNextSeq) const;

  uint32_t nextSeq() const { return nextSeq_; }
  uint32_t dropped() const { return dropped_; }
  size_t   count() const { return count_; }

  // Oldest seq still held. If the Pi asks for something older it has missed
  // entries, and drain() reports the gap.
  uint32_t oldestSeq() const;

 private:
  LogEntry entries_[LOG_RING_ENTRIES] = {};
  size_t   head_ = 0;     // next write slot
  size_t   count_ = 0;
  uint32_t nextSeq_ = 0;
  uint32_t dropped_ = 0;
};

// The one instance. A singleton rather than an injected dependency purely so
// the relay layer can log without threading a reference through every call.
EventLog& eventLog();

// Short stable names for the JSON the Pi consumes.
const char* toString(LogEvent e);
const char* toString(LogRelay r);
const char* toString(LogSafety s);
const char* toString(LogNet n);
