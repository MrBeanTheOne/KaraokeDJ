#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

// Single-producer / single-consumer lock-free ring of trivially-copyable items.
// Capacity must be a power of two. Safe: one producer thread, one consumer thread.
// clear() is only safe while the consumer is not pulling (deck not playing).
template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacityPow2) : buf_(capacityPow2), mask_(capacityPow2 - 1) {}

    size_t size() const {
        return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire);
    }
    size_t freeSpace() const { return buf_.size() - size(); }
    size_t capacity() const { return buf_.size(); }

    // Producer. Returns number of items actually written.
    size_t push(const T* src, size_t n) {
        const size_t w = w_.load(std::memory_order_relaxed);
        const size_t r = r_.load(std::memory_order_acquire);
        n = std::min(n, buf_.size() - (w - r));
        for (size_t i = 0; i < n; ++i) buf_[(w + i) & mask_] = src[i];
        w_.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer. Returns number of items actually read.
    size_t pop(T* dst, size_t n) {
        const size_t r = r_.load(std::memory_order_relaxed);
        const size_t w = w_.load(std::memory_order_acquire);
        n = std::min(n, w - r);
        for (size_t i = 0; i < n; ++i) dst[i] = buf_[(r + i) & mask_];
        r_.store(r + n, std::memory_order_release);
        return n;
    }

    void clear() { r_.store(w_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    std::vector<T> buf_;
    size_t mask_;
    std::atomic<size_t> r_{0}, w_{0};
};
