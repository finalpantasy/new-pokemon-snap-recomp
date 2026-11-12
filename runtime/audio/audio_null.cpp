// SPDX-License-Identifier: MIT
// Timer-paced backend: no device, consumes at the exact stream rate in 10 ms blocks on a real-time deadline.
// Used for tests, for machines with no audio endpoint, and as the WASAPI fallback shape.
#include "audio_internal.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace snap_audio {
namespace {

class NullBackend : public Backend {
 public:
  bool start(const DeviceFormat& f) override {
    fmt_ = f;
    run_.store(true);
    core_set_device_info(false, 0, 0.0, f.channels, f.rate, f.mode);
    th_ = std::thread([this] { loop(); });
    return true;
  }
  void stop() override {
    run_.store(false);
    if (th_.joinable()) th_.join();
  }
  int kind() const override { return SNAP_AUDIO_BACKEND_NULL; }

 private:
  void loop() {
    using clk = std::chrono::steady_clock;
    const uint32_t block = fmt_.rate / 100;  // 10 ms
    std::vector<float> buf((size_t)block * fmt_.channels);
    const auto period = std::chrono::nanoseconds(1000000000ll * block / fmt_.rate);
    auto next = clk::now() + period;
    while (run_.load(std::memory_order_relaxed)) {
      core_mix(buf.data(), block);
      std::this_thread::sleep_until(next);
      next += period;
      if (clk::now() > next + period * 10) next = clk::now() + period;  // stalled: resync, do not burst
    }
  }
  DeviceFormat fmt_{};
  std::atomic<bool> run_{false};
  std::thread th_;
};

}  // namespace

Backend* make_null_backend() { return new NullBackend(); }

}  // namespace snap_audio
