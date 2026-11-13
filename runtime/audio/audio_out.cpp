// SPDX-License-Identifier: MIT
// nn::audio AudioOut host implementation + mixer core. Design and rationale: docs/audio.md.
//
// Threads:
//   guest sink thread (Wwise) : Open/Close/Start/Stop/Append/GetReleased/query calls. Append and GetReleased are
//                               wait-free (SPSC rings), everything else takes at most the init mutex.
//   device thread (backend)   : calls core_mix() once per device period. Never takes a lock.
#include "audio_internal.h"
#include "audio_cfg.h"
#include "audio_mix.h"
#include "spsc_ring.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
  DeviceFormat devFmt{48000, 2, 0, 0};
  // output mode (docs/audio.md "Surround and spatial output"); resolved in ensure_init
  int cfgOutMode = 0, cfgGameCh = 0, cfgSpk = 0;   // from audio.json, overridden by env and SnapAudioConfig
  std::atomic<int> activeMode{0};
  std::atomic<uint32_t> guestCh{2};
  // capture (env SNAP_AUDIO_CAPTURE=<dir>, SNAP_AUDIO_CAPTURE_SECS=<n>, default 30): WAV of the device mix and of the
  // guest PCM, per-channel RMS logged every 5 s. Device-thread private.
  std::string capDir; uint32_t capSecs = 30;
  FILE* capDev = nullptr; FILE* capGuest = nullptr;
  uint32_t capDevCh = 0, capGuestCh = 0;
  uint64_t capDevFrames = 0, capGuestFrames = 0;
  double dSum[8] = {}, gSum[8] = {}; uint64_t dN = 0, gN = 0, capTick = 0;
  OutSlot slots[kMaxOuts];

  std::atomic<float> vol[SNAP_VOL_COUNT];
  std::atomic<float> deviceVol[4];
  int activeDevice = 2;  // AudioTvOutput
  float curGain = 1.0f;  // device-thread private (smoothed)
  snap_audio::mix::Upmixer upmix;   // device-thread private
  bool upmixOn = true;               // env SNAP_AUDIO_UPMIX=0 turns it off, audio.json "upmix"

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

  // volume persistence + guest category-volume sync (see docs/audio.md, "Volume")
  std::atomic<uint32_t> volGen{1};   // bumped on every set_volume; abi_audio.cpp re-applies Wwise levels when it changes
  std::atomic<uint32_t> bgmSeSet{0}; // bit per category: 1 once the user/config/env set it away from the default
  std::mutex volMu;

  // level meter / PCM dump (device thread private, env SNAP_AUDIO_LEVELS=1, SNAP_AUDIO_DUMP=<file.raw>)
  bool levelLog = false;
  FILE* levelFile = nullptr;
  FILE* dump = nullptr;
  double sumsq = 0; float peak = 0; uint64_t nacc = 0; uint64_t secs = 0;

  Globals();
};

#if defined(_WIN32)
// Paths are UTF-8 strings; getenv / fopen / MoveFileExA use the ANSI code page and break on a non-ASCII user name (review_hle #3).
std::wstring widen(const std::string& u) {
  if (u.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0);
  std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), w.data(), n);
  return w;
}
std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s((size_t)(n > 1 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}
FILE* fopen_u8(const std::string& path, const wchar_t* mode) { return _wfopen(widen(path).c_str(), mode); }
#define FOPEN_RB L"rb"
#define FOPEN_WB L"wb"
#else
FILE* fopen_u8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
#define FOPEN_RB "rb"
#define FOPEN_WB "wb"
#endif

std::string config_path() {
  std::string d;
#if defined(_WIN32)
  if (const wchar_t* e = _wgetenv(L"SNAP_CONFIG_DIR"); e && *e) d = narrow(e);
  else if (const wchar_t* a = _wgetenv(L"APPDATA"); a && *a) d = narrow(a) + "\\SnapRecomp";
#else
  if (const char* e = std::getenv("SNAP_CONFIG_DIR"); e && *e) d = e;
#endif
  else d = ".";
  return d + "\\audio.json";
}

const char* const kVolKeys[SNAP_VOL_COUNT] = {"master", "bgm", "se", "voice"};

// Tiny reader for {"master":1.0,"bgm":1.0,...}; missing or malformed keys keep the default 1.0.
void load_volumes(Globals& g) {
  FILE* f = fopen_u8(config_path(), FOPEN_RB);
  if (f) {
    char buf[1024]; size_t n = std::fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; std::fclose(f);
    for (int i = 0; i < SNAP_VOL_COUNT; ++i) {
      std::string key = std::string("\"") + kVolKeys[i] + "\"";
      const char* p = std::strstr(buf, key.c_str());
      if (!p) continue;
      p = std::strchr(p + key.size(), ':');
      if (!p) continue;
      char* end = nullptr;
      float v = std::strtof(p + 1, &end);
      if (end != p + 1 && v == v) g.vol[i].store(std::clamp(v, 0.0f, 1.0f));
    }
  }
  // env overrides (tests, scripts); not persisted
  for (int i = 0; i < SNAP_VOL_COUNT; ++i) {
    std::string name = std::string("SNAP_VOL_") + (i == 0 ? "MASTER" : i == 1 ? "BGM" : i == 2 ? "SE" : "VOICE");
    if (const char* e = std::getenv(name.c_str()); e && *e) g.vol[i].store(std::clamp((float)std::atof(e), 0.0f, 1.0f));
  }
}

bool read_json_string(const char* buf, const char* key, char* out, size_t cap) {
  std::string k = std::string("\"") + key + "\"";
  const char* p = std::strstr(buf, k.c_str());
  if (!p) return false;
  p = std::strchr(p + k.size(), ':');
  if (!p) return false;
  ++p;
  while (*p == ' ' || *p == '	') ++p;
  if (*p != '"') return false;
  ++p;
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
  out[n] = 0;
  return true;
}

int parse_mode(const char* s) {
  if (!s) return -1;
  if (!std::strcmp(s, "stereo")) return SNAP_AUDIO_OUT_STEREO;
  if (!std::strcmp(s, "speakers")) return SNAP_AUDIO_OUT_SPEAKERS;
  if (!std::strcmp(s, "spatial")) return SNAP_AUDIO_OUT_SPATIAL;
  return -1;
}
const char* mode_name(int m) { return m == SNAP_AUDIO_OUT_SPEAKERS ? "speakers" : m == SNAP_AUDIO_OUT_SPATIAL ? "spatial" : "stereo"; }

// "output"/"channels"/"speakers" keys of audio.json (stored in g.cfg*). Env overrides are applied in ensure_init.
void load_output(Globals& g) {
  FILE* f = fopen_u8(config_path(), FOPEN_RB);
  if (!f) return;
  char buf[1024]; size_t n = std::fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; std::fclose(f);
  char v[32];
  if (read_json_string(buf, "output", v, sizeof v)) { int m = parse_mode(v); if (m >= 0) g.cfgOutMode = m; }
  for (int k = 0; k < 2; ++k) {
    const char* key = k == 0 ? "channels" : "speakers";
    int x = 0;
    if (!cfg::read_int(buf, key, &x)) continue;   // key lookup, not value lookup: "output": "speakers" must not match the "speakers" key
    if (k == 0 && (x == 0 || x == 2 || x == 6)) g.cfgGameCh = x;
    if (k == 1 && (x == 0 || x == 6 || x == 8)) g.cfgSpk = x;
  }
}

void save_volumes(Globals& g) {
  std::lock_guard<std::mutex> lk(g.volMu);
  const std::string path = config_path(), tmp = path + ".tmp";
#if defined(_WIN32)
  if (const size_t cut = path.find_last_of("\\/"); cut != std::string::npos) CreateDirectoryW(widen(path.substr(0, cut)).c_str(), nullptr);   // first run: the folder may not exist
#endif
  FILE* f = fopen_u8(tmp, FOPEN_WB);
  if (!f) return;
  std::fprintf(f, "{\n  \"master\": %.4f,\n  \"bgm\": %.4f,\n  \"se\": %.4f,\n  \"voice\": %.4f,\n  \"output\": \"%s\",\n  \"channels\": %d,\n  \"speakers\": %d\n}\n",
               g.vol[0].load(), g.vol[1].load(), g.vol[2].load(), g.vol[3].load(), mode_name(g.cfgOutMode), g.cfgGameCh, g.cfgSpk);
  // a short write (disk full) or failed close must not replace the good file with a truncated one
  const bool wrote = std::ferror(f) == 0;
  const bool closed = std::fclose(f) == 0;
#if defined(_WIN32)
  if (!wrote || !closed || !MoveFileExW(widen(tmp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) DeleteFileW(widen(tmp).c_str());
#else
  if (!wrote || !closed) { std::remove(tmp.c_str()); return; }
  std::rename(tmp.c_str(), path.c_str());
#endif
}

Globals::Globals() {
  for (auto& v : vol) v.store(1.0f);
  for (auto& v : deviceVol) v.store(1.0f);
  load_volumes(*this);
  load_output(*this);
  for (int i = 1; i < SNAP_VOL_COUNT; ++i) if (vol[i].load() < 0.999f) bgmSeSet.fetch_or(1u << i);
  const char* l = std::getenv("SNAP_AUDIO_LEVELS");
  levelLog = l && *l && *l != '0';
  if (levelLog && std::strlen(l) > 1) levelFile = std::fopen(l, "ab");  // SNAP_AUDIO_LEVELS=<path> logs to a file
  if (const char* d = std::getenv("SNAP_AUDIO_DUMP"); d && *d) dump = std::fopen(d, "wb");
}

Globals& G() {
  static Globals g;
  return g;
}

void default_cfg(SnapAudioConfig* c) {
  c->backend = SNAP_AUDIO_BACKEND_AUTO;
  c->channelMode = SNAP_AUDIO_CH_AUTO;
  c->outputMode = -1;
  c->speakerChannels = 0;
  c->sampleRate = 48000;
  c->deviceBufferMs = 0;
  c->enableTap = 0;
}

void log_underrun(uint64_t n, uint32_t missing) {
  if (n <= 20 || (n % 100) == 0) std::fprintf(stderr, "[audio] underrun #%llu: %u frames short\n", (unsigned long long)n, missing);
}


// ---- capture (env SNAP_AUDIO_CAPTURE=<dir>) ----------------------------------------------------------------------------
void wav_header(FILE* f, uint32_t ch, uint32_t rate, uint64_t frames) {
  const uint32_t data = (uint32_t)(frames * ch * 2), byteRate = rate * ch * 2;
  const uint16_t blk = (uint16_t)(ch * 2), bits = 16, fmt = 1, chs = (uint16_t)ch;
  const uint32_t riff = 36 + data, fmtLen = 16;
  std::fseek(f, 0, SEEK_SET);
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmtLen, 4, 1, f); std::fwrite(&fmt, 2, 1, f); std::fwrite(&chs, 2, 1, f);
  std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&blk, 2, 1, f); std::fwrite(&bits, 2, 1, f);
  std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
  std::fseek(f, 0, SEEK_END);
}
const char* const kChName[8] = {"FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"};
void log_rms(const char* tag, double* sum, uint64_t& n, uint32_t ch) {
  if (!n) return;
  std::fprintf(stderr, "[audio-ch] %s %uch rms:", tag, ch);
  for (uint32_t c = 0; c < ch && c < 8; ++c) std::fprintf(stderr, " %s=%.5f", ch == 2 ? (c ? "R" : "L") : kChName[c], std::sqrt(sum[c] / (double)n));
  std::fprintf(stderr, "\n");
  for (uint32_t c = 0; c < 8; ++c) sum[c] = 0;
  n = 0;
}
// Guest PCM (what Wwise produced, before any downmix): per-channel RMS plus a WAV starting at the first non-silent frame.
void capture_guest(const int16_t* p, uint32_t sc, uint32_t frames) {
  Globals& g = G();
  if (sc > 8) return;
  if (!g.capGuest) {
    bool loud = false;
    for (uint32_t i = 0; i < frames * sc && !loud; ++i) loud = std::abs(p[i]) > 8;
    if (!loud) return;
    std::string path = g.capDir + "\\guest_" + std::to_string(sc) + "ch.wav";
    g.capGuest = std::fopen(path.c_str(), "wb+");
    if (!g.capGuest) return;
    g.capGuestCh = sc;
    wav_header(g.capGuest, sc, 48000, 0);
  }
  if (g.capGuestFrames < (uint64_t)g.capSecs * 48000) {
    std::fwrite(p, 2 * sc, frames, g.capGuest);
    g.capGuestFrames += frames;
    wav_header(g.capGuest, sc, 48000, g.capGuestFrames);
  }
  for (uint32_t i = 0; i < frames; ++i)
    for (uint32_t c = 0; c < sc; ++c) { const double v = p[i * sc + c] / 32768.0; g.gSum[c] += v * v; }
  g.gN += frames;
  if (g.gN >= 5 * 48000ull) log_rms("guest", g.gSum, g.gN, sc);
}
void capture_device(const float* d, uint32_t dc, uint32_t frames) {
  Globals& g = G();
  if (dc > 8) return;
  if (!g.capDev) {
    bool loud = false;
    for (uint32_t i = 0; i < frames * dc && !loud; ++i) loud = std::fabs(d[i]) > 2.5e-4f;
    if (!loud) return;
    std::string path = g.capDir + "\\device_" + std::string(mode_name(g.activeMode.load())) + "_" + std::to_string(dc) + "ch.wav";
    g.capDev = std::fopen(path.c_str(), "wb+");
    if (!g.capDev) return;
    g.capDevCh = dc;
    wav_header(g.capDev, dc, 48000, 0);
  }
  if (g.capDevCh == dc && g.capDevFrames < (uint64_t)g.capSecs * 48000) {
    std::vector<int16_t> o((size_t)frames * dc);
    for (size_t i = 0; i < o.size(); ++i) o[i] = (int16_t)std::lrintf(std::clamp(d[i], -1.0f, 1.0f) * 32767.0f);
    std::fwrite(o.data(), 2, o.size(), g.capDev);
    g.capDevFrames += frames;
    wav_header(g.capDev, dc, 48000, g.capDevFrames);
  }
  for (uint32_t i = 0; i < frames; ++i)
    for (uint32_t c = 0; c < dc; ++c) { const double v = d[i * dc + c]; g.dSum[c] += v * v; }
  g.dN += frames;
  if (g.dN >= 5 * 48000ull) log_rms("device", g.dSum, g.dN, dc);
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
    if (G().capDir.size()) capture_guest(p, sc, take);
    snap_audio::mix::accumulate_s16(d, dc, p, sc, take);
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
  // Output mode: env SNAP_AUDIO_OUTPUT > SnapAudioConfig.outputMode > audio.json "output" > stereo.
  int mode = g.cfg.outputMode >= 0 ? g.cfg.outputMode : g.cfgOutMode;
  if (const char* e = std::getenv("SNAP_AUDIO_OUTPUT"); e && parse_mode(e) >= 0) mode = parse_mode(e);
  // Channels the game is told to produce: env SNAP_AUDIO_CHANNELS (2|6) > explicit cfg.channelMode > audio.json
  // "channels" > auto (6 for speakers and spatial, 2 for stereo).
  uint32_t gameCh = mode == SNAP_AUDIO_OUT_STEREO ? 2 : 6;
  if (g.cfg.channelMode == SNAP_AUDIO_CH_STEREO) gameCh = 2;
  else if (g.cfg.channelMode == SNAP_AUDIO_CH_SURROUND51) gameCh = 6;
  else if (g.cfgGameCh) gameCh = (uint32_t)g.cfgGameCh;
  if (const char* e = std::getenv("SNAP_AUDIO_CHANNELS"); e && (std::atoi(e) == 2 || std::atoi(e) == 6)) gameCh = (uint32_t)std::atoi(e);
  uint32_t ch = 2;  // device mix channels
  if (mode == SNAP_AUDIO_OUT_SPATIAL) ch = 6;
  else if (mode == SNAP_AUDIO_OUT_SPEAKERS) {
    int spk = g.cfg.speakerChannels ? g.cfg.speakerChannels : g.cfgSpk;
    if (const char* e = std::getenv("SNAP_AUDIO_SPEAKERS"); e && (std::atoi(e) == 6 || std::atoi(e) == 8)) spk = std::atoi(e);
    ch = spk == 6 || spk == 8 ? (uint32_t)spk : (g.backend->native_channels() >= 8 ? 8u : 6u);
  }
  if (const char* d = std::getenv("SNAP_AUDIO_CAPTURE"); d && *d) {
    g.capDir = d;
    CreateDirectoryA(d, nullptr);
    if (const char* n = std::getenv("SNAP_AUDIO_CAPTURE_SECS"); n && std::atoi(n) > 0) g.capSecs = (uint32_t)std::atoi(n);
  }
  if (const char* e = std::getenv("SNAP_AUDIO_UPMIX"); e && *e) g.upmixOn = std::atoi(e) != 0;
  g.activeMode.store(mode);
  g.guestCh.store(gameCh);
  std::fprintf(stderr, "[audio] output=%s, game channels=%u, device mix channels=%u\n", mode_name(mode), gameCh, ch);
  g.devFmt = DeviceFormat{g.cfg.sampleRate ? g.cfg.sampleRate : 48000u, ch, g.cfg.deviceBufferMs, mode};
  g.reportedChannels.store((int)gameCh);
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
  if (g.upmixOn && dc >= 6) g.upmix.process(dst, dc, frames);
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
  if (g.capDir.size()) capture_device(dst, dc, frames);
  if (g.tapOn.load(std::memory_order_relaxed)) g.tap.write(dst, (size_t)frames * dc);
  if (g.levelLog || g.dump) {
    const size_t ns = (size_t)frames * dc;
    if (g.dump) {  // final mix as raw interleaved s16 (rate/channels of the device stream)
      std::vector<int16_t> o(ns);
      for (size_t i = 0; i < ns; ++i) o[i] = (int16_t)std::lrintf(dst[i] * 32767.0f);
      std::fwrite(o.data(), 2, ns, g.dump);
    }
    for (size_t i = 0; i < ns; ++i) { g.sumsq += (double)dst[i] * dst[i]; g.peak = std::max(g.peak, std::fabs(dst[i])); }
    g.nacc += frames;
    if (g.nacc >= 48000) {
      std::fprintf(g.levelFile ? g.levelFile : stderr, "[audio-level] t=%llus rms=%.5f peak=%.5f\n", (unsigned long long)++g.secs, std::sqrt(g.sumsq / (double)(g.nacc * dc)), g.peak);
      if (g.levelFile) std::fflush(g.levelFile);
      g.sumsq = 0; g.peak = 0; g.nacc = 0;
    }
  }
}

void core_notify_device_change() {
  Globals& g = G();
  g.deviceChanges.fetch_add(1, std::memory_order_relaxed);
  void* ev = g.notifyEvent.load();
  SnapAudioSignalEventFn fn = g.signalFn.load();
  if (ev && fn) fn(ev);
}

void core_set_device_info(bool attached, uint32_t bufferFrames, double latencyMs, uint32_t channels, uint32_t rate, int mode) {
  Globals& g = G();
  g.attached.store(attached ? 1 : 0);
  g.bufFrames.store(bufferFrames);
  g.latencyMs.store(latencyMs);
  (void)rate;  // the rate is fixed at start(); WASAPI converts to the endpoint
  // A backend that fell back (spatial -> speakers -> stereo) reports the channel count it really opened. Only the
  // device thread calls core_mix, and the backend calls this from that thread, so the change is race free.
  if (channels && channels != g.devChannels.load()) {
    std::fprintf(stderr, "[audio] device mix is now %u channels, mode %s\n", channels, mode_name(mode));
    g.devChannels.store(channels);
  }
  g.activeMode.store(mode);
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
  if (std::getenv("SNAP_AUDIO_TRACE")) std::fprintf(stderr, "[audio-trace] OpenDefaultAudioOut rate=%u requested channels=%u -> %u (sink cfg dword 0x%08x, +0x24 %u)\n", rate, (unsigned)p->channelCount, ch, *(const uint32_t*)((const char*)audioOut - 0x40 + 0xc), *(const uint32_t*)((const char*)audioOut - 0x40 + 0x24));
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
  if (std::getenv("SNAP_AUDIO_TRACE")) std::fprintf(stderr, "[audio-trace] GetActiveAudioDeviceChannelCountForOutput -> %d\n", g.reportedChannels.load());
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
  if (c < 0 || c >= SNAP_VOL_COUNT || v != v) return;
  Globals& g = G();
  g.vol[c].store(std::clamp(v, 0.0f, 1.0f));
  if (c > 0) g.bgmSeSet.fetch_or(1u << c);
  g.volGen.fetch_add(1);
  save_volumes(g);
}
SNAP_AUDIO_API uint32_t snap_audio_volume_generation(void) { return G().volGen.load(); }
// 1 once category c (BGM/SE/VOICE) has ever been below 1.0 this run or in the config; lets the guest bridge leave the
// original mix completely untouched until then.
SNAP_AUDIO_API int snap_audio_volume_touched(int c) { return (c > 0 && c < SNAP_VOL_COUNT) ? (int)((G().bgmSeSet.load() >> c) & 1) : 0; }
SNAP_AUDIO_API float snap_audio_get_volume(int c) { return (c >= 0 && c < SNAP_VOL_COUNT) ? G().vol[c].load() : 0.0f; }
SNAP_AUDIO_API void snap_audio_set_output(int outputMode, int gameChannels, int speakerChannels) {
  Globals& g = G();
  if (outputMode >= 0 && outputMode <= 2) g.cfgOutMode = outputMode;
  if (gameChannels == 0 || gameChannels == 2 || gameChannels == 6) g.cfgGameCh = gameChannels;
  if (speakerChannels == 0 || speakerChannels == 6 || speakerChannels == 8) g.cfgSpk = speakerChannels;
  save_volumes(g);
}
SNAP_AUDIO_API int snap_audio_get_output_mode(void) {
  Globals& g = G();
  if (const char* e = std::getenv("SNAP_AUDIO_OUTPUT"); e && parse_mode(e) >= 0) return parse_mode(e);
  return g.cfg.outputMode >= 0 && g.inited ? g.cfg.outputMode : g.cfgOutMode;
}
SNAP_AUDIO_API int snap_audio_game_channels(void) { ensure_init(); return (int)G().guestCh.load(); }
SNAP_AUDIO_API int snap_audio_active_output_mode(void) { return G().activeMode.load(); }
SNAP_AUDIO_API int snap_audio_spatial_available(void) {
  ensure_init();
  Globals& g = G();
  return g.backend ? g.backend->spatial_available() : 0;
}
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
  s->outputModeActive = g.activeMode; s->guestChannels = g.guestCh;
}
SNAP_AUDIO_API void snap_audio_reset_stats(void) {
  Globals& g = G();
  g.framesMixed = 0; g.framesFromGuest = 0; g.underruns = 0; g.underrunFrames = 0; g.appended = 0; g.releasedCount = 0;
}
SNAP_AUDIO_API uint64_t snap_audio_tap_read(float* dst, uint64_t n) { return G().tap.read(dst, (size_t)n); }
SNAP_AUDIO_API uint32_t snap_audio_tap_channels(void) { return G().devChannels; }
