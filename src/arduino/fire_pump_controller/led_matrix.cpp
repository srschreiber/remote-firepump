// led_matrix.cpp — see led_matrix.h.

#include "led_matrix.h"

#if ENABLE_LED_MATRIX

#include <string.h>

#include "Arduino_LED_Matrix.h"

namespace {

ArduinoLEDMatrix g_matrix;

constexpr uint8_t MATRIX_ROWS = 8;
constexpr uint8_t MATRIX_COLS = 12;

// ---------------------------------------------------------------------------
// 4x6 uppercase font
// ---------------------------------------------------------------------------
//
// Six rows per glyph, four pixels wide, one blank column between glyphs. Only
// the low nibble of each byte is used; bit 3 is the leftmost pixel.
//
// 4x6 rather than the more legible 5x7 because the panel is only 8 rows tall:
// a 6-row glyph leaves one blank row above and one below, which stops the
// text touching the edges and makes it readable at a distance. At 5 px per
// character only 2.4 characters fit across 12 columns, so everything scrolls.
constexpr uint8_t GLYPH_ROWS = 6;
constexpr uint8_t GLYPH_W = 4;
constexpr uint8_t GLYPH_ADVANCE = GLYPH_W + 1;  // 1 px inter-character gap

const uint8_t FONT[26][GLYPH_ROWS] = {
    {0x6, 0x9, 0x9, 0xF, 0x9, 0x9},  // A
    {0xE, 0x9, 0xE, 0x9, 0x9, 0xE},  // B
    {0x7, 0x8, 0x8, 0x8, 0x8, 0x7},  // C
    {0xE, 0x9, 0x9, 0x9, 0x9, 0xE},  // D
    {0xF, 0x8, 0xE, 0x8, 0x8, 0xF},  // E
    {0xF, 0x8, 0xE, 0x8, 0x8, 0x8},  // F
    {0x7, 0x8, 0xB, 0x9, 0x9, 0x7},  // G
    {0x9, 0x9, 0xF, 0x9, 0x9, 0x9},  // H
    {0xE, 0x4, 0x4, 0x4, 0x4, 0xE},  // I
    {0x7, 0x2, 0x2, 0x2, 0xA, 0x4},  // J
    {0x9, 0xA, 0xC, 0xC, 0xA, 0x9},  // K
    {0x8, 0x8, 0x8, 0x8, 0x8, 0xF},  // L
    {0x9, 0xF, 0xF, 0x9, 0x9, 0x9},  // M
    {0x9, 0xD, 0xD, 0xB, 0xB, 0x9},  // N
    {0x6, 0x9, 0x9, 0x9, 0x9, 0x6},  // O
    {0xE, 0x9, 0x9, 0xE, 0x8, 0x8},  // P
    {0x6, 0x9, 0x9, 0xB, 0xA, 0x5},  // Q
    {0xE, 0x9, 0x9, 0xE, 0xA, 0x9},  // R
    {0x7, 0x8, 0x6, 0x1, 0x1, 0xE},  // S
    {0xF, 0x4, 0x4, 0x4, 0x4, 0x4},  // T
    {0x9, 0x9, 0x9, 0x9, 0x9, 0x6},  // U
    {0x9, 0x9, 0x9, 0x9, 0x6, 0x6},  // V
    {0x9, 0x9, 0x9, 0xF, 0xF, 0x9},  // W
    {0x9, 0x9, 0x6, 0x6, 0x9, 0x9},  // X
    {0x9, 0x9, 0x6, 0x4, 0x4, 0x4},  // Y
    {0xF, 0x1, 0x2, 0x4, 0x8, 0xF},  // Z
};

// Returns the glyph rows for a character, or nullptr for a blank column run.
const uint8_t* glyphFor(char c) {
  if (c >= 'A' && c <= 'Z') {
    return FONT[c - 'A'];
  }
  if (c >= 'a' && c <= 'z') {
    return FONT[c - 'a'];
  }
  return nullptr;  // space and anything unmapped render blank
}

// What each state is called on the panel.
//
// Deliberately shortened from the API names. The panel is a glance display
// across a shed, and "RUNNING ASSUMED" scrolling past takes long enough that
// the operator has usually looked away. The API and the web UI keep the exact
// names and the honest qualification; this is the two-second version.
const char* displayName(PumpState s) {
  switch (s) {
    case PumpState::UNKNOWN:         return "UNKNOWN";
    case PumpState::IDLE:            return "IDLE";
    case PumpState::PRIMING:         return "VALVE OPEN";
    case PumpState::CHOKING:         return "CHOKE";
    case PumpState::CRANKING:        return "CRANKING";
    case PumpState::UNCHOKING:       return "UNCHOKE";
    case PumpState::RUNNING_ASSUMED: return "RUNNING";
    case PumpState::STOPPING:        return "STOPPING";
    case PumpState::VALVE_CLOSING:   return "CLOSING";
    case PumpState::RETRY_WAIT:      return "WAIT";
    case PumpState::FAULT:           return "FAULT";
  }
  return "UNKNOWN";
}

// Milliseconds per scrolled column. 90 ms reads as a steady crawl: fast
// enough to get through "CRANKING" inside the two-second crank, slow enough
// to actually read.
constexpr uint32_t SCROLL_INTERVAL_MS = 90;

// FAULT blinks instead of scrolling smoothly, so it is distinguishable from
// normal operation without reading the word.
constexpr uint32_t FAULT_BLINK_MS = 400;

}  // namespace

bool StatusMatrix::begin() {
  started_ = (g_matrix.begin() != 0);
  if (started_) {
    Serial.println(F("[LED] status matrix started"));
  } else {
    Serial.println(F("[LED] no hardware timer free; matrix disabled"));
  }
  return started_;
}

void StatusMatrix::render(const uint16_t rows[MATRIX_ROWS]) {
  // One byte per LED, as ArduinoLEDMatrix::loadPixels expects. Expanding here
  // rather than storing pre-packed frames costs 96 bytes of stack for the
  // duration of this call and nothing else.
  uint8_t pixels[MATRIX_ROWS * MATRIX_COLS];

  for (uint8_t r = 0; r < MATRIX_ROWS; ++r) {
    const uint16_t bits = rows[r];
    for (uint8_t c = 0; c < MATRIX_COLS; ++c) {
      const uint8_t shift = static_cast<uint8_t>(MATRIX_COLS - 1 - c);
      pixels[r * MATRIX_COLS + c] =
          static_cast<uint8_t>((bits >> shift) & 0x1u);
    }
  }

  g_matrix.loadPixels(pixels, sizeof(pixels));
}

// Draws one 12-column window of `text` at horizontal offset `scrollX`.
//
// Written as an explicit windowed blit rather than using the library's
// textScrollLeft(): that call BLOCKS for the whole duration of the scroll,
// which would stop the pump state machine being serviced and, for a long
// enough string, let the watchdog expire mid-sequence. One column per tick
// keeps the display entirely inside the main loop's normal cadence.
void StatusMatrix::renderTextWindow(const char* text, int16_t scrollX) {
  uint16_t rows[MATRIX_ROWS] = {0};

  const int16_t textWidth =
      static_cast<int16_t>(strlen(text) * GLYPH_ADVANCE);

  for (uint8_t c = 0; c < MATRIX_COLS; ++c) {
    const int16_t srcX = static_cast<int16_t>(scrollX + c);
    if (srcX < 0 || srcX >= textWidth) {
      continue;  // outside the string: leave the column dark
    }

    const uint8_t glyphIndex = static_cast<uint8_t>(srcX / GLYPH_ADVANCE);
    const uint8_t glyphCol = static_cast<uint8_t>(srcX % GLYPH_ADVANCE);
    if (glyphCol >= GLYPH_W) {
      continue;  // the inter-character gap
    }

    const uint8_t* glyph = glyphFor(text[glyphIndex]);
    if (glyph == nullptr) {
      continue;
    }

    for (uint8_t r = 0; r < GLYPH_ROWS; ++r) {
      const uint8_t bit =
          static_cast<uint8_t>((glyph[r] >> (GLYPH_W - 1 - glyphCol)) & 0x1u);
      if (bit) {
        // +1 centres the 6-row glyph in 8 rows, leaving one blank row top
        // and bottom so the text does not touch the panel edge.
        rows[r + 1] |= static_cast<uint16_t>(1u << (MATRIX_COLS - 1 - c));
      }
    }
  }

  render(rows);
}

void StatusMatrix::tick(uint32_t now, PumpState state, FaultCode fault) {
  (void)fault;
  if (!started_) {
    return;
  }

  // A state change restarts the scroll, so the operator always sees the new
  // state from its first letter rather than joining it halfway through.
  if (state != lastState_) {
    lastState_ = state;
    scrollX_ = -static_cast<int16_t>(MATRIX_COLS);
    lastFrameAt_ = now;
    frameIndex_ = 0;
  }

  const bool faulted = (state == PumpState::FAULT);
  const uint32_t interval = faulted ? FAULT_BLINK_MS : SCROLL_INTERVAL_MS;
  if (static_cast<uint32_t>(now - lastFrameAt_) < interval) {
    return;
  }
  lastFrameAt_ = now;

  const char* text = displayName(state);

  if (faulted) {
    // Blink the whole word on and off. Recognisable as "something is wrong"
    // before it is legible as a word.
    ++frameIndex_;
    if (frameIndex_ & 1u) {
      const uint16_t blank[MATRIX_ROWS] = {0};
      render(blank);
    } else {
      renderTextWindow(text, 0);
    }
    return;
  }

  renderTextWindow(text, scrollX_);

  // Scroll left, then restart from fully off-screen right. The +MATRIX_COLS
  // tail gives a clear blank gap between repeats so two loops do not read as
  // one long string.
  ++scrollX_;
  const int16_t textWidth = static_cast<int16_t>(strlen(text) * GLYPH_ADVANCE);
  if (scrollX_ >= textWidth) {
    scrollX_ = -static_cast<int16_t>(MATRIX_COLS);
  }
}

#endif  // ENABLE_LED_MATRIX
