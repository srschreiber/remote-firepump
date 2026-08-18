// http_server.h — WiFiS3 socket plumbing for the pump API.
//
// Thin by design: everything that decides *what* to answer lives in
// http_protocol.cpp and api_handler.cpp, which are unit-tested on the host.
// This class only moves bytes.
//
// One request per TCP connection, always answered with Connection: close.

#pragma once

#include <Arduino.h>
#include <WiFiS3.h>

#include "api_handler.h"
#include "config.h"
#include "http_protocol.h"
#include "net_manager.h"
#include "pump_controller.h"

class HttpServer {
 public:
  HttpServer() : server_(HTTP_PORT) {}

  void begin();

  // Non-blocking. Drains at most HTTP_BYTES_PER_PASS bytes from the current
  // client, then accepts a new one if the slot is free. Safe to call as often
  // as the main loop runs; it never waits for a byte.
  void service(uint32_t now, PumpController& pump, const NetManager& net);

  bool hasClient() const { return clientActive_; }

 private:
  void serviceExisting(uint32_t now, PumpController& pump, const NetManager& net);
  void acceptNew(uint32_t now);
  void respond(const ResponsePlan& plan);
  void closeClient();

  WiFiServer server_;
  WiFiClient client_;
  bool       clientActive_ = false;
  uint32_t   clientStartedAt_ = 0;
  bool       started_ = false;

  RequestParser parser_;

  // Reused across requests so the server allocates nothing at runtime.
  ResponsePlan plan_;
  char         head_[HTTP_HEAD_MAX] = {0};
};
