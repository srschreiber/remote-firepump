// arduino_secrets.example.h
//
// Copy this file to `arduino_secrets.h` in the same directory and fill in the
// real values. `arduino_secrets.h` is git-ignored and must never be committed.
//
//     cp arduino_secrets.example.h arduino_secrets.h
//
// The firmware never prints WIFI_PASSWORD or PUMP_API_SECRET to Serial, and
// never echoes a supplied secret back over HTTP.

#pragma once

// 2.4 GHz network only. The UNO R4 WiFi has no 5 GHz radio.
#define WIFI_SSID     "replace-me"
#define WIFI_PASSWORD "replace-me"

// Shared secret sent by the Raspberry Pi in the X-Pump-Secret header on every
// request. Generate something long and random, for example:
//
//     openssl rand -hex 32
//
// If this is left empty the firmware fails closed and answers 401 to every
// request, including /v1/status.
#define PUMP_API_SECRET "replace-with-a-long-random-secret"
