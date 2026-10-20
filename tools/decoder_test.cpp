// PC test harness for src/tap_decoder.cpp.
//
//   g++ -O2 -std=c++17 -I../src decoder_test.cpp ../src/tap_decoder.cpp -o decoder_test
//   decoder_test file.wav [volts_pp] [offset_v] [out.tap]   decode a recording
//   decoder_test --synth                                     randomized round-trip tests
//
// Samples are fed at 50 kHz as 8-bit ADC values (0..255 = 0..3.3 V), like on the Pico.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "tap_decoder.h"

static const uint32_t RATE = 50000;

struct Collector : TapDecoder::Sink {
    struct Block {
        std::vector<uint8_t> data;
        bool ok = false;
    };
    std::vector<Block> blocks;
    void blockStart() override { blocks.push_back({}); }
    void blockByte(uint8_t b) override { blocks.back().data.push_back(b); }
    void blockEnd(bool ok) override { blocks.back().ok = ok; }
};

// volts -> 8-bit ADC reading, clamped like the RP2040 pin would be
static uint8_t adc(double v) {
    if (v < 0) v = 0;
    if (v > 3.3) v = 3.3;
    return (uint8_t)lround(v / 3.3 * 255);
}

static void printBlocks(const Collector& c) {
    for (size_t i = 0; i < c.blocks.size(); i++) {
        const auto& b = c.blocks[i];
        printf("  block %zu: %zu bytes, flag %02X, %s", i, b.data.size(), b.data.empty() ? 0 : b.data[0],
               b.ok ? "OK" : "BAD");
        if (b.data.size() == 19 && b.data[0] == 0) printf("  name '%.10s'", (const char*)&b.data[2]);
        printf("\n");
    }
}

static int decodeWav(const char* path, double vpp, double offset, const char* outTap) {
    FILE* f = fopen(path, "rb");
    if (!f) return printf("can't open %s\n", path), 1;
    std::vector<uint8_t> raw;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) raw.insert(raw.end(), buf, buf + n);
    fclose(f);

    uint32_t rate = 0, channels = 0, bits = 0;
    const int16_t* pcm = nullptr;
    size_t frames = 0;
    for (size_t p = 12; p + 8 <= raw.size();) {
        uint32_t len;
        memcpy(&len, &raw[p + 4], 4);
        if (!memcmp(&raw[p], "fmt ", 4)) {
            channels = raw[p + 10] | raw[p + 11] << 8;
            memcpy(&rate, &raw[p + 12], 4);
            bits = raw[p + 22] | raw[p + 23] << 8;
        } else if (!memcmp(&raw[p], "data", 4)) {
            pcm = (const int16_t*)&raw[p + 8];
            frames = len / (channels * 2);
        }
        p += 8 + len + (len & 1);
    }
    if (!pcm || bits != 16) return printf("need 16-bit PCM wav\n"), 1;
    printf("%s: %u Hz, %u ch, %.1f s -> feeding at %u Hz, %.2f V p-p, offset %.2f V\n", path, rate, channels,
           (double)frames / rate, RATE, vpp, offset);

    Collector c;
    TapDecoder dec(RATE, c);
    for (double t = 0; t < (double)(frames - 1) / rate; t += 1.0 / RATE) {
        double pos = t * rate;
        size_t i = (size_t)pos;
        double fr = pos - i;
        double s = (pcm[i * channels] * (1 - fr) + pcm[(i + 1) * channels] * fr) / 32768.0;
        dec.feed(adc(offset + s * vpp / 2));
    }
    printBlocks(c);

    if (outTap) {
        FILE* o = fopen(outTap, "wb");
        for (auto& b : c.blocks) {
            if (b.data.size() < 2) continue;
            uint8_t len[2] = {(uint8_t)b.data.size(), (uint8_t)(b.data.size() >> 8)};
            fwrite(len, 1, 2, o);
            fwrite(b.data.data(), 1, b.data.size(), o);
        }
        fclose(o);
        printf("wrote %s\n", outTap);
    }
    return 0;
}

// Render TAP blocks into ADC samples with ROM timings plus per-pulse jitter.
struct Synth {
    std::mt19937 rng{1234};
    std::vector<uint8_t> out;
    double phase = 0;  // fractional samples carried between pulses
    bool level = false;
    double vpp, offset, noise, jitter, speed = 1.0;

    void pulse(double tstates) {
        std::normal_distribution<double> jit(1.0, jitter), nz(0.0, noise);
        double samples = tstates * speed * jit(rng) / 3500000.0 * RATE + phase;
        int whole = (int)samples;
        phase = samples - whole;
        double v = level ? offset + vpp / 2 : offset - vpp / 2;
        for (int i = 0; i < whole; i++) out.push_back(adc(v + nz(rng)));
        level = !level;
    }
    void silence(double ms) {
        std::normal_distribution<double> nz(0.0, noise);
        for (int i = 0; i < ms * RATE / 1000; i++) out.push_back(adc(offset - vpp / 2 + nz(rng)));
    }
    void block(const std::vector<uint8_t>& b) {
        int pilots = b[0] < 0x80 ? 8063 : 3223;
        for (int i = 0; i < pilots; i++) pulse(2168);
        pulse(667);
        pulse(735);
        for (uint8_t v : b)
            for (int bit = 7; bit >= 0; bit--) {
                double t = (v >> bit) & 1 ? 1710 : 855;
                pulse(t);
                pulse(t);
            }
        pulse(945);  // closing edge
        silence(1000);
    }
};

static std::vector<uint8_t> makeBlock(std::mt19937& rng, uint8_t flag, size_t len) {
    std::vector<uint8_t> b{flag};
    uint8_t x = flag;
    for (size_t i = 0; i < len; i++) {
        b.push_back((uint8_t)rng());
        x ^= b.back();
    }
    b.push_back(x);
    return b;
}

static int synthTests() {
    struct Case {
        const char* name;
        double vpp, offset, noise, jitter, speed;
    };
    const Case cases[] = {
        {"full swing, centred", 3.0, 1.65, 0.01, 0.02, 1.0},
        {"0.5 V, centred (with cap)", 0.5, 1.65, 0.01, 0.03, 1.0},
        {"0.5 V, sitting on 0 V (no cap)", 0.5, 0.25, 0.01, 0.03, 1.0},
        {"0.2 V, offset 0.3 V", 0.2, 0.3, 0.008, 0.03, 1.0},
        {"half-wave clipped (PC line-out, no cap)", 2.0, 0.0, 0.01, 0.03, 1.0},
        {"noisy 0.5 V", 0.5, 1.65, 0.05, 0.05, 1.0},
        {"8% fast", 0.5, 0.25, 0.01, 0.02, 0.92},
        {"8% slow", 0.5, 0.25, 0.01, 0.02, 1.08},
        {"heavy jitter 5% (worn tape)", 0.5, 0.25, 0.01, 0.05, 1.0},
    };
    int failures = 0;
    for (const auto& cs : cases) {
        std::mt19937 rng(42);
        std::vector<std::vector<uint8_t>> tap;
        tap.push_back(makeBlock(rng, 0x00, 17));
        tap.push_back(makeBlock(rng, 0xFF, 6912));
        tap.push_back(makeBlock(rng, 0x00, 17));
        tap.push_back(makeBlock(rng, 0xFF, 40000));

        Synth s;
        s.vpp = cs.vpp;
        s.offset = cs.offset;
        s.noise = cs.noise;
        s.jitter = cs.jitter;
        s.speed = cs.speed;
        s.silence(300);
        for (auto& b : tap) s.block(b);

        Collector c;
        TapDecoder dec(RATE, c);
        for (uint8_t v : s.out) dec.feed(v);

        bool pass = c.blocks.size() == tap.size();
        for (size_t i = 0; pass && i < tap.size(); i++) pass = c.blocks[i].ok && c.blocks[i].data == tap[i];
        printf("%-42s %s\n", cs.name, pass ? "PASS" : "FAIL");
        if (!pass) {
            printf("  decoded %zu blocks\n", c.blocks.size());
            printBlocks(c);
            failures++;
        }
    }

    // Hiss and hum with no tape signal must not produce blocks.
    {
        std::mt19937 rng(7);
        std::normal_distribution<double> nz(0.0, 0.15);
        Collector c;
        TapDecoder dec(RATE, c);
        for (uint32_t i = 0; i < RATE * 20; i++)
            dec.feed(adc(0.5 + 0.2 * sin(i * 2 * 3.14159265 * 50 / RATE) + nz(rng)));
        bool pass = c.blocks.empty();
        printf("%-42s %s\n", "20 s of noise + 50 Hz hum, no signal", pass ? "PASS" : "FAIL");
        if (!pass) {
            printBlocks(c);
            failures++;
        }
    }
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "--synth")) return synthTests();
    if (argc >= 2)
        return decodeWav(argv[1], argc > 2 ? atof(argv[2]) : 3.0, argc > 3 ? atof(argv[3]) : 1.65,
                         argc > 4 ? argv[4] : nullptr);
    printf("usage: decoder_test file.wav [volts_pp] [offset_v] [out.tap] | --synth\n");
    return 1;
}
