#include "recorder.h"

#include <FatFS.h>
#include <hardware/adc.h>
#include <hardware/dma.h>

#include "pins.h"
#include "spsc_ring.h"
#include "tap_decoder.h"
#include "tap_player.h"
#include "tape_out.h"

namespace recorder {
namespace {

constexpr uint32_t SAMPLE_RATE = 50000;
constexpr uint32_t RING_BITS = 15;  // 32 KB = 655 ms of samples, covers flash write stalls
constexpr uint32_t RING_SIZE = 1u << RING_BITS;
constexpr const char* TEMP_PATH = "/REC.TMP";

// Decoder -> core 0 events: 0..255 = data byte, or one of these.
constexpr uint16_t EV_START = 0x100;
constexpr uint16_t EV_END_OK = 0x200;
constexpr uint16_t EV_END_BAD = 0x201;

alignas(RING_SIZE) uint8_t samples[RING_SIZE];  // DMA ring needs natural alignment
SpscRing<uint16_t, 4096> events;
int dmaChan = -1;

// Shared between cores.
volatile bool startReq = false;
volatile bool stopReq = false;
volatile bool running = false;
volatile bool lost = false;
volatile uint8_t livePhase = 0;
volatile uint8_t liveLevel = 0;
volatile uint32_t liveLen = 0;

// Core 0 only.
File file;
bool isActive = false;
uint8_t block[65536];
uint32_t blockLen = 0;
bool inBlock = false;
int blocks = 0;
int errors = 0;
String last;
String name;
uint32_t startedAt = 0;

struct EventSink : TapDecoder::Sink {
    void put(uint16_t e) {
        if (events.space()) events.push(&e, 1);
        else lost = true;
    }
    void blockStart() override { put(EV_START); }
    void blockByte(uint8_t b) override { put(b); }
    void blockEnd(bool ok) override { put(ok ? EV_END_OK : EV_END_BAD); }
};

EventSink sink;
TapDecoder decoder(SAMPLE_RATE, sink);

void finishBlock(bool ok) {
    inBlock = false;
    if (blockLen < 2) return;  // noise, not a block

    uint8_t len[2] = {(uint8_t)blockLen, (uint8_t)(blockLen >> 8)};
    file.write(len, 2);
    file.write(block, blockLen);
    blocks++;
    if (!ok) errors++;

    TapBlock b = {};
    b.length = blockLen;
    b.flag = block[0];
    if (b.flag == 0x00 && blockLen == 19) {
        b.type = block[1];
        memcpy(b.name, &block[2], 10);
        for (int i = 0; i < 10; i++)
            if (b.name[i] < 32 || b.name[i] > 126) b.name[i] = '?';
        if (name.isEmpty()) name = String(b.name);
    }
    last = player::describe(b) + (ok ? " OK" : " ERR");
}

// "boulder   " -> "boulder", unsafe characters -> '_'
String fileBase() {
    String base;
    for (char c : name) base += isalnum((unsigned char)c) || c == '-' || c == ' ' ? c : '_';
    base.trim();
    return base.isEmpty() ? String("REC") : base;
}

String uniqueName() {
    String base = fileBase();
    String candidate = base + ".TAP";
    for (int i = 2; FatFS.exists("/" + candidate); i++) candidate = base + "_" + i + ".TAP";
    return candidate;
}

uint32_t samplesWritten() { return 0xFFFFFFFFu - dma_hw->ch[dmaChan].transfer_count; }

void startCapture() {
    adc_fifo_drain();
    dma_channel_config c = dma_channel_get_default_config(dmaChan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, RING_BITS);
    channel_config_set_dreq(&c, DREQ_ADC);
    dma_channel_configure(dmaChan, &c, samples, &adc_hw->fifo, 0xFFFFFFFFu, true);
    adc_run(true);
}

void stopCapture() {
    adc_run(false);
    dma_channel_abort(dmaChan);
    adc_fifo_drain();
}

}  // namespace

bool start() {
    if (isActive || player::busy()) return false;
    FatFS.remove(TEMP_PATH);
    file = FatFS.open(TEMP_PATH, "w");
    if (!file) return false;

    events.reset();
    blockLen = 0;
    inBlock = false;
    blocks = errors = 0;
    last = "";
    name = "";
    lost = false;
    livePhase = 0;
    liveLevel = 0;
    liveLen = 0;
    stopReq = false;
    startedAt = millis();
    isActive = true;
    __dmb();
    startReq = true;
    return true;
}

void service() {
    if (!isActive) return;
    uint16_t e;
    while (events.pop(e)) {
        if (e < 0x100) {
            if (inBlock && blockLen < sizeof block) block[blockLen++] = (uint8_t)e;
        } else if (e == EV_START) {
            inBlock = true;
            blockLen = 0;
        } else if (inBlock) {
            finishBlock(e == EV_END_OK);
        }
    }
}

String stop(bool keep) {
    if (!isActive) return "";
    stopReq = true;
    while (startReq || running) tight_loop_contents();
    stopReq = false;
    service();  // a block still in progress has no end event and is dropped
    file.close();
    isActive = false;

    if (!keep || blocks == 0) {
        FatFS.remove(TEMP_PATH);
        return "";
    }
    String saved = uniqueName();
    if (!FatFS.rename(TEMP_PATH, "/" + saved)) return "";
    return saved;
}

bool active() { return isActive; }
Phase phase() { return (Phase)livePhase; }
uint8_t level() { return liveLevel; }
uint32_t liveBytes() { return liveLen; }
int blockCount() { return blocks; }
int errorCount() { return errors; }
String lastBlock() { return last; }
String tapeName() { return name; }
bool overrun() { return lost; }
uint32_t elapsedMs() { return isActive ? millis() - startedAt : 0; }

void engineBegin() {
    adc_init();
    adc_gpio_init(PIN_TAPE_IN);
    adc_select_input(PIN_TAPE_IN - 26);
    adc_fifo_setup(true, true, 1, false, true);  // DMA requests, 8-bit samples
    adc_set_clkdiv(48000000.0f / SAMPLE_RATE - 1);
    dmaChan = dma_claim_unused_channel(true);
}

void engineLoop() {
    if (!startReq) return;
    running = true;
    __dmb();
    startReq = false;

    tape_out::release();
    decoder.reset();
    startCapture();

    uint32_t readPos = 0;
    while (!stopReq) {
        uint32_t written = samplesWritten();
        if (written - readPos > RING_SIZE) {
            // We fell more than a whole buffer behind; skip ahead.
            lost = true;
            readPos = written - RING_SIZE / 2;
        }
        while (readPos != written) {
            decoder.feed(samples[readPos & (RING_SIZE - 1)]);
            readPos++;
        }
        livePhase = (uint8_t)decoder.phase();
        liveLevel = decoder.amplitude();
        liveLen = decoder.phase() == TapDecoder::Phase::Data ? decoder.blockLength() : 0;
    }

    stopCapture();
    tape_out::drive();
    __dmb();
    running = false;
}

}  // namespace recorder
