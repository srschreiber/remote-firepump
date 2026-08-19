// tests/test_event_log.cpp — the event ring and GET /v1/log.
//
// The property that matters most here is that a read is NON-DESTRUCTIVE.
// The Pi "flushes" by advancing its cursor, not by consuming entries, so if
// it dies between receiving a batch and writing it to disk it can simply ask
// for the same `since` again and get the same data.

#include "test_support.h"

namespace {

std::string getLog(PumpController& p, const char* query, uint16_t& status) {
  std::string path = "/v1/log";
  if (query != nullptr) {
    path += query;
  }
  return doRequest(p, makeRequest("GET", path.c_str(), kTestSecret, nullptr),
                   status, fake::nowMs);
}

size_t countEntries(const std::string& body) {
  // Entries are tuples, so count '[' after the "entries":[ marker.
  const size_t at = body.find("\"entries\":[");
  if (at == std::string::npos) return 0;
  size_t n = 0;
  for (size_t i = at + 11; i < body.size(); ++i) {
    if (body[i] == '[') ++n;
  }
  return n;
}

}  // namespace

// ===========================================================================
// Ring mechanics
// ===========================================================================

TEST(the_ring_starts_empty_and_numbers_from_zero) {
  EventLog log;
  log.clear();
  CHECK_EQ(log.count(), 0u);
  CHECK_EQ(log.nextSeq(), 0u);
  CHECK_EQ(log.dropped(), 0u);

  LogEntry out[8];
  uint32_t next = 999;
  CHECK_EQ(log.drain(0, out, 8, next), 0u);
  CHECK_EQ(next, 0u);
}

TEST(entries_are_returned_oldest_first_with_monotonic_sequence) {
  EventLog log;
  log.clear();
  for (uint8_t i = 0; i < 5; ++i) {
    log.add(1000u + i, LogEvent::RELAY, 1, i, 0);
  }

  LogEntry out[8];
  uint32_t next = 0;
  const size_t n = log.drain(0, out, 8, next);
  CHECK_EQ(n, 5u);
  CHECK_EQ(next, 5u);
  for (size_t i = 0; i < n; ++i) {
    CHECK_EQ(out[i].seq, static_cast<uint32_t>(i));
    CHECK_EQ(out[i].uptimeMs, 1000u + i);
    CHECK_EQ(out[i].detail, static_cast<uint8_t>(i));
  }
}

TEST(reading_is_non_destructive_so_the_same_cursor_returns_the_same_batch) {
  EventLog log;
  log.clear();
  for (uint8_t i = 0; i < 6; ++i) {
    log.add(i, LogEvent::COMMAND, 0, i, 0);
  }

  LogEntry a[8], b[8];
  uint32_t nextA = 0, nextB = 0;
  const size_t na = log.drain(2, a, 8, nextA);
  const size_t nb = log.drain(2, b, 8, nextB);   // same cursor again

  CHECK_MSG(na == nb, "a repeated drain returned a different count");
  CHECK_EQ(nextA, nextB);
  for (size_t i = 0; i < na; ++i) {
    CHECK_MSG(a[i].seq == b[i].seq,
              "a repeated drain returned different entries");
  }
  // And nothing was consumed.
  CHECK_EQ(log.count(), 6u);
}

TEST(a_cursor_skips_everything_already_seen) {
  EventLog log;
  log.clear();
  for (uint8_t i = 0; i < 10; ++i) {
    log.add(i, LogEvent::RELAY, 0, i, 0);
  }

  LogEntry out[16];
  uint32_t next = 0;
  const size_t n = log.drain(7, out, 16, next);
  CHECK_EQ(n, 3u);
  CHECK_EQ(out[0].seq, 7u);
  CHECK_EQ(next, 10u);

  // Cursor at the end: nothing new.
  CHECK_EQ(log.drain(next, out, 16, next), 0u);
}

TEST(the_ring_wraps_and_counts_what_it_dropped) {
  EventLog log;
  log.clear();
  const size_t overfill = LOG_RING_ENTRIES + 25;
  for (size_t i = 0; i < overfill; ++i) {
    log.add(static_cast<uint32_t>(i), LogEvent::RELAY, 0,
            static_cast<uint8_t>(i & 0xFF), 0);
  }

  CHECK_EQ(log.count(), LOG_RING_ENTRIES);
  CHECK_MSG(log.dropped() == 25u, "dropped count did not match the overflow");
  CHECK_EQ(log.nextSeq(), static_cast<uint32_t>(overfill));
  // The oldest surviving entry is the 26th one written.
  CHECK_EQ(log.oldestSeq(), 25u);

  // A cursor older than the ring gets what remains, and `dropped` is how the
  // Pi learns it missed something rather than seeing a silent gap.
  LogEntry out[LOG_RING_ENTRIES];
  uint32_t next = 0;
  const size_t n = log.drain(0, out, LOG_RING_ENTRIES, next);
  CHECK_EQ(n, LOG_RING_ENTRIES);
  CHECK_EQ(out[0].seq, 25u);
}

TEST(a_drain_is_bounded_by_max_and_pages_with_next) {
  EventLog log;
  log.clear();
  for (size_t i = 0; i < 20; ++i) {
    log.add(static_cast<uint32_t>(i), LogEvent::RELAY, 0, 0, 0);
  }

  LogEntry out[5];
  uint32_t next = 0;
  size_t total = 0;
  uint32_t cursor = 0;
  for (int page = 0; page < 10; ++page) {
    const size_t n = log.drain(cursor, out, 5, next);
    if (n == 0) break;
    CHECK_MSG(n <= 5, "a drain exceeded its max");
    total += n;
    cursor = next;
  }
  CHECK_MSG(total == 20u, "paging did not retrieve every entry exactly once");
}

// ===========================================================================
// JSON
// ===========================================================================

TEST(log_json_has_the_documented_shape) {
  EventLog log;
  log.clear();
  log.add(4242, LogEvent::STATE_CHANGE, static_cast<uint8_t>(PumpState::CRANKING),
          static_cast<uint8_t>(PumpState::CHOKING), LOG_RELAY_STARTER);

  LogEntry out[4];
  uint32_t next = 0;
  const size_t n = log.drain(0, out, 4, next);

  char buf[LOG_BODY_MAX];
  const size_t len = buildLogJson(buf, sizeof(buf), out, n, next,
                                  log.oldestSeq(), log.dropped(), false);
  CHECK(len > 0);
  const std::string j(buf);
  CHECK_CONTAINS(j, "\"next\":1");
  CHECK_CONTAINS(j, "\"oldest\":0");
  CHECK_CONTAINS(j, "\"dropped\":0");
  CHECK_CONTAINS(j, "\"truncated\":false");
  CHECK_CONTAINS(j, "\"count\":1");
  // [seq, uptime_ms, event, state, detail, relay_bits]
  CHECK_CONTAINS(j, "[0,4242,\"STATE\",\"CRANKING\",");
  CHECK(j.front() == '{' && j.back() == '}');
}

TEST(a_full_batch_fits_the_log_response_buffer) {
  EventLog log;
  log.clear();
  // Worst case: the longest state name and the largest numbers.
  for (size_t i = 0; i < LOG_MAX_PER_RESPONSE; ++i) {
    log.add(0xFFFFFFFFu, LogEvent::STATE_CHANGE,
            static_cast<uint8_t>(PumpState::RUNNING_ASSUMED), 255, 255);
  }
  LogEntry out[LOG_MAX_PER_RESPONSE];
  uint32_t next = 0;
  const size_t n = log.drain(0, out, LOG_MAX_PER_RESPONSE, next);
  CHECK_EQ(n, LOG_MAX_PER_RESPONSE);

  char buf[LOG_BODY_MAX];
  const size_t len = buildLogJson(buf, sizeof(buf), out, n, next, 0,
                                  0xFFFFFFFFu, true);
  CHECK_MSG(len > 0, "a maximum batch did not fit LOG_BODY_MAX");
  CHECK(len < LOG_BODY_MAX);
}

TEST(log_json_fails_safely_when_the_buffer_is_too_small) {
  LogEntry e{};
  char tiny[24];
  CHECK_EQ(buildLogJson(tiny, sizeof(tiny), &e, 1, 1, 0, 0, false), 0u);
  CHECK_STREQ(tiny, "");
}

// ===========================================================================
// The endpoint
// ===========================================================================

TEST(the_log_endpoint_routes_and_requires_authentication) {
  const Route ok = resolveRoute(HttpMethod::GET, "/v1/log");
  CHECK(ok.action == RouteAction::LOG);

  const Route wrongMethod = resolveRoute(HttpMethod::POST, "/v1/log");
  CHECK(wrongMethod.action == RouteAction::ERROR_STATUS);
  CHECK_EQ(wrongMethod.errorStatus, 405);

  PumpController p;
  bootAt(p, 0);
  uint16_t status = 0;
  doRequest(p, makeRequest("GET", "/v1/log", nullptr, nullptr), status,
            fake::nowMs);
  CHECK_MSG(status == 401, "the log endpoint served an unauthenticated caller");
}

TEST(the_since_query_parameter_is_parsed_and_validated) {
  struct Case { const char* target; bool present; bool malformed; uint32_t v; };
  const Case cases[] = {
      {"/v1/log",             false, false, 0},
      {"/v1/log?since=0",     true,  false, 0},
      {"/v1/log?since=417",   true,  false, 417},
      {"/v1/log?since=abc",   true,  true,  0},
      {"/v1/log?since=-1",    true,  true,  0},
      {"/v1/log?since=",      true,  true,  0},
      {"/v1/log?other=1",     false, false, 0},
  };
  for (const Case& c : cases) {
    RequestParser parser;
    const std::string raw =
        std::string("GET ") + c.target + " HTTP/1.1\r\nHost: h\r\n\r\n";
    for (char ch : raw) {
      if (parser.feed(ch) != ParseStatus::INCOMPLETE) break;
    }
    CHECK_MSG(parser.status() == ParseStatus::COMPLETE, c.target);
    CHECK_MSG(parser.request().sincePresent == c.present, c.target);
    CHECK_MSG(parser.request().sinceMalformed == c.malformed, c.target);
    if (!c.malformed) {
      CHECK_MSG(parser.request().since == c.v, c.target);
    }
    // The query string never leaks into the routed path.
    CHECK_STREQ(parser.request().path, "/v1/log");
  }
}

TEST(a_malformed_since_is_rejected_with_400) {
  PumpController p;
  bootAt(p, 0);
  uint16_t status = 0;
  const std::string body = getLog(p, "?since=nope", status);
  CHECK_EQ(status, 400);
  CHECK_CONTAINS(body, "invalid_since");
}

TEST(a_real_start_stop_cycle_is_recoverable_from_the_log) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  std::string body = getLog(p, nullptr, status);
  CHECK_EQ(status, 200);
  CHECK_MSG(countEntries(body) > 0, "boot and reset produced no log entries");

  // Run a full cycle.
  p.handleCommand(CommandType::START, "log-1", fake::nowMs);
  advanceBy(p, VALVE_PRIME_MS + CHOKE_PREP_MS + CRANK_DURATION_MS +
                   UNCHOKE_DELAY_MS + 50);
  CHECK_STREQ(toString(p.state()), "RUNNING_ASSUMED");
  p.handleCommand(CommandType::STOP, "log-2", fake::nowMs);
  advanceBy(p, KILL_HOLD_MS + VALVE_CLOSE_DELAY_MS + 50);

  // Page the whole log out the way the Pi would.
  uint32_t cursor = 0;
  std::string all;
  for (int page = 0; page < 20; ++page) {
    char q[48];
    snprintf(q, sizeof(q), "?since=%lu", static_cast<unsigned long>(cursor));
    body = getLog(p, q, status);
    CHECK_EQ(status, 200);
    all += body;
    const size_t at = body.find("\"next\":");
    CHECK(at != std::string::npos);
    const uint32_t next =
        static_cast<uint32_t>(strtoul(body.c_str() + at + 7, nullptr, 10));
    if (next == cursor) break;
    cursor = next;
  }

  // The narrative of the run is all there.
  CHECK_CONTAINS(all, "\"PRIMING\"");
  CHECK_CONTAINS(all, "\"CRANKING\"");
  CHECK_CONTAINS(all, "\"RUNNING_ASSUMED\"");
  CHECK_CONTAINS(all, "\"STOPPING\"");
  CHECK_CONTAINS(all, "\"VALVE_CLOSING\"");
  CHECK_CONTAINS(all, "\"RELAY\"");
  CHECK_CONTAINS(all, "\"CMD\"");
  CHECK_CONTAINS(all, "\"BOOT\"");

  CHECK_MSG(eventLog().dropped() == 0,
            "a single start/stop cycle overflowed the ring");
}

TEST(logging_never_blocks_or_grows_the_ring_beyond_its_bound) {
  // Hammer it far past capacity; memory use must not move.
  EventLog log;
  log.clear();
  for (size_t i = 0; i < LOG_RING_ENTRIES * 50; ++i) {
    log.add(static_cast<uint32_t>(i), LogEvent::RELAY, 0, 0, 0);
  }
  CHECK_EQ(log.count(), LOG_RING_ENTRIES);
  CHECK_EQ(log.nextSeq(), static_cast<uint32_t>(LOG_RING_ENTRIES * 50));
  CHECK_EQ(log.dropped(),
           static_cast<uint32_t>(LOG_RING_ENTRIES * 50 - LOG_RING_ENTRIES));
}
