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
#include "pump_controller.h"

// ---------------------------------------------------------------------------
// Buffer sizes
// ---------------------------------------------------------------------------

constexpr size_t HTTP_PATH_MAX   = 96;
constexpr size_t HTTP_SECRET_MAX = 128;

// Enough for the largest status document with generous slack.
constexpr size_t HTTP_BODY_MAX   = 640;
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
};

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
  bool spare = false;

  bool        wifiConnected = false;
  const char* ip = "0.0.0.0";
  int32_t     rssiDbm = 0;

  uint32_t cooldownRemainingMs = 0;

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

// Standard reason phrase for the status codes this server emits.
const char* reasonPhrase(uint16_t status);

// Writes the response head (status line, Content-Type, Content-Length,
// Connection: close, terminating CRLF). Returns length, or 0 if truncated.
size_t buildResponseHead(char* buf, size_t cap, uint16_t status, size_t bodyLen);
