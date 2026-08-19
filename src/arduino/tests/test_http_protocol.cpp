// tests/test_http_protocol.cpp — request parsing, routing and JSON generation.

#include "test_support.h"

namespace {

// Feeds a whole request into a parser and returns the final status.
ParseStatus feedAll(RequestParser& p, const std::string& raw) {
  ParseStatus st = ParseStatus::INCOMPLETE;
  for (char c : raw) {
    st = p.feed(c);
    if (st != ParseStatus::INCOMPLETE) {
      break;
    }
  }
  return st;
}

std::string repeat(char c, size_t n) { return std::string(n, c); }

}  // namespace

// ===========================================================================
// Request line
// ===========================================================================

TEST(parses_a_well_formed_get_request) {
  RequestParser p;
  CHECK(feedAll(p, "GET /v1/status HTTP/1.1\r\nHost: x\r\n\r\n") ==
        ParseStatus::COMPLETE);
  CHECK(p.request().method == HttpMethod::GET);
  CHECK_STREQ(p.request().path, "/v1/status");
}

TEST(parses_a_well_formed_post_request) {
  RequestParser p;
  CHECK(feedAll(p, "POST /v1/start HTTP/1.1\r\n\r\n") == ParseStatus::COMPLETE);
  CHECK(p.request().method == HttpMethod::POST);
  CHECK_STREQ(p.request().path, "/v1/start");
}

TEST(accepts_bare_lf_line_endings) {
  RequestParser p;
  CHECK(feedAll(p, "GET /v1/status HTTP/1.1\nHost: x\n\n") ==
        ParseStatus::COMPLETE);
  CHECK_STREQ(p.request().path, "/v1/status");
}

TEST(unknown_methods_are_recognised_but_not_confused_with_get_or_post) {
  const char* methods[] = {"PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "TRACE"};
  for (const char* m : methods) {
    RequestParser p;
    const std::string raw = std::string(m) + " /v1/start HTTP/1.1\r\n\r\n";
    CHECK(feedAll(p, raw) == ParseStatus::COMPLETE);
    CHECK_MSG(p.request().method == HttpMethod::OTHER, m);
  }
}

TEST(query_strings_and_fragments_are_stripped_from_the_path) {
  RequestParser a;
  CHECK(feedAll(a, "GET /v1/status?verbose=1 HTTP/1.1\r\n\r\n") ==
        ParseStatus::COMPLETE);
  CHECK_STREQ(a.request().path, "/v1/status");

  RequestParser b;
  CHECK(feedAll(b, "POST /v1/stop#frag HTTP/1.1\r\n\r\n") == ParseStatus::COMPLETE);
  CHECK_STREQ(b.request().path, "/v1/stop");
}

TEST(malformed_request_lines_are_rejected_with_400) {
  const char* bad[] = {
      "GET\r\n\r\n",                        // no target, no version
      "GET /v1/status\r\n\r\n",             // no version
      "/v1/status HTTP/1.1\r\n\r\n",        // no method
      " /v1/status HTTP/1.1\r\n\r\n",       // empty method
      "GET  HTTP/1.1\r\n\r\n",              // empty target
      "GET /v1/status XYZZY/1.1\r\n\r\n",   // not an HTTP version
      "GET http://host/v1/status HTTP/1.1\r\n\r\n",  // absolute-form target
      "\r\n\r\n",                           // empty request line
  };
  for (const char* raw : bad) {
    RequestParser p;
    const ParseStatus st = feedAll(p, raw);
    CHECK_MSG(st == ParseStatus::FAILED, raw);
    CHECK_MSG(p.errorStatus() == 400, raw);
  }
}

TEST(an_over_long_request_line_is_rejected_with_413) {
  RequestParser p;
  const std::string raw =
      "GET /" + repeat('a', HTTP_MAX_REQUEST_LINE + 64) + " HTTP/1.1\r\n\r\n";
  CHECK(feedAll(p, raw) == ParseStatus::FAILED);
  CHECK_EQ(p.errorStatus(), 413);
  CHECK_STREQ(p.errorCode(), "request_line_too_long");
}

TEST(a_long_but_legal_path_falls_through_to_a_404_rather_than_matching) {
  RequestParser p;
  const std::string raw =
      "POST /v1/start" + repeat('x', HTTP_PATH_MAX + 8) + " HTTP/1.1\r\n\r\n";
  const ParseStatus st = feedAll(p, raw);
  if (st == ParseStatus::COMPLETE) {
    // Truncation can only shorten, so it can never collide with a real route.
    const Route r = resolveRoute(p.request().method, p.request().path);
    CHECK(r.action == RouteAction::ERROR_STATUS);
    CHECK_EQ(r.errorStatus, 404);
  } else {
    CHECK_EQ(p.errorStatus(), 413);
  }
}

// ===========================================================================
// Header limits
// ===========================================================================

TEST(an_over_long_single_header_line_is_rejected_with_413) {
  RequestParser p;
  const std::string raw = "GET /v1/status HTTP/1.1\r\nX-Pad: " +
                          repeat('z', HTTP_MAX_LINE_BYTES + 32) + "\r\n\r\n";
  CHECK(feedAll(p, raw) == ParseStatus::FAILED);
  CHECK_EQ(p.errorStatus(), 413);
}

TEST(an_over_long_total_header_section_is_rejected_with_413) {
  RequestParser p;
  std::string raw = "GET /v1/status HTTP/1.1\r\n";
  // Many individually-legal headers that together blow the 2 KB budget.
  for (int i = 0; i < 64; ++i) {
    raw += "X-Pad-" + std::to_string(i) + ": " + repeat('q', 100) + "\r\n";
  }
  raw += "\r\n";
  CHECK(feedAll(p, raw) == ParseStatus::FAILED);
  CHECK_EQ(p.errorStatus(), 413);
  CHECK_STREQ(p.errorCode(), "headers_too_large");
}

TEST(a_header_without_a_colon_is_rejected_with_400) {
  RequestParser p;
  CHECK(feedAll(p, "GET /v1/status HTTP/1.1\r\nthis-is-not-a-header\r\n\r\n") ==
        ParseStatus::FAILED);
  CHECK_EQ(p.errorStatus(), 400);
  CHECK_STREQ(p.errorCode(), "malformed_header");
}

TEST(a_header_with_an_empty_name_is_rejected_with_400) {
  RequestParser p;
  CHECK(feedAll(p, "GET /v1/status HTTP/1.1\r\n: value\r\n\r\n") ==
        ParseStatus::FAILED);
  CHECK_EQ(p.errorStatus(), 400);
}

TEST(an_incomplete_request_never_reports_complete) {
  RequestParser p;
  // Headers started but the blank line never arrives.
  const ParseStatus st = feedAll(p, "GET /v1/status HTTP/1.1\r\nHost: x\r\n");
  CHECK(st == ParseStatus::INCOMPLETE);
  CHECK(p.status() == ParseStatus::INCOMPLETE);
}

TEST(parser_latches_its_result_and_ignores_trailing_bytes) {
  RequestParser p;
  CHECK(feedAll(p, "GET /v1/status HTTP/1.1\r\n\r\n") == ParseStatus::COMPLETE);
  for (int i = 0; i < 100; ++i) {
    CHECK(p.feed('X') == ParseStatus::COMPLETE);
  }
  CHECK_STREQ(p.request().path, "/v1/status");
}

TEST(reset_clears_every_field_including_the_secret) {
  RequestParser p;
  feedAll(p, "POST /v1/start HTTP/1.1\r\nX-Pump-Secret: hunter2\r\n"
             "X-Request-ID: abc\r\n\r\n");
  CHECK(p.request().secretPresent);
  p.reset();
  CHECK(p.status() == ParseStatus::INCOMPLETE);
  CHECK(!p.request().secretPresent);
  CHECK_EQ(p.request().secretLen, 0u);
  CHECK(!p.request().requestIdPresent);
  CHECK_STREQ(p.request().path, "");
}

// ===========================================================================
// Header extraction
// ===========================================================================

TEST(header_names_are_matched_case_insensitively) {
  const char* variants[] = {"X-Pump-Secret", "x-pump-secret", "X-PUMP-SECRET",
                            "x-PuMp-SeCrEt"};
  for (const char* name : variants) {
    RequestParser p;
    const std::string raw = std::string("GET /v1/status HTTP/1.1\r\n") + name +
                            ": abc123\r\n\r\n";
    CHECK(feedAll(p, raw) == ParseStatus::COMPLETE);
    CHECK_MSG(p.request().secretPresent, name);
    CHECK_STREQ(p.request().secret, "abc123");
  }
}

TEST(header_values_have_surrounding_whitespace_trimmed) {
  RequestParser p;
  CHECK(feedAll(p, "GET /v1/status HTTP/1.1\r\n"
                   "X-Pump-Secret: \t  spaced-out  \t\r\n\r\n") ==
        ParseStatus::COMPLETE);
  CHECK_STREQ(p.request().secret, "spaced-out");
  CHECK_EQ(p.request().secretLen, 10u);
}

TEST(an_over_long_secret_is_stored_as_empty_so_it_can_never_match) {
  RequestParser p;
  const std::string raw = "GET /v1/status HTTP/1.1\r\nX-Pump-Secret: " +
                          repeat('k', HTTP_SECRET_MAX + 4) + "\r\n\r\n";
  const ParseStatus st = feedAll(p, raw);
  if (st == ParseStatus::COMPLETE) {
    CHECK(p.request().secretPresent);
    CHECK_EQ(p.request().secretLen, 0u);
  } else {
    CHECK_EQ(p.errorStatus(), 413);
  }
}

TEST(scrub_secret_wipes_the_buffer) {
  RequestParser p;
  feedAll(p, "GET /v1/status HTTP/1.1\r\nX-Pump-Secret: topsecret\r\n\r\n");
  CHECK_STREQ(p.request().secret, "topsecret");
  p.scrubSecret();
  CHECK_STREQ(p.request().secret, "");
  CHECK_EQ(p.request().secretLen, 0u);
}

TEST(request_id_validation_accepts_only_the_documented_alphabet) {
  struct Case { const char* id; bool valid; };
  const Case cases[] = {
      {"abc", true},
      {"ABC123", true},
      {"start-001", true},
      {"reset_idle_9", true},
      {"a", true},
      {"-", true},
      {"_", true},
      {"", false},
      {"has space", false},
      {"has.dot", false},
      {"has/slash", false},
      {"has:colon", false},
      {"semi;colon", false},
      {"quote\"", false},
      {"back\\slash", false},
      {"pct%20", false},
      {"plus+", false},
      {"tab\there", false},
  };
  for (const Case& c : cases) {
    CHECK_MSG(isValidRequestId(c.id) == c.valid, c.id);
  }

  // Exactly at the limit is fine; one over is not.
  const std::string atLimit = repeat('a', REQUEST_ID_MAX_LEN);
  const std::string overLimit = repeat('a', REQUEST_ID_MAX_LEN + 1);
  CHECK(isValidRequestId(atLimit.c_str()));
  CHECK(!isValidRequestId(overLimit.c_str()));
  CHECK(!isValidRequestId(nullptr));
}

TEST(invalid_request_id_headers_are_flagged_but_do_not_fail_the_parse) {
  RequestParser p;
  CHECK(feedAll(p, "POST /v1/start HTTP/1.1\r\nX-Request-ID: bad id!\r\n\r\n") ==
        ParseStatus::COMPLETE);
  CHECK(p.request().requestIdPresent);
  CHECK(!p.request().requestIdValid);
  CHECK_STREQ(p.request().requestId, "");
}

TEST(an_over_long_request_id_is_flagged_invalid) {
  RequestParser p;
  const std::string raw = "POST /v1/start HTTP/1.1\r\nX-Request-ID: " +
                          repeat('a', REQUEST_ID_MAX_LEN + 1) + "\r\n\r\n";
  CHECK(feedAll(p, raw) == ParseStatus::COMPLETE);
  CHECK(p.request().requestIdPresent);
  CHECK(!p.request().requestIdValid);
}

TEST(a_maximum_length_request_id_is_accepted_intact) {
  RequestParser p;
  const std::string id = repeat('z', REQUEST_ID_MAX_LEN);
  const std::string raw =
      "POST /v1/start HTTP/1.1\r\nX-Request-ID: " + id + "\r\n\r\n";
  CHECK(feedAll(p, raw) == ParseStatus::COMPLETE);
  CHECK(p.request().requestIdValid);
  CHECK_STREQ(p.request().requestId, id.c_str());
}

TEST(unknown_headers_are_ignored) {
  RequestParser p;
  CHECK(feedAll(p, "GET /v1/status HTTP/1.1\r\n"
                   "User-Agent: curl/8.0\r\n"
                   "Accept: */*\r\n"
                   "Content-Length: 0\r\n"
                   "X-Pump-Secret: ok\r\n\r\n") == ParseStatus::COMPLETE);
  CHECK_STREQ(p.request().secret, "ok");
}

// ===========================================================================
// Routing
// ===========================================================================

TEST(every_documented_endpoint_routes_correctly) {
  struct Case { const char* path; HttpMethod m; CommandType cmd; RouteAction act; };
  const Case cases[] = {
      {"/v1/status",       HttpMethod::GET,  CommandType::NONE,         RouteAction::STATUS},
      {"/v1/start",        HttpMethod::POST, CommandType::START,        RouteAction::COMMAND},
      {"/v1/stop",         HttpMethod::POST, CommandType::STOP,         RouteAction::COMMAND},
      {"/v1/start-failed", HttpMethod::POST, CommandType::START_FAILED, RouteAction::COMMAND},
      {"/v1/reset-idle",   HttpMethod::POST, CommandType::RESET_IDLE,   RouteAction::COMMAND},
  };
  for (const Case& c : cases) {
    const Route r = resolveRoute(c.m, c.path);
    CHECK_MSG(r.action == c.act, c.path);
    CHECK_MSG(r.command == c.cmd, c.path);
    CHECK_MSG(r.errorStatus == 0, c.path);
  }
}

TEST(known_paths_with_the_wrong_method_return_405) {
  const char* paths[] = {"/v1/status", "/v1/start", "/v1/stop",
                         "/v1/start-failed", "/v1/reset-idle"};
  for (const char* path : paths) {
    // Deliberately use the opposite of the correct verb, plus a third one.
    const HttpMethod wrong =
        (strcmp(path, "/v1/status") == 0) ? HttpMethod::POST : HttpMethod::GET;
    const Route a = resolveRoute(wrong, path);
    CHECK_MSG(a.action == RouteAction::ERROR_STATUS, path);
    CHECK_MSG(a.errorStatus == 405, path);
    CHECK_STREQ(a.errorCode, "method_not_allowed");

    const Route b = resolveRoute(HttpMethod::OTHER, path);
    CHECK_MSG(b.errorStatus == 405, path);
  }
}

TEST(unknown_paths_return_404) {
  const char* paths[] = {"/", "/v1", "/v1/", "/v1/statuss", "/v2/status",
                         "/v1/relay", "/v1/starter", "/admin", "/v1/status/",
                         "/V1/STATUS"};
  for (const char* path : paths) {
    const Route r = resolveRoute(HttpMethod::GET, path);
    CHECK_MSG(r.action == RouteAction::ERROR_STATUS, path);
    CHECK_MSG(r.errorStatus == 404, path);
  }
}

TEST(no_endpoint_exposes_direct_relay_control) {
  // Guards against a future edit accidentally re-adding a raw relay endpoint.
  const char* forbidden[] = {"/v1/relay", "/v1/relays", "/v1/starter",
                             "/v1/choke", "/v1/kill", "/v1/spare",
                             "/v1/gpio", "/v1/pin", "/v1/test"};
  for (const char* path : forbidden) {
    for (HttpMethod m : {HttpMethod::GET, HttpMethod::POST, HttpMethod::OTHER}) {
      const Route r = resolveRoute(m, path);
      CHECK_MSG(r.action == RouteAction::ERROR_STATUS, path);
      CHECK_MSG(r.command == CommandType::NONE, path);
    }
  }
}

// ===========================================================================
// Constant-time comparison
// ===========================================================================

TEST(constant_time_equals_behaves_like_a_correct_comparison) {
  CHECK(constantTimeEquals("abc", 3, "abc", 3));
  CHECK(!constantTimeEquals("abc", 3, "abd", 3));
  CHECK(!constantTimeEquals("abc", 3, "abcd", 4));   // prefix
  CHECK(!constantTimeEquals("abcd", 4, "abc", 3));   // suffix
  CHECK(!constantTimeEquals("", 0, "abc", 3));
  CHECK(!constantTimeEquals("abc", 3, "", 0));
  CHECK(constantTimeEquals("", 0, "", 0));
  CHECK(!constantTimeEquals(nullptr, 0, "abc", 3));
  CHECK(!constantTimeEquals("abc", 3, nullptr, 0));

  // Differences at every position are all detected.
  const std::string base = "correct-horse-battery-staple";
  for (size_t i = 0; i < base.size(); ++i) {
    std::string mutated = base;
    mutated[i] = static_cast<char>(mutated[i] ^ 0x01);
    CHECK_MSG(!constantTimeEquals(mutated.c_str(), mutated.size(),
                                  base.c_str(), base.size()),
              "a single-bit difference was not detected");
  }
  CHECK(constantTimeEquals(base.c_str(), base.size(), base.c_str(), base.size()));

  // Embedded NULs are still compared over the stated length.
  const char a[] = {'x', '\0', 'y'};
  const char b[] = {'x', '\0', 'z'};
  CHECK(!constantTimeEquals(a, 3, b, 3));
  CHECK(constantTimeEquals(a, 3, a, 3));
}

// ===========================================================================
// JSON generation
// ===========================================================================

TEST(status_json_contains_every_documented_field) {
  StatusView v;
  v.device = "fire-pump-controller";
  v.firmwareVersion = "0.1.0";
  v.state = "IDLE";
  v.stateElapsedMs = 12345;
  v.uptimeMs = 456789;
  v.engineStatus = "STOPPED_ASSUMED";
  v.runningConfirmed = false;
  v.wifiConnected = true;
  v.ip = "192.168.1.50";
  v.rssiDbm = -57;
  v.cooldownRemainingMs = 0;
  v.hasLastCommand = true;
  v.lastCommandType = "STOP";
  v.lastCommandRequestId = "stop-123";
  v.lastCommandAccepted = true;
  v.fault = nullptr;

  char buf[HTTP_BODY_MAX];
  const size_t n = buildStatusJson(buf, sizeof(buf), v);
  CHECK(n > 0);
  CHECK_EQ(strlen(buf), n);

  const std::string j(buf);
  CHECK_CONTAINS(j, "\"device\":\"fire-pump-controller\"");
  CHECK_CONTAINS(j, "\"firmware_version\":\"0.1.0\"");
  CHECK_CONTAINS(j, "\"state\":\"IDLE\"");
  CHECK_CONTAINS(j, "\"state_elapsed_ms\":12345");
  CHECK_CONTAINS(j, "\"uptime_ms\":456789");
  CHECK_CONTAINS(j, "\"engine_status\":\"STOPPED_ASSUMED\"");
  CHECK_CONTAINS(j, "\"running_confirmed\":false");
  CHECK_CONTAINS(j, "\"relay_outputs\":{\"starter\":false,\"choke\":false,"
                    "\"kill\":false,\"valve\":false}");
  CHECK_CONTAINS(j, "\"wifi\":{\"connected\":true,\"ip\":\"192.168.1.50\","
                    "\"rssi_dbm\":-57}");
  CHECK_CONTAINS(j, "\"cooldown_remaining_ms\":0");
  CHECK_CONTAINS(j, "\"last_command\":{\"type\":\"STOP\","
                    "\"request_id\":\"stop-123\",\"accepted\":true}");
  CHECK_CONTAINS(j, "\"fault\":null");
  CHECK(j.front() == '{' && j.back() == '}');
}

TEST(status_json_renders_relay_and_fault_variations) {
  StatusView v;
  v.state = "FAULT";
  v.starter = true;
  v.choke = true;
  v.kill = true;
  v.valve = true;
  v.fault = "STARTER_OVERRUN";
  v.hasLastCommand = false;
  v.wifiConnected = false;
  v.rssiDbm = 0;

  char buf[HTTP_BODY_MAX];
  CHECK(buildStatusJson(buf, sizeof(buf), v) > 0);
  const std::string j(buf);
  CHECK_CONTAINS(j, "\"starter\":true,\"choke\":true,\"kill\":true,\"valve\":true");
  CHECK_CONTAINS(j, "\"fault\":\"STARTER_OVERRUN\"");
  CHECK_CONTAINS(j, "\"last_command\":null");
  CHECK_CONTAINS(j, "\"connected\":false");
}

TEST(status_json_fits_the_response_buffer_at_maximum_size) {
  // Longest plausible document: longest state name, longest fault name,
  // maximum-length request ID and maximum numeric values.
  StatusView v;
  v.device = DEVICE_HOSTNAME;
  v.firmwareVersion = FIRMWARE_VERSION;
  v.state = "RUNNING_ASSUMED";
  v.stateElapsedMs = 0xFFFFFFFFu;
  v.uptimeMs = 0xFFFFFFFFu;
  v.engineStatus = "RUNNING_ASSUMED";
  v.starter = true; v.choke = true; v.kill = true; v.valve = true;
  v.wifiConnected = true;
  v.ip = "255.255.255.255";
  v.rssiDbm = -100;
  v.cooldownRemainingMs = 0xFFFFFFFFu;
  v.hasLastCommand = true;
  v.lastCommandType = "START_FAILED";
  const std::string maxId = repeat('w', REQUEST_ID_MAX_LEN);
  v.lastCommandRequestId = maxId.c_str();
  v.lastCommandAccepted = true;
  v.fault = "STARTER_KILL_CONFLICT";

  char buf[HTTP_BODY_MAX];
  const size_t n = buildStatusJson(buf, sizeof(buf), v);
  CHECK_MSG(n > 0, "worst-case status document does not fit HTTP_BODY_MAX");
  CHECK(n < HTTP_BODY_MAX);
}

TEST(json_builders_fail_safely_when_the_buffer_is_too_small) {
  StatusView v;
  v.state = "IDLE";
  char tiny[16];
  CHECK_EQ(buildStatusJson(tiny, sizeof(tiny), v), 0u);
  CHECK_STREQ(tiny, "");

  CHECK_EQ(buildCommandJson(tiny, sizeof(tiny), true, "IDLE", "x", false, 0), 0u);
  CHECK_STREQ(tiny, "");

  CHECK_EQ(buildErrorJson(tiny, sizeof(tiny), 409, "code", "message", "IDLE", 0, true), 0u);
  CHECK_STREQ(tiny, "");

  CHECK_EQ(buildResponseHead(tiny, sizeof(tiny), 200, 100), 0u);
}

TEST(command_json_matches_the_documented_shape) {
  char buf[HTTP_BODY_MAX];
  const size_t n =
      buildCommandJson(buf, sizeof(buf), true, "CHOKING", "start-123", false, 0);
  CHECK(n > 0);
  const std::string j(buf);
  CHECK_CONTAINS(j, "\"accepted\":true");
  CHECK_CONTAINS(j, "\"state\":\"CHOKING\"");
  CHECK_CONTAINS(j, "\"request_id\":\"start-123\"");
  CHECK_CONTAINS(j, "\"duplicate\":false");
  CHECK_CONTAINS(j, "\"running_confirmed\":false");
}

TEST(command_json_flags_duplicates_and_reports_cooldown) {
  char buf[HTTP_BODY_MAX];
  CHECK(buildCommandJson(buf, sizeof(buf), true, "IDLE", "dup-1", true, 4321) > 0);
  const std::string j(buf);
  CHECK_CONTAINS(j, "\"duplicate\":true");
  CHECK_CONTAINS(j, "\"cooldown_remaining_ms\":4321");
}

TEST(error_json_includes_state_and_optional_cooldown) {
  char buf[HTTP_BODY_MAX];
  CHECK(buildErrorJson(buf, sizeof(buf), 409, "invalid_state_for_command",
                       "command not permitted in the current state",
                       "CRANKING", 7500, true) > 0);
  std::string j(buf);
  CHECK_CONTAINS(j, "\"accepted\":false");
  CHECK_CONTAINS(j, "\"error\":\"invalid_state_for_command\"");
  CHECK_CONTAINS(j, "\"status\":409");
  CHECK_CONTAINS(j, "\"state\":\"CRANKING\"");
  CHECK_CONTAINS(j, "\"cooldown_remaining_ms\":7500");

  // Without a state, the field renders as JSON null and cooldown is omitted.
  CHECK(buildErrorJson(buf, sizeof(buf), 401, "unauthorized", "nope",
                       nullptr, 0, false) > 0);
  j = buf;
  CHECK_CONTAINS(j, "\"state\":null");
  CHECK_NOT_CONTAINS(j, "cooldown_remaining_ms");
}

TEST(json_escaping_neutralises_quotes_backslashes_and_control_characters) {
  char buf[HTTP_BODY_MAX];
  // Request IDs are validated upstream, but the escaper is the last line of
  // defence if that ever changes.
  CHECK(buildCommandJson(buf, sizeof(buf), true, "IDLE",
                         "he said \"hi\"\\ and\nnewline", false, 0) > 0);
  const std::string j(buf);
  CHECK_NOT_CONTAINS(j, "\n");
  // The raw quote must not appear unescaped in the middle of the value.
  CHECK_CONTAINS(j, "\\\"hi\\\"");
  CHECK_CONTAINS(j, "\\\\");
}

TEST(response_head_is_well_formed_and_always_closes_the_connection) {
  char buf[HTTP_HEAD_MAX];
  const size_t n = buildResponseHead(buf, sizeof(buf), 202, 57);
  CHECK(n > 0);
  const std::string h(buf);
  CHECK_CONTAINS(h, "HTTP/1.1 202 Accepted\r\n");
  CHECK_CONTAINS(h, "Content-Type: application/json\r\n");
  CHECK_CONTAINS(h, "Content-Length: 57\r\n");
  CHECK_CONTAINS(h, "Connection: close\r\n");
  CHECK_CONTAINS(h, "Cache-Control: no-store\r\n");
  CHECK(h.size() >= 4 && h.compare(h.size() - 4, 4, "\r\n\r\n") == 0);
}

TEST(every_status_code_the_api_emits_has_a_reason_phrase) {
  const uint16_t codes[] = {200, 202, 400, 401, 404, 405, 409, 413, 500};
  for (uint16_t c : codes) {
    const char* phrase = reasonPhrase(c);
    CHECK(phrase != nullptr);
    CHECK_MSG(strlen(phrase) > 0, "empty reason phrase");
    CHECK_MSG(strcmp(phrase, "Error") != 0, "status code has no specific phrase");
  }
  CHECK_STREQ(reasonPhrase(599), "Error");
}
