// api_handler.h — authentication, routing and command dispatch.
//
// This is the entire decision layer of the HTTP API, expressed as a pure
// function of (parsed request, controller, network snapshot). It performs no
// I/O, so the whole API surface — including 401 handling and idempotency —
// is exercised directly by host-side unit tests.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "http_protocol.h"
#include "pump_controller.h"

// Network facts needed by /v1/status, supplied by the caller so this module
// stays independent of WiFiS3.
struct NetInfo {
  bool        connected = false;
  const char* ip = "0.0.0.0";
  int32_t     rssiDbm = 0;
};

struct ResponsePlan {
  uint16_t status = 500;
  size_t   bodyLen = 0;
  char     body[HTTP_BODY_MAX] = {0};

  // /v1/log batches are larger than any other response, so they get their own
  // buffer rather than inflating HTTP_BODY_MAX for every reply. `useLogBody`
  // says which one holds the payload.
  bool     useLogBody = false;
  char     logBody[LOG_BODY_MAX] = {0};

  const char* payload() const { return useLogBody ? logBody : body; }
};

// Produces the complete response for a fully parsed (or failed) request.
//
// `configuredSecret` is the value of PUMP_API_SECRET. If it is null or empty
// the handler fails closed and answers 401 to everything: an unconfigured
// device must never be an open relay controller.
//
// May mutate `pump`, but only through PumpController::handleCommand(), which
// enforces every interlock itself.
void planResponse(ResponsePlan& out,
                  const RequestParser& parser,
                  PumpController& pump,
                  const NetInfo& net,
                  const char* configuredSecret,
                  uint32_t nowMs,
                  uint32_t uptimeMs);

// Fills a StatusView from the controller and network snapshot. Exposed for
// tests and for the Serial diagnostics path.
void makeStatusView(StatusView& v,
                    const PumpController& pump,
                    const NetInfo& net,
                    uint32_t nowMs,
                    uint32_t uptimeMs);
