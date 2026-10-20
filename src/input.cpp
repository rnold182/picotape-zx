#include "input.h"

namespace {

// This HW-040 moves half a quadrature cycle per detent (each pin toggles once).
// If one click moves the cursor two places, set 4; if it takes two clicks, set 1.
constexpr int COUNTS_PER_DETENT = 2;
// Flip if turning clockwise moves the selection up.
constexpr bool REVERSE = false;

constexpr uint32_t DEBOUNCE_MS = 20;
constexpr uint32_t LONG_PRESS_MS = 700;

// Indexed by (previous AB << 2) | current AB.
const int8_t QUAD_TABLE[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

uint encA, encB;
volatile uint8_t encState;
volatile int32_t encCount;

void encoderIsr() {
    uint8_t ab = (gpio_get(encA) ? 2 : 0) | (gpio_get(encB) ? 1 : 0);
    encState = ((encState << 2) | ab) & 0x0F;
    encCount += QUAD_TABLE[encState];
}

}  // namespace

void Encoder::begin(uint pinA, uint pinB) {
    encA = pinA;
    encB = pinB;
    // The HW-040 board has its own 10k pull-ups (power it from 3V3!), internal ones don't hurt.
    pinMode(encA, INPUT_PULLUP);
    pinMode(encB, INPUT_PULLUP);
    encState = (gpio_get(encA) ? 2 : 0) | (gpio_get(encB) ? 1 : 0);
    attachInterrupt(digitalPinToInterrupt(encA), encoderIsr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(encB), encoderIsr, CHANGE);
}

int Encoder::takeSteps() {
    noInterrupts();
    int32_t detents = encCount / COUNTS_PER_DETENT;
    encCount -= detents * COUNTS_PER_DETENT;
    interrupts();
    return REVERSE ? -detents : detents;
}

int32_t Encoder::rawCount() { return encCount; }

void Button::begin(uint pin) {
    pin_ = pin;
    pinMode(pin_, INPUT_PULLUP);
}

Button::Event Button::poll() {
    bool raw = !digitalRead(pin_);
    uint32_t now = millis();
    if (raw != raw_) {
        raw_ = raw;
        changedAt_ = now;
    }
    if (now - changedAt_ >= DEBOUNCE_MS && raw_ != pressed_) {
        pressed_ = raw_;
        if (pressed_) {
            pressedAt_ = now;
            longFired_ = false;
        } else if (!longFired_) {
            return Click;
        }
    }
    if (pressed_ && !longFired_ && now - pressedAt_ >= LONG_PRESS_MS) {
        longFired_ = true;
        return Long;
    }
    return None;
}
