// led_matrix.cpp — see led_matrix.h.

#include "led_matrix.h"

#if ENABLE_LED_MATRIX

#include "Arduino_LED_Matrix.h"

namespace {

ArduinoLEDMatrix g_matrix;

constexpr uint8_t MATRIX_ROWS = 8;
constexpr uint8_t MATRIX_COLS = 12;

// Frames are authored as one 12-bit word per row, top row first, with the
// most significant of the 12 bits being column 0. Written as binary literals
// so the artwork is legible in the source.
//
// A six-frame flame that leans, flickers and throws the odd spark.
const uint16_t FLAME[][MATRIX_ROWS] = {
    {   // upright
        0b000000000000,
        0b000001000000,
        0b000011100000,
        0b000111100000,
        0b001111110000,
        0b001111110000,
        0b011111111000,
        0b011111111000,
    },
    {   // leaning left
        0b000010000000,
        0b000011000000,
        0b000111000000,
        0b000111100000,
        0b001111100000,
        0b011111110000,
        0b011111111000,
        0b011111111000,
    },
    {   // leaning right
        0b000001000000,
        0b000001100000,
        0b000011100000,
        0b000111110000,
        0b001111110000,
        0b001111111000,
        0b011111111000,
        0b011111111100,
    },
    {   // ducked low
        0b000000000000,
        0b000000000000,
        0b000011000000,
        0b000111100000,
        0b001111100000,
        0b001111110000,
        0b011111111000,
        0b011111111000,
    },
    {   // spark breaking away from the tip
        0b000010000000,
        0b000000000000,
        0b000011000000,
        0b000111100000,
        0b000111110000,
        0b001111110000,
        0b011111111000,
        0b011111111100,
    },
    {   // flaring at the base
        0b000000000000,
        0b000011000000,
        0b000111000000,
        0b001111100000,
        0b001111110000,
        0b011111110000,
        0b011111111000,
        0b111111111100,
    },
};
constexpr uint8_t FLAME_FRAMES = sizeof(FLAME) / sizeof(FLAME[0]);

// A fault must not look like a healthy flame, so it gets its own shape.
const uint16_t FAULT_X[MATRIX_ROWS] = {
    0b000000000000,
    0b011000000110,
    0b001100001100,
    0b000110011000,
    0b000011110000,
    0b000110011000,
    0b001100001100,
    0b011000000110,
};

const uint16_t BLANK[MATRIX_ROWS] = {0, 0, 0, 0, 0, 0, 0, 0};

// How much of the flame is shown, and how fast it dances, per state.
struct Look {
  uint8_t  visibleRows;    // bottom N rows of the flame frame
  uint32_t intervalMs;
};

Look lookFor(PumpState state) {
  switch (state) {
    case PumpState::UNKNOWN:         return {2, 420};
    case PumpState::IDLE:            return {2, 380};
    case PumpState::RETRY_WAIT:      return {2, 300};
    case PumpState::CHOKING:         return {4, 150};
    case PumpState::CRANKING:        return {6, 90};
    case PumpState::UNCHOKING:       return {7, 110};
    case PumpState::RUNNING_ASSUMED: return {8, 100};
    case PumpState::STOPPING:        return {3, 130};
    case PumpState::FAULT:           return {8, 250};
  }
  return {2, 400};
}

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

void StatusMatrix::render(const uint16_t rows[8], uint8_t visibleRows) {
  // One byte per LED, as ArduinoLEDMatrix::loadPixels expects. Expanding here
  // rather than storing pre-packed frames keeps the artwork above readable
  // and costs 96 bytes of stack for the duration of this call.
  uint8_t pixels[MATRIX_ROWS * MATRIX_COLS];

  const uint8_t firstVisible =
      (visibleRows >= MATRIX_ROWS)
          ? 0
          : static_cast<uint8_t>(MATRIX_ROWS - visibleRows);

  for (uint8_t r = 0; r < MATRIX_ROWS; ++r) {
    const uint16_t bits = (r >= firstVisible) ? rows[r] : 0u;
    for (uint8_t c = 0; c < MATRIX_COLS; ++c) {
      const uint8_t shift = static_cast<uint8_t>(MATRIX_COLS - 1 - c);
      pixels[r * MATRIX_COLS + c] =
          static_cast<uint8_t>((bits >> shift) & 0x1u);
    }
  }

  g_matrix.loadPixels(pixels, sizeof(pixels));
}

void StatusMatrix::tick(uint32_t now, PumpState state, FaultCode fault) {
  (void)fault;
  if (!started_) {
    return;
  }

  const Look look = lookFor(state);
  if (static_cast<uint32_t>(now - lastFrameAt_) < look.intervalMs) {
    return;
  }
  lastFrameAt_ = now;
  ++frameIndex_;

  if (state == PumpState::FAULT) {
    // Blink so it is obvious something needs an operator.
    render((frameIndex_ & 1u) ? FAULT_X : BLANK, MATRIX_ROWS);
    return;
  }

  render(FLAME[frameIndex_ % FLAME_FRAMES], look.visibleRows);
}

#endif  // ENABLE_LED_MATRIX
