#pragma once
#include <Arduino.h>

// PIO-driven square-wave generator. Each FIFO word is one half-wave:
// bit 0 = output level, bits 1..31 = duration in Z80 T-states (3.5 MHz).
namespace tape_out {

constexpr uint32_t CLOCK_HZ = 3500000;  // ZX Spectrum 48K CPU clock

void begin(uint pin);
bool canPush();
void push(bool level, uint32_t tstates);
bool fifoEmpty();
// Drop anything queued and park the output low.
void abort();
// Float the output (high impedance) so it doesn't load the shared jack while
// recording, and take it back afterwards.
void release();
void drive();

}  // namespace tape_out
