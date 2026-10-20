#include "settings.h"

#include <FatFS.h>

Settings settings;

namespace {

constexpr const char* PATH = "/settings.txt";

const char* const GAP_NAMES[] = {"0.5s", "1s", "2s"};
const char* const PILOT_NAMES[] = {"full", "short"};
const char* const OFF_ON[] = {"off", "on"};
const char* const AUTOSAVE_NAMES[] = {"off", "8s"};
const char* const KNOB_NAMES[] = {"normal", "reversed"};
const char* const CONTRAST_NAMES[] = {"low", "med", "high", "max"};
const char* const LED_NAMES[] = {"off", "dim", "bright"};
const char* const SCREEN_OFF_NAMES[] = {"never", "1min", "5min", "15min"};

const SettingOption OPTIONS[] = {
    {"block_gap", "Block gap", &settings.blockGap, 3, GAP_NAMES},
    {"header_pilot", "Header pilot", &settings.shortPilot, 2, PILOT_NAMES},
    {"stop_after_data", "Stop after data", &settings.stopAfterData, 2, OFF_ON},
    {"rec_autosave", "Rec auto-save", &settings.recAutoSave, 2, AUTOSAVE_NAMES},
    {"knob", "Knob", &settings.knobReverse, 2, KNOB_NAMES},
    {"contrast", "Contrast", &settings.contrast, 4, CONTRAST_NAMES},
    {"led", "LED", &settings.led, 3, LED_NAMES},
    {"screen_off", "Screen off", &settings.screenOff, 4, SCREEN_OFF_NAMES},
};
constexpr int OPTION_COUNT = sizeof OPTIONS / sizeof OPTIONS[0];

}  // namespace

const SettingOption* settingOptions(int& count) {
    count = OPTION_COUNT;
    return OPTIONS;
}

void loadSettings() {
    File f = FatFS.open(PATH, "r");
    if (!f) return;  // first boot: keep defaults
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (line.startsWith("#") || eq < 0) continue;
        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);
        key.trim();
        val.trim();
        for (const auto& o : OPTIONS) {
            if (!key.equalsIgnoreCase(o.key)) continue;
            for (uint8_t i = 0; i < o.count; i++)
                if (val.equalsIgnoreCase(o.names[i])) *o.value = i;
        }
    }
    f.close();
}

void saveSettings() {
    File f = FatFS.open(PATH, "w");
    if (!f) return;
    f.print("# PicoTape ZX settings\n");
    for (const auto& o : OPTIONS) {
        f.print(o.key);
        f.print('=');
        f.print(o.names[*o.value]);
        f.print('\n');
    }
    f.close();
}

uint32_t blockGapMs() {
    static const uint16_t MS[] = {500, 1000, 2000};
    return MS[settings.blockGap];
}

uint8_t contrastLevel() {
    static const uint8_t LEVEL[] = {8, 48, 140, 255};
    return LEVEL[settings.contrast];
}

uint8_t ledBrightness() {
    static const uint8_t LEVEL[] = {0, 16, 80};
    return LEVEL[settings.led];
}

uint32_t screenOffMs() {
    static const uint32_t MS[] = {0, 60000, 300000, 900000};
    return MS[settings.screenOff];
}
