// 单生产者/单消费者 无锁浮点环形缓冲 (音频回调线程 <-> 推理线程)
#pragma once
#include <atomic>
#include <cstring>
#include <vector>

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = 1 << 18) { resize(capacity); }
    void resize(size_t capacity) {
        size_t c = 1;
        while (c < capacity) c <<= 1;
        buf_.assign(c, 0.f);
        mask_ = c - 1;
        r_.store(0);
        w_.store(0);
    }
    size_t size() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }
    size_t capacity() const { return buf_.size(); }
    size_t space() const { return capacity() - size(); }

    size_t write(const float* d, size_t n) {
        size_t w = w_.load(std::memory_order_relaxed);
        size_t r = r_.load(std::memory_order_acquire);
        n = std::min(n, capacity() - (w - r));
        for (size_t i = 0; i < n; ++i) buf_[(w + i) & mask_] = d[i];
        w_.store(w + n, std::memory_order_release);
        return n;
    }
    size_t read(float* d, size_t n) {
        size_t r = r_.load(std::memory_order_relaxed);
        size_t w = w_.load(std::memory_order_acquire);
        n = std::min(n, w - r);
        for (size_t i = 0; i < n; ++i) d[i] = buf_[(r + i) & mask_];
        r_.store(r + n, std::memory_order_release);
        return n;
    }
    // 消费者丢弃数据 (延迟堆积时追帧)
    size_t skip(size_t n) {
        size_t r = r_.load(std::memory_order_relaxed);
        size_t w = w_.load(std::memory_order_acquire);
        n = std::min(n, w - r);
        r_.store(r + n, std::memory_order_release);
        return n;
    }

private:
    std::vector<float> buf_;
    size_t mask_ = 0;
    std::atomic<size_t> r_{0}, w_{0};
};
