// net_manager.cpp — see net_manager.h.

#include "net_manager.h"

#include <stdio.h>
#include <string.h>

#include "arduino_secrets.h"

void NetManager::begin(uint32_t now) {
  // Remove WiFiS3's internal blocking connect loop. Association progress is
  // polled from the main loop instead, so a single call cannot monopolise the
  // CPU for ten seconds while a relay sequence is pending.
  WiFi.setTimeout(0);

  // Must be set before WiFi.begin() for the DHCP request to carry it.
  WiFi.setHostname(DEVICE_HOSTNAME);

  captureMac();
  startAttempt(now);
}

void NetManager::captureMac() {
  uint8_t raw[6] = {0};
  WiFi.macAddress(raw);
  snprintf(mac_, sizeof(mac_), "%02X:%02X:%02X:%02X:%02X:%02X",
           raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
}

void NetManager::startAttempt(uint32_t now) {
  Serial.print(F("[WIFI] associating with SSID: "));
  Serial.println(WIFI_SSID);  // the password is never printed

  // Returns almost immediately because setTimeout(0) disabled the internal
  // poll loop. The return value is intentionally ignored; association is
  // confirmed by polling WiFi.status() below.
  (void)WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  phase_ = Phase::CONNECTING;
  phaseStartedAt_ = now;
  lastPollAt_ = now;
}

bool NetManager::haveAddress() const {
  return ip_[0] != '\0' && strcmp(ip_, "0.0.0.0") != 0;
}

void NetManager::refreshLinkInfo() {
  const IPAddress ip = WiFi.localIP();
  snprintf(ip_, sizeof(ip_), "%u.%u.%u.%u",
           static_cast<unsigned>(ip[0]), static_cast<unsigned>(ip[1]),
           static_cast<unsigned>(ip[2]), static_cast<unsigned>(ip[3]));

  const char* s = WiFi.SSID();
  if (s != nullptr) {
    strncpy(ssid_, s, sizeof(ssid_) - 1);
    ssid_[sizeof(ssid_) - 1] = '\0';
  }

  rssi_ = WiFi.RSSI();
}

void NetManager::tick(uint32_t now, bool radioReconnectAllowed) {
  if (suspended_) {
    // Someone else owns the radio right now. Relay timing is unaffected --
    // the pump state machine never depends on the network.
    return;
  }

  switch (phase_) {
    case Phase::IDLE_START:
      if (radioReconnectAllowed) {
        startAttempt(now);
      }
      break;

    case Phase::CONNECTING: {
      if (static_cast<uint32_t>(now - lastPollAt_) < WIFI_POLL_INTERVAL_MS) {
        break;
      }
      lastPollAt_ = now;

      if (WiFi.status() == WL_CONNECTED) {
        // Associated, but DHCP has almost certainly not bound yet. Announcing
        // now would advertise 0.0.0.0 as the device address.
        Serial.println(F("[WIFI] associated; waiting for DHCP"));
        phase_ = Phase::AWAIT_IP;
        phaseStartedAt_ = now;
      } else if (static_cast<uint32_t>(now - phaseStartedAt_) >= WIFI_CONNECT_TIMEOUT_MS) {
        Serial.println(F("[WIFI] association timed out; backing off"));
        phase_ = Phase::BACKOFF;
        phaseStartedAt_ = now;
      }
      break;
    }

    case Phase::AWAIT_IP: {
      if (static_cast<uint32_t>(now - lastPollAt_) < WIFI_POLL_INTERVAL_MS) {
        break;
      }
      lastPollAt_ = now;

      if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[WIFI] lost association before DHCP completed"));
        phase_ = Phase::BACKOFF;
        phaseStartedAt_ = now;
        break;
      }

      refreshLinkInfo();
      if (haveAddress()) {
        phase_ = Phase::CONNECTED;
        phaseStartedAt_ = now;
        Serial.println(F("[WIFI] connected"));
        printBanner();
      } else if (static_cast<uint32_t>(now - phaseStartedAt_) >= DHCP_TIMEOUT_MS) {
        Serial.println(F("[WIFI] DHCP did not bind an address; retrying"));
        strcpy(ip_, "0.0.0.0");
        phase_ = Phase::BACKOFF;
        phaseStartedAt_ = now;
      }
      break;
    }

    case Phase::CONNECTED: {
      if (static_cast<uint32_t>(now - lastPollAt_) < WIFI_POLL_INTERVAL_MS) {
        break;
      }
      lastPollAt_ = now;

      if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[WIFI] link lost"));
        strcpy(ip_, "0.0.0.0");
        rssi_ = 0;
        phase_ = Phase::BACKOFF;
        phaseStartedAt_ = now;
        // Relay timing is unaffected: the pump state machine runs from
        // millis() and does not depend on the network in any way.
      } else {
        // Keep the reported signal strength reasonably fresh.
        rssi_ = WiFi.RSSI();
      }
      break;
    }

    case Phase::BACKOFF: {
      if (static_cast<uint32_t>(now - phaseStartedAt_) < WIFI_RETRY_INTERVAL_MS) {
        break;
      }
      if (!radioReconnectAllowed) {
        // A relay sequence is running. Do not touch the radio; try again on a
        // later iteration. Safety timing always wins.
        break;
      }
      startAttempt(now);
      break;
    }
  }
}

void NetManager::printBanner() const {
  Serial.print(F("Device: "));
  Serial.println(DEVICE_HOSTNAME);
  Serial.print(F("Firmware: "));
  Serial.println(FIRMWARE_VERSION);
  Serial.print(F("Wi-Fi connected: "));
  Serial.println(ssid_);
  Serial.print(F("IP: "));
  Serial.println(ip_);
  Serial.print(F("MAC: "));
  Serial.println(mac_);
  Serial.print(F("RSSI: "));
  Serial.print(rssi_);
  Serial.println(F(" dBm"));
  Serial.print(F("HTTP: http://"));
  Serial.print(ip_);
  Serial.print(F(":"));
  Serial.println(HTTP_PORT);
}
