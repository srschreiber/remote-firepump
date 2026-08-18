// led_matrix.h — 12x8 LED matrix status display for the UNO R4 WiFi.
//
// Replaces the board's factory "heart" animation with a dancing flame whose
// height reflects the controller state, so the pump's status is readable at a
// glance from across the shed.
//
//   UNKNOWN / IDLE / RETRY_WAIT    small flame, gentle flicker
//   CHOKING / STOPPING             medium flame
//   CRANKING / UNCHOKING           full flame, dancing fast
//   RUNNING_ASSUMED                full flame
//   FAULT                          blinking X (deliberately not flame-shaped)
//
// Three separately drawn sizes, each a complete flame with its own moving
// tip. An earlier version drew one flame and masked its top rows away by
// state, which removed the taper that reads as "flame" and left the resting
// animation looking like two rows of blocks twitching.
//
// This is cosmetic and entirely optional. It is compiled out with
// ENABLE_LED_MATRIX=0.
//
// SAFETY NOTES
//   * The matrix is charlieplexed on the RA4M1's internal pins (g_pin_cfg
//     index 28 and above). It does not touch D2-D5 or any relay line.
//   * Frames are only pushed from the main loop, after the pump state machine
//     has been serviced, and at most once every FRAME_INTERVAL. Pushing a
//     frame is a 12-byte memcpy into the scan buffer; it never blocks.
//   * Arduino_LED_Matrix::begin() starts a periodic timer ISR that refreshes
//     the display. That ISR does not touch the relay pins, the pump state
//     machine, or millis().

#pragma once

#include <Arduino.h>

#include "config.h"
#include "pump_controller.h"

#if ENABLE_LED_MATRIX

class StatusMatrix {
 public:
  // Starts the refresh timer. Returns false if no hardware timer was free, in
  // which case every later call is a no-op.
  bool begin();

  // Advances the animation if enough time has passed. Cheap and non-blocking;
  // safe to call every main-loop iteration.
  void tick(uint32_t now, PumpState state, FaultCode fault);

 private:
  void render(const uint16_t rows[8]);

  bool     started_ = false;
  uint32_t lastFrameAt_ = 0;
  uint8_t  frameIndex_ = 0;
};

#else  // !ENABLE_LED_MATRIX

// Empty stand-in so the sketch needs no conditionals of its own.
class StatusMatrix {
 public:
  bool begin() { return false; }
  void tick(uint32_t, PumpState, FaultCode) {}
};

#endif  // ENABLE_LED_MATRIX
