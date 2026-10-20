// PicoTape ZX - ZX Spectrum 48K tape deck on a Waveshare RP2040-Zero
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
#include "settings.h"
#include "tap_player.h"

namespace {

constexpr const char* VERSION = "0.3";

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
// If red and green come out swapped on your board, use NEO_RGB.
Adafruit_NeoPixel led(1, PIN_LED, NEO_GRB + NEO_KHZ800);

Encoder encoder;
Button knob;
Button backBtn;

enum class Screen { Browse, Player, Record, Usb, FileMenu, Blocks, Confirm, Settings, About };
Screen screen = Screen::Browse;

// Browse list: [Record], the files, then [USB drive] and [Settings].
enum class Kind : uint8_t { Record, File, Usb, Settings };
struct Item {
    Kind kind;
    int file;  // index into files for Kind::File
};
std::vector<String> files;
std::vector<Item> items;
int selected = -1;  // -1 = pick a default after the first scan
int scrollTop = 0;
String message;  // one-line notice on the browse screen

// Player
String playingName;
int seekTarget = -1;  // block chosen with the knob while playing, -1 = none
uint32_t seekAt = 0;

// File menu, block list, delete confirmation
const char* const MENU_ENTRIES[] = {"Play", "Blocks", "Delete"};
constexpr int MENU_COUNT = 3;
String menuFile;
int menuSel = 0;
std::vector<TapBlock> menuBlocks;
int blockSel = 0;
int blockTop = 0;

// Settings
int settingsSel = 0;
int settingsTop = 0;

// Recorder
uint32_t recActiveAt = 0;  // last time the recorder heard a signal

// Display power
bool asleep = false;
uint32_t lastInput = 0;

bool dirty = true;
uint32_t lastDraw = 0;
uint32_t ledShown = 1;  // forces the first LED update

// USB drive state, touched from USB callbacks.
volatile bool usbEnabled = false;
volatile bool usbMounted = false;
volatile bool needRescan = true;

constexpr int ROWS = 5;
constexpr int LINE_CHARS = 21;

String clip(const String& s, int n = LINE_CHARS) {
    if (n < 1) return "";
    return (int)s.length() <= n ? s : s.substring(0, n - 1) + "~";
}

int wrap(int value, int count) { return count ? ((value % count) + count) % count : 0; }

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

    items.clear();
    items.push_back({Kind::Record, -1});
    for (int i = 0; i < (int)files.size(); i++) items.push_back({Kind::File, i});
    items.push_back({Kind::Usb, -1});
    items.push_back({Kind::Settings, -1});

    if (selected < 0) selected = files.empty() ? 0 : 1;
    selected = constrain(selected, 0, (int)items.size() - 1);
}

void selectFile(const String& name) {
    for (int i = 0; i < (int)items.size(); i++)
        if (items[i].kind == Kind::File && files[items[i].file] == name) selected = i;
}

String itemLabel(const Item& it) {
    switch (it.kind) {
        case Kind::Record: return "[Record]";
        case Kind::Usb: return "[USB drive]";
        case Kind::Settings: return "[Settings]";
        default: return files[it.file];
    }
}

void applySettings() {
    encoder.setReversed(settings.knobReverse);
    oled.setContrast(contrastLevel());
    led.setBrightness(ledBrightness());
    ledShown = 1;  // brightness changed: repaint the LED
}

void wake() {
    lastInput = millis();
    if (asleep) {
        asleep = false;
        oled.setPowerSave(0);
        dirty = true;
    }
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

void drawHeader(const String& left, const String& right) {
    oled.drawBox(0, 0, 128, 11);
    oled.setDrawColor(0);
    oled.drawStr(2, 9, left.c_str());
    oled.drawStr(126 - oled.getStrWidth(right.c_str()), 9, right.c_str());
    oled.setDrawColor(1);
}

// Scrolling list under the header: label on the left, optional value on the right.
template <typename LabelFn, typename ValueFn>
void drawList(int count, int sel, int& top, int rows, LabelFn label, ValueFn value) {
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    for (int row = 0; row < rows && top + row < count; row++) {
        int i = top + row;
        int y = 12 + row * 10;
        if (i == sel) {
            oled.drawBox(0, y, 128, 10);
            oled.setDrawColor(0);
        }
        String v = value(i);
        int valueWidth = v.length() ? oled.getStrWidth(v.c_str()) : 0;
        int labelChars = (124 - valueWidth - (valueWidth ? 6 : 0)) / 6;
        oled.drawStr(2, y + 8, clip(label(i), labelChars).c_str());
        if (valueWidth) oled.drawStr(126 - valueWidth, y + 8, v.c_str());
        oled.setDrawColor(1);
    }
}

auto noValue = [](int) { return String(); };

void drawBrowse() {
    drawHeader("PICOTAPE ZX", String(files.size()) + " files");
    int rows = message.length() ? ROWS - 1 : ROWS;
    drawList((int)items.size(), selected, scrollTop, rows,
             [](int i) { return itemLabel(items[i]); }, noValue);
    if (message.length()) {
        oled.drawHLine(0, 53, 128);
        oled.drawStr(2, 62, clip(message).c_str());
    } else if (files.empty()) {
        oled.drawStr(2, 51, "No .TAP files yet:");
        oled.drawStr(2, 61, "copy via [USB drive]");
    }
}

void drawBlockLines(const std::vector<TapBlock>& blocks, int i) {
    if (i < 0 || i >= (int)blocks.size()) return;
    oled.drawStr(2, 35, clip(player::describe(blocks[i])).c_str());
    // A data block is usually described by the header just before it.
    if (blocks[i].flag != 0 && i > 0 && blocks[i - 1].flag == 0)
        oled.drawStr(2, 45, clip("  " + String(blocks[i - 1].name)).c_str());
}

void drawPlayer() {
    const auto& blocks = player::blocks();
    int n = blocks.size();

    if (seekTarget >= 0) {
        drawHeader("GO TO", String(seekTarget + 1) + "/" + n);
        oled.drawStr(2, 23, clip(playingName).c_str());
        drawBlockLines(blocks, seekTarget);
        oled.drawStr(2, 61, "click:play hold:back");
        return;
    }

    const char* label = "";
    switch (player::state()) {
        case player::State::Playing: label = "> PLAY"; break;
        case player::State::Paused: label = "|| PAUSE"; break;
        case player::State::Done: label = "DONE"; break;
        case player::State::Idle: label = "STOP"; break;
    }
    int cur = player::currentBlock();
    drawHeader(label, cur >= 0 ? String(cur + 1) + "/" + n : String(n) + " blk");

    oled.drawStr(2, 23, clip(playingName).c_str());
    if (cur >= 0) drawBlockLines(blocks, cur);
    else if (player::state() == player::State::Playing) oled.drawStr(2, 35, "Lead-in...");

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
        case recorder::Phase::Idle:
            if (settings.recAutoSave && recorder::blockCount()) {
                uint32_t idle = millis() - recActiveAt;
                uint32_t left = idle < REC_AUTOSAVE_MS ? (REC_AUTOSAVE_MS - idle + 999) / 1000 : 0;
                activity = "Auto-save in " + String(left) + "s";
            } else {
                activity = "Waiting for SAVE...";
            }
            break;
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

void drawFileMenu() {
    drawHeader(clip(menuFile, 20), "");
    static int top = 0;
    drawList(MENU_COUNT, menuSel, top, ROWS, [](int i) { return String(MENU_ENTRIES[i]); }, noValue);
}

void drawBlocks() {
    drawHeader(clip(menuFile, 14), String(menuBlocks.size()) + " blk");
    if (menuBlocks.empty()) {
        oled.drawStr(2, 30, "No blocks found.");
        return;
    }
    drawList((int)menuBlocks.size(), blockSel, blockTop, ROWS,
             [](int i) { return String(i + 1) + " " + player::describe(menuBlocks[i]); }, noValue);
}

void drawConfirm() {
    drawHeader("DELETE?", "");
    oled.drawStr(2, 26, clip(menuFile).c_str());
    oled.drawStr(2, 42, "click: delete");
    oled.drawStr(2, 54, "hold:  cancel");
}

void drawSettings() {
    int count;
    const SettingOption* opts = settingOptions(count);
    drawHeader("SETTINGS", "hold:exit");
    drawList(
        count + 1, settingsSel, settingsTop, ROWS,
        [&](int i) { return i < count ? String(opts[i].label) : String("About"); },
        [&](int i) { return i < count ? String(opts[i].names[*opts[i].value]) : String(); });
}

void drawAbout() {
    drawHeader("ABOUT", String("v") + VERSION);
    FSInfo info;
    FatFS.info(info);
    uint32_t freeKb = (info.totalBytes - info.usedBytes) / 1024;
    oled.drawStr(2, 23, "PicoTape ZX");
    oled.drawStr(2, 33, (String(files.size()) + " TAP files").c_str());
    oled.drawStr(2, 43, (String(freeKb) + " KB free of " + String((uint32_t)(info.totalBytes / 1024))).c_str());
    oled.drawStr(2, 53, "github.com/rnold182/");
    oled.drawStr(2, 63, "  picotape-zx");
}

void draw() {
    oled.clearBuffer();
    switch (screen) {
        case Screen::Browse: drawBrowse(); break;
        case Screen::Player: drawPlayer(); break;
        case Screen::Record: drawRecord(); break;
        case Screen::Usb: drawUsb(); break;
        case Screen::FileMenu: drawFileMenu(); break;
        case Screen::Blocks: drawBlocks(); break;
        case Screen::Confirm: drawConfirm(); break;
        case Screen::Settings: drawSettings(); break;
        case Screen::About: drawAbout(); break;
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
    if (color != ledShown) {
        led.setPixelColor(0, color);
        led.show();
        ledShown = color;
    }
}

// --- actions ---

void go(Screen s) {
    screen = s;
    dirty = true;
}

void playFile(const String& name, int block) {
    playingName = name;
    seekTarget = -1;
    if (player::open(("/" + name).c_str(), block)) {
        go(Screen::Player);
    } else {
        message = "Can't open file";
        go(Screen::Browse);
    }
}

void finishRecording(bool keep) {
    String saved = recorder::stop(keep);
    if (!keep) message = "Recording discarded";
    else message = saved.length() ? "Saved " + saved : String("Nothing recorded");
    scanFiles();
    if (saved.length()) selectFile(saved);
    go(Screen::Browse);
}

// --- input handling ---

void handleBrowse(int steps, Button::Event k, Button::Event back) {
    if (steps) {
        selected = wrap(selected + steps, items.size());
        message = "";
        dirty = true;
    }
    if (back != Button::None && message.length()) {
        message = "";
        dirty = true;
    }
    const Item& it = items[selected];
    if (k == Button::Long && it.kind == Kind::File) {
        menuFile = files[it.file];
        menuSel = 0;
        go(Screen::FileMenu);
        return;
    }
    if (k != Button::Click) return;

    switch (it.kind) {
        case Kind::Record:
            if (recorder::start()) {
                recActiveAt = millis();
                go(Screen::Record);
            } else {
                message = "Can't start recording";
                dirty = true;
            }
            break;
        case Kind::File: playFile(files[it.file], 0); break;
        case Kind::Usb:
            usbEnabled = true;
            go(Screen::Usb);
            break;
        case Kind::Settings:
            settingsSel = 0;
            go(Screen::Settings);
            break;
    }
}

void handlePlayer(int steps, Button::Event k, Button::Event back) {
    int n = player::blocks().size();
    if (steps && n) {
        int base = seekTarget >= 0 ? seekTarget : max(player::currentBlock(), 0);
        seekTarget = constrain(base + steps, 0, n - 1);
        seekAt = millis();
        dirty = true;
    }
    if (seekTarget >= 0 && millis() - seekAt > 5000) {  // forgot about it
        seekTarget = -1;
        dirty = true;
    }

    if (k == Button::Long || back != Button::None) {
        if (seekTarget >= 0) {
            seekTarget = -1;
            dirty = true;
        } else {
            player::stop();
            go(Screen::Browse);
        }
    } else if (k == Button::Click) {
        if (seekTarget >= 0) {
            player::seek(seekTarget);
            seekTarget = -1;
            dirty = true;
        } else if (player::state() == player::State::Done) {
            player::stop();
            go(Screen::Browse);
        } else {
            player::togglePause();
            dirty = true;
        }
    }
}

void handleRecord(Button::Event k, Button::Event back) {
    if (recorder::phase() != recorder::Phase::Idle) recActiveAt = millis();

    if (k == Button::Click) {
        finishRecording(true);
    } else if (k == Button::Long || back != Button::None) {
        finishRecording(false);
    } else if (settings.recAutoSave && recorder::blockCount() &&
               millis() - recActiveAt > REC_AUTOSAVE_MS) {
        finishRecording(true);
    }
}

void handleUsb(Button::Event k, Button::Event back) {
    // Once mounted, only the PC can end it (by ejecting); pulling the
    // drive away underneath it would corrupt the file system.
    if (usbMounted) return;
    if (!usbEnabled) {  // ejected by the PC
        go(Screen::Browse);
    } else if (k == Button::Long || back != Button::None) {
        usbEnabled = false;
        go(Screen::Browse);
    }
}

void handleFileMenu(int steps, Button::Event k, Button::Event back) {
    if (steps) {
        menuSel = wrap(menuSel + steps, MENU_COUNT);
        dirty = true;
    }
    if (k == Button::Long || back != Button::None) {
        go(Screen::Browse);
        return;
    }
    if (k != Button::Click) return;
    switch (menuSel) {
        case 0: playFile(menuFile, 0); break;
        case 1:
            if (!player::scan(("/" + menuFile).c_str(), menuBlocks)) menuBlocks.clear();
            blockSel = blockTop = 0;
            go(Screen::Blocks);
            break;
        case 2: go(Screen::Confirm); break;
    }
}

void handleBlocks(int steps, Button::Event k, Button::Event back) {
    int n = menuBlocks.size();
    if (steps && n) {
        blockSel = constrain(blockSel + steps, 0, n - 1);
        dirty = true;
    }
    if (k == Button::Long || back != Button::None) go(Screen::FileMenu);
    else if (k == Button::Click && n) playFile(menuFile, blockSel);
}

void handleConfirm(Button::Event k, Button::Event back) {
    if (k == Button::Long || back != Button::None) {
        go(Screen::FileMenu);
    } else if (k == Button::Click) {
        message = FatFS.remove("/" + menuFile) ? "Deleted " + menuFile : String("Delete failed");
        scanFiles();
        go(Screen::Browse);
    }
}

void handleSettings(int steps, Button::Event k, Button::Event back) {
    int count;
    const SettingOption* opts = settingOptions(count);
    if (steps) {
        settingsSel = wrap(settingsSel + steps, count + 1);
        dirty = true;
    }
    if (k == Button::Long || back != Button::None) {
        saveSettings();
        go(Screen::Browse);
    } else if (k == Button::Click) {
        if (settingsSel == count) {
            go(Screen::About);
        } else {
            const SettingOption& o = opts[settingsSel];
            *o.value = (*o.value + 1) % o.count;
            applySettings();
            dirty = true;
        }
    }
}

void handleAbout(Button::Event k, Button::Event back) {
    if (k != Button::None || back != Button::None) go(Screen::Settings);
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
    loadSettings();
    applySettings();

    FatFSUSB.onPlug(usbPlug);
    FatFSUSB.onUnplug(usbUnplug);
    FatFSUSB.driveReady(usbDriveReady);
    FatFSUSB.begin();
    lastInput = millis();
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

    if (steps || k != Button::None || back != Button::None) {
        bool wasAsleep = asleep;
        wake();
        if (wasAsleep) {  // the input only wakes the screen
            steps = 0;
            k = back = Button::None;
        }
    }

    static bool lastMounted = false;
    if (usbMounted != lastMounted) {
        lastMounted = usbMounted;
        wake();
        dirty = true;
    }

    switch (screen) {
        case Screen::Browse: handleBrowse(steps, k, back); break;
        case Screen::Player: handlePlayer(steps, k, back); break;
        case Screen::Record: handleRecord(k, back); break;
        case Screen::Usb: handleUsb(k, back); break;
        case Screen::FileMenu: handleFileMenu(steps, k, back); break;
        case Screen::Blocks: handleBlocks(steps, k, back); break;
        case Screen::Confirm: handleConfirm(k, back); break;
        case Screen::Settings: handleSettings(steps, k, back); break;
        case Screen::About: handleAbout(k, back); break;
    }

    // Never blank the screen while the tape is moving.
    if (recorder::active() || player::state() == player::State::Playing) lastInput = millis();
    if (!asleep && screenOffMs() && millis() - lastInput > screenOffMs()) {
        asleep = true;
        oled.setPowerSave(1);
    }

    updateLed();
    uint32_t refresh = screen == Screen::Record ? 100 : screen == Screen::Player ? 250 : 0;
    if (!asleep && (dirty || (refresh && millis() - lastDraw > refresh))) draw();
}

void setup1() {
    player::engineBegin();
    recorder::engineBegin();
}

void loop1() {
    player::engineLoop();
    recorder::engineLoop();
}
