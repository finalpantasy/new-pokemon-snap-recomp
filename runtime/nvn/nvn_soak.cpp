// SNAP_SOAK_LOG=<seconds>: one "[soak]" line to stderr every <seconds> (default off). For long-session leak hunts: process
// working set / commit / handles / GDI / USER objects / threads, Vulkan object counts (made-freed, from nvn_vkcount.h), guest VAMM
// and memory-heap totals, shader cache folder size and presents per second. Cost when off: nothing (no thread).
#include <windows.h>
#include <tlhelp32.h>
#define PSAPI_VERSION 2
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "nvn_internal.h"

extern "C" void snap_os_mem_stats(uint64_t* out);

namespace {

std::string CacheDir() {
  const char* e = getenv("SNAP_SHADERCACHE_DIR");
  std::string d;
  if (e && *e) d = e;
  else if ((e = getenv("SNAP_CONFIG_DIR")) && *e) d = std::string(e) + "\\shadercache";
  else {
    char buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("APPDATA", buf, MAX_PATH);
    d = (n ? std::string(buf) : std::string(".")) + "\\SnapRecomp\\shadercache";
  }
  if (const char* r = getenv("SNAP_XLATE_REV"); r && *r && strcmp(r, "0")) d += std::string("_r") + r;
  return d;
}

uint64_t DirBytes(const std::string& d, int depth = 0) {
  uint64_t sum = 0;
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((d + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (fd.cFileName[0] == '.' && (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2]))) continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 3) sum += DirBytes(d + "\\" + fd.cFileName, depth + 1); }
    else sum += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return sum;
}

int ThreadCount() {
  int n = 0;
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (s == INVALID_HANDLE_VALUE) return -1;
  THREADENTRY32 te{}; te.dwSize = sizeof te;
  const DWORD pid = GetCurrentProcessId();
  if (Thread32First(s, &te)) do { if (te.th32OwnerProcessID == pid) n++; } while (Thread32Next(s, &te));
  CloseHandle(s);
  return n;
}

void Loop(int secs) {
  const std::string cache = CacheDir();
  const double mb = 1.0 / (1024.0 * 1024.0);
  DWORD64 t0 = GetTickCount64(), tPrev = t0;
  int prevPresent = nvn::g_presentCount;
  int idx = 0;
  for (;;) {
    Sleep((DWORD)secs * 1000);
    HANDLE p = GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS_EX pm{}; pm.cb = sizeof pm;
    K32GetProcessMemoryInfo(p, (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
    DWORD handles = 0; GetProcessHandleCount(p, &handles);
    const DWORD gdi = GetGuiResources(p, GR_GDIOBJECTS), usr = GetGuiResources(p, GR_USEROBJECTS);
    uint64_t m[6] = {}; snap_os_mem_stats(m);
    const DWORD64 now = GetTickCount64();
    const int pc = nvn::g_presentCount;
    const double dt = (double)(now - tPrev) / 1000.0;
    auto& c = snapvk::C();
    auto live = [&](int k) { return (long long)(c.made[k].load() - c.freed[k].load()); };
    fprintf(stderr,
            "[soak] #%d t=%.0fs ws=%.0fMB commit=%.0fMB peakws=%.0fMB handles=%lu gdi=%lu user=%lu threads=%d | vk live img=%lld buf=%lld mem=%lld(made %lld, %.0fMB total) "
            "dpool=%lld pipe=%lld view=%lld samp=%lld shmod=%lld | guest vamm res=%.0fMB com=%.0fMB regions=%llu heap=%.0fMB blocks=%llu(%.0fMB) | shadercache=%.1fMB | present=%d %.1f/s\n",
            ++idx, (double)(now - t0) / 1000.0, pm.WorkingSetSize * mb, pm.PrivateUsage * mb, pm.PeakWorkingSetSize * mb, (unsigned long)handles, (unsigned long)gdi,
            (unsigned long)usr, ThreadCount(), live(snapvk::kImage), live(snapvk::kBuffer), live(snapvk::kMemory), (long long)c.made[snapvk::kMemory].load(),
            (double)c.memBytesMade.load() * mb, live(snapvk::kDescPool), live(snapvk::kPipeline), live(snapvk::kImageView), live(snapvk::kSampler),
            live(snapvk::kShaderModule), m[0] * mb, m[1] * mb, (unsigned long long)m[2], m[3] * mb, (unsigned long long)m[4], m[5] * mb,
            (double)DirBytes(cache) * mb, pc, dt > 0 ? (pc - prevPresent) / dt : 0.0);
    fflush(stderr);
    prevPresent = pc; tPrev = now;
  }
}

struct Starter {
  Starter() {
    const char* e = getenv("SNAP_SOAK_LOG");
    int s = e ? atoi(e) : 0;
    if (s > 0) std::thread(Loop, s).detach();
  }
} g_starter;

}  // namespace
