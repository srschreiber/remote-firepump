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
// Three separately drawn sizes rather than one flame with its top masked off.
// Masking removed the tapering tip -- the part that actually reads as "flame"
// -- and left only the wide, static base, so the resting animation looked
// like a couple of rows of blocks twitching. Each size is now a complete
// flame shape with its own moving tip.

// Resting: UNKNOWN / IDLE / RETRY_WAIT. Small, but unmistakably a flame.
const uint16_t FLAME_SMALL[][MATRIX_ROWS] = {
    {   // upright
        0b000000000000,
        0b000000000000,
        0b000000000000,
        0b000000000000,
        0b000001000000,
        0b000011100000,
        0b000111110000,
        0b000111110000,
    },
    {   // tip stretches up and leans left
        0b000000000000,
        0b000000000000,
        0b000000000000,
        0b000010000000,
        0b000011000000,
        0b000111100000,
        0b000111110000,
        0b001111110000,
    },
    {   // leaning right
        0b000000000000,
        0b000000000000,
        0b000000000000,
        0b000000000000,
        0b000001100000,
        0b000011110000,
        0b000111110000,
        0b001111111000,
    },
    {   // spark breaking away
        0b000000000000,
        0b000000000000,
        0b000000000000,
        0b000001000000,
        0b000000000000,
        0b000011100000,
        0b000111110000,
        0b000111110000,
    },
    {   // ducked down to an ember
        0b000000000000,
        0b000000000000,
        0b000000000000,
        0b000000000000,
        0b000000000000,
        0b000011100000,
        0b000111110000,
        0b001111111000,
    },
};

// Building: CHOKING / STOPPING.
const uint16_t FLAME_MEDIUM[][MATRIX_ROWS] = {
    {   // upright
        0b000000000000,
        0b000000000000,
        0b000001000000,
        0b000011100000,
        0b000011100000,
        0b000111110000,
        0b001111111000,
        0b001111111000,
    },
    {   // leaning left
        0b000000000000,
        0b000010000000,
        0b000011000000,
        0b000111000000,
        0b000111100000,
        0b001111100000,
        0b001111111000,
        0b011111111000,
    },
    {   // leaning right
        0b000000000000,
        0b000000000000,
        0b000001100000,
        0b000011100000,
        0b000011110000,
        0b000111110000,
        0b001111111000,
        0b011111111000,
    },
    {   // spark breaking away from the tip
        0b000001000000,
        0b000000000000,
        0b000011000000,
        0b000011100000,
        0b000111110000,
        0b000111110000,
        0b001111111000,
        0b001111111000,
    },
};

// Full: CRANKING / UNCHOKING / RUNNING_ASSUMED.
const uint16_t FLAME_LARGE[][MATRIX_ROWS] = {
    {   // upright
        0b000001000000,
        0b000011100000,
        0b000011100000,
        0b000111110000,
        0b000111110000,
        0b001111111000,
        0b011111111100,
        0b011111111100,
    },
    {   // leaning left
        0b000010000000,
        0b000011000000,
        0b000111000000,
        0b000111100000,
        0b001111100000,
        0b001111111000,
        0b011111111100,
        0b011111111100,
    },
    {   // leaning right
        0b000000100000,
        0b000001100000,
        0b000001110000,
        0b000011110000,
        0b000111111000,
        0b001111111000,
        0b011111111100,
        0b011111111100,
    },
    {   // spark breaking away, base flaring
        0b000001000000,
        0b000000000000,
        0b000011100000,
        0b000111110000,
        0b000111110000,
        0b001111111000,
        0b011111111100,
        0b111111111110,
    },
    {   // ducked
        0b000000000000,
        0b000001000000,
        0b000011100000,
        0b000011100000,
        0b000111110000,
        0b001111111000,
        0b011111111100,
        0b011111111100,
    },
    {   // full flare
        0b000010000000,
        0b000011100000,
        0b000111110000,
        0b000111110000,
        0b001111111000,
        0b011111111100,
        0b011111111100,
        0b111111111110,
    },
};

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

// Which flame to draw, and how fast it dances, per state.
struct Look {
  const uint16_t (*frames)[MATRIX_ROWS];
  uint8_t  frameCount;
  uint32_t intervalMs;
};

constexpr uint8_t SMALL_COUNT  = sizeof(FLAME_SMALL)  / sizeof(FLAME_SMALL[0]);
constexpr uint8_t MEDIUM_COUNT = sizeof(FLAME_MEDIUM) / sizeof(FLAME_MEDIUM[0]);
constexpr uint8_t LARGE_COUNT  = sizeof(FLAME_LARGE)  / sizeof(FLAME_LARGE[0]);

Look lookFor(PumpState state) {
  switch (state) {
    // Resting. Small, but always visibly alive -- this is what the controller
    // shows almost all of the time, so it needs to look like something.
    case PumpState::UNKNOWN:         return {FLAME_SMALL,  SMALL_COUNT,  200};
    case PumpState::IDLE:            return {FLAME_SMALL,  SMALL_COUNT,  180};
    case PumpState::RETRY_WAIT:      return {FLAME_SMALL,  SMALL_COUNT,  140};

    case PumpState::CHOKING:         return {FLAME_MEDIUM, MEDIUM_COUNT, 120};
    case PumpState::STOPPING:        return {FLAME_MEDIUM, MEDIUM_COUNT, 110};

    case PumpState::CRANKING:        return {FLAME_LARGE,  LARGE_COUNT,   65};
    case PumpState::UNCHOKING:       return {FLAME_LARGE,  LARGE_COUNT,   85};
    case PumpState::RUNNING_ASSUMED: return {FLAME_LARGE,  LARGE_COUNT,   80};

    case PumpState::FAULT:           return {nullptr, 0, 250};
  }
  return {FLAME_SMALL, SMALL_COUNT, 200};
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

void StatusMatrix::render(const uint16_t rows[MATRIX_ROWS]) {
  // One byte per LED, as ArduinoLEDMatrix::loadPixels expects. Expanding here
  // rather than storing pre-packed frames keeps the artwork above readable
  // and costs 96 bytes of stack for the duration of this call.
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

  if (look.frames == nullptr) {
    // FAULT: blink an X, deliberately not flame-shaped, so it is obvious at a
    // glance that this needs an operator rather than being normal running.
    render((frameIndex_ & 1u) ? FAULT_X : BLANK);
    return;
  }

  render(look.frames[frameIndex_ % look.frameCount]);
}

#endif  // ENABLE_LED_MATRIX
