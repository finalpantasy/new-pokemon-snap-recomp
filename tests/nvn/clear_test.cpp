// SPDX-License-Identifier: MIT
// Host-only test of the NVN->Vulkan layer: drives the NVN entry points in the order the game's nn::gfx
// backend uses (DeviceImpl::Initialize 0x90d590, swapchain setup 0x985790, per-frame submit/present
// 0x903da0 / 0x98cc80) and presents an animated clear color. No game data involved.
//
// Usage: clear_test.exe [--frames N] [--uncapped] [--screenshot out.bmp] [--no-validation] [--hidden]
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "nvn_host.h"

// ---- minimal NVN typedefs for the test (opaque guest objects with the sizes the game allocates) ------------
struct Obj { alignas(16) unsigned char raw[0x3000]; };
using Proc = void (*)(void);
using Boolean = unsigned char;
using Handle = unsigned long long;

#define PROC(ret, name, args) static ret(*name) args
PROC(Boolean, DeviceInitialize, (void*, const void*));
PROC(void, DeviceBuilderSetDefaults, (void*));
PROC(void, DeviceFinalize, (void*));
PROC(void, DeviceGetInteger, (const void*, int, int*));
PROC(void, DeviceSetWindowOriginMode, (void*, int));
PROC(void, DeviceSetDepthMode, (void*, int));
PROC(Boolean, DeviceRegisterFastClearColor, (void*, const float*, int));
PROC(Boolean, DeviceRegisterFastClearDepth, (void*, float));
PROC(void, MemoryPoolBuilderSetDevice, (void*, void*));
PROC(void, MemoryPoolBuilderSetDefaults, (void*));
PROC(void, MemoryPoolBuilderSetStorage, (void*, void*, size_t));
PROC(void, MemoryPoolBuilderSetFlags, (void*, int));
PROC(Boolean, MemoryPoolInitialize, (void*, const void*));
PROC(void, MemoryPoolSetDebugLabel, (void*, const char*));
PROC(void, MemoryPoolFinalize, (void*));
PROC(void*, MemoryPoolMap, (const void*));
PROC(void, MemoryPoolFlushMappedRange, (const void*, ptrdiff_t, size_t));
PROC(size_t, MemoryPoolGetSize, (const void*));
PROC(void, QueueBuilderSetDevice, (void*, void*));
PROC(void, QueueBuilderSetDefaults, (void*));
PROC(void, QueueBuilderSetFlags, (void*, int));
PROC(void, QueueBuilderSetCommandMemorySize, (void*, size_t));
PROC(void, QueueBuilderSetComputeMemorySize, (void*, size_t));
PROC(void, QueueBuilderSetControlMemorySize, (void*, size_t));
PROC(Boolean, QueueInitialize, (void*, const void*));
PROC(void, QueueFinalize, (void*));
PROC(void, QueueSubmitCommands, (void*, int, const Handle*));
PROC(void, QueueFlush, (void*));
PROC(void, QueueFinish, (void*));
PROC(void, QueuePresentTexture, (void*, void*, int));
PROC(void, QueueFenceSync, (void*, void*, int, int));
PROC(Boolean, SyncInitialize, (void*, void*));
PROC(void, SyncFinalize, (void*));
PROC(int, SyncWait, (const void*, unsigned long long));
PROC(void, WindowBuilderSetDevice, (void*, void*));
PROC(void, WindowBuilderSetDefaults, (void*));
PROC(void, WindowBuilderSetNativeWindow, (void*, void*));
PROC(void, WindowBuilderSetTextures, (void*, int, void* const*));
PROC(Boolean, WindowInitialize, (void*, const void*));
PROC(void, WindowFinalize, (void*));
PROC(int, WindowAcquireTexture, (void*, void*, int*));
PROC(void, WindowSetPresentInterval, (void*, int));
PROC(void, WindowSetCrop, (void*, int, int, int, int));
PROC(void, TextureBuilderSetDevice, (void*, void*));
PROC(void, TextureBuilderSetDefaults, (void*));
PROC(void, TextureBuilderSetFlags, (void*, int));
PROC(void, TextureBuilderSetTarget, (void*, int));
PROC(void, TextureBuilderSetSize2D, (void*, int, int));
PROC(void, TextureBuilderSetLevels, (void*, int));
PROC(void, TextureBuilderSetFormat, (void*, int));
PROC(void, TextureBuilderSetSamples, (void*, int));
PROC(size_t, TextureBuilderGetStorageSize, (const void*));
PROC(size_t, TextureBuilderGetStorageAlignment, (const void*));
PROC(void, TextureBuilderSetStorage, (void*, void*, ptrdiff_t));
PROC(int, TextureBuilderGetFlags, (const void*));
PROC(Boolean, TextureInitialize, (void*, const void*));
PROC(void, TextureFinalize, (void*));
PROC(int, TextureGetWidth, (const void*));
PROC(int, TextureGetHeight, (const void*));
PROC(int, TextureGetFormat, (const void*));
PROC(Boolean, CommandBufferInitialize, (void*, void*));
PROC(void, CommandBufferFinalize, (void*));
PROC(void, CommandBufferSetMemoryCallback, (void*, void (*)(void*, int, size_t, void*)));
PROC(void, CommandBufferSetMemoryCallbackData, (void*, void*));
PROC(void, CommandBufferAddCommandMemory, (void*, const void*, ptrdiff_t, size_t));
PROC(void, CommandBufferAddControlMemory, (void*, void*, size_t));
PROC(void, CommandBufferBeginRecording, (void*));
PROC(Handle, CommandBufferEndRecording, (void*));
PROC(Boolean, CommandBufferIsRecording, (const void*));
PROC(void, CommandBufferClearColor, (void*, int, const float*, int));
PROC(void, CommandBufferClearDepthStencil, (void*, float, Boolean, int, int));
PROC(void, CommandBufferSetViewports, (void*, int, int, const float*));
PROC(void, CommandBufferSetScissors, (void*, int, int, const int*));
PROC(void, CommandBufferSetRenderTargets, (void*, int, void* const*, const void* const*, const void*, const void*));
PROC(void, CommandBufferBarrier, (void*, int));
static void* (*DeviceGetProcAddress)(const void*, const char*);

static int g_missing = 0;
template <class F>
static void Load(F& dst, const char* name) {
  Proc p = nvnBootstrapLoader(name);
  if (!p) { fprintf(stderr, "MISSING NVN entry point %s\n", name); g_missing++; }
  dst = reinterpret_cast<F>(p);
}
#define LOAD(n) Load(n, "nvn" #n)

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }
  return DefWindowProcW(h, m, w, l);
}

static void SaveBmp(const char* path, const std::vector<unsigned char>& bgra, int w, int h) {
  BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
  ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = -h; ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
  fh.bfType = 0x4d42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + (DWORD)bgra.size();
  FILE* f = fopen(path, "wb");
  if (!f) return;
  fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(bgra.data(), 1, bgra.size(), f);
  fclose(f);
}

static float Srgb(float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }

static void HsvToRgb(float h, float s, float v, float* o) {
  float c = v * s, x = c * (1 - fabsf(fmodf(h * 6.0f, 2.0f) - 1)), m = v - c, r = 0, g = 0, b = 0;
  int i = (int)(h * 6.0f) % 6;
  switch (i) { case 0: r = c; g = x; break; case 1: r = x; g = c; break; case 2: g = c; b = x; break;
               case 3: g = x; b = c; break; case 4: r = x; b = c; break; default: r = c; b = x; }
  o[0] = r + m; o[1] = g + m; o[2] = b + m; o[3] = 1.0f;
}

int main(int argc, char** argv) {
  double pace = 60.0; int frames = 300; bool uncapped = false, validation = true, hidden = false; const char* shot = nullptr;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--uncapped")) uncapped = true;
    else if (!strcmp(argv[i], "--no-validation")) validation = false;
    else if (!strcmp(argv[i], "--pace") && i + 1 < argc) pace = atof(argv[++i]);
    else if (!strcmp(argv[i], "--hidden")) hidden = true;
    else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
  }
  SetProcessDPIAware();
  const int W = 1280, H = 720;

  // ---- window --------------------------------------------------------------------------------------------------
  WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"NvnClearTest";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  RECT r{0, 0, W, H}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"NVN clear test", (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_VISIBLE,
                            100, 100, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
  if (hidden) ShowWindow(hwnd, SW_HIDE);
  else SetWindowPos(hwnd, HWND_TOPMOST, 100, 100, 0, 0, SWP_NOSIZE);

  // ---- function table, as the lifted game would get it -----------------------------------------------------------
  if (validation) {
    nvnHostSetValidation(1);
    if (!getenv("VK_LAYER_PATH")) {
#ifdef NVN_TEST_LAYER_PATH
      _putenv("VK_LAYER_PATH=" NVN_TEST_LAYER_PATH);
#endif
    }
  }
  nvnHostSetFramePacing(pace);
  LOAD(DeviceGetProcAddress);
  Load(DeviceGetProcAddress, "nvnDeviceGetProcAddress");
  LOAD(DeviceInitialize); LOAD(DeviceBuilderSetDefaults); LOAD(DeviceFinalize); LOAD(DeviceGetInteger);
  LOAD(DeviceSetWindowOriginMode); LOAD(DeviceSetDepthMode); LOAD(DeviceRegisterFastClearColor); LOAD(DeviceRegisterFastClearDepth);
  LOAD(MemoryPoolBuilderSetDevice); LOAD(MemoryPoolBuilderSetDefaults); LOAD(MemoryPoolBuilderSetStorage);
  LOAD(MemoryPoolBuilderSetFlags); LOAD(MemoryPoolInitialize); LOAD(MemoryPoolSetDebugLabel); LOAD(MemoryPoolFinalize);
  LOAD(MemoryPoolMap); LOAD(MemoryPoolFlushMappedRange); LOAD(MemoryPoolGetSize);
  LOAD(QueueBuilderSetDevice); LOAD(QueueBuilderSetDefaults); LOAD(QueueBuilderSetFlags); LOAD(QueueBuilderSetCommandMemorySize);
  LOAD(QueueBuilderSetComputeMemorySize); LOAD(QueueBuilderSetControlMemorySize); LOAD(QueueInitialize); LOAD(QueueFinalize);
  LOAD(QueueSubmitCommands); LOAD(QueueFlush); LOAD(QueueFinish); LOAD(QueuePresentTexture); LOAD(QueueFenceSync);
  LOAD(SyncInitialize); LOAD(SyncFinalize); LOAD(SyncWait);
  LOAD(WindowBuilderSetDevice); LOAD(WindowBuilderSetDefaults); LOAD(WindowBuilderSetNativeWindow); LOAD(WindowBuilderSetTextures);
  LOAD(WindowInitialize); LOAD(WindowFinalize); LOAD(WindowAcquireTexture); LOAD(WindowSetPresentInterval); LOAD(WindowSetCrop);
  LOAD(TextureBuilderSetDevice); LOAD(TextureBuilderSetDefaults); LOAD(TextureBuilderSetFlags); LOAD(TextureBuilderSetTarget);
  LOAD(TextureBuilderSetSize2D); LOAD(TextureBuilderSetLevels); LOAD(TextureBuilderSetFormat); LOAD(TextureBuilderSetSamples);
  LOAD(TextureBuilderGetStorageSize); LOAD(TextureBuilderGetStorageAlignment); LOAD(TextureBuilderSetStorage);
  LOAD(TextureBuilderGetFlags); LOAD(TextureInitialize); LOAD(TextureFinalize); LOAD(TextureGetWidth); LOAD(TextureGetHeight);
  LOAD(TextureGetFormat);
  LOAD(CommandBufferInitialize); LOAD(CommandBufferFinalize); LOAD(CommandBufferSetMemoryCallback);
  LOAD(CommandBufferSetMemoryCallbackData); LOAD(CommandBufferAddCommandMemory); LOAD(CommandBufferAddControlMemory);
  LOAD(CommandBufferBeginRecording); LOAD(CommandBufferEndRecording); LOAD(CommandBufferIsRecording);
  LOAD(CommandBufferClearColor); LOAD(CommandBufferClearDepthStencil); LOAD(CommandBufferSetViewports);
  LOAD(CommandBufferSetScissors); LOAD(CommandBufferSetRenderTargets); LOAD(CommandBufferBarrier);
  if (g_missing) return 2;
  // Resolving through the device's getProcAddress must agree with the bootstrap loader.
  if (DeviceGetProcAddress(nullptr, "nvnDeviceInitialize") != (void*)DeviceInitialize) { fprintf(stderr, "getProcAddress mismatch\n"); return 2; }
  printf("registered NVN entry points: %d\n", nvnHostProcCount());

  // ---- Device (order from 0x90d590) --------------------------------------------------------------------------------------
  static Obj device, queue, queue2, cmdbuf, window, fenceSync, acquireSync, cmdPool, dataPool, texPool[4];
  static Obj tex[3], depthTex, builderMem;
  unsigned char devBuilder[0x40]; memset(devBuilder, 0xCD, sizeof devBuilder);
  DeviceBuilderSetDefaults(devBuilder);
  if (!DeviceInitialize(&device, devBuilder)) { fprintf(stderr, "DeviceInitialize failed\n"); return 1; }
  int infos[0x60];
  for (int i = 0; i < 0x60; i++) DeviceGetInteger(&device, i, &infos[i]);   // the game dumps all 0x60 infos
  DeviceSetWindowOriginMode(&device, 1);   // upper left
  DeviceSetDepthMode(&device, 1);          // near is zero
  DeviceRegisterFastClearDepth(&device, 1.0f);
  const float fc[4] = {0, 0, 0, 0};
  DeviceRegisterFastClearColor(&device, fc, 0x38);
  printf("api %d.%d, cmd align %d, ctrl align %d, min ctrl %d, max viewports %d\n", infos[0], infos[1], infos[0x13], infos[0x14], infos[0x16], infos[0x26]);

  // ---- memory pools (command memory, plus a CPU-mapped data pool exercising the mirror) ------------------------------------
  unsigned char pb[0x40];
  const size_t cmdBytes = 0x200000;
  void* cmdMem = _aligned_malloc(cmdBytes, 0x1000);
  MemoryPoolBuilderSetDefaults(pb); MemoryPoolBuilderSetDevice(pb, &device);
  MemoryPoolBuilderSetFlags(pb, 0xa2); MemoryPoolBuilderSetStorage(pb, cmdMem, cmdBytes);
  if (!MemoryPoolInitialize(&cmdPool, pb)) { fprintf(stderr, "cmd pool failed\n"); return 1; }
  MemoryPoolSetDebugLabel(&cmdPool, "command memory");
  const size_t dataBytes = 0x10000;
  void* dataMem = _aligned_malloc(dataBytes, 0x1000);
  memset(dataMem, 0x5A, dataBytes);
  MemoryPoolBuilderSetDefaults(pb); MemoryPoolBuilderSetDevice(pb, &device);
  MemoryPoolBuilderSetFlags(pb, 0xa2); MemoryPoolBuilderSetStorage(pb, dataMem, dataBytes);
  if (!MemoryPoolInitialize(&dataPool, pb)) { fprintf(stderr, "data pool failed\n"); return 1; }
  if (MemoryPoolMap(&dataPool) != dataMem || MemoryPoolGetSize(&dataPool) != dataBytes) { fprintf(stderr, "pool map/size mismatch\n"); return 1; }
  ((unsigned char*)MemoryPoolMap(&dataPool))[0] = 0x77;
  MemoryPoolFlushMappedRange(&dataPool, 0, 256);   // creates the Vulkan mirror and uploads

  // ---- command buffer + queue (order from 0x90e47x..0x90e7xx) --------------------------------------------------------------
  if (!CommandBufferInitialize(&cmdbuf, &device)) { fprintf(stderr, "cb init failed\n"); return 1; }
  void* ctrlMem = _aligned_malloc(0x20000, 16);
  CommandBufferAddControlMemory(&cmdbuf, ctrlMem, 0x20000);
  CommandBufferAddCommandMemory(&cmdbuf, &cmdPool, 0, cmdBytes);
  CommandBufferSetMemoryCallback(&cmdbuf, [](void*, int, size_t, void*) {});
  CommandBufferSetMemoryCallbackData(&cmdbuf, nullptr);
  unsigned char qb[0x40];
  QueueBuilderSetDefaults(qb); QueueBuilderSetDevice(qb, &device); QueueBuilderSetFlags(qb, 1);
  QueueBuilderSetComputeMemorySize(qb, 0x200000); QueueBuilderSetCommandMemorySize(qb, 0x200000); QueueBuilderSetControlMemorySize(qb, 0x20000);
  if (!QueueInitialize(&queue, qb)) { fprintf(stderr, "queue init failed\n"); return 1; }
  SyncInitialize(&fenceSync, &device);
  QueueBuilderSetDefaults(qb); QueueBuilderSetDevice(qb, &device); QueueBuilderSetFlags(qb, 3);
  if (!QueueInitialize(&queue2, qb)) { fprintf(stderr, "queue2 init failed\n"); return 1; }

  // ---- swapchain textures + window (order from 0x8f8510 and 0x985790) -------------------------------------------------------
  unsigned char tb[0x80];
  auto makeTex = [&](Obj& t, Obj& pool, int fmt, int flags) {
    TextureBuilderSetDefaults(tb); TextureBuilderSetFlags(tb, flags); TextureBuilderSetTarget(tb, 1);
    TextureBuilderSetFormat(tb, fmt); TextureBuilderSetSamples(tb, 0); TextureBuilderSetLevels(tb, 1);
    TextureBuilderSetDevice(tb, &device); TextureBuilderSetSize2D(tb, W, H);
    size_t sz = (TextureBuilderGetStorageSize(tb) + 0xfff) & ~(size_t)0xfff;
    (void)TextureBuilderGetStorageAlignment(tb);
    void* mem = _aligned_malloc(sz, 0x1000);
    MemoryPoolBuilderSetDefaults(pb); MemoryPoolBuilderSetDevice(pb, &device);
    MemoryPoolBuilderSetFlags(pb, 0x2b /* cpu uncached | gpu no-access style flags are irrelevant here */); MemoryPoolBuilderSetStorage(pb, mem, sz);
    if (!MemoryPoolInitialize(&pool, pb)) { fprintf(stderr, "tex pool failed\n"); exit(1); }
    TextureBuilderSetStorage(tb, &pool, 0);
    (void)TextureBuilderGetFlags(tb);
    if (!TextureInitialize(&t, tb)) { fprintf(stderr, "texture init failed\n"); exit(1); }
  };
  void* texPtrs[3];
  for (int i = 0; i < 3; i++) { makeTex(tex[i], texPool[i], 0x38 /*RGBA8_SRGB*/, 1 /*DISPLAY*/); texPtrs[i] = &tex[i]; }
  makeTex(depthTex, texPool[3], 0x35 /*DEPTH24_STENCIL8*/, 0);
  if (TextureGetWidth(&tex[0]) != W || TextureGetHeight(&tex[0]) != H || TextureGetFormat(&tex[0]) != 0x38) { fprintf(stderr, "texture getters wrong\n"); return 1; }

  unsigned char wb[0x40];
  WindowBuilderSetDefaults(wb); WindowBuilderSetDevice(wb, &device); WindowBuilderSetNativeWindow(wb, hwnd);
  WindowBuilderSetTextures(wb, 3, texPtrs);
  if (!WindowInitialize(&window, wb)) { fprintf(stderr, "WindowInitialize failed\n"); return 1; }
  WindowSetPresentInterval(&window, uncapped ? 0 : 1);
  WindowSetCrop(&window, 0, 0, W, H);
  SyncInitialize(&acquireSync, &device);
  int cur = 0;
  WindowAcquireTexture(&window, &acquireSync, &cur);
  SyncWait(&acquireSync, ~0ull);

  // ---- frame loop -----------------------------------------------------------------------------------------------------------
  LARGE_INTEGER qf, t0; QueryPerformanceFrequency(&qf);
  std::vector<double> ft; ft.reserve(frames);
  Obj fences[2]; for (auto& f : fences) SyncInitialize(&f, &device);
  float lastColor[4] = {0, 0, 0, 1}; bool shotDone = false; int shotPixOk = -1; double shotErr = 0;
  QueryPerformanceCounter(&t0);
  LARGE_INTEGER prev = t0;
  bool quit = false;
  for (int f = 0; f < frames && !quit; f++) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) quit = true; TranslateMessage(&msg); DispatchMessageW(&msg); }
    float color[4];
    int hold = (f >= 150 && f < 170) ? 150 : f;            // freeze the color for the screenshot check
    HsvToRgb(fmodf(hold / 300.0f * 2.0f, 1.0f), 0.8f, 0.9f, color);
    memcpy(lastColor, color, sizeof color);

    if (CommandBufferIsRecording(&cmdbuf)) { fprintf(stderr, "cb already recording\n"); return 1; }
    CommandBufferBeginRecording(&cmdbuf);
    void* rt[1] = {&tex[cur]};
    CommandBufferSetRenderTargets(&cmdbuf, 1, rt, nullptr, &depthTex, nullptr);
    const float vp[4] = {0, 0, (float)W, (float)H}; const int sc[4] = {0, 0, W, H};
    CommandBufferSetViewports(&cmdbuf, 0, 1, vp);
    CommandBufferSetScissors(&cmdbuf, 0, 1, sc);
    CommandBufferClearColor(&cmdbuf, 0, color, 0xf);
    CommandBufferClearDepthStencil(&cmdbuf, 1.0f, 1, 0, 0xff);
    CommandBufferBarrier(&cmdbuf, 0x28);
    Handle h = CommandBufferEndRecording(&cmdbuf);
    QueueSubmitCommands(&queue, 1, &h);
    QueueFenceSync(&queue, &fences[f & 1], 0, 0);
    QueueFlush(&queue);
    QueuePresentTexture(&queue, &window, cur);
    WindowAcquireTexture(&window, &acquireSync, &cur);
    SyncWait(&acquireSync, ~0ull);
    SyncWait(&fences[(f + 1) & 1], ~0ull);   // fence of the previous frame

    if (shot && !shotDone && f == 160 && !hidden) {
      Sleep(100);   // let DWM catch up with the held color
      POINT p{0, 0}; ClientToScreen(hwnd, &p);
      HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
      HBITMAP bm = CreateCompatibleBitmap(sdc, W, H); SelectObject(mdc, bm);
      BitBlt(mdc, 0, 0, W, H, sdc, p.x, p.y, SRCCOPY);
      BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
      std::vector<unsigned char> px((size_t)W * H * 4);
      GetDIBits(mdc, bm, 0, H, px.data(), &bi, DIB_RGB_COLORS);
      SaveBmp(shot, px, W, H);
      const unsigned char* c = &px[((size_t)(H / 2) * W + W / 2) * 4];   // BGRA
      float held[4]; HsvToRgb(fmodf(150 / 300.0f * 2.0f, 1.0f), 0.8f, 0.9f, held);
      double e = 0;
      for (int k = 0; k < 3; k++) e = std::max(e, fabs(Srgb(held[k]) * 255.0 - (double)c[2 - k]));
      shotErr = e; shotPixOk = e <= 3.0 ? 1 : 0; shotDone = true;
      DeleteObject(bm); DeleteDC(mdc); ReleaseDC(nullptr, sdc);
    }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    ft.push_back((double)(now.QuadPart - prev.QuadPart) * 1000.0 / qf.QuadPart);
    prev = now;
  }
  LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
  QueueFinish(&queue);

  // ---- frame statistics (skip the first 10 frames) --------------------------------------------------------------------------------
  std::vector<double> s(ft.begin() + std::min<size_t>(10, ft.size() ? ft.size() - 1 : 0), ft.end());
  std::sort(s.begin(), s.end());
  double sum = 0; for (double v : s) sum += v;
  auto pct = [&](double p) { return s.empty() ? 0.0 : s[std::min(s.size() - 1, (size_t)(p * s.size()))]; };
  { DEVMODEW dm{}; dm.dmSize = sizeof dm; EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm); printf("display refresh: %lu Hz\n", dm.dmDisplayFrequency); }
  printf("mode=%s frames=%zu avg=%.3f ms (%.1f fps) p50=%.3f p99=%.3f max=%.3f min=%.3f\n", uncapped ? "uncapped" : (pace > 0 ? "vsync+limiter" : "vsync(display-paced)"),
         ft.size(), sum / std::max<size_t>(1, s.size()), 1000.0 / std::max(1e-9, sum / std::max<size_t>(1, s.size())),
         pct(0.5), pct(0.99), s.empty() ? 0 : s.back(), s.empty() ? 0 : s.front());
  if (shot) printf("screenshot=%s center-pixel-check=%s (max channel error %.2f/255 vs sRGB-encoded clear color)\n", shot,
                   shotPixOk == 1 ? "PASS" : (shotPixOk == 0 ? "FAIL" : "n/a"), shotErr);

  // ---- teardown in the game's order (0x90f550) --------------------------------------------------------------------------------------
  for (auto& f : fences) SyncFinalize(&f);
  SyncFinalize(&acquireSync); SyncFinalize(&fenceSync);
  WindowFinalize(&window);
  for (auto& t : tex) TextureFinalize(&t);
  TextureFinalize(&depthTex);
  for (auto& p : texPool) MemoryPoolFinalize(&p);
  QueueFinalize(&queue2); QueueFinalize(&queue);
  CommandBufferFinalize(&cmdbuf);
  MemoryPoolFinalize(&dataPool); MemoryPoolFinalize(&cmdPool);
  DeviceFinalize(&device);
  DestroyWindow(hwnd);
  unsigned errs = nvnHostValidationErrors(), warns = nvnHostValidationWarnings();
  printf("validation: errors=%u warnings=%u (layers %s)\n", errs, warns, validation ? "on" : "off");
  return (errs || (shotPixOk == 0)) ? 1 : 0;
}
