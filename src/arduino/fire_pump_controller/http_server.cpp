// http_server.cpp — see http_server.h.

#include "http_server.h"

#include <string.h>

#include "arduino_secrets.h"

void HttpServer::begin() {
  if (started_) {
    return;
  }
  server_.begin();
  started_ = true;
  Serial.print(F("[HTTP] listening on port "));
  Serial.println(HTTP_PORT);
}

void HttpServer::closeClient() {
  if (clientActive_) {
    client_.stop();
    clientActive_ = false;
  }
  parser_.reset();
}

void HttpServer::respond(const ResponsePlan& plan) {
  const size_t headLen =
      buildResponseHead(head_, sizeof(head_), plan.status, plan.bodyLen);
  if (headLen == 0) {
    // Cannot happen with the fixed header set, but never send a torn response.
    return;
  }
  client_.write(reinterpret_cast<const uint8_t*>(head_), headLen);
  if (plan.bodyLen > 0) {
    client_.write(reinterpret_cast<const uint8_t*>(plan.body), plan.bodyLen);
  }
  client_.flush();

  Serial.print(F("[HTTP] "));
  Serial.print(plan.status);
  Serial.println(F(" sent"));
}

void HttpServer::serviceExisting(uint32_t now, PumpController& pump,
                                 const NetManager& net) {
  if (!clientActive_) {
    return;
  }

  // Slow or stalled clients are dropped rather than waited on. Relay timing
  // must never depend on a peer finishing its request.
  const bool timedOut =
      static_cast<uint32_t>(now - clientStartedAt_) >= HTTP_CLIENT_TIMEOUT_MS;

  int avail = client_.available();
  if (avail < 0) {
    avail = 0;
  }

  if (avail == 0 && !client_.connected()) {
    closeClient();
    return;
  }

  if (avail == 0 && timedOut) {
    Serial.println(F("[HTTP] client timed out before completing headers"));
    closeClient();
    return;
  }

  // Bounded drain: at most HTTP_BYTES_PER_PASS bytes leave the socket FIFO on
  // any single pass, so the pump state machine keeps advancing even while a
  // peer is dribbling a request one byte at a time.
  size_t budget = HTTP_BYTES_PER_PASS;
  ParseStatus st = parser_.status();
  while (budget > 0 && avail > 0 && st == ParseStatus::INCOMPLETE) {
    const int c = client_.read();
    if (c < 0) {
      break;
    }
    --avail;
    --budget;
    st = parser_.feed(static_cast<char>(c));
  }

  if (st == ParseStatus::INCOMPLETE) {
    if (timedOut) {
      Serial.println(F("[HTTP] client timed out mid-request"));
      closeClient();
    }
    // Otherwise return and pick up where we left off on the next pass.
    return;
  }

  // Headers are complete (or definitively bad): decide and answer.
  NetInfo info;
  info.connected = net.isConnected();
  info.ip = net.ipString();
  info.rssiDbm = net.rssiDbm();

  planResponse(plan_, parser_, pump, info, PUMP_API_SECRET, now, millis());

  // The supplied secret is wiped as soon as it has been checked; it is never
  // logged and never leaves this function.
  parser_.scrubSecret();

  respond(plan_);
  closeClient();
}

void HttpServer::acceptNew(uint32_t now) {
  if (clientActive_ || !started_) {
    return;
  }
  // Rate-limited: see HTTP_ACCEPT_POLL_MS. Each call below is an AT exchange
  // with the Wi-Fi co-processor, not a cheap local check.
  if (lastAcceptPollAt_ != 0 &&
      static_cast<uint32_t>(now - lastAcceptPollAt_) < HTTP_ACCEPT_POLL_MS) {
    return;
  }
  lastAcceptPollAt_ = now;

  WiFiClient incoming = server_.available();
  if (!incoming) {
    return;
  }
  client_ = incoming;
  clientActive_ = true;
  clientStartedAt_ = now;
  parser_.reset();
}

void HttpServer::service(uint32_t now, PumpController& pump,
                         const NetManager& net) {
  // Existing client first, then admit a new one: a half-finished request is
  // always driven to completion (or dropped) before another is taken on.
  serviceExisting(now, pump, net);
  acceptNew(now);
}
