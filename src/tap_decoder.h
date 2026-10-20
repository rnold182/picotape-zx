#pragma once
#include <stdint.h>

// Turns a stream of 8-bit audio samples into TAP blocks, expecting standard
// ROM SAVE timings. No Arduino dependencies, so it can be tested on a PC
// (see tools/decoder_test.cpp).
class TapDecoder {
public:
    enum class Phase : uint8_t { Idle, Pilot, Data };

    struct Sink {
        virtual void blockStart() = 0;
        virtual void blockByte(uint8_t b) = 0;
        // Always follows blockStart(). ok = at least flag + checksum and XOR is 0.
        virtual void blockEnd(bool ok) = 0;
    };

    TapDecoder(uint32_t sampleRate, Sink& sink);
    void reset();
    void feed(uint8_t sample);

    Phase phase() const;
    // Peak-to-peak signal level in ADC counts (255 = 3.3 V).
    uint8_t amplitude() const { return (uint8_t)((hi_ - lo_) >> 8); }
    uint32_t blockLength() const { return length_; }

private:
    enum class State : uint8_t { Search, Pilot, Sync2, Data };

    void edge(uint32_t tstates);
    void endBlock();

    Sink& sink_;
    uint32_t tPerSample256_;  // T-states per sample, x256
    uint32_t timeoutSamples_;
    int32_t decay_;

    // Slicer (values are sample << 8).
    int32_t hi_, lo_;
    bool level_;
    uint32_t sinceEdge_;

    // Pulse state machine.
    State state_;
    uint32_t pilot_;
    uint8_t misses_;
    uint32_t half_;
    uint8_t cur_, bits_, xor_;
    uint32_t length_;
};
