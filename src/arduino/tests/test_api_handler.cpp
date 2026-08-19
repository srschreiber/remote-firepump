// tests/test_api_handler.cpp — end-to-end API behaviour: raw request bytes in,
// HTTP status and JSON body out, against a live PumpController.

#include "test_support.h"

namespace {

// Sends a request built from parts and returns the body; status via out-param.
std::string send(PumpController& p, const char* method, const char* path,
                 const char* secret, const char* requestId, uint16_t& status) {
  return doRequest(p, makeRequest(method, path, secret, requestId), status,
                   fake::nowMs);
}

}  // namespace

// ===========================================================================
// Authentication
// ===========================================================================

TEST(a_missing_secret_returns_401_on_every_endpoint) {
  struct Ep { const char* m; const char* p; };
  const Ep endpoints[] = {
      {"GET", "/v1/status"},        {"POST", "/v1/start"},
      {"POST", "/v1/stop"},         {"POST", "/v1/start-failed"},
      {"POST", "/v1/reset-idle"},
  };
  for (const Ep& e : endpoints) {
    PumpController p;
    bootAt(p, 0);
    driveToState(p, PumpState::IDLE);

    uint16_t status = 0;
    const std::string body = send(p, e.m, e.p, nullptr, "req-1", status);
    CHECK_MSG(status == 401, e.p);
    CHECK_CONTAINS(body, "\"error\":\"unauthorized\"");
    CHECK_MSG(p.state() == PumpState::IDLE, "an unauthenticated request changed state");
    CHECK(!p.starterActive() && !p.chokeActive() && !p.valveActive());
  }
}

TEST(an_incorrect_secret_returns_401_and_never_moves_a_relay) {
  const char* wrong[] = {
      "",
      "x",
      "wrong-secret",
      "s3cr3t-abcdefghijklmnopqrstuvwxyz-012345678",   // one char short
      "s3cr3t-abcdefghijklmnopqrstuvwxyz-01234567890", // one char long
      "S3CR3T-ABCDEFGHIJKLMNOPQRSTUVWXYZ-0123456789",  // wrong case
      "s3cr3t-abcdefghijklmnopqrstuvwxyz-0123456788",  // last char differs
      "t3cr3t-abcdefghijklmnopqrstuvwxyz-0123456789",  // first char differs
  };
  for (const char* w : wrong) {
    PumpController p;
    bootAt(p, 0);
    driveToState(p, PumpState::IDLE);
    const size_t eventsBefore = fake::events.size();

    uint16_t status = 0;
    const std::string body = send(p, "POST", "/v1/start", w, "req-x", status);

    CHECK_MSG(status == 401, w);
    CHECK_CONTAINS(body, "\"accepted\":false");
    CHECK_MSG(p.state() == PumpState::IDLE, "a bad secret still started the pump");
    CHECK_MSG(fake::events.size() == eventsBefore,
              "a rejected request produced relay activity");
  }
}

TEST(the_correct_secret_is_accepted) {
  PumpController p;
  bootAt(p, 0);
  uint16_t status = 0;
  send(p, "GET", "/v1/status", kTestSecret, nullptr, status);
  CHECK_EQ(status, 200);
}

TEST(an_unconfigured_secret_fails_closed_with_401) {
  for (const char* configured : {static_cast<const char*>(nullptr), ""}) {
    PumpController p;
    bootAt(p, 0);
    uint16_t status = 0;
    doRequest(p, makeRequest("GET", "/v1/status", "anything", nullptr), status,
              fake::nowMs, configured);
    CHECK_MSG(status == 401, "an unconfigured device answered something other than 401");

    // Even sending an empty secret must not match an empty configured secret.
    doRequest(p, makeRequest("POST", "/v1/start", "", "r1"), status,
              fake::nowMs, configured);
    CHECK_EQ(status, 401);
    CHECK(p.state() == PumpState::UNKNOWN);
  }
}

TEST(authentication_is_checked_before_routing_so_endpoints_are_not_enumerable) {
  PumpController p;
  bootAt(p, 0);
  uint16_t known = 0, unknown = 0, wrongMethod = 0;

  send(p, "GET", "/v1/status", "bad", nullptr, known);
  send(p, "GET", "/v1/does-not-exist", "bad", nullptr, unknown);
  send(p, "POST", "/v1/status", "bad", nullptr, wrongMethod);

  CHECK_EQ(known, 401);
  CHECK_MSG(unknown == 401, "an unauthenticated caller could distinguish a 404");
  CHECK_MSG(wrongMethod == 401, "an unauthenticated caller could distinguish a 405");
}

TEST(no_response_body_ever_echoes_a_secret_value) {
  // Naming the header in an error message is fine and useful. What must never
  // appear is a secret *value* -- neither the one supplied by the caller nor
  // the one the device is configured with.
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  const char* attempts[] = {kTestSecret, "wrong-but-memorable-secret",
                            "partial-s3cr3t-abcdef", ""};
  const char* paths[] = {"/v1/status", "/v1/start", "/v1/stop",
                         "/v1/start-failed", "/v1/reset-idle", "/v1/nope"};
  for (const char* secret : attempts) {
    for (const char* path : paths) {
      for (const char* method : {"GET", "POST"}) {
        uint16_t status = 0;
        const std::string body = send(p, method, path, secret, "probe-1", status);

        // The configured secret must never appear, whatever happened.
        CHECK_NOT_CONTAINS(body, kTestSecret);
        // Nor must the caller's supplied value be reflected back.
        if (secret[0] != '\0') {
          CHECK_NOT_CONTAINS(body, secret);
        }
      }
    }
  }
}

// ===========================================================================
// Routing / method errors
// ===========================================================================

TEST(an_unknown_endpoint_returns_404_when_authenticated) {
  PumpController p;
  bootAt(p, 0);
  uint16_t status = 0;
  const std::string body = send(p, "GET", "/v1/nope", kTestSecret, nullptr, status);
  CHECK_EQ(status, 404);
  CHECK_CONTAINS(body, "\"error\":\"not_found\"");
}

TEST(the_wrong_method_returns_405_when_authenticated) {
  PumpController p;
  bootAt(p, 0);
  uint16_t a = 0, b = 0;
  const std::string bodyA = send(p, "GET", "/v1/start", kTestSecret, "r", a);
  CHECK_EQ(a, 405);
  CHECK_CONTAINS(bodyA, "\"error\":\"method_not_allowed\"");

  send(p, "POST", "/v1/status", kTestSecret, "r", b);
  CHECK_EQ(b, 405);
}

TEST(oversized_requests_return_413_and_leave_the_starter_alone) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  // A start request buried under an enormous header block.
  std::string raw = "POST /v1/start HTTP/1.1\r\nX-Pump-Secret: ";
  raw += kTestSecret;
  raw += "\r\nX-Request-ID: huge-1\r\n";
  for (int i = 0; i < 64; ++i) {
    raw += "X-Filler-" + std::to_string(i) + ": " + std::string(120, 'f') + "\r\n";
  }
  raw += "\r\n";

  uint16_t status = 0;
  const std::string body = doRequest(p, raw, status, fake::nowMs);
  CHECK_EQ(status, 413);
  CHECK_MSG(p.state() == PumpState::IDLE, "an oversized request started the pump");
  CHECK(!p.starterActive());
  CHECK_EQ(fake::pinLevel[PIN_RELAY_STARTER], inactiveLevel());
  CHECK_CONTAINS(body, "\"accepted\":false");
}

TEST(malformed_requests_return_400_and_leave_the_starter_alone) {
  const char* bad[] = {
      "not even http\r\n\r\n",
      "POST\r\n\r\n",
      "POST /v1/start\r\n\r\n",
      "POST /v1/start HTTP/1.1\r\nbroken-header\r\n\r\n",
      "\r\n\r\n",
      "\0\0\0\r\n\r\n",
  };
  for (const char* raw : bad) {
    PumpController p;
    bootAt(p, 0);
    driveToState(p, PumpState::IDLE);
    uint16_t status = 0;
    doRequest(p, std::string(raw), status, fake::nowMs);
    CHECK_MSG(status == 400 || status == 401 || status == 413, raw);
    CHECK_MSG(!p.starterActive(), "a malformed request energised the starter");
    CHECK_MSG(p.state() == PumpState::IDLE, "a malformed request changed state");
  }
}

TEST(an_incomplete_request_never_executes_a_command) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  // Everything but the terminating blank line.
  std::string raw = "POST /v1/start HTTP/1.1\r\nX-Pump-Secret: ";
  raw += kTestSecret;
  raw += "\r\nX-Request-ID: partial-1\r\n";

  uint16_t status = 0;
  doRequest(p, raw, status, fake::nowMs);
  CHECK_EQ(status, 400);
  CHECK_MSG(p.state() == PumpState::IDLE, "a truncated request started the pump");
  CHECK(!p.starterActive() && !p.chokeActive());
}

// ===========================================================================
// Request IDs
// ===========================================================================

TEST(an_invalid_request_id_returns_400_without_executing) {
  const char* badIds[] = {"has space", "has/slash", "semi;colon", "dot.dot",
                          "pct%20", "quote\""};
  for (const char* id : badIds) {
    PumpController p;
    bootAt(p, 0);
    driveToState(p, PumpState::IDLE);
    uint16_t status = 0;
    const std::string body = send(p, "POST", "/v1/start", kTestSecret, id, status);
    CHECK_MSG(status == 400, id);
    CHECK_CONTAINS(body, "\"error\":\"invalid_request_id\"");
    CHECK_MSG(p.state() == PumpState::IDLE, "an invalid request ID still started the pump");
  }
}

TEST(a_missing_request_id_is_accepted_but_forfeits_deduplication) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  const std::string body = send(p, "POST", "/v1/stop", kTestSecret, nullptr, status);
  CHECK_EQ(status, 202);
  CHECK_CONTAINS(body, "\"duplicate\":false");
  CHECK_CONTAINS(body, "\"request_id\":\"\"");
}

// ===========================================================================
// /v1/status
// ===========================================================================

TEST(status_reports_the_live_controller_and_network_state) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::CRANKING);

  uint16_t status = 0;
  const std::string body = send(p, "GET", "/v1/status", kTestSecret, nullptr, status);
  CHECK_EQ(status, 200);
  CHECK_CONTAINS(body, "\"state\":\"CRANKING\"");
  CHECK_CONTAINS(body, "\"engine_status\":\"STARTING\"");
  CHECK_CONTAINS(body, "\"starter\":true");
  CHECK_CONTAINS(body, "\"choke\":true");
  CHECK_CONTAINS(body, "\"kill\":false");
  CHECK_CONTAINS(body, "\"valve\":true");
  CHECK_CONTAINS(body, "\"ip\":\"192.168.1.50\"");
  CHECK_CONTAINS(body, "\"rssi_dbm\":-57");
  CHECK_CONTAINS(body, "\"device\":\"fire-pump-controller\"");
  CHECK_CONTAINS(body, "\"firmware_version\":\"0.2.0\"");
}

TEST(status_never_reports_confirmed_engine_operation_in_any_state) {
  const PumpState all[] = {
      PumpState::UNKNOWN,   PumpState::IDLE,        PumpState::PRIMING,
      PumpState::CHOKING,   PumpState::CRANKING,    PumpState::UNCHOKING,
      PumpState::RUNNING_ASSUMED, PumpState::STOPPING, PumpState::VALVE_CLOSING,
      PumpState::RETRY_WAIT, PumpState::FAULT,
  };
  for (PumpState s : all) {
    PumpController p;
    bootAt(p, 0);
    CHECK(driveToState(p, s));
    uint16_t status = 0;
    const std::string body = send(p, "GET", "/v1/status", kTestSecret, nullptr, status);
    CHECK_EQ(status, 200);
    CHECK_MSG(body.find("\"running_confirmed\":false") != std::string::npos,
              "running_confirmed was not false");
    CHECK_NOT_CONTAINS(body, "\"running_confirmed\":true");
  }
}

TEST(status_reports_a_fault_and_clears_it_after_recovery) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::FAULT);

  uint16_t status = 0;
  std::string body = send(p, "GET", "/v1/status", kTestSecret, nullptr, status);
  CHECK_CONTAINS(body, "\"state\":\"FAULT\"");
  CHECK_CONTAINS(body, "\"fault\":\"STARTER_OVERRUN\"");

  advanceBy(p, KILL_HOLD_MS + VALVE_CLOSE_DELAY_MS);
  send(p, "POST", "/v1/reset-idle", kTestSecret, "clear-1", status);
  CHECK_EQ(status, 202);

  body = send(p, "GET", "/v1/status", kTestSecret, nullptr, status);
  CHECK_CONTAINS(body, "\"fault\":null");
}

TEST(status_reports_the_last_command) {
  PumpController p;
  bootAt(p, 0);
  uint16_t status = 0;
  send(p, "POST", "/v1/reset-idle", kTestSecret, "reset-001", status);
  const std::string body = send(p, "GET", "/v1/status", kTestSecret, nullptr, status);
  CHECK_CONTAINS(body, "\"last_command\":{\"type\":\"RESET_IDLE\","
                       "\"request_id\":\"reset-001\",\"accepted\":true}");
}

// ===========================================================================
// Command endpoints
// ===========================================================================

TEST(start_returns_202_from_idle_and_409_otherwise) {
  PumpController p;
  bootAt(p, 0);

  uint16_t status = 0;
  std::string body = send(p, "POST", "/v1/start", kTestSecret, "start-001", status);
  CHECK_EQ(status, 409);
  CHECK_CONTAINS(body, "\"state\":\"UNKNOWN\"");
  CHECK_CONTAINS(body, "cooldown_remaining_ms");
  CHECK_CONTAINS(body, "\"error\":\"invalid_state_for_command\"");

  send(p, "POST", "/v1/reset-idle", kTestSecret, "reset-001", status);
  CHECK_EQ(status, 202);

  body = send(p, "POST", "/v1/start", kTestSecret, "start-002", status);
  CHECK_EQ(status, 202);
  CHECK_CONTAINS(body, "\"accepted\":true");
  CHECK_CONTAINS(body, INTAKE_VALVE_ENABLED ? "\"state\":\"PRIMING\"" : "\"state\":\"CHOKING\"");
  CHECK_CONTAINS(body, "\"request_id\":\"start-002\"");
}

TEST(a_409_reports_the_current_state_and_cooldown_remaining) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);

  uint16_t status = 0;
  send(p, "POST", "/v1/stop", kTestSecret, "stop-1", status);
  advanceBy(p, KILL_HOLD_MS + VALVE_CLOSE_DELAY_MS);

  const std::string body = send(p, "POST", "/v1/start", kTestSecret, "start-9", status);
  CHECK_EQ(status, 409);
  CHECK_CONTAINS(body, "\"state\":\"IDLE\"");
  // Cooldown must be a positive number, not zero.
  CHECK_NOT_CONTAINS(body, "\"cooldown_remaining_ms\":0}");
}

TEST(stop_returns_202_from_every_state_over_http) {
  const PumpState all[] = {
      PumpState::UNKNOWN,   PumpState::IDLE,        PumpState::PRIMING,
      PumpState::CHOKING,   PumpState::CRANKING,    PumpState::UNCHOKING,
      PumpState::RUNNING_ASSUMED, PumpState::STOPPING, PumpState::VALVE_CLOSING,
      PumpState::RETRY_WAIT, PumpState::FAULT,
  };
  for (PumpState s : all) {
    PumpController p;
    bootAt(p, 0);
    CHECK(driveToState(p, s));

    uint16_t status = 0;
    const std::string body = send(p, "POST", "/v1/stop", kTestSecret, "stop-any", status);
    CHECK_MSG(status == 202, "STOP was not accepted over HTTP");
    CHECK_CONTAINS(body, "\"state\":\"STOPPING\"");
    CHECK(p.killActive());
    CHECK(!p.starterActive());
  }
}

TEST(start_failed_returns_202_and_enters_retry_wait) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::RUNNING_ASSUMED);

  uint16_t status = 0;
  const std::string body =
      send(p, "POST", "/v1/start-failed", kTestSecret, "failed-001", status);
  CHECK_EQ(status, 202);
  CHECK_CONTAINS(body, "\"state\":\"RETRY_WAIT\"");
  CHECK_CONTAINS(body, "\"accepted\":true");
}

TEST(reset_idle_returns_202_and_reports_the_resulting_state) {
  PumpController p;
  bootAt(p, 0);
  uint16_t status = 0;
  const std::string body =
      send(p, "POST", "/v1/reset-idle", kTestSecret, "reset-001", status);
  CHECK_EQ(status, 202);
  CHECK_CONTAINS(body, "\"state\":\"IDLE\"");
}

// ===========================================================================
// Idempotency over HTTP
// ===========================================================================

TEST(a_replayed_start_request_is_marked_duplicate_and_cranks_only_once) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t s1 = 0, s2 = 0, s3 = 0;
  const std::string b1 = send(p, "POST", "/v1/start", kTestSecret, "start-001", s1);
  const std::string b2 = send(p, "POST", "/v1/start", kTestSecret, "start-001", s2);
  const std::string b3 = send(p, "POST", "/v1/start", kTestSecret, "start-001", s3);

  if (INTAKE_VALVE_ENABLED) advanceBy(p, VALVE_PRIME_MS);

  CHECK_EQ(s1, 202);
  CHECK_CONTAINS(b1, "\"duplicate\":false");
  CHECK_EQ(s2, 202);
  CHECK_CONTAINS(b2, "\"duplicate\":true");
  CHECK_EQ(s3, 202);
  CHECK_CONTAINS(b3, "\"duplicate\":true");

  advanceBy(p, CHOKE_PREP_MS + CRANK_DURATION_MS + UNCHOKE_DELAY_MS);
  CHECK_STREQ(toString(p.state()), "RUNNING_ASSUMED");

  int activations = 0;
  bool on = false;
  for (const fake::Event& e : fake::events) {
    if (e.kind != fake::EventKind::DIGITAL_WRITE || e.pin != PIN_RELAY_STARTER) continue;
    const bool nowOn = (e.value == activeLevel());
    if (nowOn && !on) ++activations;
    on = nowOn;
  }
  CHECK_MSG(activations == 1, "a replayed request cranked the engine twice");
}

TEST(duplicate_responses_report_the_current_state_not_a_stale_one) {
  PumpController p;
  bootAt(p, 0);
  driveToState(p, PumpState::IDLE);

  uint16_t status = 0;
  send(p, "POST", "/v1/start", kTestSecret, "start-001", status);
  if (INTAKE_VALVE_ENABLED) advanceBy(p, VALVE_PRIME_MS);
  advanceBy(p, CHOKE_PREP_MS);   // now CRANKING

  const std::string body = send(p, "POST", "/v1/start", kTestSecret, "start-001", status);
  CHECK_EQ(status, 202);
  CHECK_CONTAINS(body, "\"duplicate\":true");
  CHECK_CONTAINS(body, "\"state\":\"CRANKING\"");
}

// ===========================================================================
// Full byte-stream integration
// ===========================================================================

TEST(a_realistic_curl_request_is_handled_end_to_end) {
  PumpController p;
  bootAt(p, 0);

  std::string raw =
      "POST /v1/reset-idle HTTP/1.1\r\n"
      "Host: 192.168.1.50:8080\r\n"
      "User-Agent: curl/8.4.0\r\n"
      "Accept: */*\r\n"
      "X-Pump-Secret: ";
  raw += kTestSecret;
  raw += "\r\nX-Request-ID: reset-001\r\n\r\n";

  uint16_t status = 0;
  const std::string body = doRequest(p, raw, status, fake::nowMs);
  CHECK_EQ(status, 202);
  CHECK_CONTAINS(body, "\"state\":\"IDLE\"");

  // And the response head that would go on the wire.
  char head[HTTP_HEAD_MAX];
  const size_t n = buildResponseHead(head, sizeof(head), status, body.size());
  CHECK(n > 0);
  CHECK_CONTAINS(std::string(head), "Connection: close");
  CHECK_CONTAINS(std::string(head),
                 "Content-Length: " + std::to_string(body.size()));
}

TEST(a_request_body_is_ignored_rather_than_parsed) {
  PumpController p;
  bootAt(p, 0);

  std::string raw = "POST /v1/reset-idle HTTP/1.1\r\nX-Pump-Secret: ";
  raw += kTestSecret;
  raw += "\r\nX-Request-ID: body-1\r\nContent-Length: 27\r\n\r\n";
  raw += "{\"malicious\": \"payload\"}!!";

  uint16_t status = 0;
  const std::string body = doRequest(p, raw, status, fake::nowMs);
  CHECK_EQ(status, 202);
  CHECK_STREQ(toString(p.state()), "IDLE");
  CHECK_NOT_CONTAINS(body, "malicious");
}
