#pragma once
#include <Arduino.h>

// User settings, persisted as /settings.txt (key=value, editable over USB).
// Every setting is an index into its option's list of choices.
struct Settings {
    uint8_t blockGap = 1;       // 0.5 / 1 / 2 s of silence between blocks
    uint8_t shortPilot = 0;     // header pilot: full (5 s) / short (2 s)
    uint8_t stopAfterData = 0;  // pause the tape after each data block (multi-load games)
    uint8_t recAutoSave = 0;    // save a recording after 8 s of silence
    uint8_t knobReverse = 0;
    uint8_t contrast = 3;
    uint8_t led = 1;
    uint8_t screenOff = 2;      // never / 1 / 5 / 15 min
};

extern Settings settings;

struct SettingOption {
    const char* key;    // in settings.txt
    const char* label;  // on screen
    uint8_t* value;
    uint8_t count;
    const char* const* names;
};

const SettingOption* settingOptions(int& count);
void loadSettings();
void saveSettings();

uint32_t blockGapMs();
uint8_t contrastLevel();
uint8_t ledBrightness();
uint32_t screenOffMs();      // 0 = never
constexpr uint32_t REC_AUTOSAVE_MS = 8000;
