// net_manager.h — non-blocking Wi-Fi station lifecycle for the UNO R4 WiFi.
//
// The WiFiS3 stack is fundamentally synchronous: every call is an AT exchange
// with the on-board ESP32-S3. This class keeps that exposure as small and as
// predictable as possible:
//
//   * WiFi.setTimeout(0) removes WiFiS3's own internal 10-second connect poll,
//     so WiFi.begin() only sends its AT commands and returns.
//   * Association progress is then polled from the main loop, one quick
//     status query per WIFI_POLL_INTERVAL_MS, spread across many iterations.
//   * A reconnect is only ever *initiated* while the pump controller is
//     quiescent, so no relay timing sequence can be disturbed by it.
//
// Nothing here ever prints the Wi-Fi password or the API secret.

#pragma once

#include <Arduino.h>
#include <WiFiS3.h>

#include "config.h"

class NetManager {
 public:
  NetManager() = default;

  // Sets the requested DHCP hostname and kicks off the first association
  // attempt. Must be called after PumpController::begin() so that the relays
  // are already in a known-inactive state.
  void begin(uint32_t now);

  // Advances the connection state machine. `radioReconnectAllowed` must be
  // false whenever a relay timing sequence is in progress; when false, this
  // call will still observe link state but will not start a new association.
  void tick(uint32_t now, bool radioReconnectAllowed);

  bool isConnected() const { return phase_ == Phase::CONNECTED; }

  const char* ipString() const { return ip_; }
  const char* macString() const { return mac_; }
  const char* ssidString() const { return ssid_; }
  int32_t rssiDbm() const { return rssi_; }

  // Prints hostname, firmware version, SSID, IP, MAC, RSSI and HTTP port.
  void printBanner() const;

 private:
  enum class Phase : uint8_t {
    IDLE_START,   // nothing attempted yet
    CONNECTING,   // association requested, awaiting WL_CONNECTED
    CONNECTED,    // associated; link polled periodically
    BACKOFF,      // waiting out WIFI_RETRY_INTERVAL_MS before retrying
  };

  void startAttempt(uint32_t now);
  void refreshLinkInfo();
  void captureMac();

  Phase    phase_ = Phase::IDLE_START;
  uint32_t phaseStartedAt_ = 0;
  uint32_t lastPollAt_ = 0;
  bool     bannerPending_ = false;

  char    ip_[16]   = "0.0.0.0";
  char    mac_[18]  = "00:00:00:00:00:00";
  char    ssid_[33] = "";
  int32_t rssi_     = 0;
};
