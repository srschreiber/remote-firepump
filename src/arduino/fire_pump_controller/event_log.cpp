// event_log.cpp — see event_log.h.

#include "event_log.h"

#include <string.h>

EventLog& eventLog() {
  static EventLog instance;
  return instance;
}

void EventLog::clear() {
  memset(entries_, 0, sizeof(entries_));
  head_ = 0;
  count_ = 0;
  nextSeq_ = 0;
  dropped_ = 0;
}

void EventLog::add(uint32_t uptimeMs, LogEvent event, uint8_t state,
                   uint8_t detail, uint8_t flags) {
  LogEntry& e = entries_[head_];
  e.seq = nextSeq_++;
  e.uptimeMs = uptimeMs;
  e.event = static_cast<uint8_t>(event);
  e.state = state;
  e.detail = detail;
  e.flags = flags;

  head_ = (head_ + 1) % LOG_RING_ENTRIES;
  if (count_ < LOG_RING_ENTRIES) {
    ++count_;
  } else {
    // Overwrote the oldest. Count it so the Pi can tell it missed something
    // rather than silently seeing a gap in sequence numbers.
    ++dropped_;
  }
}

uint32_t EventLog::oldestSeq() const {
  if (count_ == 0) {
    return nextSeq_;
  }
  const size_t oldest = (head_ + LOG_RING_ENTRIES - count_) % LOG_RING_ENTRIES;
  return entries_[oldest].seq;
}

size_t EventLog::drain(uint32_t since, LogEntry* out, size_t max,
                       uint32_t& outNextSeq) const {
  outNextSeq = nextSeq_;
  if (out == nullptr || max == 0 || count_ == 0) {
    return 0;
  }

  size_t written = 0;
  const size_t start = (head_ + LOG_RING_ENTRIES - count_) % LOG_RING_ENTRIES;
  for (size_t i = 0; i < count_ && written < max; ++i) {
    const LogEntry& e = entries_[(start + i) % LOG_RING_ENTRIES];
    // Rollover-safe: seq is monotonic from zero and would take 136 years at
    // one entry per millisecond, but compare the same way as everything else.
    if (static_cast<int32_t>(e.seq - since) < 0) {
      continue;
    }
    out[written++] = e;
  }

  if (written > 0) {
    outNextSeq = out[written - 1].seq + 1;
  }
  return written;
}

const char* toString(LogEvent e) {
  switch (e) {
    case LogEvent::BOOT:         return "BOOT";
    case LogEvent::STATE_CHANGE: return "STATE";
    case LogEvent::RELAY:        return "RELAY";
    case LogEvent::COMMAND:      return "CMD";
    case LogEvent::FAULT:        return "FAULT";
    case LogEvent::SAFETY:       return "SAFETY";
    case LogEvent::WATER:        return "WATER";
    case LogEvent::NET:          return "NET";
  }
  return "?";
}

const char* toString(LogRelay r) {
  switch (r) {
    case LogRelay::STARTER: return "starter";
    case LogRelay::CHOKE:   return "choke";
    case LogRelay::KILL:    return "kill";
    case LogRelay::VALVE:   return "valve";
  }
  return "?";
}

const char* toString(LogSafety s) {
  switch (s) {
    case LogSafety::STARTER_REFUSED_KILL:     return "starter_refused_kill";
    case LogSafety::STARTER_REFUSED_UNPRIMED: return "starter_refused_unprimed";
    case LogSafety::STARTER_FORCED_OFF:       return "starter_forced_off";
    case LogSafety::CHOKE_FORCED_OFF:         return "choke_forced_off";
    case LogSafety::VALVE_CLOSE_REFUSED:      return "valve_close_refused";
    case LogSafety::VALVE_REOPENED:           return "valve_reopened";
  }
  return "?";
}

const char* toString(LogNet n) {
  switch (n) {
    case LogNet::ASSOCIATING:  return "associating";
    case LogNet::ASSOCIATED:   return "associated";
    case LogNet::GOT_ADDRESS:  return "got_address";
    case LogNet::LINK_LOST:    return "link_lost";
    case LogNet::DHCP_TIMEOUT: return "dhcp_timeout";
  }
  return "?";
}
