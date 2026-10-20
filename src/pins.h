#pragma once
#include <Arduino.h>

// Waveshare RP2040-Zero pin assignment (all on the side headers).
constexpr uint PIN_TAPE_OUT = 2;   // -> Spectrum EAR (via divider + jack)
constexpr uint PIN_I2C_SDA  = 4;   // GM009605 / SSD1306
constexpr uint PIN_I2C_SCL  = 5;
constexpr uint PIN_ENC_A    = 6;   // HW-040 CLK
constexpr uint PIN_ENC_B    = 7;   // HW-040 DT
constexpr uint PIN_ENC_SW   = 8;   // HW-040 SW
constexpr uint PIN_BTN_BACK = 14;  // optional Back/Stop button to GND
constexpr uint PIN_TAPE_IN  = 26;  // ADC0 <- Spectrum MIC (recording, milestone 3)
constexpr uint PIN_LED      = 16;  // on-board WS2812
