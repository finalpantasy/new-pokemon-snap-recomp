// SPDX-License-Identifier: MIT
// Regression test for the two first-screen rendering bugs of the NVN->Vulkan layer. No game data involved.
//
//  1. Window origin: nvn::MakeNvnViewport maps NVN viewports to Vulkan. The game selects UPPER_LEFT; NVN keeps NDC +y up
//     in both origin modes (OpenGL clip control), so UPPER_LEFT must use a negative-height Vulkan viewport. Checked
//     numerically with the Vulkan viewport transform, including a render-target scale.
//  2. Pitch-linear textures (TF_LINEAR with a row stride): the game's glyph atlases are R8, linear, 512 x 2048 and
//     rewritten by the CPU after creation. The layer must (a) read them row by row with the stride, not as Maxwell
//     block-linear data, and (b) re-upload them when the guest memory changes. Checked through the NVN entry points
//     (TextureBuilderSetStride, CopyTextureToBuffer read-back), including a stride larger than the width.
//
// Usage: linear_test.exe [--no-validation]      exit code 0 = all checks passed
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
#include "nvn_internal.h"

struct Obj { alignas(16) unsigned char raw[0x3000]; };
using Proc = void (*)(void);
using Boolean = unsigned char;
using Handle = unsigned long long;
using Addr = unsigned long long;
struct CopyRegion { int x, y, z, w, h, d; };

#define PROC(ret, name, args) static ret(*name) args
PROC(Boolean, DeviceInitialize, (void*, const void*));
PROC(void, DeviceBuilderSetDefaults, (void*));
PROC(void, DeviceSetWindowOriginMode, (void*, int));
PROC(void, DeviceSetDepthMode, (void*, int));
PROC(void, MemoryPoolBuilderSetDevice, (void*, void*));
PROC(void, MemoryPoolBuilderSetDefaults, (void*));
PROC(void, MemoryPoolBuilderSetStorage, (void*, void*, size_t));
PROC(void, MemoryPoolBuilderSetFlags, (void*, int));
PROC(Boolean, MemoryPoolInitialize, (void*, const void*));
PROC(void, QueueBuilderSetDevice, (void*, void*));
PROC(void, QueueBuilderSetDefaults, (void*));
PROC(void, QueueBuilderSetFlags, (void*, int));
PROC(void, QueueBuilderSetCommandMemorySize, (void*, size_t));
PROC(void, QueueBuilderSetComputeMemorySize, (void*, size_t));
PROC(void, QueueBuilderSetControlMemorySize, (void*, size_t));
PROC(Boolean, QueueInitialize, (void*, const void*));
PROC(void, QueueSubmitCommands, (void*, int, const Handle*));
PROC(void, QueueFinish, (void*));
PROC(void, QueueFenceSync, (void*, void*, int, int));
PROC(Boolean, SyncInitialize, (void*, void*));
PROC(int, SyncWait, (const void*, unsigned long long));
PROC(void, TextureBuilderSetDevice, (void*, void*));
PROC(void, TextureBuilderSetDefaults, (void*));
PROC(void, TextureBuilderSetFlags, (void*, int));
PROC(void, TextureBuilderSetTarget, (void*, int));
PROC(void, TextureBuilderSetSize2D, (void*, int, int));
PROC(void, TextureBuilderSetLevels, (void*, int));
PROC(void, TextureBuilderSetFormat, (void*, int));
PROC(void, TextureBuilderSetSamples, (void*, int));
PROC(void, TextureBuilderSetStride, (void*, ptrdiff_t));
PROC(size_t, TextureBuilderGetStorageSize, (const void*));
PROC(void, TextureBuilderSetStorage, (void*, void*, ptrdiff_t));
PROC(Boolean, TextureInitialize, (void*, const void*));
PROC(Boolean, CommandBufferInitialize, (void*, void*));
PROC(void, CommandBufferSetMemoryCallback, (void*, void (*)(void*, int, size_t, void*)));
PROC(void, CommandBufferSetMemoryCallbackData, (void*, void*));
PROC(void, CommandBufferAddCommandMemory, (void*, const void*, ptrdiff_t, size_t));
PROC(void, CommandBufferAddControlMemory, (void*, void*, size_t));
PROC(void, CommandBufferBeginRecording, (void*));
PROC(Handle, CommandBufferEndRecording, (void*));
PROC(void, CommandBufferCopyTextureToBuffer, (void*, const void*, const void*, const CopyRegion*, Addr, int));
PROC(void, BufferBuilderSetDevice, (void*, void*));
PROC(void, BufferBuilderSetDefaults, (void*));
PROC(void, BufferBuilderSetStorage, (void*, void*, ptrdiff_t, size_t));
PROC(Boolean, BufferInitialize, (void*, const void*));
PROC(void*, BufferMap, (const void*));
PROC(Addr, BufferGetAddress, (const void*));

static int g_missing = 0, g_fail = 0;
template <class F> static void Load(F& dst, const char* name) {
  Proc p = nvnBootstrapLoader(name);
  if (!p) { fprintf(stderr, "MISSING NVN entry point %s\n", name); g_missing++; }
  dst = reinterpret_cast<F>(p);
}
#define LOAD(n) Load(n, "nvn" #n)
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Vulkan viewport transform of NDC y to a framebuffer row: y_f = (h/2) * y_ndc + (y + h/2)
static float RowOf(const VkViewport& v, float ndcY) { return (v.height * 0.5f) * ndcY + (v.y + v.height * 0.5f); }

static void TestViewport() {
  // UPPER_LEFT: NDC +1 is the first row of the rectangle (image top), NDC -1 the row below its last row.
  VkViewport u = nvn::MakeNvnViewport(nvn::ORIGIN_UPPER_LEFT, 10, 20, 100, 50);
  CHECK(u.height < 0, "UPPER_LEFT viewport must have a negative height (NDC +y up), got %g", u.height);
  CHECK(std::fabs(RowOf(u, +1) - 20) < 1e-4f, "UPPER_LEFT: NDC +1 -> row %g, expected 20 (top of the rectangle)", RowOf(u, +1));
  CHECK(std::fabs(RowOf(u, -1) - 70) < 1e-4f, "UPPER_LEFT: NDC -1 -> row %g, expected 70 (bottom of the rectangle)", RowOf(u, -1));
  CHECK(u.x == 10 && u.width == 100, "UPPER_LEFT x/width unchanged");
  // render-target scale (nvnHostSetRenderScale) multiplies everything, the flip survives it
  VkViewport s = nvn::MakeNvnViewport(nvn::ORIGIN_UPPER_LEFT, 10, 20, 100, 50, 2.0f);
  CHECK(std::fabs(RowOf(s, +1) - 40) < 1e-4f && std::fabs(RowOf(s, -1) - 140) < 1e-4f, "scaled UPPER_LEFT rows %g..%g, expected 40..140", RowOf(s, +1), RowOf(s, -1));
  // full-target default viewport (1920x1080): +1 -> row 0, -1 -> row 1080
  VkViewport f = nvn::MakeNvnViewport(nvn::ORIGIN_UPPER_LEFT, 0, 0, 1920, 1080);
  CHECK(RowOf(f, +1) == 0 && RowOf(f, -1) == 1080, "full viewport rows %g..%g", RowOf(f, +1), RowOf(f, -1));
  // LOWER_LEFT: rows count from the window origin, NDC -1 is the first row (image bottom), no flip.
  VkViewport l = nvn::MakeNvnViewport(nvn::ORIGIN_LOWER_LEFT, 10, 20, 100, 50);
  CHECK(l.height > 0 && std::fabs(RowOf(l, -1) - 20) < 1e-4f && std::fabs(RowOf(l, +1) - 70) < 1e-4f,
        "LOWER_LEFT: NDC -1 -> row %g (expected 20), +1 -> %g (expected 70)", RowOf(l, -1), RowOf(l, +1));
  printf("viewport origin mapping: %s\n", g_fail ? "FAIL" : "ok");
}

int main(int argc, char** argv) {
  bool validation = true;
  for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--no-validation")) validation = false;
  TestViewport();

  if (validation) {
    nvnHostSetValidation(1);
    if (!getenv("VK_LAYER_PATH")) {
#ifdef NVN_TEST_LAYER_PATH
      _putenv("VK_LAYER_PATH=" NVN_TEST_LAYER_PATH);
#endif
    }
  }
  LOAD(DeviceInitialize); LOAD(DeviceBuilderSetDefaults); LOAD(DeviceSetWindowOriginMode); LOAD(DeviceSetDepthMode);
  LOAD(MemoryPoolBuilderSetDevice); LOAD(MemoryPoolBuilderSetDefaults); LOAD(MemoryPoolBuilderSetStorage); LOAD(MemoryPoolBuilderSetFlags);
  LOAD(MemoryPoolInitialize);
  LOAD(QueueBuilderSetDevice); LOAD(QueueBuilderSetDefaults); LOAD(QueueBuilderSetFlags); LOAD(QueueBuilderSetCommandMemorySize);
  LOAD(QueueBuilderSetComputeMemorySize); LOAD(QueueBuilderSetControlMemorySize); LOAD(QueueInitialize); LOAD(QueueSubmitCommands);
  LOAD(QueueFinish); LOAD(QueueFenceSync); LOAD(SyncInitialize); LOAD(SyncWait);
  LOAD(TextureBuilderSetDevice); LOAD(TextureBuilderSetDefaults); LOAD(TextureBuilderSetFlags); LOAD(TextureBuilderSetTarget);
  LOAD(TextureBuilderSetSize2D); LOAD(TextureBuilderSetLevels); LOAD(TextureBuilderSetFormat); LOAD(TextureBuilderSetSamples);
  LOAD(TextureBuilderSetStride); LOAD(TextureBuilderGetStorageSize); LOAD(TextureBuilderSetStorage); LOAD(TextureInitialize);
  LOAD(CommandBufferInitialize); LOAD(CommandBufferSetMemoryCallback); LOAD(CommandBufferSetMemoryCallbackData);
  LOAD(CommandBufferAddCommandMemory); LOAD(CommandBufferAddControlMemory); LOAD(CommandBufferBeginRecording);
  LOAD(CommandBufferEndRecording); LOAD(CommandBufferCopyTextureToBuffer);
  LOAD(BufferBuilderSetDevice); LOAD(BufferBuilderSetDefaults); LOAD(BufferBuilderSetStorage); LOAD(BufferInitialize);
  LOAD(BufferMap); LOAD(BufferGetAddress);
  if (g_missing) return 2;

  static Obj device, queue, cmdbuf, fence, cmdPool, rbPool, rbBuf, texPool, tex;
  unsigned char devBuilder[0x40]; memset(devBuilder, 0xCD, sizeof devBuilder);
  DeviceBuilderSetDefaults(devBuilder);
  if (!DeviceInitialize(&device, devBuilder)) { fprintf(stderr, "DeviceInitialize failed\n"); return 1; }
  DeviceSetWindowOriginMode(&device, 1); DeviceSetDepthMode(&device, 1);

  unsigned char pb[0x40];
  const size_t cmdBytes = 0x200000, rbBytes = 4u << 20;
  void* cmdMem = _aligned_malloc(cmdBytes, 0x1000);
  MemoryPoolBuilderSetDefaults(pb); MemoryPoolBuilderSetDevice(pb, &device);
  MemoryPoolBuilderSetFlags(pb, 0xa2); MemoryPoolBuilderSetStorage(pb, cmdMem, cmdBytes);
  if (!MemoryPoolInitialize(&cmdPool, pb)) { fprintf(stderr, "cmd pool failed\n"); return 1; }
  void* rbMem = _aligned_malloc(rbBytes, 0x1000);
  memset(rbMem, 0xEE, rbBytes);
  MemoryPoolBuilderSetDefaults(pb); MemoryPoolBuilderSetDevice(pb, &device);
  MemoryPoolBuilderSetFlags(pb, 0xa2); MemoryPoolBuilderSetStorage(pb, rbMem, rbBytes);
  if (!MemoryPoolInitialize(&rbPool, pb)) { fprintf(stderr, "readback pool failed\n"); return 1; }
  unsigned char bb[0x40];
  BufferBuilderSetDefaults(bb); BufferBuilderSetDevice(bb, &device); BufferBuilderSetStorage(bb, &rbPool, 0, rbBytes);
  if (!BufferInitialize(&rbBuf, bb)) { fprintf(stderr, "readback buffer failed\n"); return 1; }
  const Addr rbAddr = BufferGetAddress(&rbBuf);
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
  SyncInitialize(&fence, &device);

  // Linear R8 texture, 40 x 24 texels, stride 64 bytes (> width), in a CPU-writable pool (flags like the game's data pools).
  const int W = 40, H = 24, STRIDE = 64, R8 = 1, TF_LINEAR = 0x10;
  const size_t poolBytes = 0x10000;
  uint8_t* mem = static_cast<uint8_t*>(_aligned_malloc(poolBytes, 0x1000));
  memset(mem, 0xAA, poolBytes);
  MemoryPoolBuilderSetDefaults(pb); MemoryPoolBuilderSetDevice(pb, &device);
  MemoryPoolBuilderSetFlags(pb, 0xa2); MemoryPoolBuilderSetStorage(pb, mem, poolBytes);
  if (!MemoryPoolInitialize(&texPool, pb)) { fprintf(stderr, "texture pool failed\n"); return 1; }
  const ptrdiff_t texOff = 0x1000;
  unsigned char tb[0x80];
  TextureBuilderSetDefaults(tb); TextureBuilderSetDevice(tb, &device); TextureBuilderSetFlags(tb, TF_LINEAR); TextureBuilderSetTarget(tb, 1);
  TextureBuilderSetFormat(tb, R8); TextureBuilderSetSamples(tb, 0); TextureBuilderSetLevels(tb, 1); TextureBuilderSetSize2D(tb, W, H);
  TextureBuilderSetStride(tb, STRIDE);
  const size_t need = TextureBuilderGetStorageSize(tb);
  CHECK(need >= (size_t)STRIDE * H, "linear storage size %zu < stride * height %d", need, STRIDE * H);
  TextureBuilderSetStorage(tb, &texPool, texOff);
  if (!TextureInitialize(&tex, tb)) { fprintf(stderr, "TextureInitialize failed\n"); return 1; }

  auto fill = [&](unsigned seed) {
    for (int y = 0; y < H; y++)
      for (int x = 0; x < STRIDE; x++) mem[texOff + (size_t)y * STRIDE + x] = x < W ? (uint8_t)((x * 7 + y * 13 + seed) & 0xFF) : 0xAA;
  };
  auto readback = [&](std::vector<uint8_t>& out) {
    memset(rbMem, 0xEE, 4096);
    CommandBufferBeginRecording(&cmdbuf);
    CopyRegion r{0, 0, 0, W, H, 1};
    CommandBufferCopyTextureToBuffer(&cmdbuf, &tex, nullptr, &r, rbAddr, 0);
    Handle h = CommandBufferEndRecording(&cmdbuf);
    QueueSubmitCommands(&queue, 1, &h);
    QueueFenceSync(&queue, &fence, 0, 0);
    SyncWait(&fence, ~0ull);
    QueueFinish(&queue);
    out.assign(static_cast<const uint8_t*>(BufferMap(&rbBuf)), static_cast<const uint8_t*>(BufferMap(&rbBuf)) + (size_t)W * H);
  };
  auto verify = [&](const char* what, unsigned seed) {
    std::vector<uint8_t> got; readback(got);
    int bad = 0, first = -1;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        if (got[(size_t)y * W + x] != (uint8_t)((x * 7 + y * 13 + seed) & 0xFF)) { if (first < 0) first = y * W + x; bad++; }
    CHECK(bad == 0, "%s: %d of %d texels differ (first at %d: got %u)", what, bad, W * H, first, first >= 0 ? got[first] : 0u);
    printf("%s: %s\n", what, bad ? "FAIL" : "ok");
  };

  fill(0);
  verify("linear R8 with stride > width, first upload", 0);
  fill(77);                                   // the CPU rewrites the atlas (new glyphs) between submits
  verify("linear R8 after a CPU rewrite (re-upload)", 77);
  verify("linear R8 unchanged (no spurious change)", 77);

  const unsigned errs = nvnHostValidationErrors(), warns = nvnHostValidationWarnings();
  printf("validation: errors=%u warnings=%u%s\n", errs, warns, validation ? "" : " (layers off)");
  if (g_fail || errs) { printf("linear_test: FAIL (%d)\n", g_fail); return 1; }
  printf("linear_test: PASS\n");
  return 0;
}
