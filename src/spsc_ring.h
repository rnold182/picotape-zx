#pragma once
#include <stdint.h>
#include <hardware/sync.h>

// Single-producer / single-consumer ring, safe across the two RP2040 cores.
// Indices only ever increase; size must be a power of two.
template <typename T, uint32_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");

public:
    // Only call while neither side is using the ring.
    void reset() { head_ = tail_ = 0; }

    uint32_t used() const { return head_ - tail_; }
    uint32_t space() const { return N - used(); }

    // Producer side. Caller guarantees n <= space().
    void push(const T* data, uint32_t n) {
        uint32_t h = head_;
        for (uint32_t i = 0; i < n; i++) buf_[(h + i) & (N - 1)] = data[i];
        __dmb();
        head_ = h + n;
    }

    // Consumer side.
    bool pop(T& out) {
        uint32_t t = tail_;
        if (t == head_) return false;
        __dmb();
        out = buf_[t & (N - 1)];
        __dmb();
        tail_ = t + 1;
        return true;
    }

private:
    T buf_[N];
    volatile uint32_t head_ = 0;
    volatile uint32_t tail_ = 0;
};
