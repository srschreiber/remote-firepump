// led_matrix.h — 12x8 LED matrix status display for the UNO R4 WiFi.
//
// Replaces the board's factory "heart" animation with the controller state,
// scrolled as text, so the pump's status is readable at a glance from across
// the shed:
//
//   UNKNOWN  IDLE  VALVE OPEN  CHOKE  CRANKING  UNCHOKE
//   RUNNING  STOPPING  CLOSING  WAIT           FAULT (blinks)
//
// An earlier version animated a flame whose height reflected the state. It
// looked better, but it could not answer the only question actually being
// asked from across a shed -- "which phase is it in?" -- without the operator
// having memorised what four flame heights meant.
//
// This is cosmetic and entirely optional. It is compiled out with
// ENABLE_LED_MATRIX=0.
//
// SAFETY NOTES
//   * The matrix is charlieplexed on the RA4M1's internal pins (g_pin_cfg
//     index 28 and above). It does not touch D2-D6 or any relay line.
//   * Frames are only pushed from the main loop, after the pump state machine
//     has been serviced, and at most once per scroll interval. Pushing a
//     frame is a 96-byte copy into the scan buffer; it never blocks.
//   * The text is scrolled ONE COLUMN PER TICK by this file, deliberately not
//     with the library's textScrollLeft(). That call blocks for the entire
//     scroll, which would stop the state machine being serviced and, for a
//     long enough string, allow the ~5.6 s watchdog to expire mid-sequence.
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

  // Advances the scroll if enough time has passed. Cheap and non-blocking;
  // safe to call every main-loop iteration.
  void tick(uint32_t now, PumpState state, FaultCode fault);

 private:
  void render(const uint16_t rows[8]);

  // Draws one 12-column window of `text` at horizontal offset `scrollX`.
  // Negative offsets leave the leading columns dark, which is how each pass
  // enters from the right-hand edge.
  void renderTextWindow(const char* text, int16_t scrollX);

  bool      started_ = false;
  uint32_t  lastFrameAt_ = 0;
  uint8_t   frameIndex_ = 0;

  // Horizontal scroll position, in columns, into the rendered string. Starts
  // fully off-screen right so a new state scrolls in rather than appearing
  // mid-word.
  int16_t   scrollX_ = -12;

  // Last state drawn, so a change can restart the scroll from the beginning.
  PumpState lastState_ = PumpState::UNKNOWN;
};

#else  // !ENABLE_LED_MATRIX

// Empty stand-in so the sketch needs no conditionals of its own.
class StatusMatrix {
 public:
  bool begin() { return false; }
  void tick(uint32_t, PumpState, FaultCode) {}
};

#endif  // ENABLE_LED_MATRIX
