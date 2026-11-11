// SPDX-License-Identifier: MIT
// nn::audio AudioOut host implementation + mixer core. Design and rationale: docs/audio.md.
//
// Threads:
//   guest sink thread (Wwise) : Open/Close/Start/Stop/Append/GetReleased/query calls. Append and GetReleased are
//                               wait-free (SPSC rings), everything else takes at most the init mutex.
//   device thread (backend)   : calls core_mix() once per device period. Never takes a lock.
#include "audio_internal.h"
#include "spsc_ring.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace snap_audio;

namespace {

constexpr int kMaxOuts = 4;
constexpr uint32_t kOpenMagic = 0x414f5554;  // 'AOUT' at AudioOut+0, never zero while open

struct OutSlot {
  std::atomic<int> alive{0};
  std::atomic<int> inUse{0};    // device thread is touching this slot (Close waits on it)
  std::atomic<int> started{0};
  uint32_t rate = 48000, channels = 2;
  SpscRing<SnapAudioOutBuffer*, 64> submit;    // guest -> device
  SpscRing<SnapAudioOutBuffer*, 64> released;  // device -> guest
  // device-thread private
  SnapAudioOutBuffer* cur = nullptr;
  uint64_t curFrame = 0;
};

const char* const kDeviceNames[4] = {"AudioStereoJackOutput", "AudioBuiltInSpeakerOutput", "AudioTvOutput", "AudioUsbOutput"};

struct Globals {
  std::mutex initMu;
  bool inited = false;
  SnapAudioConfig cfg{};
  bool cfgSet = false;
  std::unique_ptr<Backend> backend;
  DeviceFormat devFmt{48000, 2, 0};
  OutSlot slots[kMaxOuts];

  std::atomic<float> vol[SNAP_VOL_COUNT];
  std::atomic<float> deviceVol[4];
  int activeDevice = 2;  // AudioTvOutput
  float curGain = 1.0f;  // device-thread private (smoothed)

  std::atomic<void*> notifyEvent{nullptr};
  std::atomic<SnapAudioSignalEventFn> signalFn{nullptr};

  // stats
  std::atomic<uint64_t> framesMixed{0}, framesFromGuest{0}, underruns{0}, underrunFrames{0};
  std::atomic<uint64_t> appended{0}, releasedCount{0}, deviceChanges{0};
  std::atomic<uint32_t> bufFrames{0}, devChannels{2}, devRate{48000};
  std::atomic<double> latencyMs{0.0};
  std::atomic<int> attached{0};
  std::atomic<int> reportedChannels{2};

  SampleRing<(1u << 20)> tap;  // 1M samples
  std::atomic<int> tapOn{0};

  Globals() {
    for (auto& v : vol) v.store(1.0f);
    for (auto& v : deviceVol) v.store(1.0f);
  }
};

Globals& G() {
  static Globals g;
  return g;
}

void default_cfg(SnapAudioConfig* c) {
  c->backend = SNAP_AUDIO_BACKEND_AUTO;
  c->channelMode = SNAP_AUDIO_CH_STEREO;
  c->sampleRate = 48000;
  c->deviceBufferMs = 0;
  c->enableTap = 0;
}

void log_underrun(uint64_t n, uint32_t missing) {
  if (n <= 20 || (n % 100) == 0) std::fprintf(stderr, "[audio] underrun #%llu: %u frames short\n", (unsigned long long)n, missing);
}

// Mix `n` frames of slot `s` into dst (dc channels), accumulate. Returns frames actually produced.
uint32_t mix_slot(OutSlot& s, float* dst, uint32_t dc, uint32_t frames) {
  const uint32_t sc = s.channels;
  uint32_t done = 0;
  while (done < frames) {
    if (!s.cur) {
      SnapAudioOutBuffer* b = nullptr;
      if (!s.submit.pop(b)) break;
      s.cur = b;
      s.curFrame = 0;
    }
    const uint64_t bytes = s.cur->dataSize;
    const uint64_t total = bytes / (2ull * sc);
    const int16_t* pcm = reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(s.cur->buffer) + s.cur->dataOffset);
    uint64_t avail = total - s.curFrame;
    uint32_t take = (uint32_t)std::min<uint64_t>(avail, frames - done);
    const int16_t* p = pcm + s.curFrame * sc;
    float* d = dst + (size_t)done * dc;
    constexpr float k = 1.0f / 32768.0f;
    if (sc == dc) {
      for (uint32_t i = 0; i < take * dc; ++i) d[i] += p[i] * k;
    } else if (sc == 1) {
      for (uint32_t i = 0; i < take; ++i)
        for (uint32_t c = 0; c < std::min(dc, 2u); ++c) d[i * dc + c] += p[i] * k;
    } else if (sc == 2) {  // stereo into a 5.1 device: FL/FR only
      for (uint32_t i = 0; i < take; ++i) { d[i * dc] += p[i * 2] * k; d[i * dc + 1] += p[i * 2 + 1] * k; }
    } else if (sc >= 6 && dc == 2) {  // 5.1 -> stereo (FL FR FC LFE BL BR), LFE dropped
      constexpr float kc = 0.70710678f, norm = 1.0f / 1.70710678f;
      for (uint32_t i = 0; i < take; ++i) {
        const int16_t* f = p + i * sc;
        d[i * 2] += (f[0] + kc * f[2] + kc * f[4]) * k * norm;
        d[i * 2 + 1] += (f[1] + kc * f[2] + kc * f[5]) * k * norm;
      }
    } else {
      const uint32_t m = std::min(sc, dc);
      for (uint32_t i = 0; i < take; ++i)
        for (uint32_t c = 0; c < m; ++c) d[i * dc + c] += p[i * sc + c] * k;
    }
    s.curFrame += take;
    done += take;
    if (s.curFrame >= total) {
      SnapAudioOutBuffer* b = s.cur;
      s.cur = nullptr;
      while (!s.released.push(b)) std::this_thread::yield();  // 64-deep, guest drains in GetReleased
      G().releasedCount.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return done;
}

uint32_t slot_of(const void* audioOut) {
  const uint32_t* w = static_cast<const uint32_t*>(audioOut);
  if (!w || w[0] != kOpenMagic || w[1] >= (uint32_t)kMaxOuts) return ~0u;
  return w[1];
}

void ensure_init() {
  Globals& g = G();
  std::lock_guard<std::mutex> lk(g.initMu);
  if (g.inited) return;
  if (!g.cfgSet) default_cfg(&g.cfg);
  int kind = g.cfg.backend;
  if (kind == SNAP_AUDIO_BACKEND_AUTO) {
    const char* e = std::getenv("SNAP_AUDIO_BACKEND");
    if (e && !std::strcmp(e, "null")) kind = SNAP_AUDIO_BACKEND_NULL;
    else if (e && !std::strcmp(e, "wasapi")) kind = SNAP_AUDIO_BACKEND_WASAPI;
  }
#if defined(_WIN32)
  if (kind == SNAP_AUDIO_BACKEND_AUTO) kind = SNAP_AUDIO_BACKEND_WASAPI;
  if (kind == SNAP_AUDIO_BACKEND_WASAPI) g.backend.reset(make_wasapi_backend());
#endif
  if (!g.backend) g.backend.reset(make_null_backend());
  g.tapOn.store(g.cfg.enableTap);
  uint32_t ch = g.cfg.channelMode == SNAP_AUDIO_CH_SURROUND51 ? 6 : 2;
  if (g.cfg.channelMode == SNAP_AUDIO_CH_AUTO) ch = g.backend->native_channels() >= 6 ? 6 : 2;
  g.devFmt = DeviceFormat{g.cfg.sampleRate ? g.cfg.sampleRate : 48000u, ch, g.cfg.deviceBufferMs};
  g.reportedChannels.store((int)ch);
  g.devChannels.store(ch);
  g.devRate.store(g.devFmt.rate);
  if (!g.backend->start(g.devFmt)) {
    std::fprintf(stderr, "[audio] backend start failed, falling back to the timer backend\n");
    g.backend.reset(make_null_backend());
    g.backend->start(g.devFmt);
  }
  g.inited = true;
}

}  // namespace

// ---- core entry points used by backends --------------------------------------------------------------------------
namespace snap_audio {

void core_mix(float* dst, uint32_t frames) {
  Globals& g = G();
  const uint32_t dc = g.devChannels.load(std::memory_order_relaxed);
  std::memset(dst, 0, sizeof(float) * (size_t)frames * dc);
  uint32_t shortest = 0;
  bool anyStarted = false, short_ = false;
  uint64_t fromGuest = 0;
  for (int i = 0; i < kMaxOuts; ++i) {
    OutSlot& s = g.slots[i];
    if (!s.alive.load(std::memory_order_seq_cst)) continue;
    s.inUse.store(1, std::memory_order_seq_cst);
    if (s.alive.load(std::memory_order_seq_cst) && s.started.load(std::memory_order_acquire)) {
      anyStarted = true;
      const uint32_t got = mix_slot(s, dst, dc, frames);
      fromGuest += got;
      if (got < frames) { short_ = true; shortest = std::max(shortest, frames - got); }
    }
    s.inUse.store(0, std::memory_order_seq_cst);
  }
  // master x device volume, ramped over the block
  const float target = g.vol[SNAP_VOL_MASTER].load(std::memory_order_relaxed) * g.deviceVol[g.activeDevice].load(std::memory_order_relaxed);
  const float start = g.curGain;
  g.curGain = target;
  const float step = frames ? (target - start) / (float)frames : 0.0f;
  float gain = start;
  for (uint32_t f = 0; f < frames; ++f) {
    for (uint32_t c = 0; c < dc; ++c) {
      float v = dst[f * dc + c] * gain;
      dst[f * dc + c] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
    }
    gain += step;
  }
  g.framesMixed.fetch_add(frames, std::memory_order_relaxed);
  g.framesFromGuest.fetch_add(fromGuest, std::memory_order_relaxed);
  if (anyStarted && short_) {
    const uint64_t n = g.underruns.fetch_add(1, std::memory_order_relaxed) + 1;
    g.underrunFrames.fetch_add(shortest, std::memory_order_relaxed);
    log_underrun(n, shortest);
  }
  if (g.tapOn.load(std::memory_order_relaxed)) g.tap.write(dst, (size_t)frames * dc);
}

void core_notify_device_change() {
  Globals& g = G();
  g.deviceChanges.fetch_add(1, std::memory_order_relaxed);
  void* ev = g.notifyEvent.load();
  SnapAudioSignalEventFn fn = g.signalFn.load();
  if (ev && fn) fn(ev);
}

void core_set_device_info(bool attached, uint32_t bufferFrames, double latencyMs, uint32_t channels, uint32_t rate) {
  Globals& g = G();
  g.attached.store(attached ? 1 : 0);
  g.bufFrames.store(bufferFrames);
  g.latencyMs.store(latencyMs);
  (void)channels; (void)rate;  // the stream format is fixed at start(); WASAPI converts to the endpoint
}

}  // namespace snap_audio

// ---- nn::audio surface ---------------------------------------------------------------------------------------------
SNAP_AUDIO_API void snap_nn_audio_InitializeAudioOutParameter(SnapAudioOutParameter* p) {
  std::memset(p, 0, sizeof(*p));
}

SNAP_AUDIO_API uint32_t snap_nn_audio_OpenDefaultAudioOut(void* audioOut, const SnapAudioOutParameter* p) {
  ensure_init();
  Globals& g = G();
  uint32_t rate = p->sampleRate ? p->sampleRate : g.devFmt.rate;
  uint32_t ch = p->channelCount ? p->channelCount : (uint32_t)g.reportedChannels.load();
  if (rate != g.devFmt.rate || (ch != 1 && ch != 2 && ch != 6)) {
    std::fprintf(stderr, "[audio] OpenDefaultAudioOut: unsupported rate %u / channels %u\n", rate, ch);
    return 0x1;  // error (the game treats nonzero as failure)
  }
  std::lock_guard<std::mutex> lk(g.initMu);
  for (int i = 0; i < kMaxOuts; ++i) {
    OutSlot& s = g.slots[i];
    if (s.alive.load()) continue;
    s.rate = rate; s.channels = ch; s.cur = nullptr; s.curFrame = 0; s.started.store(0);
    s.submit.reset(); s.released.reset();
    uint32_t* w = static_cast<uint32_t*>(audioOut);
    w[0] = kOpenMagic; w[1] = (uint32_t)i; w[2] = rate; w[3] = ch;  // +8 rate, +12 channels (host-read only)
    s.alive.store(1, std::memory_order_seq_cst);
    return 0;
  }
  return 0x2;
}

SNAP_AUDIO_API void snap_nn_audio_CloseAudioOut(void* audioOut) {
  const uint32_t i = slot_of(audioOut);
  if (i == ~0u) return;
  OutSlot& s = G().slots[i];
  s.started.store(0);
  s.alive.store(0, std::memory_order_seq_cst);
  while (s.inUse.load(std::memory_order_seq_cst)) std::this_thread::yield();  // device thread done with the buffers
  static_cast<uint32_t*>(audioOut)[0] = 0;
}

SNAP_AUDIO_API uint32_t snap_nn_audio_StartAudioOut(void* audioOut) {
  const uint32_t i = slot_of(audioOut);
  if (i == ~0u) return 0x3;
  G().slots[i].started.store(1, std::memory_order_release);
  return 0;
}

SNAP_AUDIO_API void snap_nn_audio_StopAudioOut(void* audioOut) {
  const uint32_t i = slot_of(audioOut);
  if (i != ~0u) G().slots[i].started.store(0, std::memory_order_release);
}

SNAP_AUDIO_API int snap_nn_audio_GetAudioOutState(const void* audioOut) {
  const uint32_t i = slot_of(audioOut);
  if (i == ~0u) return SNAP_AUDIO_STATE_STOPPED;
  return G().slots[i].started.load(std::memory_order_acquire) ? SNAP_AUDIO_STATE_STARTED : SNAP_AUDIO_STATE_STOPPED;
}

SNAP_AUDIO_API uint32_t snap_nn_audio_GetAudioOutChannelCount(const void* audioOut) {
  const uint32_t i = slot_of(audioOut);
  return i == ~0u ? 0 : G().slots[i].channels;
}

SNAP_AUDIO_API void snap_nn_audio_SetAudioOutBufferInfo(SnapAudioOutBuffer* b, void* data, uint64_t bufferSize, uint64_t dataSize) {
  b->next = nullptr;
  b->buffer = data;
  b->bufferSize = bufferSize;
  b->dataSize = dataSize;
  b->dataOffset = 0;
}

SNAP_AUDIO_API void* snap_nn_audio_GetAudioOutBufferDataPointer(const SnapAudioOutBuffer* b) { return b->buffer; }

SNAP_AUDIO_API void snap_nn_audio_AppendAudioOutBuffer(void* audioOut, SnapAudioOutBuffer* b) {
  const uint32_t i = slot_of(audioOut);
  if (i == ~0u) return;
  OutSlot& s = G().slots[i];
  if (!s.submit.push(b)) {
    std::fprintf(stderr, "[audio] AppendAudioOutBuffer: queue full, buffer dropped\n");
    return;
  }
  G().appended.fetch_add(1, std::memory_order_relaxed);
}

SNAP_AUDIO_API SnapAudioOutBuffer* snap_nn_audio_GetReleasedAudioOutBuffer(void* audioOut) {
  const uint32_t i = slot_of(audioOut);
  if (i == ~0u) return nullptr;
  SnapAudioOutBuffer* b = nullptr;
  return G().slots[i].released.pop(b) ? b : nullptr;
}

SNAP_AUDIO_API int snap_nn_audio_ListAudioDeviceName(SnapAudioDeviceName* names, int count) {
  int n = std::min(count, 4);
  for (int i = 0; i < n; ++i) {
    std::memset(names[i].name, 0, sizeof(names[i].name));
    std::strncpy(names[i].name, kDeviceNames[i], sizeof(names[i].name) - 1);
  }
  return n;
}

SNAP_AUDIO_API void snap_nn_audio_SetAudioDeviceOutputVolume(const SnapAudioDeviceName* name, float volume) {
  for (int i = 0; i < 4; ++i)
    if (!std::strncmp(name->name, kDeviceNames[i], sizeof(name->name))) {
      G().deviceVol[i].store(std::clamp(volume, 0.0f, 1.0f));
      return;
    }
}

SNAP_AUDIO_API void snap_nn_audio_GetActiveAudioDeviceName(SnapAudioDeviceName* name) {
  std::memset(name->name, 0, sizeof(name->name));
  std::strncpy(name->name, kDeviceNames[G().activeDevice], sizeof(name->name) - 1);
}

SNAP_AUDIO_API int snap_nn_audio_GetActiveAudioDeviceChannelCountForOutput(void) {
  ensure_init();
  Globals& g = G();
  if (g.cfg.channelMode == SNAP_AUDIO_CH_AUTO) {
    const int ch = g.backend->native_channels() >= 6 ? 6 : 2;
    g.reportedChannels.store(ch);
  }
  return g.reportedChannels.load();
}

SNAP_AUDIO_API uint32_t snap_nn_audio_AcquireAudioDeviceNotificationForOutput(void* systemEvent) {
  G().notifyEvent.store(systemEvent);
  return 0;
}

// ---- override table --------------------------------------------------------------------------------------------------
SNAP_AUDIO_API const SnapAudioOverride* snap_audio_overrides(int* count) {
  static const SnapAudioOverride t[] = {
      {"_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", (void*)&snap_nn_audio_InitializeAudioOutParameter},
      {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", (void*)&snap_nn_audio_OpenDefaultAudioOut},
      {"_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", (void*)&snap_nn_audio_CloseAudioOut},
      {"_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", (void*)&snap_nn_audio_StartAudioOut},
      {"_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", (void*)&snap_nn_audio_StopAudioOut},
      {"_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", (void*)&snap_nn_audio_GetAudioOutState},
      {"_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", (void*)&snap_nn_audio_GetAudioOutChannelCount},
      {"_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", (void*)&snap_nn_audio_SetAudioOutBufferInfo},
      {"_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", (void*)&snap_nn_audio_GetAudioOutBufferDataPointer},
      {"_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", (void*)&snap_nn_audio_AppendAudioOutBuffer},
      {"_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", (void*)&snap_nn_audio_GetReleasedAudioOutBuffer},
      {"_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi", (void*)&snap_nn_audio_ListAudioDeviceName},
      {"_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", (void*)&snap_nn_audio_SetAudioDeviceOutputVolume},
      {"_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", (void*)&snap_nn_audio_GetActiveAudioDeviceName},
      {"_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", (void*)&snap_nn_audio_GetActiveAudioDeviceChannelCountForOutput},
      {"_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", (void*)&snap_nn_audio_AcquireAudioDeviceNotificationForOutput},
  };
  *count = (int)(sizeof(t) / sizeof(t[0]));
  return t;
}

// ---- host controls -----------------------------------------------------------------------------------------------------
SNAP_AUDIO_API void snap_audio_default_config(SnapAudioConfig* cfg) { default_cfg(cfg); }
SNAP_AUDIO_API void snap_audio_configure(const SnapAudioConfig* cfg) {
  Globals& g = G();
  std::lock_guard<std::mutex> lk(g.initMu);
  if (g.inited) return;
  g.cfg = *cfg;
  g.cfgSet = true;
}
SNAP_AUDIO_API void snap_audio_shutdown(void) {
  Globals& g = G();
  std::lock_guard<std::mutex> lk(g.initMu);
  if (!g.inited) return;
  g.backend->stop();
  g.backend.reset();
  for (auto& s : g.slots) s.alive.store(0);
  g.inited = false;
  g.cfgSet = false;
}
SNAP_AUDIO_API void snap_audio_set_volume(int c, float v) {
  if (c >= 0 && c < SNAP_VOL_COUNT) G().vol[c].store(std::clamp(v, 0.0f, 1.0f));
}
SNAP_AUDIO_API float snap_audio_get_volume(int c) { return (c >= 0 && c < SNAP_VOL_COUNT) ? G().vol[c].load() : 0.0f; }
SNAP_AUDIO_API void snap_audio_set_os_hooks(SnapAudioSignalEventFn signal) { G().signalFn.store(signal); }

SNAP_AUDIO_API void snap_audio_get_stats(SnapAudioStats* s) {
  Globals& g = G();
  s->framesMixed = g.framesMixed; s->framesFromGuest = g.framesFromGuest;
  s->underruns = g.underruns; s->underrunFrames = g.underrunFrames;
  s->buffersAppended = g.appended; s->buffersReleased = g.releasedCount;
  s->deviceChanges = g.deviceChanges; s->deviceBufferFrames = g.bufFrames; s->deviceLatencyMs = g.latencyMs;
  s->deviceChannels = g.devChannels; s->deviceRate = g.devRate;
  s->backendInUse = g.backend ? g.backend->kind() : 0;
  s->deviceAttached = g.attached;
}
SNAP_AUDIO_API void snap_audio_reset_stats(void) {
  Globals& g = G();
  g.framesMixed = 0; g.framesFromGuest = 0; g.underruns = 0; g.underrunFrames = 0; g.appended = 0; g.releasedCount = 0;
}
SNAP_AUDIO_API uint64_t snap_audio_tap_read(float* dst, uint64_t n) { return G().tap.read(dst, (size_t)n); }
SNAP_AUDIO_API uint32_t snap_audio_tap_channels(void) { return G().devChannels; }
