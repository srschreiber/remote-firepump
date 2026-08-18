// api_handler.cpp — see api_handler.h.

#include "api_handler.h"

#include <string.h>

namespace {

void emitError(ResponsePlan& out, uint16_t status, const char* code,
               const char* message, const char* state,
               uint32_t cooldownMs, bool includeCooldown) {
  out.status = status;
  out.bodyLen = buildErrorJson(out.body, sizeof(out.body), status, code,
                               message, state, cooldownMs, includeCooldown);
  if (out.bodyLen == 0) {
    // Should be unreachable: every message above is a short literal. Fall back
    // to a minimal document rather than emitting a zero-length body.
    static const char kFallback[] = "{\"accepted\":false,\"error\":\"internal\"}";
    memcpy(out.body, kFallback, sizeof(kFallback));
    out.bodyLen = sizeof(kFallback) - 1;
    out.status = 500;
  }
}

}  // namespace

void makeStatusView(StatusView& v,
                    const PumpController& pump,
                    const NetInfo& net,
                    uint32_t nowMs,
                    uint32_t uptimeMs) {
  v.device = DEVICE_HOSTNAME;
  v.firmwareVersion = FIRMWARE_VERSION;
  v.state = toString(pump.state());
  v.stateElapsedMs = pump.stateElapsedMs(nowMs);
  v.uptimeMs = uptimeMs;
  v.engineStatus = toString(pump.engineStatus());

  // Structurally false: this build has no sensor that could confirm running.
  v.runningConfirmed = pump.runningConfirmed();

  v.starter = pump.starterActive();
  v.choke   = pump.chokeActive();
  v.kill    = pump.killActive();
  v.spare   = pump.spareActive();

  v.wifiConnected = net.connected;
  v.ip = net.ip;
  v.rssiDbm = net.rssiDbm;

  v.cooldownRemainingMs = pump.cooldownRemainingMs(nowMs);

  v.hasLastCommand = pump.hasLastCommand();
  v.lastCommandType = toString(pump.lastCommandType());
  v.lastCommandRequestId = pump.lastCommandRequestId();
  v.lastCommandAccepted = pump.lastCommandAccepted();

  v.fault = toString(pump.fault());  // nullptr when no fault
}

void planResponse(ResponsePlan& out,
                  const RequestParser& parser,
                  PumpController& pump,
                  const NetInfo& net,
                  const char* configuredSecret,
                  uint32_t nowMs,
                  uint32_t uptimeMs) {
  out = ResponsePlan();

  // ---- 1. Parse failures ---------------------------------------------------
  if (parser.status() == ParseStatus::FAILED) {
    const uint16_t st = parser.errorStatus();
    emitError(out, st, parser.errorCode(),
              st == 413 ? "request exceeds size limits"
                        : "malformed request",
              toString(pump.state()), 0, false);
    return;
  }
  if (parser.status() != ParseStatus::COMPLETE) {
    emitError(out, 400, "incomplete_request", "request headers incomplete",
              toString(pump.state()), 0, false);
    return;
  }

  const ParsedRequest& req = parser.request();

  // ---- 2. Authentication ---------------------------------------------------
  // Checked before routing so that an unauthenticated caller learns nothing
  // about which endpoints exist. The supplied secret is never logged.
  const bool secretConfigured =
      (configuredSecret != nullptr && configuredSecret[0] != '\0');
  const bool authOk =
      secretConfigured && req.secretPresent &&
      constantTimeEquals(req.secret, req.secretLen,
                         configuredSecret, strlen(configuredSecret));

  if (!authOk) {
    emitError(out, 401, "unauthorized", "missing or incorrect X-Pump-Secret",
              nullptr, 0, false);
    return;
  }

  // ---- 3. Routing ----------------------------------------------------------
  const Route route = resolveRoute(req.method, req.path);
  if (route.action == RouteAction::ERROR_STATUS) {
    emitError(out, route.errorStatus, route.errorCode,
              route.errorStatus == 405 ? "method not allowed for this path"
                                       : "unknown endpoint",
              toString(pump.state()), 0, false);
    return;
  }

  // ---- 4. GET /v1/status ---------------------------------------------------
  if (route.action == RouteAction::STATUS) {
    StatusView v;
    makeStatusView(v, pump, net, nowMs, uptimeMs);
    out.status = 200;
    out.bodyLen = buildStatusJson(out.body, sizeof(out.body), v);
    if (out.bodyLen == 0) {
      emitError(out, 500, "status_serialization_failed",
                "status document exceeded the response buffer",
                toString(pump.state()), 0, false);
    }
    return;
  }

  // ---- 5. State-changing commands -----------------------------------------
  if (req.requestIdPresent && !req.requestIdValid) {
    emitError(out, 400, "invalid_request_id",
              "X-Request-ID must be 1-64 chars of A-Z a-z 0-9 - _",
              toString(pump.state()), 0, false);
    return;
  }
  if (REQUIRE_REQUEST_ID && !req.requestIdPresent) {
    emitError(out, 400, "missing_request_id",
              "X-Request-ID is required for state-changing requests",
              toString(pump.state()), 0, false);
    return;
  }

  const char* requestId = req.requestIdValid ? req.requestId : "";

  const CommandResult result = pump.handleCommand(route.command, requestId, nowMs);

  if (!result.accepted) {
    // 409: valid, authenticated, but not permitted from the current state.
    emitError(out, result.httpStatus, "invalid_state_for_command",
              "command not permitted in the current state",
              toString(result.state), result.cooldownRemainingMs, true);
    return;
  }

  out.status = result.httpStatus;  // 202
  out.bodyLen = buildCommandJson(out.body, sizeof(out.body), true,
                                 toString(result.state), requestId,
                                 result.duplicate, result.cooldownRemainingMs);
  if (out.bodyLen == 0) {
    emitError(out, 500, "response_serialization_failed",
              "command response exceeded the response buffer",
              toString(result.state), result.cooldownRemainingMs, true);
  }
}
