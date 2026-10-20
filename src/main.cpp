// ZX Spectrum 48K tape deck - Waveshare RP2040-Zero
// Core 0: UI, file system, USB drive.  Core 1: tape signal engine.

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <FatFS.h>
#include <FatFSUSB.h>
#include <U8g2lib.h>
#include <Wire.h>

#include <algorithm>
#include <vector>

#include "input.h"
#include "pins.h"
#include "recorder.h"
#include "tap_player.h"

namespace {

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
// If red and green come out swapped on your board, use NEO_RGB.
Adafruit_NeoPixel led(1, PIN_LED, NEO_GRB + NEO_KHZ800);

Encoder encoder;
Button knob;
Button backBtn;

enum class Screen { Browse, Player, Record, Usb };
Screen screen = Screen::Browse;

// Browse list: fixed actions first, then files.
enum : int { ITEM_REC = 0, ITEM_USB = 1, FIRST_FILE = 2 };
std::vector<String> files;
int selected = FIRST_FILE;
int scrollTop = 0;
String playingName;
String message;  // one-line notice on the browse screen

bool dirty = true;
uint32_t lastDraw = 0;

// USB drive state, touched from USB callbacks.
volatile bool usbEnabled = false;
volatile bool usbMounted = false;
volatile bool needRescan = true;

constexpr int ROWS = 5;
constexpr int LINE_CHARS = 21;

int itemCount() { return FIRST_FILE + (int)files.size(); }

String clip(const String& s, int n = LINE_CHARS) {
    return (int)s.length() <= n ? s : s.substring(0, n - 1) + "~";
}

void scanFiles() {
    files.clear();
    Dir dir = FatFS.openDir("/");
    while (dir.next()) {
        if (!dir.isFile()) continue;
        String name = dir.fileName();
        String lower = name;
        lower.toLowerCase();
        if (lower.endsWith(".tap")) files.push_back(name);
    }
    std::sort(files.begin(), files.end(),
              [](const String& a, const String& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    selected = constrain(selected, 0, itemCount() - 1);
    if (files.empty()) selected = ITEM_REC;
}

void selectFile(const String& name) {
    for (size_t i = 0; i < files.size(); i++)
        if (files[i] == name) selected = FIRST_FILE + (int)i;
}

// --- USB mass storage callbacks ---

bool usbDriveReady(uint32_t) { return usbEnabled && !player::busy() && !recorder::active(); }

void usbPlug(uint32_t) {
    FatFS.end();
    usbMounted = true;
}

void usbUnplug(uint32_t) {
    FatFS.begin();
    usbMounted = false;
    usbEnabled = false;
    needRescan = true;
}

// --- drawing ---

void drawHeader(const char* left, const String& right) {
    oled.drawBox(0, 0, 128, 11);
    oled.setDrawColor(0);
    oled.drawStr(2, 9, left);
    oled.drawStr(126 - oled.getStrWidth(right.c_str()), 9, right.c_str());
    oled.setDrawColor(1);
}

void drawBrowse() {
    drawHeader("TAPE DECK", String(files.size()) + " files");
    if (selected < scrollTop) scrollTop = selected;
    if (selected >= scrollTop + ROWS) scrollTop = selected - ROWS + 1;

    for (int row = 0; row < ROWS; row++) {
        int i = scrollTop + row;
        if (i >= itemCount()) break;
        String label = i == ITEM_REC   ? String("[Record]")
                       : i == ITEM_USB ? String("[USB drive]")
                                       : files[i - FIRST_FILE];
        int y = 12 + row * 10;
        if (i == selected) {
            oled.drawBox(0, y, 128, 10);
            oled.setDrawColor(0);
        }
        oled.drawStr(2, y + 8, clip(label).c_str());
        oled.setDrawColor(1);
    }
    if (files.empty()) {
        oled.drawStr(2, 41, "No .TAP files yet.");
        oled.drawStr(2, 51, "Use [USB drive] to");
        oled.drawStr(2, 61, "copy some on.");
    } else if (message.length()) {
        oled.setDrawColor(0);
        oled.drawBox(0, 54, 128, 10);
        oled.setDrawColor(1);
        oled.drawStr(2, 62, clip(message).c_str());
    }
}

void drawPlayer() {
    const char* label = "";
    switch (player::state()) {
        case player::State::Playing: label = "> PLAY"; break;
        case player::State::Paused: label = "|| PAUSE"; break;
        case player::State::Done: label = "DONE"; break;
        case player::State::Idle: label = "STOP"; break;
    }
    const auto& blocks = player::blocks();
    int cur = player::currentBlock();
    String counter = cur >= 0 ? String(cur + 1) + "/" + blocks.size() : String(blocks.size()) + " blk";
    drawHeader(label, counter);

    oled.drawStr(2, 23, clip(playingName).c_str());
    if (cur >= 0 && cur < (int)blocks.size()) {
        oled.drawStr(2, 35, clip(player::describe(blocks[cur])).c_str());
        // A data block is usually described by the header just before it.
        if (blocks[cur].flag != 0 && cur > 0 && blocks[cur - 1].flag == 0)
            oled.drawStr(2, 45, clip("  " + String(blocks[cur - 1].name)).c_str());
    } else if (player::state() == player::State::Playing) {
        oled.drawStr(2, 35, "Lead-in...");
    }

    uint32_t size = player::fileSize();
    uint32_t done = player::state() == player::State::Done ? size : player::bytesPlayed();
    int pct = size ? (int)((uint64_t)done * 100 / size) : 0;
    oled.drawFrame(0, 52, 100, 10);
    oled.drawBox(2, 54, pct * 96 / 100, 6);
    String p = String(pct) + "%";
    oled.drawStr(127 - oled.getStrWidth(p.c_str()), 61, p.c_str());
}

void drawRecord() {
    uint32_t sec = recorder::elapsedMs() / 1000;
    char clock[16];
    snprintf(clock, sizeof clock, "REC %02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
    String right = String(recorder::blockCount()) + " blk";
    if (recorder::errorCount()) right += " " + String(recorder::errorCount()) + "!";
    drawHeader(clock, right);

    String activity;
    switch (recorder::phase()) {
        case recorder::Phase::Idle: activity = "Waiting for SAVE..."; break;
        case recorder::Phase::Pilot: activity = "Pilot tone"; break;
        case recorder::Phase::Data: activity = "Data " + String(recorder::liveBytes()) + " bytes"; break;
    }
    if (recorder::overrun()) activity = "! samples lost";
    oled.drawStr(2, 22, clip(activity).c_str());
    oled.drawStr(2, 33, clip(recorder::lastBlock()).c_str());

    // Input level: full bar = 1.65 V peak-to-peak; tick marks the decoder's minimum.
    uint8_t lvl = recorder::level();
    oled.drawStr(2, 45, "In");
    oled.drawFrame(16, 37, 82, 9);
    oled.drawBox(18, 39, min<int>(lvl, 128) * 78 / 128, 5);
    oled.drawVLine(18 + 6 * 78 / 128, 46, 2);
    char volts[8];
    int mv = lvl * 3300 / 255;
    snprintf(volts, sizeof volts, "%d.%02dV", mv / 1000, mv % 1000 / 10);
    oled.drawStr(101, 45, volts);

    oled.drawStr(2, 61, "click:save hold:drop");
}

void drawUsb() {
    drawHeader("USB DRIVE", usbMounted ? "mounted" : "waiting");
    if (usbMounted) {
        oled.drawStr(2, 25, "Copy .TAP files on,");
        oled.drawStr(2, 35, "then EJECT the drive");
        oled.drawStr(2, 45, "on your PC to return");
        oled.drawStr(2, 55, "to the tape deck.");
    } else {
        oled.drawStr(2, 25, "Connect to a PC via");
        oled.drawStr(2, 35, "USB-C. The drive will");
        oled.drawStr(2, 45, "appear in a moment.");
        oled.drawStr(2, 61, "Hold knob: cancel");
    }
}

void draw() {
    oled.clearBuffer();
    switch (screen) {
        case Screen::Browse: drawBrowse(); break;
        case Screen::Player: drawPlayer(); break;
        case Screen::Record: drawRecord(); break;
        case Screen::Usb: drawUsb(); break;
    }
    oled.sendBuffer();
    lastDraw = millis();
    dirty = false;
}

void updateLed() {
    uint32_t color = 0;
    if (screen == Screen::Usb) {
        color = led.Color(0, 0, 255);
    } else if (screen == Screen::Record) {
        color = led.Color(255, 0, 0);
    } else if (screen == Screen::Player) {
        switch (player::state()) {
            case player::State::Playing: color = led.Color(0, 255, 0); break;
            case player::State::Paused: color = led.Color(255, 160, 0); break;
            default: break;
        }
    }
    static uint32_t shown = 1;
    if (color != shown) {
        led.setPixelColor(0, color);
        led.show();
        shown = color;
    }
}

// --- input handling ---

void handleBrowse(int steps, Button::Event k, Button::Event back) {
    if (steps) {
        selected = constrain(selected + steps, 0, itemCount() - 1);
        message = "";
        dirty = true;
    }
    if (back != Button::None && message.length()) {
        message = "";
        dirty = true;
    }
    if (k != Button::Click) return;

    if (selected == ITEM_REC) {
        if (recorder::start()) screen = Screen::Record;
        else message = "Can't start recording";
    } else if (selected == ITEM_USB) {
        usbEnabled = true;
        screen = Screen::Usb;
    } else {
        playingName = files[selected - FIRST_FILE];
        if (player::open(("/" + playingName).c_str())) {
            screen = Screen::Player;
        } else {
            message = "Can't open file";
        }
    }
    dirty = true;
}

void handlePlayer(Button::Event k, Button::Event back) {
    bool done = player::state() == player::State::Done;
    if (k == Button::Long || back != Button::None || (k == Button::Click && done)) {
        player::stop();
        screen = Screen::Browse;
        dirty = true;
    } else if (k == Button::Click) {
        player::togglePause();
        dirty = true;
    }
}

void handleRecord(Button::Event k, Button::Event back) {
    if (k == Button::Click) {
        String saved = recorder::stop(true);
        message = saved.length() ? "Saved " + saved : String("Nothing recorded");
        scanFiles();
        if (saved.length()) selectFile(saved);
    } else if (k == Button::Long || back != Button::None) {
        recorder::stop(false);
        message = "Recording discarded";
    } else {
        return;
    }
    screen = Screen::Browse;
    dirty = true;
}

void handleUsb(Button::Event k, Button::Event back) {
    // Once mounted, only the PC can end it (by ejecting); pulling the
    // drive away underneath it would corrupt the file system.
    if (usbMounted) return;
    if (!usbEnabled) {  // ejected by the PC
        screen = Screen::Browse;
        dirty = true;
    } else if (k == Button::Long || back != Button::None) {
        usbEnabled = false;
        screen = Screen::Browse;
        dirty = true;
    }
}

// Hold the knob while powering up to get here. Shows raw encoder pin levels
// and counts so wiring problems are easy to spot. Never returns.
void encoderTest() {
    int32_t total = 0;
    for (;;) {
        total += encoder.takeSteps();
        oled.clearBuffer();
        drawHeader("ENCODER TEST", "");
        oled.drawStr(2, 23, ("CLK(GP6): " + String(gpio_get(PIN_ENC_A))).c_str());
        oled.drawStr(2, 33, ("DT (GP7): " + String(gpio_get(PIN_ENC_B))).c_str());
        oled.drawStr(2, 43, ("SW (GP8): " + String(gpio_get(PIN_ENC_SW))).c_str());
        oled.drawStr(2, 53, ("raw: " + String(encoder.rawCount())).c_str());
        oled.drawStr(2, 63, ("detents: " + String(total)).c_str());
        oled.sendBuffer();
        delay(30);
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);

    encoder.begin(PIN_ENC_A, PIN_ENC_B);
    knob.begin(PIN_ENC_SW);
    backBtn.begin(PIN_BTN_BACK);

    led.begin();
    led.setBrightness(24);
    led.show();

    Wire.setSDA(PIN_I2C_SDA);
    Wire.setSCL(PIN_I2C_SCL);
    oled.setBusClock(400000);
    oled.begin();
    oled.setFont(u8g2_font_6x10_tf);

    if (!digitalRead(PIN_ENC_SW)) encoderTest();

    if (!FatFS.begin()) {
        oled.clearBuffer();
        oled.drawStr(2, 30, "Formatting storage...");
        oled.sendBuffer();
        FatFS.format();
        FatFS.begin();
    }

    FatFSUSB.onPlug(usbPlug);
    FatFSUSB.onUnplug(usbUnplug);
    FatFSUSB.driveReady(usbDriveReady);
    FatFSUSB.begin();
}

void loop() {
    player::service();
    recorder::service();

    if (needRescan && !usbMounted) {
        needRescan = false;
        scanFiles();
        dirty = true;
    }

    int steps = encoder.takeSteps();
    Button::Event k = knob.poll();
    Button::Event back = backBtn.poll();

    static bool lastMounted = false;
    if (usbMounted != lastMounted) {
        lastMounted = usbMounted;
        dirty = true;
    }

    switch (screen) {
        case Screen::Browse: handleBrowse(steps, k, back); break;
        case Screen::Player: handlePlayer(k, back); break;
        case Screen::Record: handleRecord(k, back); break;
        case Screen::Usb: handleUsb(k, back); break;
    }

    updateLed();
    uint32_t refresh = screen == Screen::Record ? 100 : screen == Screen::Player ? 250 : 0;
    if (dirty || (refresh && millis() - lastDraw > refresh)) draw();
}

void setup1() {
    player::engineBegin();
    recorder::engineBegin();
}

void loop1() {
    player::engineLoop();
    recorder::engineLoop();
}
