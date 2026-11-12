// SPDX-License-Identifier: MIT
// Host-only test: drives the AudioOut API in the exact order the game's Wwise sink does (call sites at main
// 0x124780 open, 0x124b10 submit, 0x124ce0 poll, 0x124a10 close) with a generated 10 s sweep, checks that nothing
// underruns, that the captured mix equals what was submitted, and writes analysis/audio/sink_test.wav.
//
// usage: sink_test.exe [--backend null|wasapi] [--seconds N] [--volume V] [--wav path] [--log path]
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstdarg>
#include <vector>

#include "audio_out.h"

#pragma comment(lib, "winmm.lib")

static FILE* g_log = nullptr;
static void say(const char* fmt, ...) {
  char b[512];
  va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  std::fputs(b, stdout);
  if (g_log) std::fputs(b, g_log);
}

// Wwise sink state, as built by the constructor at 0x124640 and Open at 0x124780.
struct Sink {
  alignas(16) unsigned char audioOut[0x168];  // guest nn::audio::AudioOut storage, game memsets 0x168
  SnapAudioOutBuffer bufs[4];
  void* data[4];
  uint32_t numBufs = 4;       // AkPlatformInitSettings default
  uint32_t frames = 1024;     // AkInitSettings::uNumSamplesPerFrame
  uint32_t channels = 2;
  uint32_t rate = 48000;
  uint32_t readIdx = 0, writeIdx = 0, pending = 0;
};

static bool sink_open(Sink& s, uint32_t wantCh) {
  std::memset(s.audioOut, 0, sizeof(s.audioOut));
  SnapAudioOutParameter p;
  snap_nn_audio_InitializeAudioOutParameter(&p);
  p.sampleRate = s.rate;
  p.channelCount = (uint16_t)wantCh;
  if (snap_nn_audio_OpenDefaultAudioOut(s.audioOut, &p) != 0) return false;
  s.channels = snap_nn_audio_GetAudioOutChannelCount(s.audioOut);
  const uint64_t dataSize = (uint64_t)(s.frames & 0xffff) * 2 * s.channels;  // ubfiz #1,#16 * channels
  const uint64_t cap = (dataSize + 0xfff) & ~0xfffull;
  s.readIdx = s.writeIdx = s.pending = 0;
  for (uint32_t i = 0; i < s.numBufs; ++i) {
    s.data[i] = _aligned_malloc((size_t)cap, 4096);
    snap_nn_audio_SetAudioOutBufferInfo(&s.bufs[i], s.data[i], cap, dataSize);
  }
  for (uint32_t i = 0; i < s.numBufs; ++i) {  // prefill: vtable slot 0x78 -> 0x124c30, silent buffers
    SnapAudioOutBuffer* b = &s.bufs[s.writeIdx];
    std::memset(snap_nn_audio_GetAudioOutBufferDataPointer(b), 0, (size_t)dataSize);
    snap_nn_audio_AppendAudioOutBuffer(s.audioOut, b);
    s.writeIdx = (s.writeIdx + 1) % s.numBufs; ++s.pending;
  }
  return snap_nn_audio_StartAudioOut(s.audioOut) == 0;  // 0x124d60
}

static void sink_close(Sink& s) {  // 0x124a10
  if (*reinterpret_cast<uint32_t*>(s.audioOut) != 0) {
    if (snap_nn_audio_GetAudioOutState(s.audioOut) == SNAP_AUDIO_STATE_STARTED) snap_nn_audio_StopAudioOut(s.audioOut);
    snap_nn_audio_CloseAudioOut(s.audioOut);
  }
  for (uint32_t i = 0; i < s.numBufs; ++i) { _aligned_free(s.data[i]); s.data[i] = nullptr; }
}

// 0x124ce0: drain released buffers, return the number of free slots
static uint32_t sink_free_slots(Sink& s) {
  while (snap_nn_audio_GetReleasedAudioOutBuffer(s.audioOut)) {
    s.readIdx = (s.readIdx + 1) % s.numBufs;
    --s.pending;
  }
  return s.numBufs - s.pending;
}

// 0x124b10: float interleaved -> s16 with fcvtzs #15 (x * 32768, truncate toward zero) + sqxtn (saturate)
static void sink_submit(Sink& s, const float* in) {
  SnapAudioOutBuffer* b = &s.bufs[s.writeIdx];
  int16_t* out = static_cast<int16_t*>(snap_nn_audio_GetAudioOutBufferDataPointer(b));
  const uint32_t n = s.frames * s.channels;
  for (uint32_t i = 0; i < n; ++i) {
    long v = (long)(in[i] * 32768.0f);
    out[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
  }
  snap_nn_audio_AppendAudioOutBuffer(s.audioOut, b);
  s.writeIdx = (s.writeIdx + 1) % s.numBufs;
  ++s.pending;
}

static void write_wav(const char* path, const std::vector<int16_t>& pcm, uint32_t ch, uint32_t rate) {
  FILE* f = std::fopen(path, "wb");
  if (!f) { say("cannot write %s\n", path); return; }
  uint32_t dataBytes = (uint32_t)(pcm.size() * 2), riff = 36 + dataBytes, fmtLen = 16, byteRate = rate * ch * 2;
  uint16_t tag = 1, chs = (uint16_t)ch, align = (uint16_t)(ch * 2), bits = 16;
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmtLen, 4, 1, f); std::fwrite(&tag, 2, 1, f); std::fwrite(&chs, 2, 1, f);
  std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f);
  std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
  std::fwrite(pcm.data(), 2, pcm.size(), f);
  std::fclose(f);
}

struct Run {
  std::vector<float> expect;   // what the game side submitted, post-quantization, interleaved at sink channels
  std::vector<float> tapped;   // final mix (device channels)
  uint64_t underrunsWhilePlaying = 0;  // snapshot before the tail drains (running dry after the last buffer is expected)
};

static void drain_tap(std::vector<float>& dst) {
  float tmp[8192];
  for (;;) {
    uint64_t n = snap_audio_tap_read(tmp, 8192);
    if (!n) break;
    dst.insert(dst.end(), tmp, tmp + n);
  }
}

// Plays `seconds` of generated audio through the sink loop. mode 0 = sweep (stereo), 1 = 5.1 center tone.
static void play(Sink& s, double seconds, int mode, Run& r) {
  const uint64_t totalBufs = (uint64_t)(seconds * s.rate / s.frames);
  std::vector<float> in((size_t)s.frames * s.channels);
  double phase = 0.0;
  uint64_t produced = 0, frameCounter = 0;
  while (produced < totalBufs) {
    uint32_t freeSlots = sink_free_slots(s);
    while (freeSlots && produced < totalBufs) {
      for (uint32_t f = 0; f < s.frames; ++f, ++frameCounter) {
        const double t = (double)frameCounter / s.rate;
        float v;
        if (mode == 0) {  // log sweep 100 Hz -> 6 kHz over `seconds`, 0.25 amplitude, plus a fixed 440 Hz at 0.1
          const double f0 = 100.0, f1 = 6000.0, k = std::log(f1 / f0) / seconds;
          phase += 2.0 * M_PI * (f0 * std::exp(k * t)) / s.rate;
          v = (float)(0.25 * std::sin(phase) + 0.1 * std::sin(2.0 * M_PI * 440.0 * t));
          for (uint32_t c = 0; c < s.channels; ++c) in[f * s.channels + c] = c ? v * 0.8f : v;
        } else {
          for (uint32_t c = 0; c < s.channels; ++c) in[f * s.channels + c] = (c == 2) ? 0.5f : 0.0f;
        }
      }
      for (float x : in) {  // quantize exactly like the submit path so the comparison is bit-exact
        long q = (long)(x * 32768.0f);
        q = q > 32767 ? 32767 : (q < -32768 ? -32768 : q);
        r.expect.push_back((float)q / 32768.0f);
      }
      sink_submit(s, in.data());
      ++produced; --freeSlots;
    }
    Sleep(2);  // sink thread wait; the real Wwise loop sleeps between polls
    drain_tap(r.tapped);
  }
  { SnapAudioStats sn; snap_audio_get_stats(&sn); r.underrunsWhilePlaying = sn.underruns; }
  // let the last buffers play out
  for (int i = 0; i < 200 && s.pending > 0; ++i) { Sleep(5); sink_free_slots(s); drain_tap(r.tapped); }
  drain_tap(r.tapped);
}

int main(int argc, char** argv) {
  _putenv_s("SNAP_CONFIG_DIR", ".");  // keep the test volume out of the real audio.json
  std::string backend = "null", wavPath = "analysis/audio/sink_test.wav", logPath = "analysis/audio/sink_test.log";
  double seconds = 10.0, volume = 1.0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--backend" && i + 1 < argc) backend = argv[++i];
    else if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
    else if (a == "--volume" && i + 1 < argc) volume = atof(argv[++i]);
    else if (a == "--wav" && i + 1 < argc) wavPath = argv[++i];
    else if (a == "--log" && i + 1 < argc) logPath = argv[++i];
  }
  g_log = std::fopen(logPath.c_str(), "w");
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  timeBeginPeriod(1);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);  // the sink thread; the process stays BelowNormal

  SnapAudioConfig cfg;
  snap_audio_default_config(&cfg);
  cfg.backend = backend == "wasapi" ? SNAP_AUDIO_BACKEND_WASAPI : SNAP_AUDIO_BACKEND_NULL;
  cfg.enableTap = 1;
  snap_audio_configure(&cfg);
  snap_audio_set_volume(SNAP_VOL_MASTER, (float)volume);
  say("backend=%s seconds=%.1f master volume=%.3f\n", backend.c_str(), seconds, volume);

  int failures = 0;
  auto check = [&](bool ok, const char* what) { say("%s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; };

  // The game queries these before opening (0x1232e0 / 0x123480).
  SnapAudioDeviceName dn;
  snap_nn_audio_GetActiveAudioDeviceName(&dn);
  const int devCh = snap_nn_audio_GetActiveAudioDeviceChannelCountForOutput();
  say("active device '%s', channels %d\n", dn.name, devCh);
  check(devCh == 2 || devCh == 6, "device channel count is 2 or 6");

  // ---- phase 1: 10 s stereo sweep ----
  Sink s;
  Run r1;
  snap_audio_reset_stats();
  check(sink_open(s, 0), "open default AudioOut (rate 48000, channels = device)");
  check(s.channels == (uint32_t)devCh, "AudioOut channel count matches the device query");
  play(s, seconds, 0, r1);
  SnapAudioStats st;
  snap_audio_get_stats(&st);
  say("backend in use=%d attached=%d deviceBufferFrames=%u latency=%.2f ms | queue 4 x %u frames = %.2f ms\n",
      st.backendInUse, st.deviceAttached, st.deviceBufferFrames, st.deviceLatencyMs, s.frames, 4.0 * s.frames * 1000.0 / s.rate);
  say("frames mixed=%llu from guest=%llu appended=%llu released=%llu underruns while feeding=%llu, total incl. tail=%llu (short frames %llu)\n",
      (unsigned long long)st.framesMixed, (unsigned long long)st.framesFromGuest, (unsigned long long)st.buffersAppended,
      (unsigned long long)st.buffersReleased, (unsigned long long)r1.underrunsWhilePlaying, (unsigned long long)st.underruns, (unsigned long long)st.underrunFrames);
  check(r1.underrunsWhilePlaying == 0, "no underruns while the sink was feeding");
  check(st.buffersAppended == st.buffersReleased + s.pending, "every appended buffer was released or is still queued");

  // Compare the tapped mix against what was submitted (volume applied). Tap starts with the silent prefill.
  const uint32_t tch = snap_audio_tap_channels();
  size_t first = 0;
  while (first < r1.tapped.size() && r1.tapped[first] == 0.0f) ++first;
  size_t efirst = 0;
  while (efirst < r1.expect.size() && r1.expect[efirst] == 0.0f) ++efirst;
  double maxErr = 0.0;
  size_t cmp = 0;
  if (first < r1.tapped.size() && s.channels == tch) {
    for (size_t i = first, j = efirst; i < r1.tapped.size() && j < r1.expect.size(); ++i, ++j, ++cmp) {
      double e = std::fabs(r1.tapped[i] - r1.expect[j] * volume);
      if (e > maxErr) maxErr = e;
    }
  }
  say("compared %zu samples, max error %.3g\n", cmp, maxErr);
  check(cmp >= (size_t)(seconds * s.rate * s.channels * 0.95) && maxErr < 2e-4, "captured mix equals submitted PCM (x master volume)");

  // WAV proof
  std::vector<int16_t> pcm(r1.tapped.size());
  for (size_t i = 0; i < pcm.size(); ++i) {
    float v = r1.tapped[i];
    pcm[i] = (int16_t)(v > 1.f ? 32767 : (v < -1.f ? -32768 : v * 32767.f));
  }
  write_wav(wavPath.c_str(), pcm, tch, 48000);
  say("wrote %s (%zu frames, %u channels)\n", wavPath.c_str(), pcm.size() / tch, tch);
  sink_close(s);
  check(*reinterpret_cast<uint32_t*>(s.audioOut) == 0, "close clears the open marker");

  // ---- phase 2: reopen as 5.1, center tone -> stereo downmix ----
  if (tch == 2) {
    Run r2;
    s.channels = 6;
    check(sink_open(s, 6), "reopen as 5.1 after close (slot reuse)");
    snap_audio_reset_stats();
    drain_tap(r2.tapped);
    r2.tapped.clear();
    play(s, 0.5, 1, r2);
    snap_audio_get_stats(&st);
    check(r2.underrunsWhilePlaying == 0, "5.1 phase: no underruns while feeding");
    float peak = 0.f;
    for (size_t i = r2.tapped.size() > 20000 ? r2.tapped.size() / 2 : 0; i < r2.tapped.size(); ++i) peak = std::fmax(peak, std::fabs(r2.tapped[i]));
    const double want = 0.5 * 0.70710678 / 1.70710678 * volume;
    say("5.1 center downmix peak %.5f, expected %.5f\n", peak, want);
    check(std::fabs(peak - want) < 5e-4, "5.1 -> stereo downmix level");
    sink_close(s);
  }

  snap_audio_shutdown();
  timeEndPeriod(1);
  say("%s (%d failure(s))\n", failures ? "RESULT: FAIL" : "RESULT: PASS", failures);
  if (g_log) std::fclose(g_log);
  return failures ? 1 : 0;
}
