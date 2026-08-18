// http_protocol.cpp — see http_protocol.h.

#include "http_protocol.h"

#include <stdio.h>
#include <string.h>

namespace {

char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool headerNameIs(const char* name, size_t nameLen, const char* lowerLiteral) {
  const size_t litLen = strlen(lowerLiteral);
  if (nameLen != litLen) {
    return false;
  }
  for (size_t i = 0; i < nameLen; ++i) {
    if (lowerAscii(name[i]) != lowerLiteral[i]) {
      return false;
    }
  }
  return true;
}

const char* jsonBool(bool v) { return v ? "true" : "false"; }

}  // namespace

// ---------------------------------------------------------------------------
// RequestParser
// ---------------------------------------------------------------------------

void RequestParser::reset() {
  phase_ = Phase::REQUEST_LINE;
  status_ = ParseStatus::INCOMPLETE;
  errorStatus_ = 0;
  errorCode_ = "";
  lineLen_ = 0;
  lineOverflow_ = false;
  totalBytes_ = 0;
  line_[0] = '\0';
  req_ = ParsedRequest();
}

void RequestParser::scrubSecret() {
  memset(req_.secret, 0, sizeof(req_.secret));
  req_.secretLen = 0;
}

void RequestParser::fail(uint16_t status, const char* code) {
  status_ = ParseStatus::FAILED;
  errorStatus_ = status;
  errorCode_ = code;
  phase_ = Phase::DONE;
}

ParseStatus RequestParser::feed(char c) {
  if (status_ != ParseStatus::INCOMPLETE) {
    return status_;  // latched
  }

  ++totalBytes_;
  if (totalBytes_ > HTTP_MAX_HEADER_BYTES) {
    fail(413, "headers_too_large");
    return status_;
  }

  if (c == '\r') {
    return status_;  // CR is ignored; LF terminates a line
  }

  if (c != '\n') {
    if (lineLen_ < HTTP_MAX_LINE_BYTES) {
      line_[lineLen_++] = c;
      line_[lineLen_] = '\0';
    } else {
      lineOverflow_ = true;
    }
    if (lineOverflow_) {
      fail(413, phase_ == Phase::REQUEST_LINE ? "request_line_too_long"
                                              : "header_line_too_long");
    } else if (phase_ == Phase::REQUEST_LINE && lineLen_ > HTTP_MAX_REQUEST_LINE) {
      fail(413, "request_line_too_long");
    }
    return status_;
  }

  // End of line.
  line_[lineLen_] = '\0';

  if (phase_ == Phase::REQUEST_LINE) {
    handleRequestLine();
    if (status_ == ParseStatus::INCOMPLETE) {
      phase_ = Phase::HEADERS;
    }
  } else if (lineLen_ == 0) {
    // Blank line: the header section is complete. Any body is ignored; these
    // endpoints take no request body.
    status_ = ParseStatus::COMPLETE;
    phase_ = Phase::DONE;
  } else {
    handleHeaderLine();
  }

  lineLen_ = 0;
  lineOverflow_ = false;
  line_[0] = '\0';
  return status_;
}

void RequestParser::handleRequestLine() {
  // Expected shape: METHOD SP request-target SP HTTP-version
  const char* sp1 = strchr(line_, ' ');
  if (sp1 == nullptr) {
    fail(400, "malformed_request_line");
    return;
  }
  const size_t methodLen = static_cast<size_t>(sp1 - line_);
  if (methodLen == 0) {
    fail(400, "malformed_request_line");
    return;
  }

  const char* target = sp1 + 1;
  const char* sp2 = strchr(target, ' ');
  if (sp2 == nullptr) {
    fail(400, "malformed_request_line");
    return;
  }
  const size_t targetLen = static_cast<size_t>(sp2 - target);
  if (targetLen == 0) {
    fail(400, "malformed_request_line");
    return;
  }

  const char* version = sp2 + 1;
  if (strncmp(version, "HTTP/", 5) != 0) {
    fail(400, "malformed_request_line");
    return;
  }

  if (methodLen == 3 && strncmp(line_, "GET", 3) == 0) {
    req_.method = HttpMethod::GET;
  } else if (methodLen == 4 && strncmp(line_, "POST", 4) == 0) {
    req_.method = HttpMethod::POST;
  } else {
    req_.method = HttpMethod::OTHER;
  }

  if (target[0] != '/') {
    // Absolute-form and authority-form targets are not supported.
    fail(400, "malformed_request_target");
    return;
  }

  // Drop any query string; none of these endpoints take parameters.
  size_t pathLen = targetLen;
  for (size_t i = 0; i < targetLen; ++i) {
    if (target[i] == '?' || target[i] == '#') {
      pathLen = i;
      break;
    }
  }

  if (pathLen >= HTTP_PATH_MAX) {
    // Longer than any route this firmware serves. Truncating can only make
    // the path shorter, so it can never collide with a real endpoint; it will
    // fall through to 404.
    pathLen = HTTP_PATH_MAX - 1;
  }
  memcpy(req_.path, target, pathLen);
  req_.path[pathLen] = '\0';
}

void RequestParser::handleHeaderLine() {
  const char* colon = strchr(line_, ':');
  if (colon == nullptr) {
    fail(400, "malformed_header");
    return;
  }

  const size_t nameLen = static_cast<size_t>(colon - line_);
  if (nameLen == 0) {
    fail(400, "malformed_header");
    return;
  }

  // Skip optional whitespace after the colon.
  const char* value = colon + 1;
  while (*value == ' ' || *value == '\t') {
    ++value;
  }
  size_t valueLen = strlen(value);
  // Trim trailing whitespace.
  while (valueLen > 0 && (value[valueLen - 1] == ' ' || value[valueLen - 1] == '\t')) {
    --valueLen;
  }

  if (headerNameIs(line_, nameLen, "x-pump-secret")) {
    req_.secretPresent = true;
    if (valueLen >= HTTP_SECRET_MAX) {
      // Store nothing. An over-long value cannot equal a configured secret
      // that fits in the buffer, so this deterministically yields 401 without
      // creating a separate, distinguishable error path.
      req_.secret[0] = '\0';
      req_.secretLen = 0;
    } else {
      memcpy(req_.secret, value, valueLen);
      req_.secret[valueLen] = '\0';
      req_.secretLen = valueLen;
    }
    return;
  }

  // Optional start-sequence timing overrides.
  struct TimingHeader {
    const char* name;
    bool ParsedRequest::*present;
    uint32_t ParsedRequest::*value;
  };
  static const TimingHeader kTimingHeaders[] = {
      {"x-choke-ms",   &ParsedRequest::chokeMsPresent,   &ParsedRequest::chokeMs},
      {"x-crank-ms",   &ParsedRequest::crankMsPresent,   &ParsedRequest::crankMs},
      {"x-unchoke-ms", &ParsedRequest::unchokeMsPresent, &ParsedRequest::unchokeMs},
  };
  for (const TimingHeader& h : kTimingHeaders) {
    if (!headerNameIs(line_, nameLen, h.name)) {
      continue;
    }
    req_.*(h.present) = true;
    uint32_t parsed = 0;
    if (parseUint32(value, valueLen, parsed)) {
      req_.*(h.value) = parsed;
    } else {
      req_.timingMalformed = true;
    }
    return;
  }

  if (headerNameIs(line_, nameLen, "x-request-id")) {
    req_.requestIdPresent = true;
    if (valueLen == 0 || valueLen > REQUEST_ID_MAX_LEN) {
      req_.requestIdValid = false;
      req_.requestId[0] = '\0';
      return;
    }
    char tmp[REQUEST_ID_MAX_LEN + 1];
    memcpy(tmp, value, valueLen);
    tmp[valueLen] = '\0';
    if (isValidRequestId(tmp)) {
      memcpy(req_.requestId, tmp, valueLen + 1);
      req_.requestIdValid = true;
    } else {
      req_.requestId[0] = '\0';
      req_.requestIdValid = false;
    }
    return;
  }

  // Every other header is accepted and ignored.
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

Route resolveRoute(HttpMethod method, const char* path) {
  struct Entry {
    const char* path;
    HttpMethod  method;
    RouteAction action;
    CommandType command;
  };
  static const Entry table[] = {
      {"/v1/status",       HttpMethod::GET,  RouteAction::STATUS,  CommandType::NONE},
      {"/v1/start",        HttpMethod::POST, RouteAction::COMMAND, CommandType::START},
      {"/v1/stop",         HttpMethod::POST, RouteAction::COMMAND, CommandType::STOP},
      {"/v1/start-failed", HttpMethod::POST, RouteAction::COMMAND, CommandType::START_FAILED},
      {"/v1/reset-idle",   HttpMethod::POST, RouteAction::COMMAND, CommandType::RESET_IDLE},

      // Maintenance endpoints. Present in the table unconditionally so the
      // logic is always compiled and always unit-tested, but skipped below
      // unless ENABLE_MAINTENANCE_API is set -- in which case they behave
      // exactly like any other unknown path and return 404.
      {"/v1/maintenance/choke/on",    HttpMethod::POST, RouteAction::COMMAND, CommandType::MAINT_CHOKE_ON},
      {"/v1/maintenance/choke/off",   HttpMethod::POST, RouteAction::COMMAND, CommandType::MAINT_CHOKE_OFF},
      {"/v1/maintenance/starter/on",  HttpMethod::POST, RouteAction::COMMAND, CommandType::MAINT_STARTER_ON},
      {"/v1/maintenance/starter/off", HttpMethod::POST, RouteAction::COMMAND, CommandType::MAINT_STARTER_OFF},
      {"/v1/maintenance/kill/on",     HttpMethod::POST, RouteAction::COMMAND, CommandType::MAINT_KILL_ON},
      {"/v1/maintenance/kill/off",    HttpMethod::POST, RouteAction::COMMAND, CommandType::MAINT_KILL_OFF},
  };

  Route r;
  bool pathKnown = false;

  for (const Entry& e : table) {
    if (strcmp(path, e.path) != 0) {
      continue;
    }
    if (isMaintenanceCommand(e.command) && !MAINTENANCE_API_ENABLED) {
      continue;   // invisible: falls through to 404
    }
    pathKnown = true;
    if (method == e.method) {
      r.action = e.action;
      r.command = e.command;
      r.errorStatus = 0;
      r.errorCode = "";
      return r;
    }
  }

  r.action = RouteAction::ERROR_STATUS;
  r.command = CommandType::NONE;
  if (pathKnown) {
    r.errorStatus = 405;
    r.errorCode = "method_not_allowed";
  } else {
    r.errorStatus = 404;
    r.errorCode = "not_found";
  }
  return r;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool parseUint32(const char* s, size_t len, uint32_t& out) {
  if (s == nullptr || len == 0 || len > 10) {
    return false;   // 4294967295 is ten digits
  }
  uint32_t acc = 0;
  for (size_t i = 0; i < len; ++i) {
    const char c = s[i];
    if (c < '0' || c > '9') {
      return false;
    }
    const uint32_t digit = static_cast<uint32_t>(c - '0');
    // Overflow check before it can happen.
    if (acc > (0xFFFFFFFFu - digit) / 10u) {
      return false;
    }
    acc = acc * 10u + digit;
  }
  out = acc;
  return true;
}

bool isValidRequestId(const char* id) {
  if (id == nullptr) {
    return false;
  }
  size_t n = 0;
  for (; id[n] != '\0'; ++n) {
    if (n >= REQUEST_ID_MAX_LEN) {
      return false;
    }
    const char c = id[n];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) {
      return false;
    }
  }
  return n > 0;
}

bool constantTimeEquals(const char* a, size_t aLen, const char* b, size_t bLen) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  // Fold the length difference into the accumulator instead of returning
  // early, and always walk the full configured-secret length so the work done
  // does not depend on where the first mismatching byte is.
  volatile uint32_t diff = static_cast<uint32_t>(aLen ^ bLen);
  const size_t n = (bLen > 0) ? bLen : 1;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t ca = (i < aLen) ? static_cast<uint8_t>(a[i]) : 0u;
    const uint8_t cb = (i < bLen) ? static_cast<uint8_t>(b[i]) : 0u;
    diff |= static_cast<uint32_t>(ca ^ cb);
  }
  return diff == 0;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

namespace {

// Copies `in` into `out` escaping the two characters JSON forbids raw and
// dropping anything non-printable. Request IDs and state names are already
// restricted character sets; this is belt-and-braces against a future change.
void jsonEscape(char* out, size_t cap, const char* in) {
  if (cap == 0) {
    return;
  }
  size_t o = 0;
  for (size_t i = 0; in != nullptr && in[i] != '\0'; ++i) {
    const char c = in[i];
    const char* rep = nullptr;
    if (c == '"') {
      rep = "\\\"";
    } else if (c == '\\') {
      rep = "\\\\";
    } else if (c < 0x20 || c == 0x7F) {
      continue;  // drop control characters
    }
    if (rep != nullptr) {
      if (o + 2 >= cap) break;
      out[o++] = rep[0];
      out[o++] = rep[1];
    } else {
      if (o + 1 >= cap) break;
      out[o++] = c;
    }
  }
  out[o] = '\0';
}

size_t finish(char* buf, size_t cap, int written) {
  if (written < 0 || static_cast<size_t>(written) >= cap) {
    buf[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(written);
}

}  // namespace

size_t buildStatusJson(char* buf, size_t cap, const StatusView& v) {
  if (buf == nullptr || cap == 0) {
    return 0;
  }

  char reqId[REQUEST_ID_MAX_LEN + 3];
  jsonEscape(reqId, sizeof(reqId), v.lastCommandRequestId);

  char lastCommand[REQUEST_ID_MAX_LEN + 96];
  if (v.hasLastCommand) {
    const int n = snprintf(lastCommand, sizeof(lastCommand),
                           "{\"type\":\"%s\",\"request_id\":\"%s\",\"accepted\":%s}",
                           v.lastCommandType, reqId,
                           jsonBool(v.lastCommandAccepted));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(lastCommand)) {
      buf[0] = '\0';
      return 0;
    }
  } else {
    strcpy(lastCommand, "null");
  }

  char faultField[48];
  if (v.fault == nullptr) {
    strcpy(faultField, "null");
  } else {
    const int n = snprintf(faultField, sizeof(faultField), "\"%s\"", v.fault);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(faultField)) {
      buf[0] = '\0';
      return 0;
    }
  }

  const int written = snprintf(
      buf, cap,
      "{"
      "\"device\":\"%s\","
      "\"firmware_version\":\"%s\","
      "\"state\":\"%s\","
      "\"state_elapsed_ms\":%lu,"
      "\"uptime_ms\":%lu,"
      "\"engine_status\":\"%s\","
      "\"running_confirmed\":%s,"
      "\"relay_outputs\":{\"starter\":%s,\"choke\":%s,\"kill\":%s,\"spare\":%s},"
      "\"wifi\":{\"connected\":%s,\"ip\":\"%s\",\"rssi_dbm\":%ld},"
      "\"cooldown_remaining_ms\":%lu,"
      "\"timings\":{\"choke_prep_ms\":%lu,\"crank_ms\":%lu,"
      "\"unchoke_delay_ms\":%lu,\"kill_hold_ms\":%lu,"
      "\"min_recrank_gap_ms\":%lu,\"max_crank_ms\":%lu},"
      "\"maintenance_api\":%s,"
      "\"last_command\":%s,"
      "\"fault\":%s"
      "}",
      v.device, v.firmwareVersion, v.state,
      static_cast<unsigned long>(v.stateElapsedMs),
      static_cast<unsigned long>(v.uptimeMs),
      v.engineStatus, jsonBool(v.runningConfirmed),
      jsonBool(v.starter), jsonBool(v.choke), jsonBool(v.kill), jsonBool(v.spare),
      jsonBool(v.wifiConnected), v.ip, static_cast<long>(v.rssiDbm),
      static_cast<unsigned long>(v.cooldownRemainingMs),
      static_cast<unsigned long>(v.chokePrepMs),
      static_cast<unsigned long>(v.crankMs),
      static_cast<unsigned long>(v.unchokeDelayMs),
      static_cast<unsigned long>(KILL_HOLD_MS),
      static_cast<unsigned long>(MIN_RECRANK_GAP_MS),
      static_cast<unsigned long>(MAX_CRANK_MS),
      jsonBool(MAINTENANCE_API_ENABLED),
      lastCommand, faultField);

  return finish(buf, cap, written);
}

size_t buildCommandJson(char* buf, size_t cap,
                        bool accepted, const char* state,
                        const char* requestId, bool duplicate,
                        uint32_t cooldownRemainingMs) {
  if (buf == nullptr || cap == 0) {
    return 0;
  }
  char reqId[REQUEST_ID_MAX_LEN + 3];
  jsonEscape(reqId, sizeof(reqId), requestId);

  const int written = snprintf(
      buf, cap,
      "{\"accepted\":%s,\"state\":\"%s\",\"request_id\":\"%s\","
      "\"duplicate\":%s,\"cooldown_remaining_ms\":%lu,"
      "\"running_confirmed\":false}",
      jsonBool(accepted), state, reqId, jsonBool(duplicate),
      static_cast<unsigned long>(cooldownRemainingMs));

  return finish(buf, cap, written);
}

size_t buildErrorJson(char* buf, size_t cap,
                      uint16_t status, const char* code, const char* message,
                      const char* state, uint32_t cooldownRemainingMs,
                      bool includeCooldown) {
  if (buf == nullptr || cap == 0) {
    return 0;
  }

  char stateField[40];
  if (state == nullptr) {
    strcpy(stateField, "null");
  } else {
    const int n = snprintf(stateField, sizeof(stateField), "\"%s\"", state);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(stateField)) {
      buf[0] = '\0';
      return 0;
    }
  }

  char cooldownField[64];
  if (includeCooldown) {
    const int n = snprintf(cooldownField, sizeof(cooldownField),
                           ",\"cooldown_remaining_ms\":%lu",
                           static_cast<unsigned long>(cooldownRemainingMs));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(cooldownField)) {
      buf[0] = '\0';
      return 0;
    }
  } else {
    cooldownField[0] = '\0';
  }

  const int written = snprintf(
      buf, cap,
      "{\"accepted\":false,\"error\":\"%s\",\"message\":\"%s\","
      "\"status\":%u,\"state\":%s%s}",
      code, message, static_cast<unsigned>(status), stateField, cooldownField);

  return finish(buf, cap, written);
}

const char* reasonPhrase(uint16_t status) {
  switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Content Too Large";
    case 500: return "Internal Server Error";
    default:  return "Error";
  }
}

size_t buildResponseHead(char* buf, size_t cap, uint16_t status, size_t bodyLen) {
  if (buf == nullptr || cap == 0) {
    return 0;
  }
  const int written = snprintf(
      buf, cap,
      "HTTP/1.1 %u %s\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: %lu\r\n"
      "Cache-Control: no-store\r\n"
      "Connection: close\r\n"
      "\r\n",
      static_cast<unsigned>(status), reasonPhrase(status),
      static_cast<unsigned long>(bodyLen));

  return finish(buf, cap, written);
}
