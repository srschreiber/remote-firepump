// tests/test_extensions.cpp — per-request timing overrides and the
// flag-gated maintenance relay API.
//
// Built at both relay polarities AND with ENABLE_MAINTENANCE_API both set and
// clear, so the flag's effect on reachability is proven in each direction.

#include "test_support.h"

namespace {

std::string sendWithHeaders(PumpController& p, const char* method,
                            const char* path, const char* extraHeaders,
                            uint16_t& status, const char* requestId = "ext-1") {
  std::string raw;
  raw += method;
  raw += " ";
  raw += path;
  raw += " HTTP/1.1\r\nHost: h\r\nX-Pump-Secret: ";
  raw += kTestSecret;
  raw += "\r\n";
  if (requestId != nullptr) {
    raw += "X-Request-ID: ";
    raw += requestId;
    raw += "\r\n";
  }
  if (extraHeaders != nullptr) {
    raw += extraHeaders;
  }
  raw += "\r\n";
  return doRequest(p, raw, status, fake::nowMs);
}

// Satisfies the crank preconditions through the maintenance API, the same way
// the real start sequence does: intake open, prime dwell elapsed, kill released.
void primeAndPermitCranking(PumpController& p) {
  uint16_t st = 0;
  if (INTAKE_VALVE_ENABLED) {
    sendWithHeaders(p, "POST", "/v1/maintenance/valve/on", nullptr, st, "pre-v");
    advanceBy(p, VALVE_PRIME_MS);
  }
  sendWithHeaders(p, "POST", "/v1/maintenance/kill/off", nullptr, st, "pre-k");
}

uint32_t starterOnMs() {
  bool on = false;
  uint32_t onAt = 0;
  for (const fake::Event& e : fake::events) {
    if (e.kind != fake::EventKind::DIGITAL_WRITE || e.pin != PIN_RELAY_STARTER) {
      continue;
    }
    const bool nowOn = (e.value == activeLevel());
    if (nowOn && !on) {
      on = true;
      onAt = e.at;
    } else if (!nowOn && on) {
      return static_cast<uint32_t>(e.at - onAt);
    }
  }
  return 0;
}

}  // namespace

// ===========================================================================
// parseUint32
// ===========================================================================

TEST(parse_uint32_accepts_only_plain_decimal_numbers) {
  uint32_t v = 0;

  CHECK(parseUint32("0", 1, v) && v == 0u);
  CHECK(parseUint32("7", 1, v) && v == 7u);
  CHECK(parseUint32("1000", 4, v) && v == 1000u);
  CHECK(parseUint32("4294967295", 10, v) && v == 4294967295u);
  CHECK(parseUint32("0001", 4, v) && v == 1u);

  // Rejections.
  CHECK(!parseUint32("", 0, v));
  CHECK(!parseUint32(nullptr, 3, v));
  CHECK(!parseUint32("4294967296", 10, v));   // one past the top
  CHECK(!parseUint32("99999999999", 11, v));  // too many digits
  CHECK(!parseUint32("-1", 2, v));
  CHECK(!parseUint32("+1", 2, v));
  CHECK(!parseUint32("1.5", 3, v));
  CHECK(!parseUint32("1e3", 3, v));
  CHECK(!parseUint32(" 1", 2, v));
  CHECK(!parseUint32("1 ", 2, v));
  CHECK(!parseUint32("0x10", 4, v));
  CHECK(!parseUint32("abc", 3, v));
}

// ===========================================================================
// Timing override parsing
// ===========================================================================

TEST(timing_override_headers_are_parsed_case_insensitively) {
  RequestParser p;
  const std::string raw =
      "POST /v1/start HTTP/1.1\r\n"
      "X-Choke-Ms: 250\r\n"
      "x-crank-ms: 1500\r\n"
      "X-UNCHOKE-MS: 750\r\n\r\n";
  ParseStatus st = ParseStatus::INCOMPLETE;
  for (char c : raw) {
    st = p.feed(c);
    if (st != ParseStatus::INCOMPLETE) break;
  }
  CHECK(st == ParseStatus::COMPLETE);

  const ParsedRequest& r = p.request();
  CHECK(r.anyTimingOverride());
  CHECK(!r.timingMalformed);
  CHECK(r.chokeMsPresent);   CHECK_EQ(r.chokeMs, 250u);
  CHECK(r.crankMsPresent);   CHECK_EQ(r.crankMs, 1500u);
  CHECK(r.unchokeMsPresent); CHECK_EQ(r.unchokeMs, 750u);
}

TEST(a_request_without_timing_headers_reports_no_override) {
  RequestParser p;
  const std::string raw = "POST /v1/start HTTP/1.1\r\nHost: h\r\n\r\n";
  for (char c : raw) {
    if (p.feed(c) != ParseStatus::INCOMPLETE) break;
  }
  CHECK(!p.request().anyTimingOverride());
  CHECK(!p.request().timingMalformed);
}

TEST(a_non_numeric_timing_header_is_flagged_malformed) {
  RequestParser p;
  const std::string raw =
      "POST /v1/start HTTP/1.1\r\nX-Crank-Ms: soon\r\n\r\n";
  for (char c : raw) {
    if (p.feed(c) != ParseStatus::INCOMPLETE) break;
  }
  CHECK(p.request().crankMsPresent);
  CHECK(p.request().timingMalformed);
}

// ===========================================================================
// Timing overrides end to end
// ===========================================================================

TEST(a_crank_override_changes_the_actual_starter_engagement_time) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  const std::string body = sendWithHeaders(
      p, "POST", "/v1/start", "X-Crank-Ms: 3500\r\n", status, "ov-crank");
  CHECK_EQ(status, 202);
  // START now primes first; skip the dwell to reach the choke phase.
  if (INTAKE_VALVE_ENABLED) advanceBy(p, VALVE_PRIME_MS);
  CHECK_STREQ(toString(p.state()), "CHOKING");
  (void)body;

  // Default choke prep still applies.
  advanceBy(p, CHOKE_PREP_MS);
  CHECK_STREQ(toString(p.state()), "CRANKING");

  advanceBy(p, 3500 - 1);
  CHECK_MSG(p.starterActive(), "starter released before the requested 3500 ms");
  advanceBy(p, 1);
  CHECK_MSG(!p.starterActive(), "starter ran past the requested 3500 ms");
  CHECK_EQ(starterOnMs(), 3500u);
}

TEST(all_three_timing_overrides_take_effect_together) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  sendWithHeaders(p, "POST", "/v1/start",
                  "X-Choke-Ms: 200\r\nX-Crank-Ms: 900\r\nX-Unchoke-Ms: 100\r\n",
                  status, "ov-all");
  CHECK_EQ(status, 202);
  if (INTAKE_VALVE_ENABLED) advanceBy(p, VALVE_PRIME_MS);

  advanceBy(p, 199);
  CHECK_STREQ(toString(p.state()), "CHOKING");
  advanceBy(p, 1);
  CHECK_STREQ(toString(p.state()), "CRANKING");

  advanceBy(p, 899);
  CHECK_STREQ(toString(p.state()), "CRANKING");
  advanceBy(p, 1);
  CHECK_STREQ(toString(p.state()), "UNCHOKING");

  advanceBy(p, 99);
  CHECK(p.chokeActive());
  advanceBy(p, 1);
  CHECK_STREQ(toString(p.state()), "RUNNING_ASSUMED");
  CHECK(!p.chokeActive());

  CHECK_EQ(starterOnMs(), 900u);
}

TEST(omitted_timing_headers_fall_back_to_the_configured_defaults) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  sendWithHeaders(p, "POST", "/v1/start", "X-Crank-Ms: 1200\r\n", status, "ov-mix");
  CHECK_EQ(status, 202);

  CHECK_EQ(p.activeTimings().chokePrepMs, CHOKE_PREP_MS);
  CHECK_EQ(p.activeTimings().crankMs, 1200u);
  CHECK_EQ(p.activeTimings().unchokeDelayMs, UNCHOKE_DELAY_MS);
}

TEST(a_crank_override_above_the_hard_ceiling_is_rejected_with_400) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  char header[64];
  snprintf(header, sizeof(header), "X-Crank-Ms: %lu\r\n",
           static_cast<unsigned long>(MAX_CRANK_MS + 1));

  uint16_t status = 0;
  const std::string body =
      sendWithHeaders(p, "POST", "/v1/start", header, status, "ov-toolong");

  CHECK_EQ(status, 400);
  CHECK_CONTAINS(body, "crank_ms_out_of_range");
  CHECK_MSG(p.state() == PumpState::IDLE,
            "an out-of-range override still started the pump");
  CHECK_EQ(starterOnMs(), 0u);
}

TEST(exactly_the_hard_ceiling_is_accepted) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  char header[64];
  snprintf(header, sizeof(header), "X-Crank-Ms: %lu\r\n",
           static_cast<unsigned long>(MAX_CRANK_MS));

  uint16_t status = 0;
  sendWithHeaders(p, "POST", "/v1/start", header, status, "ov-max");
  CHECK_EQ(status, 202);
  if (INTAKE_VALVE_ENABLED) advanceBy(p, VALVE_PRIME_MS);

  advanceBy(p, CHOKE_PREP_MS + MAX_CRANK_MS + UNCHOKE_DELAY_MS);
  CHECK_EQ(starterOnMs(), MAX_CRANK_MS);
  CHECK_MSG(starterOnMs() <= 5000, "starter exceeded the five second limit");
  // The backstop fires exactly at the ceiling, so this run ends in FAULT --
  // which is correct and safe: the starter is released either way.
  CHECK(!p.starterActive());
}

TEST(out_of_range_choke_and_unchoke_overrides_are_rejected) {
  struct Case { const char* header; const char* code; };
  char chokeHdr[64], unchokeHdr[64];
  snprintf(chokeHdr, sizeof(chokeHdr), "X-Choke-Ms: %lu\r\n",
           static_cast<unsigned long>(MAX_CHOKE_PREP_OVERRIDE_MS + 1));
  snprintf(unchokeHdr, sizeof(unchokeHdr), "X-Unchoke-Ms: %lu\r\n",
           static_cast<unsigned long>(MAX_UNCHOKE_DELAY_OVERRIDE_MS + 1));

  const Case cases[] = {
      {chokeHdr, "choke_ms_out_of_range"},
      {unchokeHdr, "unchoke_ms_out_of_range"},
  };
  for (const Case& c : cases) {
    PumpController p;
    bootAt(p, 0);
    driveToState(p, PumpState::IDLE);
    uint16_t status = 0;
    const std::string body =
        sendWithHeaders(p, "POST", "/v1/start", c.header, status, "ov-range");
    CHECK_MSG(status == 400, c.header);
    CHECK_CONTAINS(body, c.code);
    CHECK(p.state() == PumpState::IDLE);
  }
}

TEST(a_malformed_timing_header_is_rejected_with_400) {
  const char* bad[] = {
      "X-Crank-Ms: soon\r\n",
      "X-Crank-Ms: -500\r\n",
      "X-Crank-Ms: 1.5\r\n",
      "X-Crank-Ms: \r\n",
      "X-Choke-Ms: 0x10\r\n",
      "X-Unchoke-Ms: 99999999999\r\n",
  };
  for (const char* header : bad) {
    PumpController p;
    bootAt(p, 0);
    driveToState(p, PumpState::IDLE);
    uint16_t status = 0;
    const std::string body =
        sendWithHeaders(p, "POST", "/v1/start", header, status, "ov-bad");
    CHECK_MSG(status == 400, header);
    CHECK_CONTAINS(body, "invalid_timing_override");
    CHECK_MSG(p.state() == PumpState::IDLE, header);
  }
}

TEST(timing_headers_on_a_non_start_endpoint_are_rejected) {
  const char* paths[] = {"/v1/stop", "/v1/start-failed", "/v1/reset-idle"};
  for (const char* path : paths) {
    PumpController p;
    bootAt(p, 0);
    driveToState(p, PumpState::IDLE);
    uint16_t status = 0;
    const std::string body =
        sendWithHeaders(p, "POST", path, "X-Crank-Ms: 1000\r\n", status, "ov-wrong");
    CHECK_MSG(status == 400, path);
    CHECK_CONTAINS(body, "timing_override_not_applicable");
  }
}

TEST(a_zero_crank_override_never_engages_the_starter) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  sendWithHeaders(p, "POST", "/v1/start", "X-Crank-Ms: 0\r\n", status, "ov-zero");
  CHECK_EQ(status, 202);

  if (INTAKE_VALVE_ENABLED) advanceBy(p, VALVE_PRIME_MS);
  advanceBy(p, CHOKE_PREP_MS + UNCHOKE_DELAY_MS + 10);
  CHECK_STREQ(toString(p.state()), "RUNNING_ASSUMED");
  // The starter may flick on and straight back off within one tick; what
  // matters is that it is never held.
  CHECK(starterOnMs() <= 1u);
  CHECK(!p.starterActive());
}

TEST(overrides_do_not_survive_a_reboot) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);
  uint16_t status = 0;
  sendWithHeaders(p, "POST", "/v1/start", "X-Crank-Ms: 4000\r\n", status, "ov-boot");
  CHECK_EQ(p.activeTimings().crankMs, 4000u);

  bootAt(p, 500000);
  CHECK_EQ(p.activeTimings().crankMs, CRANK_DURATION_MS);
  CHECK_EQ(p.activeTimings().chokePrepMs, CHOKE_PREP_MS);
  CHECK_EQ(p.activeTimings().unchokeDelayMs, UNCHOKE_DELAY_MS);
}

TEST(the_controller_clamps_overrides_even_if_http_validation_is_bypassed) {
  // Defence in depth: call handleCommand directly with absurd values, as a
  // future caller might if it skipped the HTTP layer.
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  StartTimings evil;
  evil.chokePrepMs = 0xFFFFFFFFu;
  evil.crankMs = 0xFFFFFFFFu;
  evil.unchokeDelayMs = 0xFFFFFFFFu;

  const CommandResult r =
      p.handleCommand(CommandType::START, "evil-1", fake::nowMs, &evil);
  CHECK(r.accepted);
  CHECK_MSG(p.activeTimings().crankMs <= MAX_CRANK_MS,
            "crank override was not clamped to the hard ceiling");
  CHECK_MSG(p.activeTimings().chokePrepMs <= MAX_CHOKE_PREP_OVERRIDE_MS,
            "choke override was not clamped");
  CHECK_MSG(p.activeTimings().unchokeDelayMs <= MAX_UNCHOKE_DELAY_OVERRIDE_MS,
            "unchoke override was not clamped");

  advanceBy(p, 120000, 25);
  CHECK_MSG(starterOnMs() <= MAX_CRANK_MS, "starter exceeded MAX_CRANK_MS");
  CHECK(!p.starterActive());
}

TEST(status_reports_the_timings_in_use) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  std::string body = sendWithHeaders(p, "GET", "/v1/status", nullptr, status, nullptr);
  CHECK_EQ(status, 200);
  CHECK_CONTAINS(body, "\"choke_prep_ms\":1000");
  CHECK_CONTAINS(body, "\"crank_ms\":2000");
  CHECK_CONTAINS(body, "\"unchoke_delay_ms\":500");
  CHECK_CONTAINS(body, "\"kill_hold_ms\":3000");
  CHECK_CONTAINS(body, "\"min_recrank_gap_ms\":10000");
  CHECK_CONTAINS(body, "\"max_crank_ms\":5000");

  sendWithHeaders(p, "POST", "/v1/start", "X-Crank-Ms: 1234\r\n", status, "ov-status");
  body = sendWithHeaders(p, "GET", "/v1/status", nullptr, status, nullptr);
  CHECK_CONTAINS(body, "\"crank_ms\":1234");
  CHECK_MSG(body.find("\"max_crank_ms\":5000") != std::string::npos,
            "the hard ceiling must not move with an override");
}

TEST(status_advertises_whether_the_maintenance_api_is_enabled) {
  PumpController p;
  bootAt(p, 0);
  uint16_t status = 0;
  const std::string body =
      sendWithHeaders(p, "GET", "/v1/status", nullptr, status, nullptr);
  CHECK_CONTAINS(body, MAINTENANCE_API_ENABLED ? "\"maintenance_api\":true"
                                               : "\"maintenance_api\":false");
}

// ===========================================================================
// Maintenance API reachability
// ===========================================================================

namespace {
const char* const kMaintPaths[] = {
    "/v1/maintenance/choke/on",    "/v1/maintenance/choke/off",
    "/v1/maintenance/starter/on",  "/v1/maintenance/starter/off",
    "/v1/maintenance/kill/on",     "/v1/maintenance/kill/off",
};
}  // namespace

TEST(maintenance_routes_follow_the_compile_time_flag) {
  for (const char* path : kMaintPaths) {
    const Route r = resolveRoute(HttpMethod::POST, path);
    if (MAINTENANCE_API_ENABLED) {
      CHECK_MSG(r.action == RouteAction::COMMAND, path);
      CHECK_MSG(isMaintenanceCommand(r.command), path);
    } else {
      // Indistinguishable from any other unknown path.
      CHECK_MSG(r.action == RouteAction::ERROR_STATUS, path);
      CHECK_MSG(r.errorStatus == 404, path);
      CHECK_MSG(r.command == CommandType::NONE, path);
    }
  }
}

TEST(maintenance_endpoints_still_require_authentication) {
  for (const char* path : kMaintPaths) {
    PumpController p;
    bootAt(p, 0);
    uint16_t status = 0;
    std::string raw = "POST ";
    raw += path;
    raw += " HTTP/1.1\r\nX-Pump-Secret: wrong\r\nX-Request-ID: m1\r\n\r\n";
    doRequest(p, raw, status, fake::nowMs);
    CHECK_MSG(status == 401, path);
    CHECK_MSG(!p.chokeActive() && !p.starterActive() && !p.valveActive(), path);
  }
}

TEST(maintenance_relay_control_works_when_enabled) {
  if (!MAINTENANCE_API_ENABLED) {
    // The disabled case is covered by maintenance_routes_follow_the_flag.
    PumpController p;
    bootAt(p, 0);
    uint16_t status = 0;
    sendWithHeaders(p, "POST", "/v1/maintenance/choke/on", nullptr, status, "m-off");
    CHECK_EQ(status, 404);
    return;
  }

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  sendWithHeaders(p, "POST", "/v1/maintenance/choke/on", nullptr, status, "m-1");
  CHECK_EQ(status, 202);
  CHECK_MSG(p.chokeActive(), "manual choke-on did not energise the relay");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_CHOKE], activeLevel());

  sendWithHeaders(p, "POST", "/v1/maintenance/choke/off", nullptr, status, "m-2");
  CHECK_EQ(status, 202);
  CHECK(!p.chokeActive());
  CHECK_EQ(fake::pinLevel[PIN_RELAY_CHOKE], inactiveLevel());

  primeAndPermitCranking(p);
  sendWithHeaders(p, "POST", "/v1/maintenance/starter/on", nullptr, status, "m-3");
  CHECK_EQ(status, 202);
  CHECK(p.starterActive());

  sendWithHeaders(p, "POST", "/v1/maintenance/starter/off", nullptr, status, "m-4");
  CHECK_EQ(status, 202);
  CHECK(!p.starterActive());

  sendWithHeaders(p, "POST", "/v1/maintenance/kill/on", nullptr, status, "m-5");
  CHECK_EQ(status, 202);
  CHECK(p.killActive());

  sendWithHeaders(p, "POST", "/v1/maintenance/kill/off", nullptr, status, "m-6");
  CHECK_EQ(status, 202);
  CHECK(!p.killActive());

  checkStarterNeverCrankedWithKillAsserted("maintenance relay control");
}

TEST(maintenance_cannot_energise_the_starter_while_kill_is_grounded) {
  if (!MAINTENANCE_API_ENABLED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  sendWithHeaders(p, "POST", "/v1/maintenance/kill/on", nullptr, status, "k-1");
  CHECK_EQ(status, 202);
  CHECK(p.killActive());

  const std::string body = sendWithHeaders(
      p, "POST", "/v1/maintenance/starter/on", nullptr, status, "k-2");
  CHECK_MSG(status == 409, "starter-on was permitted while kill was grounded");
  CHECK_MSG(!p.starterActive(), "starter energised alongside kill");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_STARTER], inactiveLevel());
  CHECK_CONTAINS(body, "\"accepted\":false");

  // And the device is still healthy -- refusal, not a fault.
  CHECK_MSG(toString(p.fault()) == nullptr,
            "a refused maintenance command raised a fault");
}

TEST(maintenance_kill_on_releases_a_manually_engaged_starter_first) {
  if (!MAINTENANCE_API_ENABLED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  primeAndPermitCranking(p);
  sendWithHeaders(p, "POST", "/v1/maintenance/starter/on", nullptr, status, "o-1");
  CHECK(p.starterActive());

  const size_t before = fake::events.size();
  sendWithHeaders(p, "POST", "/v1/maintenance/kill/on", nullptr, status, "o-2");
  CHECK_EQ(status, 202);
  CHECK(!p.starterActive());
  CHECK(p.killActive());

  size_t starterOff = SIZE_MAX, killOn = SIZE_MAX;
  for (size_t i = before; i < fake::events.size(); ++i) {
    const fake::Event& e = fake::events[i];
    if (e.kind != fake::EventKind::DIGITAL_WRITE) continue;
    if (e.pin == PIN_RELAY_STARTER && e.value == inactiveLevel() && starterOff == SIZE_MAX) starterOff = i;
    // Asserting the kill means DE-energising K3, so its NC contact closes.
    if (e.pin == PIN_RELAY_KILL && e.value == killPinLevelFor(true) && killOn == SIZE_MAX) killOn = i;
  }
  CHECK(starterOff != SIZE_MAX && killOn != SIZE_MAX);
  CHECK_MSG(starterOff < killOn, "the ignition was grounded before the starter opened");
}

TEST(a_manually_engaged_starter_is_still_force_released_at_max_crank) {
  if (!MAINTENANCE_API_ENABLED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  primeAndPermitCranking(p);
  sendWithHeaders(p, "POST", "/v1/maintenance/starter/on", nullptr, status, "t-1");
  CHECK(p.starterActive());

  advanceBy(p, MAX_CRANK_MS + 100, 10);

  CHECK_MSG(!p.starterActive(),
            "a manually engaged starter survived past MAX_CRANK_MS");
  CHECK_EQ(fake::pinLevel[PIN_RELAY_STARTER], inactiveLevel());
  CHECK_STREQ(toString(p.state()), "FAULT");
  CHECK_STREQ(toString(p.fault()), "STARTER_OVERRUN");
  CHECK(starterOnMs() <= MAX_CRANK_MS);
}

TEST(a_manually_engaged_choke_is_still_force_released_at_max_choke) {
  if (!MAINTENANCE_API_ENABLED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  sendWithHeaders(p, "POST", "/v1/maintenance/choke/on", nullptr, status, "c-1");
  CHECK(p.chokeActive());

  advanceBy(p, MAX_CHOKE_MS + 100, 50);

  CHECK_MSG(!p.chokeActive(), "a manually engaged choke survived past MAX_CHOKE_MS");
  CHECK_STREQ(toString(p.fault()), "CHOKE_OVERRUN");
}

TEST(start_is_refused_while_a_relay_is_manually_energised) {
  if (!MAINTENANCE_API_ENABLED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  sendWithHeaders(p, "POST", "/v1/maintenance/choke/on", nullptr, status, "s-1");
  CHECK(p.chokeActive());
  CHECK_STREQ(toString(p.state()), "IDLE");   // still IDLE by design

  const std::string body =
      sendWithHeaders(p, "POST", "/v1/start", nullptr, status, "s-2");
  CHECK_MSG(status == 409,
            "START was layered on top of a manually energised relay");
  CHECK_CONTAINS(body, "\"accepted\":false");

  // Release it and START becomes available again.
  sendWithHeaders(p, "POST", "/v1/maintenance/choke/off", nullptr, status, "s-3");
  sendWithHeaders(p, "POST", "/v1/start", nullptr, status, "s-4");
  CHECK_EQ(status, 202);
}

TEST(maintenance_is_refused_during_an_active_sequence) {
  if (!MAINTENANCE_API_ENABLED) return;

  const PumpState busy[] = {PumpState::CHOKING, PumpState::CRANKING,
                            PumpState::UNCHOKING, PumpState::STOPPING,
                            PumpState::RUNNING_ASSUMED, PumpState::RETRY_WAIT,
                            PumpState::FAULT};
  for (PumpState s : busy) {
    PumpController p;
    bootAt(p, 0);
    CHECK(driveToState(p, s));
    const bool chokeBefore = p.chokeActive();

    uint16_t status = 0;
    sendWithHeaders(p, "POST", "/v1/maintenance/choke/on", nullptr, status, "b-1");
    CHECK_MSG(status == 409, "maintenance accepted mid-sequence");
    CHECK_MSG(p.chokeActive() == chokeBefore,
              "a refused maintenance command still moved a relay");
  }
}

TEST(stop_still_overrides_manually_energised_relays) {
  if (!MAINTENANCE_API_ENABLED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  primeAndPermitCranking(p);
  sendWithHeaders(p, "POST", "/v1/maintenance/starter/on", nullptr, status, "x-1");
  sendWithHeaders(p, "POST", "/v1/maintenance/choke/on", nullptr, status, "x-2");
  CHECK(p.starterActive());
  CHECK(p.chokeActive());

  sendWithHeaders(p, "POST", "/v1/stop", nullptr, status, "x-3");
  CHECK_EQ(status, 202);
  CHECK_MSG(!p.starterActive(), "STOP left a manual starter energised");
  CHECK_MSG(!p.chokeActive(), "STOP left a manual choke energised");
  CHECK(p.killActive());
  CHECK_STREQ(toString(p.state()), "STOPPING");
}

TEST(maintenance_commands_are_deduplicated_by_request_id) {
  if (!MAINTENANCE_API_ENABLED) return;

  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  const std::string a =
      sendWithHeaders(p, "POST", "/v1/maintenance/choke/on", nullptr, status, "d-1");
  CHECK_CONTAINS(a, "\"duplicate\":false");

  const std::string b =
      sendWithHeaders(p, "POST", "/v1/maintenance/choke/on", nullptr, status, "d-1");
  CHECK_CONTAINS(b, "\"duplicate\":true");
  CHECK(p.chokeActive());
}
