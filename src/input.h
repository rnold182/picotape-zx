#pragma once
#include <Arduino.h>

// HW-040 / KY-040 quadrature encoder, decoded in GPIO interrupts.
class Encoder {
public:
    void begin(uint pinA, uint pinB);
    // Detents turned since the last call (positive = clockwise).
    int takeSteps();
    void setReversed(bool r) { reversed_ = r; }
    // Raw quadrature transitions counted so far (for the test screen).
    int32_t rawCount();

private:
    bool reversed_ = false;
};

// Debounced active-low push button with click / long-press events.
class Button {
public:
    enum Event : uint8_t { None, Click, Long };

    void begin(uint pin);
    Event poll();

private:
    uint pin_ = 0;
    bool raw_ = false;
    bool pressed_ = false;
    bool longFired_ = false;
    uint32_t changedAt_ = 0;
    uint32_t pressedAt_ = 0;
};
