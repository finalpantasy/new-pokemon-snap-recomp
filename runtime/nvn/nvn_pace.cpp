// Frame pacing: one frame clock for the host limiter and the guest vsync event. See nvn_pace.h and analysis/perf/pacing.md.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include "nvn_pace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace snap_pace {
namespace {

int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool EnvZero(const char* n) { const char* e = getenv(n); return e && e[0] == '0'; }
const bool kLegacy = [] { const char* e = getenv("SNAP_PACE"); return e && !_stricmp(e, "legacy"); }();
const char* const kLogPath = getenv("SNAP_PACE_LOG");
const int64_t kT0 = NowNs();   // free-grid anchor (shared by the limiter and the guest vsync)

// Sleep until t on the steady clock: high-resolution waitable timer, then at most spinNs of spinning. Without that timer
// (pre-1803 Windows) Sleep(1) with a 1 ms system timer period.
void WaitUntil(int64_t t, int64_t spinNs) {
  thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  static std::once_flag period;
  const int64_t margin = std::max<int64_t>(spinNs, 100000);
  for (;;) {
    const int64_t left = t - NowNs();
    if (left <= margin) break;
    if (timer) {
      LARGE_INTEGER due; due.QuadPart = -(left - margin) / 100;
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, INFINITE);
    } else {
      std::call_once(period, [] { timeBeginPeriod(1); });
      if (left < 1500000 + margin) break;
      Sleep(1);
    }
  }
  if (spinNs > 0) while (NowNs() < t) YieldProcessor();
}

// ---- display: monitor of the window and its exact refresh rate
double QueryHz(const wchar_t* gdiName) {
  UINT32 np = 0, nm = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np) {
    std::vector<DISPLAYCONFIG_PATH_INFO> p(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, p.data(), &nm, m.data(), nullptr) == ERROR_SUCCESS) {
      for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME; sn.header.size = sizeof sn;
        sn.header.adapterId = p[i].sourceInfo.adapterId; sn.header.id = p[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || wcscmp(sn.viewGdiDeviceName, gdiName)) continue;
        const UINT32 mi = p[i].targetInfo.modeInfoIdx;
        if (mi != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && mi < nm && m[mi].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
          const auto& v = m[mi].targetMode.targetVideoSignalInfo.vSyncFreq;
          if (v.Denominator && v.Numerator) return (double)v.Numerator / v.Denominator;
        }
        const auto& r = p[i].targetInfo.refreshRate;
        if (r.Denominator && r.Numerator) return (double)r.Numerator / r.Denominator;
      }
    }
  }
  DEVMODEW dm{}; dm.dmSize = sizeof dm;
  if (EnumDisplaySettingsW(gdiName, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
    double f = dm.dmDisplayFrequency;
    if (dm.dmDisplayFrequency % 60 == 59) f = (f + 1) * 1000.0 / 1001.0;   // 59 / 119 Hz are NTSC rates
    return f;
  }
  return 0;
}

// ---- vblank clock: a thread blocked in D3DKMTWaitForVerticalBlankEvent on the window's monitor
struct KmtOpen { WCHAR DeviceName[32]; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; };
struct KmtWait { UINT hAdapter; UINT hDevice; UINT VidPnSourceId; };
struct KmtClose { UINT hAdapter; };
struct VBlank {
  std::mutex m;
  std::condition_variable cv;
  wchar_t want[32] = {};        // monitor (GDI name) to follow
  std::atomic<int64_t> tvRef{0};  // exact vblank period from QueryDisplayConfig (0 = measure)
  int64_t lastV = 0;            // phase estimate of the latest vblank (steady ns, tracks the earliest wake-ups)
  uint64_t seq = 0;             // vblank counter
  int64_t tvMeas = 0;           // measured period (filtered)
  bool ok = false;
};
VBlank g_vb;
std::once_flag g_vbOnce;
std::atomic<bool> g_vbDead{false};

void VBlankMain() {
  HMODULE g = LoadLibraryW(L"gdi32.dll");
  auto open = g ? (LONG(APIENTRY*)(KmtOpen*))GetProcAddress(g, "D3DKMTOpenAdapterFromGdiDisplayName") : nullptr;
  auto wait = g ? (LONG(APIENTRY*)(const KmtWait*))GetProcAddress(g, "D3DKMTWaitForVerticalBlankEvent") : nullptr;
  auto close = g ? (LONG(APIENTRY*)(const KmtClose*))GetProcAddress(g, "D3DKMTCloseAdapter") : nullptr;
  if (!open || !wait || !close) { fprintf(stderr, "[pace] D3DKMT vblank wait unavailable: steady-clock pacing\n"); g_vbDead = true; return; }
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  KmtOpen o{};
  wchar_t cur[32] = {};
  bool isOpen = false;
  int fails = 0;
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(g_vb.m);
      if (wcscmp(cur, g_vb.want)) {
        if (isOpen) { KmtClose c{o.hAdapter}; close(&c); isOpen = false; }
        wcscpy_s(cur, g_vb.want);
        o = KmtOpen{}; wcscpy_s(o.DeviceName, cur);
        isOpen = cur[0] && open(&o) == 0;
        g_vb.lastV = 0; g_vb.tvMeas = 0; g_vb.ok = false;
        fprintf(stderr, "[pace] vblank clock on %ls: %s\n", cur, isOpen ? "open" : "D3DKMTOpenAdapterFromGdiDisplayName failed");
      }
    }
    if (!isOpen) { Sleep(50); continue; }
    KmtWait w{o.hAdapter, 0, o.VidPnSourceId};
    const LONG st = wait(&w);
    const int64_t t = NowNs();
    if (st != 0) {
      if (++fails == 20) fprintf(stderr, "[pace] vblank wait failing (0x%lx)\n", (unsigned long)st);
      { std::lock_guard<std::mutex> lk(g_vb.m); g_vb.ok = false; }
      Sleep(fails > 20 ? 100 : 5);
      continue;
    }
    fails = 0;
    {
      std::lock_guard<std::mutex> lk(g_vb.m);
      VBlank& v = g_vb;
      const int64_t ref = v.tvRef.load();
      if (v.lastV == 0) { v.lastV = t; v.seq++; }
      else {
        const int64_t d = t - v.lastV;
        const int64_t tv = ref > 0 ? ref : (v.tvMeas > 0 ? v.tvMeas : d);
        const int64_t n = std::max<int64_t>(1, (d + tv / 2) / tv);
        const int64_t per = d / n;
        if (n <= 8 && std::llabs(per - tv) < tv / 10) {
          v.tvMeas = v.tvMeas ? v.tvMeas + (per - v.tvMeas) / 32 : per;
          const int64_t pred = v.lastV + n * (ref > 0 ? ref : v.tvMeas);
          const int64_t e = t - pred;   // wake-ups are never early, so an early sample is the better phase estimate
          v.lastV = e < 0 ? t : pred + e / 16;
        } else v.lastV = t;   // display mode change or a long stall: re-anchor
        v.seq += (uint64_t)n;
      }
      v.ok = true;
    }
    g_vb.cv.notify_all();
  }
}
void StartVBlank() {
  if (kLegacy || EnvZero("SNAP_PACE_VBLANK")) { g_vbDead = true; return; }
  std::call_once(g_vbOnce, [] { std::thread(VBlankMain).detach(); });
}
struct VbSnap { bool ok; int64_t lastV, tv; uint64_t seq; };
VbSnap Snapshot(int64_t now) {
  std::lock_guard<std::mutex> lk(g_vb.m);
  const int64_t ref = g_vb.tvRef.load();
  VbSnap s{g_vb.ok, g_vb.lastV, ref > 0 ? ref : g_vb.tvMeas, g_vb.seq};
  if (s.tv <= 0 || now - s.lastV > 250000000) s.ok = false;   // no vblank for 250 ms (display off, wait failing)
  return s;
}

// ---- clock shared with the guest vsync thread
struct Clock {
  std::atomic<int64_t> U{16666667};   // guest vsync period on the free grid
  std::atomic<int> kv{0};             // vblanks per guest vsync when locked to the display (0 = free grid)
  std::atomic<uint64_t> parity{0};    // locked: vsync on vblank seqs == parity (mod kv)
} g_clk;
std::atomic<uint64_t> g_vsTicks{0};
std::atomic<int64_t> g_vsLast{0};

// ---- present wait (VK_KHR_present_id + VK_KHR_present_wait)
VkPhysicalDevicePresentIdFeaturesKHR g_fId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
VkPhysicalDevicePresentWaitFeaturesKHR g_fWait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
bool g_pwOn = false;
VkDevice g_dev = VK_NULL_HANDLE;
PFN_vkWaitForPresentKHR g_waitPresent = nullptr;
VkSwapchainKHR g_sc = VK_NULL_HANDLE;
uint64_t g_nextId = 1, g_pendingId = 0, g_last1 = 0, g_last2 = 0;   // ids of the last two presents on g_sc
VkPresentIdKHR g_pid{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
int g_pwTimeouts = 0;
bool g_pwSuspended = false;
bool g_chained = false;

// ---- limiter state (present thread)
struct Rel {
  HMONITOR mon = nullptr;
  wchar_t name[32] = {};
  double hz = 0;
  int64_t hzAt = 0;
  int mode = 0;          // 0 none, 1 free grid, 2 locked to vblanks
  int kf = 0;            // vblanks per frame when locked
  int64_t lastIdx = 0;   // free grid index of the last release
  uint64_t lastSeq = 0;  // locked: vblank seq whose mid-point released the last frame
  int64_t lastP = 0;
  int lastLogMode = -1, lastLogK = -1;
  double lastLogHz = -1;
} g_rel;

// ---- SNAP_PACE_LOG
struct Log {
  FILE* f = nullptr;
  int64_t mk[6] = {};
  int64_t prevRelease = 0, prevEnd = 0, pwNs = 0, lateNs = 0;
  int pwTo = 0, mode = 0, kf = 0;
  double hz = 0;
  uint64_t n = 0, vsPrev = 0;
  bool open = false;
} g_log;
enum { K_PACE_BEGIN = 4, K_PACE_END = 5 };

void LogRow() {
  Log& L = g_log;
  if (!L.f) return;
  const int64_t rel = L.mk[K_PACE_END], beg = L.mk[K_PACE_BEGIN];
  auto ms = [](int64_t a, int64_t b) { return (a && b && b >= a) ? (double)(b - a) / 1e6 : 0.0; };
  const uint64_t vs = g_vsTicks.load();
  const int64_t vl = g_vsLast.load();
  fprintf(L.f, "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%llu,%.3f,%c,%d,%.3f,%.3f\n", (unsigned long long)L.n, (double)(rel - kT0) / 1e6,
          ms(L.prevRelease, rel), ms(L.prevEnd, beg), ms(beg, rel), (double)L.pwNs / 1e6, L.pwTo, ms(L.mk[M_ACQ_BEGIN], L.mk[M_ACQ_END]),
          ms(L.mk[M_QP_BEGIN], L.mk[M_QP_END]), (unsigned long long)(vs - L.vsPrev), vl && rel ? (double)(rel - vl) / 1e6 : 0.0,
          kLegacy ? 'L' : L.mode == 2 ? 'K' : L.mode == 1 ? 'F' : 'N', L.kf, L.hz, (double)L.lateNs / 1e6);
  if ((++L.n & 127) == 0) fflush(L.f);
  L.vsPrev = vs;
  L.prevRelease = rel;
  L.prevEnd = L.mk[M_QP_END] ? L.mk[M_QP_END] : rel;
  L.pwNs = 0; L.pwTo = 0; L.lateNs = 0;
  memset(L.mk, 0, sizeof L.mk);
}

void UpdateDisplay(void* hwnd, int64_t now) {
  Rel& R = g_rel;
  static const char* monEnv = getenv("SNAP_PACE_MONITOR");   // test override: monitor number (\\.\DISPLAY<n>)
  static const double hzEnv = getenv("SNAP_PACE_REFRESH") ? atof(getenv("SNAP_PACE_REFRESH")) : 0.0;
  HMONITOR m = MonitorFromWindow((HWND)hwnd, MONITOR_DEFAULTTOPRIMARY);
  if (m == R.mon && now - R.hzAt < 30000000000LL) return;
  R.mon = m;
  R.hzAt = now;
  wchar_t name[32] = {};
  if (monEnv && *monEnv) swprintf(name, 32, L"\\\\.\\DISPLAY%d", atoi(monEnv));
  else { MONITORINFOEXW mi{}; mi.cbSize = sizeof mi; if (GetMonitorInfoW(m, &mi)) wcscpy_s(name, mi.szDevice); }
  const double hz = hzEnv > 0 ? hzEnv : (name[0] ? QueryHz(name) : 0.0);
  const bool changed = wcscmp(name, R.name) != 0 || std::fabs(hz - R.hz) > 0.001;
  wcscpy_s(R.name, name);
  R.hz = hz;
  g_vb.tvRef = hz > 1 ? (int64_t)std::llround(1e9 / hz) : 0;
  { std::lock_guard<std::mutex> lk(g_vb.m); wcscpy_s(g_vb.want, name); }
  if (changed) { R.mode = 0; fprintf(stderr, "[pace] display %ls %.3f Hz\n", name, hz); }
}

// Waits for the present two frames back (queue depth cap). Never past `deadline`; after 3 timeouts in a row (minimized, hidden
// desktop, a driver that never signals) it only polls until a present completes again.
void QueueCap(VkSwapchainKHR sc, int64_t deadline) {
  if (!g_pwOn || !g_waitPresent || sc != g_sc || !g_last2) return;
  const int64_t t0 = NowNs();
  const uint64_t timeout = g_pwSuspended ? 0 : (uint64_t)std::max<int64_t>(0, deadline - t0);
  const VkResult r = g_waitPresent(g_dev, sc, g_last2, timeout);
  g_log.pwNs = NowNs() - t0;
  if (r == VK_SUCCESS) { g_pwTimeouts = 0; if (g_pwSuspended) { g_pwSuspended = false; fprintf(stderr, "[pace] present wait resumed\n"); } return; }
  if (r == VK_TIMEOUT) {
    if (!g_pwSuspended) { g_log.pwTo = 1; if (++g_pwTimeouts >= 3) { g_pwSuspended = true; fprintf(stderr, "[pace] present wait timing out: queue cap paused\n"); } }
    return;
  }
  g_pwSuspended = true;   // out of date / surface lost: the next swapchain resets this
}

}  // namespace

bool Legacy() { return kLegacy; }
bool LogOn() { return kLogPath && *kLogPath; }

void AppendDeviceExt(VkPhysicalDevice phys, std::vector<const char*>& dext, VkPhysicalDeviceVulkan11Features* f11) {
  if (kLegacy || EnvZero("SNAP_PACE_WAIT")) return;
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> ep(n);
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, ep.data());
  auto has = [&](const char* s) { for (auto& e : ep) if (!strcmp(e.extensionName, s)) return true; return false; };
  if (!has(VK_KHR_PRESENT_ID_EXTENSION_NAME) || !has(VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) { fprintf(stderr, "[pace] present wait not offered\n"); return; }
  VkPhysicalDevicePresentIdFeaturesKHR qi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
  VkPhysicalDevicePresentWaitFeaturesKHR qw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
  qi.pNext = &qw;
  VkPhysicalDeviceFeatures2 q2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &qi};
  vkGetPhysicalDeviceFeatures2(phys, &q2);
  if (!qi.presentId || !qw.presentWait) { fprintf(stderr, "[pace] present wait feature off\n"); return; }
  for (auto& d : dext) if (!strcmp(d, VK_KHR_PRESENT_ID_EXTENSION_NAME) || !strcmp(d, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) return;   // already added
  dext.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
  dext.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
  g_fId.presentId = VK_TRUE; g_fWait.presentWait = VK_TRUE;
  g_fId.pNext = &g_fWait; g_fWait.pNext = f11->pNext; f11->pNext = &g_fId;
  g_pwOn = true;
}

void DeviceCreated(VkDevice dev) {
  g_dev = dev;
  if (g_pwOn) g_waitPresent = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(dev, "vkWaitForPresentKHR");
  if (!g_waitPresent) g_pwOn = false;
  if (!kLegacy) fprintf(stderr, "[pace] model display (SNAP_PACE=legacy for the old limiter), present wait %s\n", g_pwOn ? "on" : "off");
}

void SwapchainCreated(VkSwapchainKHR sc, void*) {
  g_sc = sc;
  g_last1 = g_last2 = 0;
  g_pwTimeouts = 0;
  g_pwSuspended = false;
}

const void* PresentChain(VkSwapchainKHR sc) {
  g_chained = false;
  if (!g_pwOn || sc != g_sc) return nullptr;
  g_pendingId = g_nextId++;
  g_pid.pNext = nullptr; g_pid.swapchainCount = 1; g_pid.pPresentIds = &g_pendingId;
  g_chained = true;
  return &g_pid;
}

void Presented(VkSwapchainKHR sc, VkResult r) {
  if (!g_chained || sc != g_sc) return;
  g_chained = false;
  if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { g_last2 = g_last1; g_last1 = g_pendingId; }
}

void Stamp(Mark m) {
  if (!LogOn()) return;
  g_log.mk[m] = NowNs();
}

void Release(void* hwnd, VkSwapchainKHR sc, int64_t base, int64_t vsyncNs) {
  if (kLegacy) return;
  StartVBlank();
  Rel& R = g_rel;
  int64_t now = NowNs();
  UpdateDisplay(hwnd, now);
  const double tvD = R.hz > 1 ? 1e9 / R.hz : 0.0;
  const VbSnap vb = Snapshot(now);
  // Lock to the display when its refresh is a whole multiple of the guest vsync rate (60, 59.94, 120 Hz...).
  auto multiple = [](double x) { const int k = (int)std::llround(x); return (k >= 1 && std::fabs(x - k) <= 0.006 * k) ? k : 0; };
  // Guest vsync period: whole display vblanks when the refresh allows it (59.94 Hz -> 16.683 ms), so the free grid of the
  // vsync thread and of the limiter below stay on one clock even without the vblank thread.
  const int kvD = (tvD > 0 && vsyncNs > 0) ? multiple(vsyncNs / tvD) : 0;
  g_clk.U = kvD ? (int64_t)std::llround(kvD * tvD) : (vsyncNs > 0 ? vsyncNs : 16666667);
  const int kv = vb.ok ? kvD : 0;
  const int kf = (kv && base > 0) ? multiple(base / tvD) : 0;
  g_clk.kv = kv;
  if (base <= 0) { R.mode = 0; if (LogOn()) g_log.mode = 0; return; }
  int64_t T;
  if (kf) {
    const int64_t tv = vb.tv;
    const uint64_t sNow = vb.seq + (uint64_t)(std::max<int64_t>(0, now - vb.lastV) / tv);
    uint64_t next = R.lastSeq + (uint64_t)kf;
    // Release half a vblank after vblank `next`: the frame shows on vblank next+1 whichever side of the mid-point jitter puts it.
    T = vb.lastV + ((int64_t)next - (int64_t)vb.seq) * tv + tv / 2;
    if (R.mode != 2 || R.kf != kf || R.lastSeq == 0 || next + (uint64_t)kf <= sNow || next > sNow + 2 * (uint64_t)kf) {
      next = sNow;   // (re)start, or more than a frame behind: release now (waiting for a mid-point would only slow a late game)
      T = now;
    }
    R.lastSeq = next;
    R.mode = 2; R.kf = kf;
    g_clk.parity = next % (uint64_t)kv;   // guest vsync on vblank `next`, half a vblank before the release (legacy: ~1.3 ms before)
  } else {
    // Free grid on the steady clock. With a known whole-multiple refresh but no vblank clock, use the display's exact period.
    int64_t P = base;
    if (kvD) if (const int k = multiple(base / tvD)) P = (int64_t)std::llround(k * tvD);
    if (R.mode != 1 || R.lastP != P) R.lastIdx = (now - kT0) / P - 1;
    int64_t idx = R.lastIdx + 1;
    T = kT0 + idx * P;
    if (now - T > P) { idx = (now - kT0) / P; T = kT0 + idx * P; }   // more than a period behind: drop the schedule
    R.lastIdx = idx; R.lastP = P;
    R.mode = 1; R.kf = 0;
  }
  if (R.mode != R.lastLogMode || R.kf != R.lastLogK || R.hz != R.lastLogHz) {
    fprintf(stderr, "[pace] %s, %ls %.3f Hz, vblank clock %s, frame %.3f ms\n", R.mode == 2 ? "locked to vblank" : "steady-clock grid", R.name, R.hz,
            vb.ok ? "ok" : (g_vbDead ? "unavailable" : "not ready"), R.mode == 2 ? kf * vb.tv / 1e6 : R.lastP / 1e6);
    R.lastLogMode = R.mode; R.lastLogK = R.kf; R.lastLogHz = R.hz;
  }
  QueueCap(sc, T + (R.mode == 2 ? vb.tv / 2 : base / 2));
  now = NowNs();
  if (T > now) WaitUntil(T, 300000);
  if (LogOn()) { g_log.mode = R.mode; g_log.kf = R.kf; g_log.hz = R.hz; g_log.lateNs = std::max<int64_t>(0, now - T); }
}

}  // namespace snap_pace

// Called from nvn_vk.cpp Pace() around the limiter (both models) so SNAP_PACE_LOG has the release time.
namespace snap_pace {
void PaceMark(bool end) {
  if (!LogOn()) return;
  Log& L = g_log;
  if (!L.open) {
    L.open = true;
    L.f = fopen(kLogPath, "w");
    if (L.f) fprintf(L.f, "n,t_ms,dt_ms,work_ms,pace_ms,pw_ms,pw_to,acq_ms,qp_ms,vs_n,vs_ph_ms,mode,kf,hz,late_ms\n");
  }
  if (!end) { if (L.mk[K_PACE_END]) LogRow(); L.mk[K_PACE_BEGIN] = NowNs(); }
  else L.mk[K_PACE_END] = NowNs();
}
}  // namespace snap_pace

// ---- guest vsync (runtime/hle/services_sys.cpp, nn::vi::GetDisplayVsyncEvent)
// snap_pace_vsync_wait blocks until the next guest vsync of the shared frame clock and returns 1; returns 0 under SNAP_PACE=legacy
// (the caller keeps its own 60 Hz sleep loop). snap_pace_vsync_note counts a tick for SNAP_PACE_LOG (legacy loop).
extern "C" void snap_pace_vsync_note() {
  snap_pace::g_vsTicks.fetch_add(1);
  snap_pace::g_vsLast = snap_pace::NowNs();
}

extern "C" int snap_pace_vsync_wait(int64_t* last) {
  using namespace snap_pace;
  if (kLegacy) return 0;
  StartVBlank();
  static uint64_t lastSeq = 0;
  const int kv = g_clk.kv.load();
  bool ticked = false;
  if (kv > 0 && !g_vbDead) {
    std::unique_lock<std::mutex> lk(g_vb.m);
    const uint64_t k = (uint64_t)kv, par = g_clk.parity.load() % k;
    const uint64_t s0 = g_vb.seq;
    // Next vblank with the limiter's parity after the last tick; the current one only if the last tick was right before it.
    const uint64_t lo = (lastSeq && s0 >= lastSeq && s0 - lastSeq <= k) ? std::max(lastSeq + 1, s0) : s0 + 1;
    const uint64_t want = lo + (par + k - lo % k) % k;
    ticked = g_vb.cv.wait_for(lk, std::chrono::milliseconds(100), [&] { return g_vb.seq >= want; });
    if (ticked) lastSeq = g_vb.seq - ((g_vb.seq - want) % k);
  }
  if (!ticked) {   // free grid shared with the limiter (also the fallback when the vblank clock stops)
    const int64_t U = g_clk.U.load(), now = NowNs();
    // 1 ms before each limiter tick (the old free-running thread happened to sit ~1.3 ms before the release; ticking exactly on
    // the release made the order of the two a coin flip).
    const int64_t a = kT0 - std::min<int64_t>(1000000, U / 8);
    int64_t next = a + ((now - a) / U + 1) * U;
    if (*last && next < *last + U / 2) next += U;
    WaitUntil(next, 0);
    lastSeq = 0;
  }
  *last = NowNs();
  snap_pace_vsync_note();
  return 1;
}
