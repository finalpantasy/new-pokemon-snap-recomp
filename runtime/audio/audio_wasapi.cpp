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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { SetEvent(ev_); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
 private:
  std::atomic<ULONG> ref_{1};
  HANDLE ev_;
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
  w.dwChannelMask = ch == 6 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
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

  bool open_stream(IMMDeviceEnumerator* en, Stream& s) {
    IMMDevice* dev = nullptr;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return false;
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&s.ac);
    safe_release(dev);
    if (FAILED(hr)) return false;
    WAVEFORMATEXTENSIBLE wf;
    make_format(wf, fmt_.rate, fmt_.channels);
    s.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    bool done = false;
    lowLat_ = false;
    if (!std::getenv("SNAP_AUDIO_NO_LOWLAT") && fmt_.bufferMs == 0) {
      IAudioClient3* a3 = nullptr;
      if (SUCCEEDED(s.ac->QueryInterface(IID_PPV_ARGS(&a3)))) {
        UINT32 defP = 0, fund = 0, minP = 0, maxP = 0;
        if (SUCCEEDED(a3->GetSharedModeEnginePeriod(&wf.Format, &defP, &fund, &minP, &maxP)) &&
            SUCCEEDED(a3->InitializeSharedAudioStream(flags, minP, &wf.Format, nullptr))) {
          done = true; lowLat_ = true;
        }
        a3->Release();
      }
    }
    if (!done) {
      const REFERENCE_TIME dur = fmt_.bufferMs ? (REFERENCE_TIME)fmt_.bufferMs * 10000 : 0;
      hr = s.ac->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, &wf.Format, nullptr);
      if (FAILED(hr)) {
        // A failed Initialize leaves the client unusable; reactivate once without the event mode being the issue.
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
    core_set_device_info(true, s.frames, lat / 10000.0 + bufMs, fmt_.channels, fmt_.rate);
    return SUCCEEDED(s.ac->Start());
  }

  void timer_wait(std::chrono::milliseconds total) {  // consume at stream rate while there is no device
    using clk = std::chrono::steady_clock;
    const uint32_t block = fmt_.rate / 100;
    std::vector<float> buf((size_t)block * fmt_.channels);
    const auto period = std::chrono::nanoseconds(1000000000ll * block / fmt_.rate);
    auto next = clk::now() + period;
    const auto end = clk::now() + total;
    while (run_.load() && clk::now() < end) {
      core_mix(buf.data(), block);
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
      if (!en || !open_stream(en, s)) {
        core_set_device_info(false, 0, 0.0, fmt_.channels, fmt_.rate);
        timer_wait(std::chrono::milliseconds(500));  // no endpoint: keep time, retry
        continue;
      }
      if (!first) core_notify_device_change();
      first = false;
      std::fprintf(stderr, "[audio] WASAPI stream: %u frames (%.1f ms)%s\n", s.frames, 1000.0 * s.frames / fmt_.rate,
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
      if (rebuild) core_set_device_info(false, 0, 0.0, fmt_.channels, fmt_.rate);
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
