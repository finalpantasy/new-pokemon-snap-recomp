// SPDX-License-Identifier: MIT
// Real-asset texture test of the NVN->Vulkan layer. For one NUTEXB loaded from the extracted game data at run time:
//   1. creates the texture through the NVN entry points with TextureBuilderSetPackagedTextureData (the game's path),
//   2. reads every mip level and layer back with CommandBufferCopyTextureToBuffer (raw block data, exercises the Maxwell
//      deswizzle, the layout rules and the GPU->guest readback),
//   3. decodes level 0 / layer 0 on the GPU by copying it into an RGBA8 (or RGBA16F) texture with
//      CommandBufferCopyTextureToTexture and reads that back too.
// tests/nvn/texture_compare.py compares both against an independent numpy deswizzle and texture2ddecoder.
// No game data is stored in the repository: the NUTEXB path is a command line argument.
//
// Usage: texture_test.exe <file.nutexb> <out-prefix> [--no-validation]
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nvn_host.h"

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
PROC(void*, MemoryPoolMap, (const void*));
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
PROC(void, TextureBuilderSetSize3D, (void*, int, int, int));
PROC(void, TextureBuilderSetLevels, (void*, int));
PROC(void, TextureBuilderSetFormat, (void*, int));
PROC(void, TextureBuilderSetSamples, (void*, int));
PROC(size_t, TextureBuilderGetStorageSize, (const void*));
PROC(void, TextureBuilderSetStorage, (void*, void*, ptrdiff_t));
PROC(void, TextureBuilderSetPackagedTextureData, (void*, const void*));
PROC(Boolean, TextureInitialize, (void*, const void*));
PROC(void, TextureViewSetDefaults, (void*));
PROC(void, TextureViewSetLevels, (void*, int, int));
PROC(void, TextureViewSetLayers, (void*, int, int));
PROC(Boolean, CommandBufferInitialize, (void*, void*));
PROC(void, CommandBufferSetMemoryCallback, (void*, void (*)(void*, int, size_t, void*)));
PROC(void, CommandBufferSetMemoryCallbackData, (void*, void*));
PROC(void, CommandBufferAddCommandMemory, (void*, const void*, ptrdiff_t, size_t));
PROC(void, CommandBufferAddControlMemory, (void*, void*, size_t));
PROC(void, CommandBufferBeginRecording, (void*));
PROC(Handle, CommandBufferEndRecording, (void*));
PROC(void, CommandBufferClearColor, (void*, int, const float*, int));
PROC(void, CommandBufferSetRenderTargets, (void*, int, void* const*, const void* const*, const void*, const void*));
PROC(void, CommandBufferCopyTextureToBuffer, (void*, const void*, const void*, const CopyRegion*, Addr, int));
PROC(void, CommandBufferCopyTextureToTexture, (void*, const void*, const void*, const CopyRegion*, const void*, const void*, const CopyRegion*, int));
PROC(void, BufferBuilderSetDevice, (void*, void*));
PROC(void, BufferBuilderSetDefaults, (void*));
PROC(void, BufferBuilderSetStorage, (void*, void*, ptrdiff_t, size_t));
PROC(Boolean, BufferInitialize, (void*, const void*));
PROC(void*, BufferMap, (const void*));
PROC(Addr, BufferGetAddress, (const void*));

static int g_missing = 0;
template <class F> static void Load(F& dst, const char* name) {
  Proc p = nvnBootstrapLoader(name);
  if (!p) { fprintf(stderr, "MISSING NVN entry point %s\n", name); g_missing++; }
  dst = reinterpret_cast<F>(p);
}
#define LOAD(n) Load(n, "nvn" #n)

static std::vector<unsigned char> ReadAll(const char* path) {
  std::vector<unsigned char> d;
  FILE* f = fopen(path, "rb");
  if (!f) return d;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  d.resize((size_t)n);
  if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
  fclose(f);
  return d;
}
static void WriteAll(const std::string& path, const void* p, size_t n) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  fwrite(p, 1, n, f); fclose(f);
}
// nn::gfx image format (NUTEXB footer) -> NVN format, taken from the game's conversion at 0x924d60
static int NvnFormat(unsigned f) {
  switch (f) {
    case 0x100: return 0x01; case 0x180: return 0x49; case 0x280: return 0x4B; case 0x400: return 0x25; case 0x405: return 0x38;
    case 0x434: return 0x2E; case 0x435: return 0x29; case 0x450: return 0x9A; case 0x455: return 0x9C;
    case 0x480: return 0x42; case 0x485: return 0x46; case 0x490: return 0x43; case 0x495: return 0x47; case 0x4A0: return 0x44;
    case 0x4A5: return 0x48; case 0x4D7: return 0x50; case 0x4D8: return 0x4F; case 0x4E0: return 0x4D; case 0x4E5: return 0x4E;
    default: return 0;
  }
}
// bytes per block, block dims of the NVN format (as the compare script also knows them)
static void BlockInfo(int nvn, unsigned* bpb, unsigned* bw) {
  *bw = 1;
  switch (nvn) {
    case 0x01: *bpb = 1; return; case 0x25: case 0x38: case 0x9A: case 0x9C: *bpb = 4; return;
    case 0x2E: *bpb = 16; return; case 0x29: *bpb = 8; return;
    case 0x42: case 0x46: case 0x49: *bpb = 8; *bw = 4; return;
    default: *bpb = 16; *bw = 4; return;
  }
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: texture_test <file.nutexb> <out-prefix> [--no-validation]\n"); return 2; }
  const char* path = argv[1]; const std::string out = argv[2];
  bool validation = true;
  for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--no-validation")) validation = false;
  auto file = ReadAll(path);
  if (file.size() < 0xb0) { fprintf(stderr, "cannot read %s\n", path); return 2; }
  const unsigned char* ft = file.data() + file.size() - 0xb0;
  unsigned u[11]; memcpy(u, ft + 0xb0 - 44, sizeof u);
  const unsigned W = u[0], H = u[1], D = u[2], FMT = u[3], DIM = u[4], MIPS = u[5], LAYERS = u[7];
  if (u[9] != 0x54455820) { fprintf(stderr, "not a nutexb\n"); return 2; }
  const int nvn = NvnFormat(FMT);
  if (!nvn) { fprintf(stderr, "format 0x%x not in the test map\n", FMT); return 2; }
  unsigned bpb, bw; BlockInfo(nvn, &bpb, &bw);
  printf("%s: %ux%ux%u fmt 0x%x (nvn 0x%x) dim %u mips %u layers %u\n", path, W, H, D, FMT, nvn, DIM, MIPS, LAYERS);

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
  LOAD(MemoryPoolInitialize); LOAD(MemoryPoolMap);
  LOAD(QueueBuilderSetDevice); LOAD(QueueBuilderSetDefaults); LOAD(QueueBuilderSetFlags); LOAD(QueueBuilderSetCommandMemorySize);
  LOAD(QueueBuilderSetComputeMemorySize); LOAD(QueueBuilderSetControlMemorySize); LOAD(QueueInitialize); LOAD(QueueSubmitCommands);
  LOAD(QueueFinish); LOAD(QueueFenceSync); LOAD(SyncInitialize); LOAD(SyncWait);
  LOAD(TextureBuilderSetDevice); LOAD(TextureBuilderSetDefaults); LOAD(TextureBuilderSetFlags); LOAD(TextureBuilderSetTarget);
  LOAD(TextureBuilderSetSize2D); LOAD(TextureBuilderSetSize3D); LOAD(TextureBuilderSetLevels); LOAD(TextureBuilderSetFormat);
  LOAD(TextureBuilderSetSamples); LOAD(TextureBuilderGetStorageSize); LOAD(TextureBuilderSetStorage);
  LOAD(TextureBuilderSetPackagedTextureData); LOAD(TextureInitialize); LOAD(TextureViewSetDefaults); LOAD(TextureViewSetLevels);
  LOAD(TextureViewSetLayers);
  LOAD(CommandBufferInitialize); LOAD(CommandBufferSetMemoryCallback); LOAD(CommandBufferSetMemoryCallbackData);
  LOAD(CommandBufferAddCommandMemory); LOAD(CommandBufferAddControlMemory); LOAD(CommandBufferBeginRecording);
  LOAD(CommandBufferEndRecording); LOAD(CommandBufferCopyTextureToBuffer); LOAD(CommandBufferCopyTextureToTexture);
  LOAD(CommandBufferClearColor); LOAD(CommandBufferSetRenderTargets);
  LOAD(BufferBuilderSetDevice); LOAD(BufferBuilderSetDefaults); LOAD(BufferBuilderSetStorage); LOAD(BufferInitialize);
  LOAD(BufferMap); LOAD(BufferGetAddress);
  if (g_missing) return 2;

  static Obj device, queue, cmdbuf, fence, cmdPool, rbPool, rbBuf, srcTex, dstTex, texStore[2];
  unsigned char devBuilder[0x40]; memset(devBuilder, 0xCD, sizeof devBuilder);
  DeviceBuilderSetDefaults(devBuilder);
  if (!DeviceInitialize(&device, devBuilder)) { fprintf(stderr, "DeviceInitialize failed\n"); return 1; }
  DeviceSetWindowOriginMode(&device, 1); DeviceSetDepthMode(&device, 1);

  unsigned char pb[0x40];
  const size_t cmdBytes = 0x200000, rbBytes = 64u << 20;
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

  // The packaged texture, created like the game does (TextureBuilder + pool storage + packaged data pointer)
  unsigned char tb[0x80];
  const int target = DIM == 4 ? 8 : DIM == 3 ? 2 : 1;
  auto makeTex = [&](Obj& t, Obj& store, int fmt, int tgt, int w, int h, int d, int levels, const void* packaged) {
    TextureBuilderSetDefaults(tb); TextureBuilderSetDevice(tb, &device); TextureBuilderSetFlags(tb, 0); TextureBuilderSetTarget(tb, tgt);
    TextureBuilderSetFormat(tb, fmt); TextureBuilderSetSamples(tb, 0); TextureBuilderSetLevels(tb, levels);
    if (tgt == 2) TextureBuilderSetSize3D(tb, w, h, d); else TextureBuilderSetSize2D(tb, w, h);
    size_t sz = (TextureBuilderGetStorageSize(tb) + 0xfff) & ~(size_t)0xfff;
    void* mem = _aligned_malloc(sz, 0x1000);
    MemoryPoolBuilderSetDefaults(pb); MemoryPoolBuilderSetDevice(pb, &device);
    MemoryPoolBuilderSetFlags(pb, 0x29); MemoryPoolBuilderSetStorage(pb, mem, sz);
    if (!MemoryPoolInitialize(&store, pb)) { fprintf(stderr, "texture pool failed\n"); exit(1); }
    TextureBuilderSetStorage(tb, &store, 0);
    if (packaged) TextureBuilderSetPackagedTextureData(tb, packaged);
    if (!TextureInitialize(&t, tb)) { fprintf(stderr, "TextureInitialize failed\n"); exit(1); }
    return sz;
  };
  const size_t storage = makeTex(srcTex, texStore[0], nvn, target, (int)W, (int)H, (int)D, (int)MIPS, file.data());
  printf("storage size from the layer: %zu bytes, nutexb data %zu\n", storage, file.size() - 0xb0);

  // decode target: RGBA8 (sRGB source -> sRGB target so the GPU round trip is the identity), RGBA16F for float sources
  const bool isF = nvn == 0x2E || nvn == 0x29 || nvn == 0x4F || nvn == 0x50;
  const bool isS = nvn == 0x38 || nvn == 0x46 || nvn == 0x47 || nvn == 0x48 || nvn == 0x4E || nvn == 0x9C;
  const int dstFmt = isF ? 0x29 : isS ? 0x38 : 0x25;
  makeTex(dstTex, texStore[1], dstFmt, 1, (int)W, (int)H, 1, 1, nullptr);

  // ---- record: every level/layer of the source -> readback buffer, then the GPU decode of level 0 ----------------------------
  const unsigned layers = DIM == 4 ? 6 : 1;
  const unsigned depth = DIM == 3 ? D : 1;
  struct Item { unsigned level, layer; size_t off, size; unsigned w, h, d; };
  std::vector<Item> items;
  size_t cur = 0;
  CommandBufferBeginRecording(&cmdbuf);
  for (unsigned l = 0; l < layers; l++)
    for (unsigned m = 0; m < MIPS; m++) {
      Item it{m, l, cur, 0, std::max(W >> m, 1u), std::max(H >> m, 1u), std::max(depth >> m, 1u)};
      it.size = (size_t)((it.w + bw - 1) / bw) * ((it.h + bw - 1) / bw) * it.d * bpb;
      unsigned char view[0x10]; TextureViewSetDefaults(view); TextureViewSetLevels(view, (int)m, 1);
      if (DIM == 4) TextureViewSetLayers(view, (int)l, 1);
      CopyRegion r{0, 0, 0, (int)it.w, (int)it.h, (int)it.d};
      CommandBufferCopyTextureToBuffer(&cmdbuf, &srcTex, view, &r, rbAddr + cur, 0);
      cur += (it.size + 255) & ~(size_t)255;
      items.push_back(it);
    }
  const size_t decOff = cur;
  const unsigned decBpp = isF ? 8 : 4;
  {
    unsigned char sview[0x10]; TextureViewSetDefaults(sview); TextureViewSetLevels(sview, 0, 1);
    CopyRegion sr{0, 0, 0, (int)W, (int)H, 1}, dr{0, 0, 0, (int)W, (int)H, 1};
    CommandBufferCopyTextureToTexture(&cmdbuf, &srcTex, sview, &sr, &dstTex, nullptr, &dr, 0);
    CommandBufferCopyTextureToBuffer(&cmdbuf, &dstTex, nullptr, &dr, rbAddr + decOff, 0);
  }
  Handle h = CommandBufferEndRecording(&cmdbuf);
  if (decOff + (size_t)W * H * decBpp > rbBytes) { fprintf(stderr, "readback pool too small\n"); return 1; }
  QueueSubmitCommands(&queue, 1, &h);
  QueueFenceSync(&queue, &fence, 0, 0);
  SyncWait(&fence, ~0ull);
  QueueFinish(&queue);

  const unsigned char* rb = static_cast<const unsigned char*>(BufferMap(&rbBuf));
  FILE* man = fopen((out + ".manifest.tsv").c_str(), "w");
  fprintf(man, "file\t%s\nformat\t0x%x\nnvn\t0x%x\nwidth\t%u\nheight\t%u\ndepth\t%u\ndim\t%u\nmips\t%u\nlayers\t%u\ndecode_format\t0x%x\n",
          path, FMT, nvn, W, H, D, DIM, MIPS, LAYERS, dstFmt);
  for (auto& it : items) {
    char name[64]; snprintf(name, sizeof name, ".l%u.z%u.bin", it.level, it.layer);
    WriteAll(out + name, rb + it.off, it.size);
    fprintf(man, "level\t%u\t%u\t%u\t%u\t%u\t%zu\n", it.level, it.layer, it.w, it.h, it.d, it.size);
  }
  WriteAll(out + ".decoded.bin", rb + decOff, (size_t)W * H * decBpp);
  fclose(man);
  // Partial clear-channel mask check: clear RGBA8 to (1,0,0,1), then clear only G and B (mask 0b0110) to (0.9,0.5,1,0).
  // Expected pixel: R 255 (kept), G 128, B 255, A 255 (kept).
  int clearFail = 0;
  {
    static Obj rtTex, rtStore;
    makeTex(rtTex, rtStore, 0x25, 1, 8, 8, 1, 1, nullptr);
    CommandBufferBeginRecording(&cmdbuf);
    void* rts[1] = {&rtTex};
    CommandBufferSetRenderTargets(&cmdbuf, 1, rts, nullptr, nullptr, nullptr);
    const float c1[4] = {1, 0, 0, 1}, c2[4] = {0.9f, 0.5f, 1, 0};
    CommandBufferClearColor(&cmdbuf, 0, c1, 0xF);
    CommandBufferClearColor(&cmdbuf, 0, c2, 0x6);
    CopyRegion rr{0, 0, 0, 8, 8, 1};
    CommandBufferCopyTextureToBuffer(&cmdbuf, &rtTex, nullptr, &rr, rbAddr, 0);
    Handle h2 = CommandBufferEndRecording(&cmdbuf);
    QueueSubmitCommands(&queue, 1, &h2);
    QueueFenceSync(&queue, &fence, 0, 0);
    SyncWait(&fence, ~0ull);
    QueueFinish(&queue);
    const unsigned char* px = rb;
    printf("masked clear pixel (expect 255,128,255,255): %u,%u,%u,%u\n", px[0], px[1], px[2], px[3]);
    if (px[0] != 255 || std::abs((int)px[1] - 128) > 1 || px[2] != 255 || px[3] != 255) clearFail = 1;
  }
  const unsigned errs = nvnHostValidationErrors(), warns = nvnHostValidationWarnings();
  printf("wrote %zu subresources + decoded level 0; validation: errors=%u warnings=%u%s\n", items.size(), errs, warns, validation ? "" : " (layers off)");
  if (clearFail) { printf("masked clear: FAIL\n"); return 4; }
  printf("masked clear: ok\n");
  return errs ? 3 : 0;
}
