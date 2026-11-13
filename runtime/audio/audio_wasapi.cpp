// SPDX-License-Identifier: MIT
// WASAPI shared-mode, event-driven render backend (Windows). Follows the default render endpoint: when the default
// device changes, or the current one is invalidated or unplugged, the stream is rebuilt on the new default device;
// while no endpoint exists a timer keeps consuming at stream rate so the guest never stalls.
#if defined(_WIN32)
#include "audio_internal.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <SpatialAudioClient.h>
#include <propidl.h>
#include "audio_mix.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

namespace snap_audio {
namespace {

template <class T> void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

class Notify : public IMMNotificationClient {
 public:
  explicit Notify(HANDLE ev) : ev_(ev) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override { ULONG r = --ref_; if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** o) override {
    if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient)) { *o = this; AddRef(); return S_OK; }
    *o = nullptr; return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == eRender && role == eConsole) SetEvent(ev_);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  // Only the render endpoint in use matters (a USB mic, a headset's capture side or an idle second card must not rebuild the stream and
  // signal the game). With no endpoint open (cur_ empty) any render device becoming active is worth a retry.
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
    std::lock_guard<std::mutex> g(mu_);
    if (!id) return S_OK;
    if (!cur_.empty()) { if (cur_ == id) SetEvent(ev_); }
    else if (state == DEVICE_STATE_ACTIVE) SetEvent(ev_);
    return S_OK;
  }
  void set_current(const wchar_t* id) { std::lock_guard<std::mutex> g(mu_); cur_ = id ? id : L""; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
 private:
  std::atomic<ULONG> ref_{1};
  HANDLE ev_;
  std::mutex mu_;
  std::wstring cur_;   // id of the render endpoint the stream is built on
};

void make_format(WAVEFORMATEXTENSIBLE& w, uint32_t rate, uint32_t ch) {
  ZeroMemory(&w, sizeof(w));
  w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  w.Format.nChannels = (WORD)ch;
  w.Format.nSamplesPerSec = rate;
  w.Format.wBitsPerSample = 32;
  w.Format.nBlockAlign = (WORD)(ch * 4);
  w.Format.nAvgBytesPerSec = rate * ch * 4;
  w.Format.cbSize = 22;
  w.Samples.wValidBitsPerSample = 32;
  w.dwChannelMask = ch == 8 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)
          : ch == 6 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
                            : (ch == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT));
  w.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

class WasapiBackend : public Backend {
 public:
  bool start(const DeviceFormat& f) override {
    fmt_ = f;
    // Probe once so a machine with no usable COM or enumerator falls back to the timer backend.
    IMMDeviceEnumerator* e = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    if (FAILED(hr)) {
      if (hr == CO_E_NOTINITIALIZED) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
      }
      if (FAILED(hr)) return false;
    }
    e->Release();
    stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    run_.store(true);
    th_ = std::thread([this] { thread_main(); });
    return true;
  }
  void stop() override {
    run_.store(false);
    if (stopEv_) SetEvent(stopEv_);
    if (th_.joinable()) th_.join();
    if (stopEv_) { CloseHandle(stopEv_); stopEv_ = nullptr; }
  }
  int kind() const override { return SNAP_AUDIO_BACKEND_WASAPI; }
  int spatial_available() override {
    HRESULT hi = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int ok = 0;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
      ok = probe_spatial(dev, 48000, nullptr) ? 1 : 0;
    }
    safe_release(dev); safe_release(en);
    if (SUCCEEDED(hi)) CoUninitialize();
    return ok;
  }
  uint32_t native_channels() override {
    HRESULT hi = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uint32_t ch = 2;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    WAVEFORMATEX* mf = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
        SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) && SUCCEEDED(ac->GetMixFormat(&mf))) {
      ch = mf->nChannels;
    }
    if (mf) CoTaskMemFree(mf);
    safe_release(ac); safe_release(dev); safe_release(en);
    if (SUCCEEDED(hi)) CoUninitialize();
    return ch;
  }

 private:
  struct Stream {
    IAudioClient* ac = nullptr;
    IAudioRenderClient* rc = nullptr;
    HANDLE ev = nullptr;
    UINT32 frames = 0;
    ~Stream() {
      if (ac) ac->Stop();
      safe_release(rc); safe_release(ac);
      if (ev) CloseHandle(ev);
    }
  };


  // ---- Windows spatial audio: static bed objects (Windows Sonic, Dolby Atmos for Headphones, DTS) ---------------------
  struct Spatial {
    ISpatialAudioClient* sac = nullptr;
    ISpatialAudioObjectRenderStream* st = nullptr;
    ISpatialAudioObject* obj[6] = {};
    HANDLE ev = nullptr;
    bool started = false;
    ~Spatial() {
      if (st && started) st->Stop();
      for (auto& o : obj) safe_release(o);
      safe_release(st); safe_release(sac);
      if (ev) CloseHandle(ev);
    }
  };

  static AudioObjectType bed_type(mix::BedObject b) {
    switch (b) {
      case mix::BedObject::FrontLeft: return AudioObjectType_FrontLeft;
      case mix::BedObject::FrontRight: return AudioObjectType_FrontRight;
      case mix::BedObject::FrontCenter: return AudioObjectType_FrontCenter;
      case mix::BedObject::LowFrequency: return AudioObjectType_LowFrequency;
      case mix::BedObject::SideLeft: return AudioObjectType_SideLeft;
      default: return AudioObjectType_SideRight;
    }
  }

  static void spatial_format(WAVEFORMATEX& f, uint32_t rate) {
    ZeroMemory(&f, sizeof(f));
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; f.nChannels = 1; f.nSamplesPerSec = rate; f.wBitsPerSample = 32;
    f.nBlockAlign = 4; f.nAvgBytesPerSec = rate * 4; f.cbSize = 0;
  }

  static AudioObjectType bed_mask() {
    unsigned m = 0;
    for (auto b : mix::kBed51) m |= (unsigned)bed_type(b);
    return (AudioObjectType)m;
  }

  // Probe: activate the client and ask whether the 6 bed objects are available. Nothing is started.
  static bool probe_spatial(IMMDevice* dev, uint32_t rate, ISpatialAudioClient** out) {
    ISpatialAudioClient* sac = nullptr;
    if (FAILED(dev->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr, (void**)&sac)) || !sac) return false;
    WAVEFORMATEX f; spatial_format(f, rate);
    bool ok = SUCCEEDED(sac->IsAudioObjectFormatSupported(&f));
    AudioObjectType native = AudioObjectType_None;
    if (ok) ok = SUCCEEDED(sac->GetNativeStaticObjectTypeMask(&native));
    if (ok) ok = (native & bed_mask()) == bed_mask();
    if (ok && out) { *out = sac; return true; }
    sac->Release();
    return ok;
  }

  bool open_spatial(IMMDeviceEnumerator* en, Spatial& sp) {
    IMMDevice* dev = nullptr;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return false;
    bool ok = probe_spatial(dev, fmt_.rate, &sp.sac);
    safe_release(dev);
    if (!ok) return false;
    WAVEFORMATEX f; spatial_format(f, fmt_.rate);
    sp.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SpatialAudioObjectRenderStreamActivationParams ap = {};
    ap.ObjectFormat = &f;
    ap.StaticObjectTypeMask = bed_mask();
    ap.MinDynamicObjectCount = 0;
    ap.MaxDynamicObjectCount = 0;
    ap.Category = AudioCategory_GameMedia;
    ap.EventHandle = sp.ev;
    ap.NotifyObject = nullptr;
    PROPVARIANT pv; PropVariantInit(&pv);
    pv.vt = VT_BLOB; pv.blob.cbSize = sizeof(ap); pv.blob.pBlobData = (BYTE*)&ap;
    HRESULT hr = sp.sac->ActivateSpatialAudioStream(&pv, IID_PPV_ARGS(&sp.st));
    if (FAILED(hr) || !sp.st) { std::fprintf(stderr, "[audio] spatial stream activation failed 0x%08lx\n", (unsigned long)hr); return false; }
    for (int i = 0; i < 6; ++i)
      if (FAILED(sp.st->ActivateSpatialAudioObject(bed_type(mix::kBed51[i]), &sp.obj[i]))) return false;
    if (FAILED(sp.st->Start())) return false;
    sp.started = true;
    return true;
  }

  // Returns false if the stream broke (rebuild), true if stopped.
  bool run_spatial(Spatial& sp, HANDLE changeEv) {
    HANDLE h[3] = {sp.ev, changeEv, stopEv_};
    std::vector<float> mixbuf;
    bool first = true;
    while (run_.load()) {
      DWORD w = WaitForMultipleObjects(3, h, FALSE, 250);
      if (w == WAIT_OBJECT_0 + 2) return true;
      if (w == WAIT_OBJECT_0 + 1) return false;
      if (w == WAIT_TIMEOUT) continue;
      UINT32 dyn = 0, frames = 0;
      HRESULT hr = sp.st->BeginUpdatingAudioObjects(&dyn, &frames);
      if (FAILED(hr)) return false;
      mixbuf.resize((size_t)frames * 6);
      core_mix(mixbuf.data(), frames);
      for (int c = 0; c < 6; ++c) {
        BYTE* buf = nullptr; UINT32 bytes = 0;
        if (SUCCEEDED(sp.obj[c]->GetBuffer(&buf, &bytes)) && buf) {
          float* o = reinterpret_cast<float*>(buf);
          const UINT32 n = std::min<UINT32>(frames, bytes / 4);
          for (UINT32 i = 0; i < n; ++i) o[i] = mixbuf[(size_t)i * 6 + c];
        }
      }
      hr = sp.st->EndUpdatingAudioObjects();
      if (FAILED(hr)) return false;
      if (first) { first = false; core_set_device_info(true, frames, 1000.0 * frames / fmt_.rate * 2.0, 6, fmt_.rate, SNAP_AUDIO_OUT_SPATIAL); }
    }
    return true;
  }

  bool open_stream(IMMDeviceEnumerator* en, Stream& s, uint32_t ch) {
    IMMDevice* dev = nullptr;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return false;
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&s.ac);
    safe_release(dev);
    if (FAILED(hr)) return false;
    WAVEFORMATEXTENSIBLE wf;
    make_format(wf, fmt_.rate, ch);
    s.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    bool done = false, triedLowLat = false;
    lowLat_ = false;
    if (!std::getenv("SNAP_AUDIO_NO_LOWLAT") && fmt_.bufferMs == 0) {
      IAudioClient3* a3 = nullptr;
      if (SUCCEEDED(s.ac->QueryInterface(IID_PPV_ARGS(&a3)))) {
        triedLowLat = true;
        UINT32 defP = 0, fund = 0, minP = 0, maxP = 0;
        if (SUCCEEDED(a3->GetSharedModeEnginePeriod(&wf.Format, &defP, &fund, &minP, &maxP)) &&
            SUCCEEDED(a3->InitializeSharedAudioStream(flags, minP, &wf.Format, nullptr))) {
          done = true; lowLat_ = true;
        }
        a3->Release();
      }
    }
    if (!done) {
      if (triedLowLat) {   // a client whose InitializeSharedAudioStream failed cannot be initialized again: take a fresh one
        safe_release(s.ac);
        IMMDevice* d2 = nullptr;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d2))) return false;
        hr = d2->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&s.ac);
        safe_release(d2);
        if (FAILED(hr)) return false;
      }
      const REFERENCE_TIME dur = fmt_.bufferMs ? (REFERENCE_TIME)fmt_.bufferMs * 10000 : 0;
      hr = s.ac->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, &wf.Format, nullptr);
      if (FAILED(hr)) {
        std::fprintf(stderr, "[audio] WASAPI Initialize failed 0x%08lx\n", (unsigned long)hr);
        return false;
      }
    }
    if (FAILED(s.ac->SetEventHandle(s.ev)) || FAILED(s.ac->GetBufferSize(&s.frames)) ||
        FAILED(s.ac->GetService(IID_PPV_ARGS(&s.rc))))
      return false;
    // prefill with silence, then start
    BYTE* p = nullptr;
    if (SUCCEEDED(s.rc->GetBuffer(s.frames, &p))) s.rc->ReleaseBuffer(s.frames, AUDCLNT_BUFFERFLAGS_SILENT);
    REFERENCE_TIME lat = 0;
    s.ac->GetStreamLatency(&lat);
    const double bufMs = 1000.0 * s.frames / fmt_.rate;
    core_set_device_info(true, s.frames, lat / 10000.0 + bufMs, ch, fmt_.rate, ch == 2 ? SNAP_AUDIO_OUT_STEREO : SNAP_AUDIO_OUT_SPEAKERS);
    return SUCCEEDED(s.ac->Start());
  }

  void timer_wait(std::chrono::milliseconds total) {  // consume at stream rate while there is no device
    using clk = std::chrono::steady_clock;
    const uint32_t block = fmt_.rate / 100;
    std::vector<float> buf((size_t)block * 8);
    const auto period = std::chrono::nanoseconds(1000000000ll * block / fmt_.rate);
    auto next = clk::now() + period;
    const auto end = clk::now() + total;
    while (run_.load() && clk::now() < end) {
      core_mix(buf.data(), block);   // the core's current channel count never exceeds 8
      std::this_thread::sleep_until(next);
      next += period;
    }
  }

  void thread_main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    HANDLE changeEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    Notify* nc = new Notify(changeEv);
    if (en) en->RegisterEndpointNotificationCallback(nc);
    bool first = true;
    while (run_.load()) {
      Stream s;
      Spatial sp;
      // Mode ladder: the configured mode first, then speakers 5.1, then stereo. Re-tried from the top on every rebuild.
      int mode = fmt_.mode;
      uint32_t ch = fmt_.channels;
      bool ok = false, spatialOpen = false;
      {  // remember which endpoint this build uses (device-state notifications for other endpoints are ignored)
        IMMDevice* cd = nullptr; LPWSTR cid = nullptr;
        if (en && SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &cd)) && SUCCEEDED(cd->GetId(&cid))) nc->set_current(cid);
        else nc->set_current(nullptr);
        if (cid) CoTaskMemFree(cid);
        safe_release(cd);
      }
      if (en && mode == SNAP_AUDIO_OUT_SPATIAL) {
        if (open_spatial(en, sp)) { ok = true; spatialOpen = true; }
        else {
          std::fprintf(stderr, "[audio] spatial audio unavailable on the default device (enable Windows Sonic or Dolby Atmos for it), falling back to 5.1 speakers\n");
          sp.~Spatial(); new (&sp) Spatial();
          mode = SNAP_AUDIO_OUT_SPEAKERS; ch = 6;
        }
      }
      if (en && !ok && mode == SNAP_AUDIO_OUT_SPEAKERS) {
        if (open_stream(en, s, ch)) ok = true;
        else { std::fprintf(stderr, "[audio] %u-channel stream failed, falling back to stereo\n", ch); s.~Stream(); new (&s) Stream(); mode = SNAP_AUDIO_OUT_STEREO; ch = 2; }
      }
      if (en && !ok) { ok = open_stream(en, s, 2); ch = 2; mode = SNAP_AUDIO_OUT_STEREO; }
      if (!ok) {
        core_set_device_info(false, 0, 0.0, 0, fmt_.rate, mode);
        timer_wait(std::chrono::milliseconds(500));  // no endpoint: keep time, retry
        continue;
      }
      if (!first) core_notify_device_change();
      first = false;
      if (spatialOpen) {
        std::fprintf(stderr, "[audio] spatial audio stream: 6 static bed objects (FL FR FC LFE SL SR)\n");
        core_set_device_info(true, 480, 20.0, 6, fmt_.rate, SNAP_AUDIO_OUT_SPATIAL);
        if (!run_spatial(sp, changeEv)) core_set_device_info(false, 0, 0.0, 0, fmt_.rate, SNAP_AUDIO_OUT_SPATIAL);
        continue;
      }
      std::fprintf(stderr, "[audio] WASAPI stream: %u ch, %u frames (%.1f ms)%s\n", ch, s.frames, 1000.0 * s.frames / fmt_.rate,
                   lowLat_ ? " low-latency" : "");
      HANDLE h[3] = {s.ev, changeEv, stopEv_};
      bool rebuild = false;
      while (run_.load() && !rebuild) {
        DWORD w = WaitForMultipleObjects(3, h, FALSE, 250);
        if (w == WAIT_OBJECT_0 + 2) break;
        if (w == WAIT_OBJECT_0 + 1) { rebuild = true; break; }
        UINT32 pad = 0;
        HRESULT hr = s.ac->GetCurrentPadding(&pad);
        if (FAILED(hr)) { rebuild = true; break; }
        const UINT32 avail = s.frames - pad;
        if (!avail) continue;
        BYTE* p = nullptr;
        hr = s.rc->GetBuffer(avail, &p);
        if (FAILED(hr)) { rebuild = true; break; }  // AUDCLNT_E_DEVICE_INVALIDATED etc.
        core_mix(reinterpret_cast<float*>(p), avail);
        s.rc->ReleaseBuffer(avail, 0);
      }
      if (rebuild) core_set_device_info(false, 0, 0.0, 0, fmt_.rate, mode);
    }
    if (en) { en->UnregisterEndpointNotificationCallback(nc); en->Release(); }
    nc->Release();
    CloseHandle(changeEv);
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoUninitialize();
  }

  DeviceFormat fmt_{};
  bool lowLat_ = false;
  std::atomic<bool> run_{false};
  HANDLE stopEv_ = nullptr;
  std::thread th_;
};

}  // namespace

Backend* make_wasapi_backend() { return new WasapiBackend(); }

}  // namespace snap_audio
#endif
