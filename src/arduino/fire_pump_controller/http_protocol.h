// http_protocol.h — pure HTTP request parsing, routing and JSON generation.
//
// Deliberately free of WiFiS3 and of any socket concept so the whole protocol
// surface can be exercised by host-side unit tests. The only Arduino coupling
// is CommandType/PumpState, which come from the equally portable
// pump_controller.h.
//
// Everything here is fixed-size. No String, no heap.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "event_log.h"
#include "pump_controller.h"

// ---------------------------------------------------------------------------
// Buffer sizes
// ---------------------------------------------------------------------------

constexpr size_t HTTP_PATH_MAX   = 96;
constexpr size_t HTTP_SECRET_MAX = 128;

// Enough for the largest status document with generous slack. The test
// `status_json_fits_the_response_buffer_at_maximum_size` fails if a new field
// ever pushes the worst case past this.
constexpr size_t HTTP_BODY_MAX   = 1280;
// Response status line + fixed headers.
constexpr size_t HTTP_HEAD_MAX   = 192;

enum class HttpMethod : uint8_t {
  UNKNOWN = 0,
  GET,
  POST,
  OTHER,
};

enum class ParseStatus : uint8_t {
  INCOMPLETE = 0,  // more bytes required
  COMPLETE,        // full header section received
  FAILED,          // stop reading; errorStatus() explains why
};

struct ParsedRequest {
  HttpMethod method = HttpMethod::UNKNOWN;
  char path[HTTP_PATH_MAX] = {0};

  bool   secretPresent = false;
  char   secret[HTTP_SECRET_MAX] = {0};
  size_t secretLen = 0;

  bool requestIdPresent = false;
  bool requestIdValid   = false;
  char requestId[REQUEST_ID_MAX_LEN + 1] = {0};

  // Optional per-request start-sequence timing overrides, from the
  // X-Choke-Ms / X-Crank-Ms / X-Unchoke-Ms headers.
  //
  // `*Present` means the header was sent. `timingMalformed` means one of them
  // was not a plain decimal number that fits in uint32_t, which is a 400
  // regardless of endpoint. Range checking happens in api_handler.cpp, where
  // the configured ceilings live.
  bool     chokeMsPresent   = false;
  bool     crankMsPresent   = false;
  bool     unchokeMsPresent = false;
  bool     timingMalformed  = false;
  uint32_t chokeMs   = 0;
  uint32_t crankMs   = 0;
  uint32_t unchokeMs = 0;

  // X-Danger-Override. `dangerOverride` is set only by an EXACT match on
  // DANGER_OVERRIDE_TOKEN; any other value sets `dangerOverrideMalformed`,
  // which is a 400. Silently ignoring a misspelt override would leave the
  // caller believing an interlock was bypassed when it was not.
  bool dangerOverride          = false;
  bool dangerOverrideMalformed = false;

  // GET /v1/log?since=<seq>. The one endpoint that takes a query parameter.
  bool     sincePresent = false;
  bool     sinceMalformed = false;
  uint32_t since = 0;

  bool anyTimingOverride() const {
    return chokeMsPresent || crankMsPresent || unchokeMsPresent;
  }
};

// Parses a plain decimal uint32_t. Rejects empty input, any non-digit, and
// anything that would overflow. No sign, no whitespace, no leading '+'.
bool parseUint32(const char* s, size_t len, uint32_t& out);

// Incremental, byte-at-a-time HTTP request-header parser.
//
// Never blocks and never allocates. Feed bytes as they arrive; the caller can
// interleave state-machine servicing freely between feeds.
class RequestParser {
 public:
  RequestParser() { reset(); }

  void reset();

  // Consume one byte. Once COMPLETE or FAILED is returned the parser latches
  // that result until reset().
  ParseStatus feed(char c);

  ParseStatus status() const { return status_; }

  // Valid only when status() == FAILED.
  uint16_t errorStatus() const { return errorStatus_; }
  const char* errorCode() const { return errorCode_; }

  const ParsedRequest& request() const { return req_; }

  // Wipes the captured secret from RAM. Call once the secret has been checked.
  void scrubSecret();

 private:
  enum class Phase : uint8_t { REQUEST_LINE, HEADERS, DONE };

  void fail(uint16_t status, const char* code);
  void handleRequestLine();
  void handleHeaderLine();

  Phase       phase_ = Phase::REQUEST_LINE;
  ParseStatus status_ = ParseStatus::INCOMPLETE;
  uint16_t    errorStatus_ = 0;
  const char* errorCode_ = "";

  // +1 so a maximum-length line always has room for its NUL terminator.
  char   line_[HTTP_MAX_LINE_BYTES + 1];
  size_t lineLen_ = 0;
  bool   lineOverflow_ = false;
  size_t totalBytes_ = 0;

  ParsedRequest req_;
};

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

enum class RouteAction : uint8_t {
  STATUS = 0,   // GET /v1/status
  LOG,          // GET /v1/log
  COMMAND,      // a state-changing endpoint
  ERROR_STATUS, // 404 / 405
};

struct Route {
  RouteAction action = RouteAction::ERROR_STATUS;
  CommandType command = CommandType::NONE;
  uint16_t    errorStatus = 404;
  const char* errorCode = "not_found";
};

// Maps method+path onto an action. Path matching is exact.
Route resolveRoute(HttpMethod method, const char* path);

// ---------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------

// Request IDs accept only [A-Za-z0-9_-], length 1..REQUEST_ID_MAX_LEN.
bool isValidRequestId(const char* id);

// Length-and-content comparison that performs the same work regardless of
// where (or whether) the first difference occurs, so a caller cannot learn
// anything about partial matches from response timing.
bool constantTimeEquals(const char* a, size_t aLen, const char* b, size_t bLen);

// ---------------------------------------------------------------------------
// JSON generation
// ---------------------------------------------------------------------------

// Everything /v1/status reports, gathered by the caller so that this module
// stays independent of WiFiS3.
struct StatusView {
  const char* device = "";
  const char* firmwareVersion = "";
  const char* state = "";
  uint32_t    stateElapsedMs = 0;
  uint32_t    uptimeMs = 0;
  const char* engineStatus = "";
  bool        runningConfirmed = false;

  bool starter = false;
  bool choke = false;
  bool kill = false;
  bool valve = false;   // K4: true means the NC intake valve is OPEN
  bool valveEnabled = INTAKE_VALVE_ENABLED;
  bool waterOk = false; // debounced water-available interlock

  // K3 wiring. When true the relay is ENERGISED to permit running, so the
  // logical "kill asserted" is the inverse of the coil state. A UI that shows
  // which relays are drawing current needs this to render K3 correctly.
  bool killFailSafeNC = KILL_RELAY_FAIL_SAFE_NC;

  // Tank level. Diagnostic only -- see tank_level.h. flowLpm is a MAGNITUDE;
  // tankTrend carries the direction, so a refilling tank never appears as a
  // negative flow.
  bool        tankFitted = TANK_LEVEL_ENABLED;
  const char* tankStatus = "WARMING_UP";
  const char* tankTrend  = "UNKNOWN";
  int32_t     tankLevelMm = 0;
  float       tankVolumeL = 0.0f;
  float       tankFlowLpm = 0.0f;
  float       tankVolts = 0.0f;

  // Whether a water sensor exists at all. Without this the client cannot tell
  // "no water" from "no sensor": both leave waterOk false on older builds.
  bool waterSensorFitted = WATER_INTERLOCK_REQUIRED;

  // True while a start sequence authorised by DANGER_OVERRIDE is running.
  // overrideCount is cumulative since boot and never resets, so an override
  // used once cannot disappear from the record.
  bool     overrideActive = false;
  uint32_t overrideCount  = 0;

  bool        wifiConnected = false;
  const char* ip = "0.0.0.0";
  int32_t     rssiDbm = 0;

  uint32_t cooldownRemainingMs = 0;

  // Timings the current (or most recent) start sequence is using, so a client
  // can render an accurate progress indicator even when overrides were sent.
  uint32_t chokePrepMs = CHOKE_PREP_MS;
  uint32_t crankMs = CRANK_DURATION_MS;
  uint32_t unchokeDelayMs = UNCHOKE_DELAY_MS;

  bool        hasLastCommand = false;
  const char* lastCommandType = "NONE";
  const char* lastCommandRequestId = "";
  bool        lastCommandAccepted = false;

  const char* fault = nullptr;  // nullptr renders as JSON null
};

// All builders write a NUL-terminated document and return its length, or 0 if
// the buffer was too small (in which case buf is left as an empty string).

size_t buildStatusJson(char* buf, size_t cap, const StatusView& v);

size_t buildCommandJson(char* buf, size_t cap,
                        bool accepted, const char* state,
                        const char* requestId, bool duplicate,
                        uint32_t cooldownRemainingMs);

size_t buildErrorJson(char* buf, size_t cap,
                      uint16_t status, const char* code, const char* message,
                      const char* state /* may be nullptr */,
                      uint32_t cooldownRemainingMs, bool includeCooldown);

// Serialises a drained batch of log entries.
//
// Entries are emitted as positional tuples, not objects:
//
//   [seq, uptime_ms, "event", "state", detail, relay_bits]
//
// which is about a third the size of the equivalent object form and lets a
// full batch fit in one bounded response. The Pi expands them; the tuple
// order is fixed and documented in PI_INTEGRATION.md.
size_t buildLogJson(char* buf, size_t cap,
                    const LogEntry* entries, size_t count,
                    uint32_t nextSeq, uint32_t oldestSeq, uint32_t dropped,
                    bool truncated);

// Standard reason phrase for the status codes this server emits.
const char* reasonPhrase(uint16_t status);

// Writes the response head (status line, Content-Type, Content-Length,
// Connection: close, terminating CRLF). Returns length, or 0 if truncated.
size_t buildResponseHead(char* buf, size_t cap, uint16_t status, size_t bodyLen);
