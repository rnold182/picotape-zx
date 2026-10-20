#include "tap_decoder.h"

namespace {

// Half-period windows in T-states (ROM SAVE: pilot 2168, sync 667/735, bits 855/1710).
constexpr uint32_t PILOT_MIN = 1700;
constexpr uint32_t PILOT_MAX = 2700;
constexpr uint32_t SYNC_MAX = 1100;
constexpr uint32_t DATA_MAX = 2600;
constexpr uint32_t BIT_THRESHOLD = 2565;  // full period between 1710 (0) and 3420 (1)
constexpr uint32_t MIN_PILOT = 256;       // pulses before we believe it's a pilot tone
constexpr uint8_t MAX_PILOT_MISSES = 4;   // consecutive odd pulses tolerated in a pilot
constexpr uint32_t TIMEOUT_T = 4000;      // no edge for this long ends a block
constexpr uint32_t MAX_BLOCK = 65535;
constexpr int32_t MIN_SPAN = 6 << 8;      // ~80 mV p-p; below that is treated as silence
constexpr uint32_t SINCE_EDGE_CAP = 1u << 16;

bool isPilot(uint32_t t) { return t >= PILOT_MIN && t <= PILOT_MAX; }

}  // namespace

TapDecoder::TapDecoder(uint32_t sampleRate, Sink& sink) : sink_(sink) {
    tPerSample256_ = (uint32_t)((3500000ull << 8) / sampleRate);
    timeoutSamples_ = (uint32_t)((uint64_t)TIMEOUT_T * sampleRate / 3500000);
    // Envelope followers forget full scale in about 200 ms.
    decay_ = (int32_t)((255u << 8) * 5 / sampleRate);
    if (decay_ < 1) decay_ = 1;
    reset();
}

void TapDecoder::reset() {
    hi_ = lo_ = 128 << 8;
    level_ = false;
    sinceEdge_ = SINCE_EDGE_CAP;
    state_ = State::Search;
    pilot_ = 0;
    misses_ = 0;
    half_ = 0;
    cur_ = bits_ = xor_ = 0;
    length_ = 0;
}

TapDecoder::Phase TapDecoder::phase() const {
    switch (state_) {
        case State::Data: return Phase::Data;
        case State::Pilot:
        case State::Sync2: return Phase::Pilot;
        default: return Phase::Idle;
    }
}

void TapDecoder::feed(uint8_t sample) {
    int32_t s = (int32_t)sample << 8;
    if (s > hi_) hi_ = s; else hi_ -= decay_;
    if (s < lo_) lo_ = s; else lo_ += decay_;
    if (hi_ < lo_) hi_ = lo_ = (hi_ + lo_) / 2;

    if (sinceEdge_ < SINCE_EDGE_CAP) sinceEdge_++;

    int32_t span = hi_ - lo_;
    if (span >= MIN_SPAN) {
        // Threshold sits mid-way between the peaks, so DC offset doesn't matter.
        int32_t mid = lo_ + span / 2;
        int32_t hyst = span / 6;
        bool newLevel = level_ ? s >= mid - hyst : s > mid + hyst;
        if (newLevel != level_) {
            level_ = newLevel;
            uint32_t t = (sinceEdge_ * tPerSample256_) >> 8;
            sinceEdge_ = 0;
            edge(t);
            return;
        }
    }

    if (state_ != State::Search && sinceEdge_ == timeoutSamples_) {
        if (state_ == State::Data) endBlock();
        state_ = State::Search;
        pilot_ = 0;
    }
}

void TapDecoder::edge(uint32_t t) {
    switch (state_) {
        case State::Search:
            // A stray pulse only sets the count back, so a jittery pilot still locks.
            if (!isPilot(t)) pilot_ /= 2;
            else if (++pilot_ >= MIN_PILOT) {
                state_ = State::Pilot;
                misses_ = 0;
            }
            break;

        case State::Pilot:
            if (isPilot(t)) {
                misses_ = 0;
            } else if (t <= SYNC_MAX) {
                state_ = State::Sync2;
            } else if (++misses_ > MAX_PILOT_MISSES) {
                state_ = State::Search;
                pilot_ = 0;
            }
            break;

        case State::Sync2:
            if (t <= SYNC_MAX) {
                state_ = State::Data;
                half_ = 0;
                cur_ = bits_ = xor_ = 0;
                length_ = 0;
                sink_.blockStart();
            } else {
                state_ = State::Search;
                pilot_ = isPilot(t) ? 1 : 0;
            }
            break;

        case State::Data:
            if (t > DATA_MAX) {
                endBlock();
                state_ = State::Search;
                pilot_ = isPilot(t) ? 1 : 0;
                break;
            }
            if (!half_) {
                half_ = t;
                break;
            }
            cur_ = (uint8_t)((cur_ << 1) | (half_ + t > BIT_THRESHOLD ? 1 : 0));
            half_ = 0;
            if (++bits_ == 8) {
                sink_.blockByte(cur_);
                xor_ ^= cur_;
                bits_ = 0;
                if (++length_ >= MAX_BLOCK) {
                    endBlock();
                    state_ = State::Search;
                    pilot_ = 0;
                }
            }
            break;
    }
}

void TapDecoder::endBlock() { sink_.blockEnd(length_ >= 2 && xor_ == 0); }
