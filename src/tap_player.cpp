#include "tap_player.h"

#include <FatFS.h>

#include "pins.h"
#include "settings.h"
#include "spsc_ring.h"
#include "tape_out.h"

namespace player {
namespace {

// Standard ROM loader timings, in T-states.
constexpr uint32_t T_PILOT = 2168;
constexpr uint32_t T_SYNC1 = 667;
constexpr uint32_t T_SYNC2 = 735;
constexpr uint32_t T_BIT0 = 855;
constexpr uint32_t T_BIT1 = 1710;
constexpr uint32_t PILOT_HEADER = 8063;
constexpr uint32_t PILOT_DATA = 3223;
constexpr uint32_t T_PER_MS = tape_out::CLOCK_HZ / 1000;
constexpr uint32_t LEAD_IN_MS = 500;
constexpr size_t MAX_BLOCKS = 256;

SpscRing<uint8_t, 4096> ring;

// Shared between cores.
volatile bool startReq = false;
volatile bool stopReq = false;
volatile bool running = false;
volatile bool paused = false;
volatile bool eof = false;
volatile uint32_t consumed = 0;
volatile int32_t curBlock = -1;
volatile int32_t firstBlock = 0;
volatile bool inBlock = false;  // between the start of a pilot and the last bit

// Core 0 only.
File file;
bool isOpen = false;
bool finished = false;
String openPath;
uint32_t size = 0;
std::vector<TapBlock> blockList;

// Core 1 only: level of the next pulse.
bool level = true;

void scanBlocks(File& f, std::vector<TapBlock>& list) {
    list.clear();
    uint32_t fileLen = f.size();
    uint32_t pos = 0;
    while (pos + 2 <= fileLen && list.size() < MAX_BLOCKS) {
        f.seek(pos);
        uint8_t hdr[14];  // len(2) + flag + type + name(10)
        int got = f.read(hdr, sizeof hdr);
        if (got < 2) break;
        TapBlock b = {};
        b.offset = pos;
        b.length = hdr[0] | (hdr[1] << 8);
        if (got >= 3) b.flag = hdr[2];
        if (b.flag == 0x00 && b.length == 19 && got >= 14) {
            b.type = hdr[3];
            memcpy(b.name, &hdr[4], 10);
            for (int i = 0; i < 10; i++)
                if (b.name[i] < 32 || b.name[i] > 126) b.name[i] = '?';
        }
        if (b.length) list.push_back(b);
        pos += 2 + b.length;
    }
}

void fill() {
    uint8_t chunk[256];
    while (!eof) {
        uint32_t n = min<uint32_t>(ring.space(), sizeof chunk);
        if (n == 0) return;
        int got = file.read(chunk, n);
        if (got <= 0) {
            __dmb();
            eof = true;
            return;
        }
        ring.push(chunk, got);
    }
}

// --- core 1 engine ---

int nextByte() {
    uint8_t b;
    for (;;) {
        if (ring.pop(b)) {
            consumed = consumed + 1;
            return b;
        }
        if (stopReq) return -1;
        if (eof) {
            // The producer may have pushed its last bytes just before setting eof.
            if (!ring.pop(b)) return -1;
            consumed = consumed + 1;
            return b;
        }
    }
}

bool pushRaw(bool lvl, uint32_t tstates) {
    while (paused || !tape_out::canPush()) {
        if (stopReq) return false;
    }
    if (stopReq) return false;
    tape_out::push(lvl, tstates);
    return true;
}

bool pulse(uint32_t tstates) {
    if (!pushRaw(level, tstates)) return false;
    level = !level;
    return true;
}

// Hold the line low, in short chunks so stop/pause stay responsive.
bool holdLow(uint32_t ms) {
    while (ms) {
        uint32_t chunk = min<uint32_t>(ms, 10);
        if (!pushRaw(false, chunk * T_PER_MS)) return false;
        ms -= chunk;
    }
    level = true;
    return true;
}

// Silence after a block. The first part is on the opposite level so the
// loader sees the closing edge of the last bit, then the line rests low.
bool blockPause(uint32_t ms) {
    if (level) {
        if (!pulse(T_PER_MS)) return false;
        ms = ms > 1 ? ms - 1 : 0;
    }
    return holdLow(ms);
}

bool sendByte(uint8_t v) {
    for (int bit = 7; bit >= 0; bit--) {
        uint32_t t = (v >> bit) & 1 ? T_BIT1 : T_BIT0;
        if (!pulse(t) || !pulse(t)) return false;
    }
    return true;
}

bool playBlock(uint8_t flag, uint32_t len) {
    uint32_t pilots = flag < 0x80 && !settings.shortPilot ? PILOT_HEADER : PILOT_DATA;
    for (uint32_t i = 0; i < pilots; i++)
        if (!pulse(T_PILOT)) return false;
    if (!pulse(T_SYNC1) || !pulse(T_SYNC2)) return false;
    if (!sendByte(flag)) return false;
    for (uint32_t i = 1; i < len; i++) {
        int b = nextByte();
        if (b < 0 || !sendByte(b)) return false;
    }
    return true;
}

void runTape() {
    level = true;
    if (!holdLow(LEAD_IN_MS)) return;
    for (int32_t n = firstBlock;;) {
        int lo = nextByte();
        if (lo < 0) return;
        int hi = nextByte();
        if (hi < 0) return;
        uint32_t len = lo | (hi << 8);
        if (len == 0) continue;
        int flag = nextByte();
        if (flag < 0) return;
        curBlock = n++;
        inBlock = true;
        bool ok = playBlock(flag, len);
        inBlock = false;
        if (!ok || !blockPause(blockGapMs())) return;
        if (flag >= 0x80 && settings.stopAfterData) paused = true;
    }
}

}  // namespace

bool scan(const char* path, std::vector<TapBlock>& out) {
    File f = FatFS.open(path, "r");
    if (!f) return false;
    scanBlocks(f, out);
    f.close();
    return true;
}

bool open(const char* path, int startBlock) {
    String p = path;  // path may point into openPath
    stop();
    file = FatFS.open(p, "r");
    if (!file) return false;
    openPath = p;
    size = file.size();
    scanBlocks(file, blockList);
    if (startBlock < 0 || startBlock >= (int)blockList.size()) startBlock = 0;
    uint32_t start = blockList.empty() ? 0 : blockList[startBlock].offset;
    file.seek(start);

    ring.reset();
    eof = false;
    paused = false;
    stopReq = false;
    consumed = start;
    firstBlock = startBlock;
    curBlock = -1;
    finished = false;
    isOpen = true;
    fill();
    __dmb();
    startReq = true;
    return true;
}

bool seek(int block) {
    if (openPath.isEmpty()) return false;
    return open(openPath.c_str(), block);
}

void togglePause() {
    if (!isOpen) return;
    if (paused && inBlock) {
        // Part of this block has already been sent; the Spectrum would reject
        // the rest, so start the block again from its pilot tone.
        seek(curBlock);
        return;
    }
    paused = !paused;
}

void stop() {
    if (isOpen || running || startReq) {
        stopReq = true;
        while (startReq || running) tight_loop_contents();
        stopReq = false;
    }
    if (isOpen) file.close();
    isOpen = false;
    finished = false;
    paused = false;
    curBlock = -1;
}

void service() {
    if (!isOpen) return;
    fill();
    if (!startReq && !running) {  // engine reached the end of the tape
        file.close();
        isOpen = false;
        finished = true;
    }
}

bool busy() { return isOpen || running || startReq; }

State state() {
    if (isOpen) return paused ? State::Paused : State::Playing;
    return finished ? State::Done : State::Idle;
}

int currentBlock() { return curBlock; }
uint32_t bytesPlayed() { return consumed; }
uint32_t fileSize() { return size; }
const std::vector<TapBlock>& blocks() { return blockList; }

String describe(const TapBlock& b) {
    static const char* const TYPES[] = {"Program", "Num array", "Chr array", "Bytes"};
    if (b.flag == 0x00 && b.length == 19) {
        const char* t = b.type < 4 ? TYPES[b.type] : "Header";
        return String(t) + ": " + b.name;
    }
    if (b.flag == 0xFF) return "Data " + String(b.length - 2) + " bytes";
    return "Block " + String(b.length) + " bytes";
}

void engineBegin() { tape_out::begin(PIN_TAPE_OUT); }

void engineLoop() {
    if (!startReq) return;
    running = true;
    __dmb();
    startReq = false;

    runTape();
    if (!stopReq) {
        // Let the final silence drain before parking the output.
        while (!tape_out::fifoEmpty() && !stopReq) tight_loop_contents();
    }
    tape_out::abort();
    curBlock = -1;
    inBlock = false;
    __dmb();
    running = false;
}

}  // namespace player
