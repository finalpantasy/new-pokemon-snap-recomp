// SPDX-License-Identifier: MIT
// Lock-free single-producer single-consumer rings (power-of-two capacity). Wait-free on both sides.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace snap_audio {

template <typename T, size_t N>
class SpscRing {
  static_assert((N & (N - 1)) == 0 && N >= 2, "N must be a power of two");
 public:
  // Producer side. Returns false when full.
  bool push(const T& v) {
    const size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) == N) return false;
    buf_[h & (N - 1)] = v;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  // Consumer side. Returns false when empty.
  bool pop(T& v) {
    const size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    v = buf_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
  size_t size() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }
  // Only safe while neither side is running.
  void reset() { head_.store(0); tail_.store(0); }
 private:
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
  T buf_[N];
};

// SPSC ring of float samples with bulk transfer (the capture tap).
template <size_t N>
class SampleRing {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");
 public:
  size_t write(const float* src, size_t n) {  // returns samples written (drops the rest)
    const size_t h = head_.load(std::memory_order_relaxed);
    const size_t room = N - (h - tail_.load(std::memory_order_acquire));
    if (n > room) n = room;
    for (size_t i = 0; i < n; ++i) buf_[(h + i) & (N - 1)] = src[i];
    head_.store(h + n, std::memory_order_release);
    return n;
  }
  size_t read(float* dst, size_t n) {
    const size_t t = tail_.load(std::memory_order_relaxed);
    const size_t avail = head_.load(std::memory_order_acquire) - t;
    if (n > avail) n = avail;
    for (size_t i = 0; i < n; ++i) dst[i] = buf_[(t + i) & (N - 1)];
    tail_.store(t + n, std::memory_order_release);
    return n;
  }
  void reset() { head_.store(0); tail_.store(0); }
 private:
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
  float buf_[N];
};

}  // namespace snap_audio
