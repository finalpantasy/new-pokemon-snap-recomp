// SPDX-License-Identifier: MIT
// Regression test of the NVN->Vulkan layer for render targets that are also sampled, and for sampled-view types.
//  1. Mip chain: a 64x64 RGBA8 texture with 4 levels is rendered one level at a time while the draw samples the level above
//     (the game's bloom chain). Every draw must render (none may be skipped as a feedback loop) and all levels must read back
//     as the level-0 color. Uses the game shader pair nuSystemConstantVS/PS loaded at run time from the extracted game data.
//  2. Array layers: ClearColor / masked clears on a render-target view must touch only the bound layer.
//  3. View types: the view bound for a sampler is created with the dimension the shader declares (2D, 2D array, cube, 3D, 1D)
//     or refused (a typed dummy is bound instead), with a view cache, level / layer windows and attachment views.
// Validation must report 0 errors and 0 warnings.
//
// Usage: mip_test.exe [--no-validation] [--shader-dir DIR]
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nvn_host.h"
#include "nvn_internal.h"

struct Obj { alignas(16) unsigned char raw[0x3000]; };
using Proc = void (*)(void);
using Boolean = unsigned char;
using Handle = unsigned long long;
using Addr = unsigned long long;

#define PROC(ret, name, args) static ret(*name) args
PROC(Boolean, DeviceInitialize, (void*, const void*));
PROC(void, DeviceBuilderSetDefaults, (void*));
PROC(void, DeviceFinalize, (void*));
PROC(void, DeviceGetInteger, (const void*, int, int*));
PROC(Handle, DeviceGetTextureHandle, (const void*, int, int));
PROC(void, DeviceSetWindowOriginMode, (void*, int));
PROC(void, DeviceSetDepthMode, (void*, int));
PROC(Boolean, DeviceRegisterFastClearColor, (void*, const float*, int));
PROC(Boolean, DeviceRegisterFastClearDepth, (void*, float));
PROC(void, MemoryPoolBuilderSetDevice, (void*, void*));
PROC(void, MemoryPoolBuilderSetDefaults, (void*));
PROC(void, MemoryPoolBuilderSetStorage, (void*, void*, size_t));
PROC(void, MemoryPoolBuilderSetFlags, (void*, int));
PROC(Boolean, MemoryPoolInitialize, (void*, const void*));
PROC(void, MemoryPoolFinalize, (void*));
PROC(void*, MemoryPoolMap, (const void*));
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
PROC(Boolean, TextureInitialize, (void*, const void*));
PROC(void, TextureFinalize, (void*));
PROC(int, TextureGetWidth, (const void*));
PROC(int, TextureGetHeight, (const void*));
PROC(void, TextureViewSetDefaults, (void*));
PROC(void, TextureViewSetLevels, (void*, int, int));
PROC(Boolean, CommandBufferInitialize, (void*, void*));
PROC(void, CommandBufferFinalize, (void*));
PROC(void, CommandBufferSetMemoryCallback, (void*, void (*)(void*, int, size_t, void*)));
PROC(void, CommandBufferSetMemoryCallbackData, (void*, void*));
PROC(void, CommandBufferAddCommandMemory, (void*, const void*, ptrdiff_t, size_t));
PROC(void, CommandBufferAddControlMemory, (void*, void*, size_t));
PROC(void, CommandBufferBeginRecording, (void*));
PROC(Handle, CommandBufferEndRecording, (void*));
PROC(void, CommandBufferClearColor, (void*, int, const float*, int));
PROC(void, CommandBufferClearDepthStencil, (void*, float, Boolean, int, int));
PROC(void, CommandBufferSetViewports, (void*, int, int, const float*));
PROC(void, CommandBufferSetScissors, (void*, int, int, const int*));
PROC(void, CommandBufferSetRenderTargets, (void*, int, void* const*, const void* const*, const void*, const void*));
PROC(void, CommandBufferBarrier, (void*, int));
PROC(void, TextureBuilderSetSize3D, (void*, int, int, int));
PROC(void, TextureViewSetLayers, (void*, int, int));
PROC(void, CommandBufferCopyTextureToBuffer, (void*, const void*, const void*, const int*, Addr, int));
// milestone 2
PROC(Boolean, ProgramInitialize, (void*, void*));
PROC(void, ProgramFinalize, (void*));
PROC(Boolean, ProgramSetShaders, (void*, int, const void*));
PROC(Boolean, TexturePoolInitialize, (void*, const void*, ptrdiff_t, int));
PROC(void, TexturePoolFinalize, (void*));
PROC(void, TexturePoolRegisterTexture, (const void*, int, const void*, const void*));
PROC(Boolean, SamplerPoolInitialize, (void*, const void*, ptrdiff_t, int));
PROC(void, SamplerPoolFinalize, (void*));
PROC(void, SamplerPoolRegisterSampler, (const void*, int, const void*));
PROC(void, BufferBuilderSetDevice, (void*, void*));
PROC(void, BufferBuilderSetDefaults, (void*));
PROC(void, BufferBuilderSetStorage, (void*, void*, ptrdiff_t, size_t));
PROC(Boolean, BufferInitialize, (void*, const void*));
PROC(void, BufferFinalize, (void*));
PROC(void*, BufferMap, (const void*));
PROC(Addr, BufferGetAddress, (const void*));
PROC(void, BufferFlushMappedRange, (const void*, ptrdiff_t, size_t));
PROC(void*, BufferGetMemoryPool, (const void*));
PROC(ptrdiff_t, BufferGetMemoryOffset, (const void*));
PROC(size_t, BufferGetSize, (const void*));
PROC(void, SamplerBuilderSetDevice, (void*, void*));
PROC(void, SamplerBuilderSetDefaults, (void*));
PROC(void, SamplerBuilderSetMinMagFilter, (void*, int, int));
PROC(void, SamplerBuilderSetWrapMode, (void*, int, int, int));
PROC(void, SamplerBuilderSetMaxAnisotropy, (void*, float));
PROC(Boolean, SamplerInitialize, (void*, const void*));
PROC(void, SamplerFinalize, (void*));
PROC(void, BlendStateSetDefaults, (void*));
PROC(void, BlendStateSetBlendFunc, (void*, int, int, int, int));
PROC(void, BlendStateSetBlendEquation, (void*, int, int));
PROC(void, ColorStateSetDefaults, (void*));
PROC(void, ColorStateSetBlendEnable, (void*, int, Boolean));
PROC(void, ChannelMaskStateSetDefaults, (void*));
PROC(void, ChannelMaskStateSetChannelMask, (void*, int, Boolean, Boolean, Boolean, Boolean));
PROC(void, MultisampleStateSetDefaults, (void*));
PROC(void, MultisampleStateSetMultisampleEnable, (void*, Boolean));
PROC(void, MultisampleStateSetSamples, (void*, int));
PROC(void, PolygonStateSetDefaults, (void*));
PROC(void, PolygonStateSetCullFace, (void*, int));
PROC(void, PolygonStateSetFrontFace, (void*, int));
PROC(void, PolygonStateSetPolygonMode, (void*, int));
PROC(void, DepthStencilStateSetDefaults, (void*));
PROC(void, DepthStencilStateSetDepthTestEnable, (void*, Boolean));
PROC(void, DepthStencilStateSetDepthWriteEnable, (void*, Boolean));
PROC(void, DepthStencilStateSetDepthFunc, (void*, int));
PROC(void, VertexAttribStateSetDefaults, (void*));
PROC(void, VertexAttribStateSetFormat, (void*, int, ptrdiff_t));
PROC(void, VertexAttribStateSetStreamIndex, (void*, int));
PROC(void, VertexStreamStateSetDefaults, (void*));
PROC(void, VertexStreamStateSetStride, (void*, ptrdiff_t));
PROC(void, CommandBufferBindBlendState, (void*, const void*));
PROC(void, CommandBufferBindChannelMaskState, (void*, const void*));
PROC(void, CommandBufferBindColorState, (void*, const void*));
PROC(void, CommandBufferBindMultisampleState, (void*, const void*));
PROC(void, CommandBufferBindPolygonState, (void*, const void*));
PROC(void, CommandBufferBindDepthStencilState, (void*, const void*));
PROC(void, CommandBufferBindVertexAttribState, (void*, int, const void*));
PROC(void, CommandBufferBindVertexStreamState, (void*, int, const void*));
PROC(void, CommandBufferBindProgram, (void*, const void*, int));
PROC(void, CommandBufferBindVertexBuffers, (void*, int, int, const void*));
PROC(void, CommandBufferBindUniformBuffers, (void*, int, int, int, const void*));
PROC(void, CommandBufferBindTexture, (void*, int, int, Handle));
PROC(void, CommandBufferDrawElementsBaseVertex, (void*, int, int, int, Addr, int));
PROC(void, CommandBufferSetStencilValueMask, (void*, int, int));
PROC(void, CommandBufferSetStencilMask, (void*, int, int));
PROC(void, CommandBufferSetStencilRef, (void*, int, int));
PROC(void, CommandBufferSetPolygonOffsetClamp, (void*, float, float, float));
PROC(void, CommandBufferCopyBufferToTexture, (void*, Addr, const void*, const void*, const void*, int));
PROC(void, CommandBufferSetTexturePool, (void*, const void*));
PROC(void, CommandBufferSetSamplerPool, (void*, const void*));
PROC(void, CommandBufferSetShaderScratchMemory, (void*, const void*, ptrdiff_t, size_t));
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

// ---- loading the game's shaders at run time (SSBH SHDR container + GLSLC program binary) ----------------------------------
// Container/binary layout: scripts/nushdb_parse.py (derived from the game's own files; SSBH outer layout from ssbh_lib, MIT).
struct ShaderBlob {
  std::vector<unsigned char> file;
  const unsigned char* control = nullptr;   // control section (the layer reads program/const/data sizes from it)
  const unsigned char* data = nullptr;
  unsigned dataSize = 0, ctrlSize = 0;
  int nvnStage = -1;
};
template <class T> static T Rd(const unsigned char* p) { T v; memcpy(&v, p, sizeof v); return v; }
static bool LoadNushdb(const std::string& path, ShaderBlob* out) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  out->file.resize((size_t)n);
  bool ok = fread(out->file.data(), 1, (size_t)n, f) == (size_t)n;
  fclose(f);
  const unsigned char* b = out->file.data();
  if (!ok || n < 0x40 || memcmp(b, "HBSS", 4) != 0 || memcmp(b + 0x10, "RDHS", 4) != 0) { fprintf(stderr, "%s: not an SSBH SHDR file\n", path.c_str()); return false; }
  const unsigned long long rel = Rd<unsigned long long>(b + 0x18), count = Rd<unsigned long long>(b + 0x20);
  if (count < 1) return false;
  const unsigned char* e = b + 0x18 + rel;                        // first entry
  const unsigned long long binRel = Rd<unsigned long long>(e + 0x10), binSize = Rd<unsigned long long>(e + 0x18);
  const unsigned char* blob = e + 0x10 + binRel;
  if (blob + binSize > b + n || Rd<unsigned>(blob) != 0x19866891u) { fprintf(stderr, "%s: bad program binary\n", path.c_str()); return false; }
  const unsigned nsec = Rd<unsigned>(blob + 0x4C);
  for (unsigned k = 0; k < nsec; k++) {
    const unsigned char* sh = blob + 0x90 * (k + 1);
    if (Rd<unsigned>(sh + 8) != 0) continue;                      // section type 0 = gpucode
    const unsigned off = Rd<unsigned>(sh + 4);
    out->nvnStage = (int)Rd<unsigned>(sh + 0x2C);
    out->ctrlSize = Rd<unsigned>(sh + 0x34); out->dataSize = Rd<unsigned>(sh + 0x38);
    out->control = blob + off;
    out->data = blob + off + Rd<unsigned>(sh + 0x3C);
    return true;
  }
  return false;
}

// ---- tiny matrix helpers (column-major, Vulkan clip space: y down, depth 0..1) -------------------------------------------------
struct M4 { float m[16]; };   // m[col*4+row]
static M4 Ident() { M4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
static M4 Mul(const M4& a, const M4& b) {
  M4 r{};
  for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) { float s = 0; for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k]; r.m[c * 4 + rr] = s; }
  return r;
}
static M4 RotY(float a) { M4 r = Ident(); r.m[0] = cosf(a); r.m[8] = sinf(a); r.m[2] = -sinf(a); r.m[10] = cosf(a); return r; }
static M4 RotX(float a) { M4 r = Ident(); r.m[5] = cosf(a); r.m[9] = -sinf(a); r.m[6] = sinf(a); r.m[10] = cosf(a); return r; }
static M4 Trans(float x, float y, float z) { M4 r = Ident(); r.m[12] = x; r.m[13] = y; r.m[14] = z; return r; }
static M4 Scale(float s) { M4 r = Ident(); r.m[0] = r.m[5] = r.m[10] = s; return r; }
static M4 Persp(float fovY, float aspect, float zn, float zf) {
  const float f = 1.0f / tanf(fovY * 0.5f);
  M4 r{};
  r.m[0] = f / aspect; r.m[5] = -f;                    // -f: clip y points down in Vulkan/NVN upper-left window space
  r.m[10] = zf / (zn - zf); r.m[11] = -1.0f; r.m[14] = zn * zf / (zn - zf);
  return r;
}

struct Vtx { float p[3], uv[2]; };

// ---- checks -----------------------------------------------------------------------------------------------------------------
static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Rgba { int r, g, b, a; };
static bool Near(const unsigned char* p, Rgba e, int tol) {
  return abs(p[0] - e.r) <= tol && abs(p[1] - e.g) <= tol && abs(p[2] - e.b) <= tol && abs(p[3] - e.a) <= tol;
}

int main(int argc, char** argv) {
  bool validation = true;
  std::string shaderDir;
#ifdef NVN_TEST_ROOT
  shaderDir = std::string(NVN_TEST_ROOT) + "/extracted/drp-v262144/resource/nx/resource.drp";
#endif
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--no-validation")) validation = false;
    else if (!strcmp(argv[i], "--shader-dir") && i + 1 < argc) shaderDir = argv[++i];
  }
  ShaderBlob vsb, psb;
  if (!LoadNushdb(shaderDir + "/nuSystemConstantVS.nushdb", &vsb) || !LoadNushdb(shaderDir + "/nuSystemConstantPS.nushdb", &psb)) return 2;

  if (validation) {
    nvnHostSetValidation(1);
    if (!getenv("VK_LAYER_PATH")) {
#ifdef NVN_TEST_LAYER_PATH
      _putenv("VK_LAYER_PATH=" NVN_TEST_LAYER_PATH);
#endif
    }
  }
#ifdef NVN_TEST_ROOT
  if (!getenv("NVN_CACHE_DIR")) _putenv("NVN_CACHE_DIR=" NVN_TEST_ROOT "/build/nvn/cache");
#endif

  LOAD(DeviceInitialize); LOAD(DeviceBuilderSetDefaults); LOAD(DeviceFinalize); LOAD(DeviceGetInteger); LOAD(DeviceGetTextureHandle);
  LOAD(DeviceSetWindowOriginMode); LOAD(DeviceSetDepthMode);
  LOAD(MemoryPoolBuilderSetDevice); LOAD(MemoryPoolBuilderSetDefaults); LOAD(MemoryPoolBuilderSetStorage); LOAD(MemoryPoolBuilderSetFlags);
  LOAD(MemoryPoolInitialize); LOAD(MemoryPoolFinalize); LOAD(MemoryPoolMap);
  LOAD(QueueBuilderSetDevice); LOAD(QueueBuilderSetDefaults); LOAD(QueueBuilderSetFlags); LOAD(QueueBuilderSetCommandMemorySize);
  LOAD(QueueBuilderSetComputeMemorySize); LOAD(QueueBuilderSetControlMemorySize); LOAD(QueueInitialize); LOAD(QueueFinalize);
  LOAD(QueueSubmitCommands); LOAD(QueueFinish);
  LOAD(TextureBuilderSetDevice); LOAD(TextureBuilderSetDefaults); LOAD(TextureBuilderSetFlags); LOAD(TextureBuilderSetTarget);
  LOAD(TextureBuilderSetSize2D); LOAD(TextureBuilderSetSize3D); LOAD(TextureBuilderSetLevels); LOAD(TextureBuilderSetFormat); LOAD(TextureBuilderSetSamples);
  LOAD(TextureBuilderGetStorageSize); LOAD(TextureBuilderGetStorageAlignment); LOAD(TextureBuilderSetStorage); LOAD(TextureInitialize);
  LOAD(TextureFinalize); LOAD(TextureViewSetDefaults); LOAD(TextureViewSetLevels); LOAD(TextureViewSetLayers);
  LOAD(CommandBufferInitialize); LOAD(CommandBufferFinalize); LOAD(CommandBufferSetMemoryCallback);
  LOAD(CommandBufferSetMemoryCallbackData); LOAD(CommandBufferAddCommandMemory); LOAD(CommandBufferAddControlMemory);
  LOAD(CommandBufferBeginRecording); LOAD(CommandBufferEndRecording); LOAD(CommandBufferClearColor);
  LOAD(CommandBufferSetViewports); LOAD(CommandBufferSetScissors); LOAD(CommandBufferSetRenderTargets); LOAD(CommandBufferBarrier);
  LOAD(ProgramInitialize); LOAD(ProgramFinalize); LOAD(ProgramSetShaders);
  LOAD(TexturePoolInitialize); LOAD(TexturePoolFinalize); LOAD(TexturePoolRegisterTexture);
  LOAD(SamplerPoolInitialize); LOAD(SamplerPoolFinalize); LOAD(SamplerPoolRegisterSampler);
  LOAD(BufferBuilderSetDevice); LOAD(BufferBuilderSetDefaults); LOAD(BufferBuilderSetStorage); LOAD(BufferInitialize); LOAD(BufferFinalize);
  LOAD(BufferMap); LOAD(BufferGetAddress); LOAD(BufferFlushMappedRange);
  LOAD(SamplerBuilderSetDevice); LOAD(SamplerBuilderSetDefaults); LOAD(SamplerBuilderSetMinMagFilter); LOAD(SamplerBuilderSetWrapMode);
  LOAD(SamplerInitialize); LOAD(SamplerFinalize);
  LOAD(BlendStateSetDefaults); LOAD(BlendStateSetBlendFunc); LOAD(BlendStateSetBlendEquation); LOAD(ColorStateSetDefaults);
  LOAD(ColorStateSetBlendEnable); LOAD(ChannelMaskStateSetDefaults); LOAD(ChannelMaskStateSetChannelMask);
  LOAD(MultisampleStateSetDefaults); LOAD(MultisampleStateSetMultisampleEnable); LOAD(MultisampleStateSetSamples);
  LOAD(PolygonStateSetDefaults); LOAD(PolygonStateSetCullFace); LOAD(PolygonStateSetFrontFace); LOAD(PolygonStateSetPolygonMode);
  LOAD(DepthStencilStateSetDefaults); LOAD(DepthStencilStateSetDepthTestEnable); LOAD(DepthStencilStateSetDepthWriteEnable);
  LOAD(VertexAttribStateSetDefaults); LOAD(VertexAttribStateSetFormat);
  LOAD(VertexAttribStateSetStreamIndex); LOAD(VertexStreamStateSetDefaults); LOAD(VertexStreamStateSetStride);
  LOAD(CommandBufferBindBlendState); LOAD(CommandBufferBindChannelMaskState); LOAD(CommandBufferBindColorState);
  LOAD(CommandBufferBindMultisampleState); LOAD(CommandBufferBindPolygonState); LOAD(CommandBufferBindDepthStencilState);
  LOAD(CommandBufferBindVertexAttribState); LOAD(CommandBufferBindVertexStreamState); LOAD(CommandBufferBindProgram);
  LOAD(CommandBufferBindVertexBuffers); LOAD(CommandBufferBindUniformBuffers); LOAD(CommandBufferBindTexture);
  LOAD(CommandBufferDrawElementsBaseVertex); LOAD(CommandBufferSetStencilValueMask); LOAD(CommandBufferSetStencilMask);
  LOAD(CommandBufferSetStencilRef); LOAD(CommandBufferSetPolygonOffsetClamp); LOAD(CommandBufferCopyTextureToBuffer);
  LOAD(CommandBufferSetTexturePool); LOAD(CommandBufferSetSamplerPool); LOAD(CommandBufferSetShaderScratchMemory);
  if (g_missing) return 2;

  static Obj device, queue, cmdbuf, cmdPool, dataPool, codePool, descPool, texPoolA, texPoolB, texA, texB, vbuf, ibuf, ubuf, codeBufVs, codeBufPs, sampler, texDesc, sampDesc, prog;
  static Obj blendSt, colorSt, chmaskSt, msSt, polySt, dsSt, attrSt[3], streamSt;
  unsigned char devBuilder[0x40]; memset(devBuilder, 0xCD, sizeof devBuilder);
  DeviceBuilderSetDefaults(devBuilder);
  if (!DeviceInitialize(&device, devBuilder)) { fprintf(stderr, "DeviceInitialize failed\n"); return 1; }
  DeviceSetWindowOriginMode(&device, 1);
  DeviceSetDepthMode(&device, 1);

  unsigned char pb[0x40];
  auto makePool = [&](Obj& pool, size_t bytes, int flags, void** mem) {
    *mem = _aligned_malloc(bytes, 0x1000);
    memset(*mem, 0, bytes);
    MemoryPoolBuilderSetDefaults(pb); MemoryPoolBuilderSetDevice(pb, &device);
    MemoryPoolBuilderSetFlags(pb, flags); MemoryPoolBuilderSetStorage(pb, *mem, bytes);
    if (!MemoryPoolInitialize(&pool, pb)) { fprintf(stderr, "pool init failed\n"); exit(1); }
  };
  void *cmdMem, *dataMem, *codeMem, *descMem;
  const size_t cmdBytes = 0x200000;
  makePool(cmdPool, cmdBytes, 0xa2, &cmdMem);
  makePool(dataPool, 0x100000, 0x22, &dataMem);
  makePool(codePool, 0x20000, 0x62, &codeMem);
  makePool(descPool, 0x10000, 0x22, &descMem);
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

  unsigned char tb[0x80];
  // render-target textures: COMPRESSIBLE (0x8) is the flag the game puts on them
  auto makeTex = [&](Obj& t, Obj* pool, int target, int w, int h, int d, int levels) {
    TextureBuilderSetDefaults(tb); TextureBuilderSetDevice(tb, &device); TextureBuilderSetFlags(tb, 8); TextureBuilderSetTarget(tb, target);
    TextureBuilderSetFormat(tb, 0x25 /*RGBA8*/); TextureBuilderSetSamples(tb, 0); TextureBuilderSetLevels(tb, levels);
    if (target == 1) TextureBuilderSetSize2D(tb, w, h); else TextureBuilderSetSize3D(tb, w, h, d);
    size_t sz = (TextureBuilderGetStorageSize(tb) + 0xfff) & ~(size_t)0xfff;
    (void)TextureBuilderGetStorageAlignment(tb);
    void* mem = _aligned_malloc(sz, 0x1000);
    MemoryPoolBuilderSetDefaults(pb); MemoryPoolBuilderSetDevice(pb, &device);
    MemoryPoolBuilderSetFlags(pb, 0x2b); MemoryPoolBuilderSetStorage(pb, mem, sz);
    if (!MemoryPoolInitialize(pool, pb)) { fprintf(stderr, "tex pool failed\n"); exit(1); }
    TextureBuilderSetStorage(tb, pool, 0);
    if (!TextureInitialize(&t, tb)) { fprintf(stderr, "texture init failed\n"); exit(1); }
  };
  const int TW = 64, LEVELS = 4;
  makeTex(texA, &texPoolA, 1 /*2D*/, TW, TW, 1, LEVELS);        // mip chain: each level is rendered from the one above it
  makeTex(texB, &texPoolB, 4 /*2D_ARRAY*/, 16, 16, 3, 1);      // three layers, rendered one at a time

  // shader program
  auto makeBuffer = [&](Obj& buf, Obj& pool, ptrdiff_t off, size_t size) {
    unsigned char bb[0x40];
    BufferBuilderSetDefaults(bb); BufferBuilderSetDevice(bb, &device); BufferBuilderSetStorage(bb, &pool, off, size);
    if (!BufferInitialize(&buf, bb)) { fprintf(stderr, "buffer init failed\n"); exit(1); }
  };
  memcpy((unsigned char*)codeMem + 0, vsb.data, vsb.dataSize);
  memcpy((unsigned char*)codeMem + 0x1000, psb.data, psb.dataSize);
  makeBuffer(codeBufVs, codePool, 0, 0x1000);
  makeBuffer(codeBufPs, codePool, 0x1000, 0x1000);
  struct ShaderData { Addr data; const void* control; };
  ShaderData sd[2] = {{BufferGetAddress(&codeBufVs), vsb.control}, {BufferGetAddress(&codeBufPs), psb.control}};
  if (!ProgramInitialize(&prog, &device)) { fprintf(stderr, "ProgramInitialize failed\n"); return 1; }
  if (!ProgramSetShaders(&prog, 2, sd)) { fprintf(stderr, "ProgramSetShaders failed (see log; set NVN_LOG=1)\n"); return 1; }

  // one full-target quad (clip space -1..1) with texture coordinates 0..1
  makeBuffer(vbuf, dataPool, 0, 0x1000);
  makeBuffer(ibuf, dataPool, 0x1000, 0x1000);
  makeBuffer(ubuf, dataPool, 0x2000, 0x2000);
  {
    const Vtx quad[4] = {{{-1, -1, 0.5f}, {0, 1}}, {{1, -1, 0.5f}, {1, 1}}, {{1, 1, 0.5f}, {1, 0}}, {{-1, 1, 0.5f}, {0, 0}}};
    const unsigned short idx[6] = {0, 1, 2, 0, 2, 3};
    memcpy(BufferMap(&vbuf), quad, sizeof quad);
    memcpy(BufferMap(&ibuf), idx, sizeof idx);
    unsigned char* ub = static_cast<unsigned char*>(BufferMap(&ubuf));
    const M4 id = Ident();
    memcpy(ub + 0x000 + 128, id.m, 64);                 // nuPerWorldCBuffer: WVP = identity
    const float uvT[8] = {1, 0, 0, 0, 0, 1, 0, 0};      // nuUVTransformCBuffer: identity
    memcpy(ub + 0x200, uvT, sizeof uvT);
    const float diffuse[4] = {1, 1, 1, 1};              // nuPerMaterialCBuffer
    memcpy(ub + 0x300, diffuse, sizeof diffuse);
    BufferFlushMappedRange(&vbuf, 0, 0x1000); BufferFlushMappedRange(&ibuf, 0, 0x1000); BufferFlushMappedRange(&ubuf, 0, 0x2000);
  }

  // sampler (linear, clamp), descriptor pools, one handle per source mip level
  {
    unsigned char sb[0x60];
    SamplerBuilderSetDefaults(sb); SamplerBuilderSetDevice(sb, &device);
    SamplerBuilderSetMinMagFilter(sb, 1, 1);
    SamplerBuilderSetWrapMode(sb, 2, 2, 2);
    if (!SamplerInitialize(&sampler, sb)) { fprintf(stderr, "SamplerInitialize failed\n"); return 1; }
  }
  if (!TexturePoolInitialize(&texDesc, &descPool, 0, 4096) || !SamplerPoolInitialize(&sampDesc, &descPool, 0x8000, 256)) { fprintf(stderr, "pool init failed\n"); return 1; }
  const int kTexId = 300, kSampId = 7;
  SamplerPoolRegisterSampler(&sampDesc, kSampId, &sampler);
  for (int l = 0; l < LEVELS - 1; l++) {   // texture id 300+l samples mip level l only
    unsigned char view[0x10]; TextureViewSetDefaults(view); TextureViewSetLevels(view, l, 1);
    TexturePoolRegisterTexture(&texDesc, kTexId + l, &texA, view);
  }

  // state objects
  BlendStateSetDefaults(&blendSt); BlendStateSetBlendFunc(&blendSt, 2, 1, 2, 1); BlendStateSetBlendEquation(&blendSt, 1, 1);
  ColorStateSetDefaults(&colorSt); ColorStateSetBlendEnable(&colorSt, 0, 0);
  ChannelMaskStateSetDefaults(&chmaskSt); ChannelMaskStateSetChannelMask(&chmaskSt, 0, 1, 1, 1, 1);
  MultisampleStateSetDefaults(&msSt); MultisampleStateSetMultisampleEnable(&msSt, 1); MultisampleStateSetSamples(&msSt, 0);
  PolygonStateSetDefaults(&polySt); PolygonStateSetCullFace(&polySt, 0 /*NONE*/); PolygonStateSetFrontFace(&polySt, 1); PolygonStateSetPolygonMode(&polySt, 2);
  DepthStencilStateSetDefaults(&dsSt); DepthStencilStateSetDepthTestEnable(&dsSt, 0); DepthStencilStateSetDepthWriteEnable(&dsSt, 0);
  VertexAttribStateSetDefaults(&attrSt[0]); VertexAttribStateSetFormat(&attrSt[0], 0x22 /*RGB32F*/, 0); VertexAttribStateSetStreamIndex(&attrSt[0], 0);
  VertexAttribStateSetDefaults(&attrSt[1]);
  VertexAttribStateSetDefaults(&attrSt[2]); VertexAttribStateSetFormat(&attrSt[2], 0x16 /*RG32F*/, 12); VertexAttribStateSetStreamIndex(&attrSt[2], 0);
  VertexStreamStateSetDefaults(&streamSt); VertexStreamStateSetStride(&streamSt, sizeof(Vtx));
  unsigned char attrArr[3][4]; for (int i = 0; i < 3; i++) memcpy(attrArr[i], &attrSt[i], 4);

  // ---- test 1 + 2: one command list ------------------------------------------------------------------------------------------------
  //  mip chain: level 0 cleared, then levels 1..3 each drawn by sampling the level above while rendering into the level below
  //  layers: layers 0 and 1 cleared to different colors, layer 2 cleared then masked-cleared (blue only)
  const Addr vbBase = BufferGetAddress(&vbuf), ibBase = BufferGetAddress(&ibuf), ubBase = BufferGetAddress(&ubuf);
  const Addr outBase = vbBase - 0 + 0x40000 - 0x40000;   // (unused marker)
  (void)outBase;
  unsigned char* outMem = static_cast<unsigned char*>(dataMem) + 0x10000;
  const Addr outAddr = BufferGetAddress(&vbuf) + 0x10000;   // data pool: vertex buffer starts at pool offset 0
  struct Range { Addr a; size_t s; };
  const float c0[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  CommandBufferBeginRecording(&cmdbuf);
  CommandBufferSetShaderScratchMemory(&cmdbuf, &dataPool, 0, 0);
  CommandBufferSetTexturePool(&cmdbuf, &texDesc);
  CommandBufferSetSamplerPool(&cmdbuf, &sampDesc);
  for (int l = 0; l < LEVELS; l++) {
    unsigned char rv[0x10]; TextureViewSetDefaults(rv); TextureViewSetLevels(rv, l, 1);
    void* rt[1] = {&texA}; const void* vws[1] = {rv};
    CommandBufferSetRenderTargets(&cmdbuf, 1, rt, vws, nullptr, nullptr);
    const int sz = TW >> l;
    const float vp[4] = {0, 0, (float)sz, (float)sz}; const int sc[4] = {0, 0, sz, sz};
    CommandBufferSetViewports(&cmdbuf, 0, 1, vp);
    CommandBufferSetScissors(&cmdbuf, 0, 1, sc);
    if (l == 0) { CommandBufferClearColor(&cmdbuf, 0, c0, 0xf); continue; }
    CommandBufferBindBlendState(&cmdbuf, &blendSt);
    CommandBufferBindColorState(&cmdbuf, &colorSt);
    CommandBufferBindChannelMaskState(&cmdbuf, &chmaskSt);
    CommandBufferBindMultisampleState(&cmdbuf, &msSt);
    CommandBufferBindDepthStencilState(&cmdbuf, &dsSt);
    CommandBufferBindPolygonState(&cmdbuf, &polySt);
    CommandBufferSetStencilValueMask(&cmdbuf, 3, 0xff); CommandBufferSetStencilMask(&cmdbuf, 3, 0xff); CommandBufferSetStencilRef(&cmdbuf, 3, 0);
    CommandBufferSetPolygonOffsetClamp(&cmdbuf, 0.0f, 0.0f, 0.0f);
    CommandBufferBindProgram(&cmdbuf, &prog, 0x3);
    CommandBufferBindTexture(&cmdbuf, 1, 0, DeviceGetTextureHandle(&device, kTexId + (l - 1), kSampId));
    CommandBufferBindVertexAttribState(&cmdbuf, 3, attrArr);
    CommandBufferBindVertexStreamState(&cmdbuf, 1, &streamSt);
    const Range vbr = {vbBase, 0x1000};
    CommandBufferBindVertexBuffers(&cmdbuf, 0, 1, &vbr);
    { const Range r1 = {ubBase + 0x000, 0x100}; CommandBufferBindUniformBuffers(&cmdbuf, 0, 1, 1, &r1); }
    { const Range r6 = {ubBase + 0x200, 0x60}; CommandBufferBindUniformBuffers(&cmdbuf, 0, 6, 1, &r6); }
    { const Range r6 = {ubBase + 0x300, 0x10}; CommandBufferBindUniformBuffers(&cmdbuf, 1, 6, 1, &r6); }
    CommandBufferDrawElementsBaseVertex(&cmdbuf, 4 /*TRIANGLES*/, 1 /*U16*/, 6, ibBase, 0);
  }
  // layers of texB
  {
    const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, zero[4] = {0, 0, 0, 0}, blue[4] = {0, 0, 1, 1};
    const float* colors[3] = {red, green, zero};
    const float vp[4] = {0, 0, 16, 16}; const int sc[4] = {0, 0, 16, 16};
    for (int ly = 0; ly < 3; ly++) {
      unsigned char rv[0x10]; TextureViewSetDefaults(rv); TextureViewSetLayers(rv, ly, 1);
      void* rt[1] = {&texB}; const void* vws[1] = {rv};
      CommandBufferSetRenderTargets(&cmdbuf, 1, rt, vws, nullptr, nullptr);
      CommandBufferSetViewports(&cmdbuf, 0, 1, vp); CommandBufferSetScissors(&cmdbuf, 0, 1, sc);
      CommandBufferClearColor(&cmdbuf, 0, colors[ly], 0xf);
      if (ly == 2) CommandBufferClearColor(&cmdbuf, 0, blue, 0x4);   // masked: only blue
    }
  }
  CommandBufferBarrier(&cmdbuf, 0x28);
  // read everything back: levels of texA at out+0x0.., layers of texB after
  size_t off = 0, levelOff[LEVELS], layerOff[3];
  for (int l = 0; l < LEVELS; l++) {
    unsigned char rv[0x10]; TextureViewSetDefaults(rv); TextureViewSetLevels(rv, l, 1);
    const int region[6] = {0, 0, 0, TW >> l, TW >> l, 1};
    levelOff[l] = off;
    CommandBufferCopyTextureToBuffer(&cmdbuf, &texA, rv, region, outAddr + off, 0);
    off += (size_t)(TW >> l) * (TW >> l) * 4;
  }
  for (int ly = 0; ly < 3; ly++) {
    unsigned char rv[0x10]; TextureViewSetDefaults(rv); TextureViewSetLayers(rv, ly, 1);
    const int region[6] = {0, 0, 0, 16, 16, 1};
    layerOff[ly] = off;
    CommandBufferCopyTextureToBuffer(&cmdbuf, &texB, rv, region, outAddr + off, 0);
    off += 16 * 16 * 4;
  }
  Handle h = CommandBufferEndRecording(&cmdbuf);
  QueueSubmitCommands(&queue, 1, &h);
  QueueFinish(&queue);

  const Rgba exp0 = {64, 128, 191, 255};
  for (int l = 0; l < LEVELS; l++) {
    const int n = (TW >> l) * (TW >> l);
    int bad = 0;
    for (int i = 0; i < n; i++) if (!Near(outMem + levelOff[l] + i * 4, exp0, 4)) bad++;
    CHECK(bad == 0, "mip level %d: %d of %d texels differ from the level-0 color (a draw that samples the level above was skipped or hit the wrong level); first texel %d,%d,%d,%d",
          l, bad, n, outMem[levelOff[l]], outMem[levelOff[l] + 1], outMem[levelOff[l] + 2], outMem[levelOff[l] + 3]);
  }
  const Rgba layerExp[3] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 0}};
  for (int ly = 0; ly < 3; ly++) {
    int bad = 0;
    for (int i = 0; i < 256; i++) if (!Near(outMem + layerOff[ly] + i * 4, layerExp[ly], 1)) bad++;
    CHECK(bad == 0, "array layer %d: %d of 256 texels wrong (ClearColor must touch only the bound layer); first texel %d,%d,%d,%d",
          ly, bad, outMem[layerOff[ly]], outMem[layerOff[ly] + 1], outMem[layerOff[ly] + 2], outMem[layerOff[ly] + 3]);
  }
  printf("mip chain render-and-sample (%d levels) and per-layer clears: %s\n", LEVELS, g_fail ? "FAIL" : "ok");

  // ---- test 3: view types ---------------------------------------------------------------------------------------------------------------
  // The translated shader declares a dimension (SamplerType: low byte 1 = 1D, 3 = 2D, 4 = 3D, 5 = cube, bit 8 = array); the view the layer
  // binds must have exactly that type, or none when the texture cannot be viewed that way (a typed dummy is bound instead).
  {
    using namespace nvn;
    static Obj tex2d, texArr, texCube, tex3d, p2d, pArr, pCube, p3d;
    auto mk = [&](Obj& t, Obj& pool, int target, int w, int h, int d, int levels) {
      TextureBuilderSetDefaults(tb); TextureBuilderSetDevice(tb, &device); TextureBuilderSetFlags(tb, 0); TextureBuilderSetTarget(tb, target);
      TextureBuilderSetFormat(tb, 0x25); TextureBuilderSetSamples(tb, 0); TextureBuilderSetLevels(tb, levels);
      if (target == 1 || target == 8) TextureBuilderSetSize2D(tb, w, h); else TextureBuilderSetSize3D(tb, w, h, d);
      size_t sz = (TextureBuilderGetStorageSize(tb) + 0xfff) & ~(size_t)0xfff;
      void* mem = _aligned_malloc(sz, 0x1000);
      MemoryPoolBuilderSetDefaults(pb); MemoryPoolBuilderSetDevice(pb, &device); MemoryPoolBuilderSetFlags(pb, 0x2b); MemoryPoolBuilderSetStorage(pb, mem, sz);
      if (!MemoryPoolInitialize(&pool, pb)) { fprintf(stderr, "pool failed\n"); exit(1); }
      TextureBuilderSetStorage(tb, &pool, 0);
      if (!TextureInitialize(&t, tb)) { fprintf(stderr, "texture failed\n"); exit(1); }
    };
    mk(tex2d, p2d, 1, 32, 32, 1, 3);
    mk(texArr, pArr, 4, 32, 32, 4, 2);
    mk(texCube, pCube, 8, 32, 32, 6, 2);
    mk(tex3d, p3d, 2, 16, 16, 8, 1);
    auto* t2 = Impl<TextureImpl>(&tex2d, MAGIC_TEXTURE);
    auto* ta = Impl<TextureImpl>(&texArr, MAGIC_TEXTURE);
    auto* tc = Impl<TextureImpl>(&texCube, MAGIC_TEXTURE);
    auto* t3 = Impl<TextureImpl>(&tex3d, MAGIC_TEXTURE);
    const int S2D = 3, S2DA = 3 | 0x100, SCUBE = 5, SCUBEA = 5 | 0x100, S3D = 4, S1D = 1;
    struct Case { TextureImpl* t; const char* name; int shader; VkImageViewType expect; bool ok; };
    const Case cases[] = {
        {t2, "2D texture, 2D shader", S2D, VK_IMAGE_VIEW_TYPE_2D, true},
        {t2, "2D texture, 2D-array shader", S2DA, VK_IMAGE_VIEW_TYPE_2D_ARRAY, true},      // the game's vp_t_tcb_14: array sampler on a plain 2D texture
        {t2, "2D texture, cube shader", SCUBE, VK_IMAGE_VIEW_TYPE_CUBE, false},
        {t2, "2D texture, 3D shader", S3D, VK_IMAGE_VIEW_TYPE_3D, false},
        {t2, "2D texture, 1D shader", S1D, VK_IMAGE_VIEW_TYPE_1D, false},
        {ta, "2D-array texture, 2D shader", S2D, VK_IMAGE_VIEW_TYPE_2D, true},
        {ta, "2D-array texture, 2D-array shader", S2DA, VK_IMAGE_VIEW_TYPE_2D_ARRAY, true},
        {tc, "cube texture, cube shader", SCUBE, VK_IMAGE_VIEW_TYPE_CUBE, true},
        {tc, "cube texture, 2D shader", S2D, VK_IMAGE_VIEW_TYPE_2D, true},
        {tc, "cube texture, 2D-array shader", S2DA, VK_IMAGE_VIEW_TYPE_2D_ARRAY, true},
        {tc, "cube texture, cube-array shader", SCUBEA, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, true},
        {t3, "3D texture, 3D shader", S3D, VK_IMAGE_VIEW_TYPE_3D, true},
        {t3, "3D texture, 2D shader", S2D, VK_IMAGE_VIEW_TYPE_2D, false},
    };
    for (const Case& c : cases) {
      VkImageView v = c.t->View(nullptr, true, c.shader);
      CHECK((v != VK_NULL_HANDLE) == c.ok, "view type: %s: view %s, expected %s", c.name, v ? "created" : "refused", c.ok ? "created" : "refused");
      if (v) {
        CHECK(c.t->ViewTypeOf(v) == c.expect, "view type: %s: got type %d, expected %d", c.name, (int)c.t->ViewTypeOf(v), (int)c.expect);
        CHECK(c.t->View(nullptr, true, c.shader) == v, "view type: %s: second request must hit the view cache", c.name);
      }
    }
    // an explicit level window (sampling mip 1 of a 3-level texture) and a layer window on the array texture
    { unsigned char vw[0x10]; TextureViewSetDefaults(vw); TextureViewSetLevels(vw, 1, 1);
      VkImageView v = t2->View(reinterpret_cast<const TexView*>(vw), true, S2D);
      CHECK(v && t2->ViewTypeOf(v) == VK_IMAGE_VIEW_TYPE_2D, "view type: level window view");
      CHECK(v != t2->View(nullptr, true, S2D), "view type: a level window must not share the whole-texture view"); }
    { unsigned char vw[0x10]; TextureViewSetDefaults(vw); TextureViewSetLayers(vw, 2, 1);
      VkImageView v = ta->View(reinterpret_cast<const TexView*>(vw), true, S2D);
      CHECK(v && ta->ViewTypeOf(v) == VK_IMAGE_VIEW_TYPE_2D, "view type: layer window view");
      SubRange r = ta->RangeOf(reinterpret_cast<const TexView*>(vw));
      CHECK(r.yb == 2 && r.yn == 1 && r.lb == 0 && r.ln == 2, "view type: RangeOf layer window (%u,%u,%u,%u)", r.lb, r.ln, r.yb, r.yn); }
    // an attachment view is one level, one layer, plain 2D
    { unsigned char vw[0x10]; TextureViewSetDefaults(vw); TextureViewSetLevels(vw, 2, 1);
      SubRange r{}; VkImageView v = t2->RenderView(reinterpret_cast<const TexView*>(vw), &r);
      CHECK(v && t2->ViewTypeOf(v) == VK_IMAGE_VIEW_TYPE_2D && r.lb == 2 && r.ln == 1 && r.yn == 1, "view type: attachment view of level 2"); }
    // attachment overlap: the sampled window of a mip chain that excludes the level being rendered
    printf("view types: %s\n", g_fail ? "FAIL" : "ok");
    for (Obj* t : {&tex2d, &texArr, &texCube, &tex3d}) TextureFinalize(t);
    for (Obj* p : {&p2d, &pArr, &pCube, &p3d}) MemoryPoolFinalize(p);
  }

  // teardown
  ProgramFinalize(&prog);
  SamplerPoolFinalize(&sampDesc); TexturePoolFinalize(&texDesc);
  SamplerFinalize(&sampler);
  TextureFinalize(&texA); TextureFinalize(&texB);
  for (Obj* b : {&vbuf, &ibuf, &ubuf, &codeBufVs, &codeBufPs}) BufferFinalize(b);
  MemoryPoolFinalize(&texPoolA); MemoryPoolFinalize(&texPoolB);
  MemoryPoolFinalize(&descPool); MemoryPoolFinalize(&codePool); MemoryPoolFinalize(&dataPool);
  QueueFinalize(&queue);
  CommandBufferFinalize(&cmdbuf);
  MemoryPoolFinalize(&cmdPool);
  DeviceFinalize(&device);
  unsigned errs = nvnHostValidationErrors(), warns = nvnHostValidationWarnings();
  printf("validation: errors=%u warnings=%u (layers %s)\n", errs, warns, validation ? "on" : "off");
  printf("%s\n", (errs || warns || g_fail) ? "mip_test: FAIL" : "mip_test: PASS");
  return (errs || warns || g_fail) ? 1 : 0;
}
