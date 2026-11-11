// SPDX-License-Identifier: MIT
// Milestone 2 test of the NVN->Vulkan layer: two textured, depth-tested, rotating cubes drawn through NVN entry
// points with a real game shader pair (vdkEfxSystemConstantVS / vdkEfxSystemConstantPS), loaded at run time from the
// extracted game data (no game data is stored in this repository). The mesh and the texture are procedural.
//
// NVN call order follows the game's nn::gfx backend: device/queue/command-buffer setup as in clear_test.cpp, then
//   per resource: BufferBuilder*/BufferInitialize, TextureBuilder*/TextureInitialize, SamplerBuilder*/SamplerInitialize,
//   TexturePoolInitialize/RegisterTexture, SamplerPoolInitialize/RegisterSampler, DeviceGetTextureHandle;
//   per frame (cf. the sampler-bind loop at 0x915e80 and the draw path): SetRenderTargets, viewports/scissors, clear,
//   Bind*State, BindProgram, SetTexturePool/SetSamplerPool, BindTexture, BindVertexAttribState/StreamState,
//   BindVertexBuffers, BindUniformBuffers, DrawElementsBaseVertex, Barrier, submit, fence, present.
// Shader interface used (from the reflection of those two shaders, analysis/shaders): VS attribute 0 = vec3 position,
// attribute 2 = vec2 texcoord; VS UBO binding 1 nuPerWorldCBuffer (WVP, four vec4 columns at +128), VS UBO binding 6
// nuUVTransformCBuffer (two vec4 rows), PS UBO binding 6 nuPerMaterialCBuffer (diffuse vec4), PS sampler binding 0.
//
// Usage: mesh_test.exe [--frames N] [--uncapped] [--screenshot out.bmp] [--no-validation] [--hidden] [--shader-dir DIR]
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

int main(int argc, char** argv) {
  double pace = 60.0; int frames = 300; bool uncapped = false, validation = true, hidden = false; const char* shot = nullptr;
  std::string shaderDir;
#ifdef NVN_TEST_ROOT
  shaderDir = std::string(NVN_TEST_ROOT) + "/extracted/drp-v262144/resource/nx/resource.drp";
#endif
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--uncapped")) uncapped = true;
    else if (!strcmp(argv[i], "--no-validation")) validation = false;
    else if (!strcmp(argv[i], "--hidden")) hidden = true;
    else if (!strcmp(argv[i], "--shader-dir") && i + 1 < argc) shaderDir = argv[++i];
    else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
  }
  SetProcessDPIAware();
  const int W = 1280, H = 720;

  // ---- shaders from the extracted game data --------------------------------------------------------------------------------
  ShaderBlob vsb, psb;
  if (!LoadNushdb(shaderDir + "/nuSystemConstantVS.nushdb", &vsb) || !LoadNushdb(shaderDir + "/nuSystemConstantPS.nushdb", &psb)) return 2;
  printf("shaders: VS stage %d data 0x%x ctrl 0x%x | PS stage %d data 0x%x ctrl 0x%x\n", vsb.nvnStage, vsb.dataSize, vsb.ctrlSize, psb.nvnStage, psb.dataSize, psb.ctrlSize);

  // ---- window ---------------------------------------------------------------------------------------------------------------
  WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"NvnMeshTest";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  RECT r{0, 0, W, H}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"NVN mesh test", (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_VISIBLE,
                            100, 100, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
  if (hidden) ShowWindow(hwnd, SW_HIDE);
  else SetWindowPos(hwnd, HWND_TOPMOST, 100, 100, 0, 0, SWP_NOSIZE);

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
  nvnHostSetFramePacing(pace);

  Load(DeviceGetProcAddress, "nvnDeviceGetProcAddress");
  LOAD(DeviceInitialize); LOAD(DeviceBuilderSetDefaults); LOAD(DeviceFinalize); LOAD(DeviceGetInteger); LOAD(DeviceGetTextureHandle);
  LOAD(DeviceSetWindowOriginMode); LOAD(DeviceSetDepthMode); LOAD(DeviceRegisterFastClearColor); LOAD(DeviceRegisterFastClearDepth);
  LOAD(MemoryPoolBuilderSetDevice); LOAD(MemoryPoolBuilderSetDefaults); LOAD(MemoryPoolBuilderSetStorage); LOAD(MemoryPoolBuilderSetFlags);
  LOAD(MemoryPoolInitialize); LOAD(MemoryPoolFinalize); LOAD(MemoryPoolMap);
  LOAD(QueueBuilderSetDevice); LOAD(QueueBuilderSetDefaults); LOAD(QueueBuilderSetFlags); LOAD(QueueBuilderSetCommandMemorySize);
  LOAD(QueueBuilderSetComputeMemorySize); LOAD(QueueBuilderSetControlMemorySize); LOAD(QueueInitialize); LOAD(QueueFinalize);
  LOAD(QueueSubmitCommands); LOAD(QueueFlush); LOAD(QueueFinish); LOAD(QueuePresentTexture); LOAD(QueueFenceSync);
  LOAD(SyncInitialize); LOAD(SyncFinalize); LOAD(SyncWait);
  LOAD(WindowBuilderSetDevice); LOAD(WindowBuilderSetDefaults); LOAD(WindowBuilderSetNativeWindow); LOAD(WindowBuilderSetTextures);
  LOAD(WindowInitialize); LOAD(WindowFinalize); LOAD(WindowAcquireTexture); LOAD(WindowSetPresentInterval); LOAD(WindowSetCrop);
  LOAD(TextureBuilderSetDevice); LOAD(TextureBuilderSetDefaults); LOAD(TextureBuilderSetFlags); LOAD(TextureBuilderSetTarget);
  LOAD(TextureBuilderSetSize2D); LOAD(TextureBuilderSetLevels); LOAD(TextureBuilderSetFormat); LOAD(TextureBuilderSetSamples);
  LOAD(TextureBuilderGetStorageSize); LOAD(TextureBuilderGetStorageAlignment); LOAD(TextureBuilderSetStorage); LOAD(TextureInitialize);
  LOAD(TextureFinalize); LOAD(TextureGetWidth); LOAD(TextureGetHeight); LOAD(TextureViewSetDefaults); LOAD(TextureViewSetLevels);
  LOAD(CommandBufferInitialize); LOAD(CommandBufferFinalize); LOAD(CommandBufferSetMemoryCallback);
  LOAD(CommandBufferSetMemoryCallbackData); LOAD(CommandBufferAddCommandMemory); LOAD(CommandBufferAddControlMemory);
  LOAD(CommandBufferBeginRecording); LOAD(CommandBufferEndRecording); LOAD(CommandBufferClearColor); LOAD(CommandBufferClearDepthStencil);
  LOAD(CommandBufferSetViewports); LOAD(CommandBufferSetScissors); LOAD(CommandBufferSetRenderTargets); LOAD(CommandBufferBarrier);
  LOAD(ProgramInitialize); LOAD(ProgramFinalize); LOAD(ProgramSetShaders);
  LOAD(TexturePoolInitialize); LOAD(TexturePoolFinalize); LOAD(TexturePoolRegisterTexture);
  LOAD(SamplerPoolInitialize); LOAD(SamplerPoolFinalize); LOAD(SamplerPoolRegisterSampler);
  LOAD(BufferBuilderSetDevice); LOAD(BufferBuilderSetDefaults); LOAD(BufferBuilderSetStorage); LOAD(BufferInitialize); LOAD(BufferFinalize);
  LOAD(BufferMap); LOAD(BufferGetAddress); LOAD(BufferFlushMappedRange); LOAD(BufferGetMemoryPool); LOAD(BufferGetMemoryOffset); LOAD(BufferGetSize);
  LOAD(SamplerBuilderSetDevice); LOAD(SamplerBuilderSetDefaults); LOAD(SamplerBuilderSetMinMagFilter); LOAD(SamplerBuilderSetWrapMode);
  LOAD(SamplerBuilderSetMaxAnisotropy); LOAD(SamplerInitialize); LOAD(SamplerFinalize);
  LOAD(BlendStateSetDefaults); LOAD(BlendStateSetBlendFunc); LOAD(BlendStateSetBlendEquation); LOAD(ColorStateSetDefaults);
  LOAD(ColorStateSetBlendEnable); LOAD(ChannelMaskStateSetDefaults); LOAD(ChannelMaskStateSetChannelMask);
  LOAD(MultisampleStateSetDefaults); LOAD(MultisampleStateSetMultisampleEnable); LOAD(MultisampleStateSetSamples);
  LOAD(PolygonStateSetDefaults); LOAD(PolygonStateSetCullFace); LOAD(PolygonStateSetFrontFace); LOAD(PolygonStateSetPolygonMode);
  LOAD(DepthStencilStateSetDefaults); LOAD(DepthStencilStateSetDepthTestEnable); LOAD(DepthStencilStateSetDepthWriteEnable);
  LOAD(DepthStencilStateSetDepthFunc); LOAD(VertexAttribStateSetDefaults); LOAD(VertexAttribStateSetFormat);
  LOAD(VertexAttribStateSetStreamIndex); LOAD(VertexStreamStateSetDefaults); LOAD(VertexStreamStateSetStride);
  LOAD(CommandBufferBindBlendState); LOAD(CommandBufferBindChannelMaskState); LOAD(CommandBufferBindColorState);
  LOAD(CommandBufferBindMultisampleState); LOAD(CommandBufferBindPolygonState); LOAD(CommandBufferBindDepthStencilState);
  LOAD(CommandBufferBindVertexAttribState); LOAD(CommandBufferBindVertexStreamState); LOAD(CommandBufferBindProgram);
  LOAD(CommandBufferBindVertexBuffers); LOAD(CommandBufferBindUniformBuffers); LOAD(CommandBufferBindTexture);
  LOAD(CommandBufferDrawElementsBaseVertex); LOAD(CommandBufferSetStencilValueMask); LOAD(CommandBufferSetStencilMask);
  LOAD(CommandBufferSetStencilRef); LOAD(CommandBufferSetPolygonOffsetClamp); LOAD(CommandBufferCopyBufferToTexture);
  LOAD(CommandBufferSetTexturePool); LOAD(CommandBufferSetSamplerPool); LOAD(CommandBufferSetShaderScratchMemory);
  if (g_missing) return 2;
  printf("registered NVN entry points: %d\n", nvnHostProcCount());

  // ---- device, queue, command buffer (as clear_test.cpp / 0x90d590) ------------------------------------------------------------
  static Obj device, queue, cmdbuf, window, acquireSync, cmdPool, dataPool, codePool, texStoragePool, descPool, texPool[4];
  static Obj tex[3], depthTex, meshTex, vbuf, ibuf, ubuf, codeBufVs, codeBufPs, sampler, texDesc, sampDesc, prog;
  static Obj blendSt, colorSt, chmaskSt, msSt, polySt, polyNoCullSt, dsSt, attrSt[3], streamSt;
  unsigned char devBuilder[0x40]; memset(devBuilder, 0xCD, sizeof devBuilder);
  DeviceBuilderSetDefaults(devBuilder);
  if (!DeviceInitialize(&device, devBuilder)) { fprintf(stderr, "DeviceInitialize failed\n"); return 1; }
  int infos[0x60];
  for (int i = 0; i < 0x60; i++) DeviceGetInteger(&device, i, &infos[i]);
  DeviceSetWindowOriginMode(&device, 1);
  DeviceSetDepthMode(&device, 1);
  DeviceRegisterFastClearDepth(&device, 1.0f);
  const float fc[4] = {0, 0, 0, 0};
  DeviceRegisterFastClearColor(&device, fc, 0x38);
  const int uboAlign = infos[4] > 0 ? infos[4] : 256;

  unsigned char pb[0x40];
  auto makePool = [&](Obj& pool, size_t bytes, int flags, void** mem) {
    *mem = _aligned_malloc(bytes, 0x1000);
    memset(*mem, 0, bytes);
    MemoryPoolBuilderSetDefaults(pb); MemoryPoolBuilderSetDevice(pb, &device);
    MemoryPoolBuilderSetFlags(pb, flags); MemoryPoolBuilderSetStorage(pb, *mem, bytes);
    if (!MemoryPoolInitialize(&pool, pb)) { fprintf(stderr, "pool init failed\n"); exit(1); }
  };
  void *cmdMem, *dataMem, *codeMem, *texMem, *descMem;
  const size_t cmdBytes = 0x200000;
  makePool(cmdPool, cmdBytes, 0xa2, &cmdMem);
  makePool(dataPool, 0x100000, 0x22, &dataMem);      // CPU_UNCACHED | GPU_CACHED: vertex, index, uniform, upload data
  makePool(codePool, 0x20000, 0x62, &codeMem);       // + SHADER_CODE
  makePool(descPool, 0x10000, 0x22, &descMem);       // backing of the texture/sampler descriptor pools

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

  // ---- swapchain + depth ----------------------------------------------------------------------------------------------------------
  unsigned char tb[0x80];
  auto makeTex = [&](Obj& t, Obj* pool, int fmt, int flags, int w, int h, int levels) {
    TextureBuilderSetDefaults(tb); TextureBuilderSetDevice(tb, &device); TextureBuilderSetFlags(tb, flags); TextureBuilderSetTarget(tb, 1);
    TextureBuilderSetFormat(tb, fmt); TextureBuilderSetSamples(tb, 0); TextureBuilderSetLevels(tb, levels); TextureBuilderSetSize2D(tb, w, h);
    size_t sz = (TextureBuilderGetStorageSize(tb) + 0xfff) & ~(size_t)0xfff;
    (void)TextureBuilderGetStorageAlignment(tb);
    void* mem = _aligned_malloc(sz, 0x1000);
    MemoryPoolBuilderSetDefaults(pb); MemoryPoolBuilderSetDevice(pb, &device);
    MemoryPoolBuilderSetFlags(pb, 0x2b); MemoryPoolBuilderSetStorage(pb, mem, sz);
    if (!MemoryPoolInitialize(pool, pb)) { fprintf(stderr, "tex pool failed\n"); exit(1); }
    TextureBuilderSetStorage(tb, pool, 0);
    if (!TextureInitialize(&t, tb)) { fprintf(stderr, "texture init failed\n"); exit(1); }
  };
  void* texPtrs[3];
  for (int i = 0; i < 3; i++) { makeTex(tex[i], &texPool[i], 0x38 /*RGBA8_SRGB*/, 1 /*DISPLAY*/, W, H, 1); texPtrs[i] = &tex[i]; }
  makeTex(depthTex, &texPool[3], 0x35 /*DEPTH24_STENCIL8*/, 0, W, H, 1);
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

  // ---- shader program: data sections in a SHADER_CODE pool, control sections stay in host memory ------------------------------
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

  // ---- mesh: 24 vertices / 36 indices per cube; the second cube is the same data drawn with a base vertex ------------------------
  std::vector<Vtx> verts;
  std::vector<unsigned short> idx;
  {
    const float N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const float U[6][3] = {{0, 1, 0}, {0, 0, 1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const float V[6][3] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 0, 0}};   // u x v = n: counter-clockwise seen from outside
    const float sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1}, tu[4] = {0, 1, 1, 0}, tv[4] = {1, 1, 0, 0};
    for (int f = 0; f < 6; f++) {
      for (int k = 0; k < 4; k++) {
        Vtx v{};
        for (int a = 0; a < 3; a++) v.p[a] = N[f][a] + sx[k] * U[f][a] + sy[k] * V[f][a];
        v.uv[0] = tu[k]; v.uv[1] = tv[k];
        verts.push_back(v);
      }
      const unsigned short b = (unsigned short)(f * 4);
      for (unsigned short i : {0, 1, 2, 0, 2, 3}) idx.push_back((unsigned short)(b + i));
    }
  }
  const int nVerts = (int)verts.size();
  makeBuffer(vbuf, dataPool, 0, 0x4000);
  makeBuffer(ibuf, dataPool, 0x4000, 0x1000);
  makeBuffer(ubuf, dataPool, 0x8000, 0x4000);
  {
    auto* vp = static_cast<Vtx*>(BufferMap(&vbuf));
    memcpy(vp, verts.data(), verts.size() * sizeof(Vtx));
    memcpy(vp + nVerts, verts.data(), verts.size() * sizeof(Vtx));   // second cube's copy at base vertex 24
    memcpy(BufferMap(&ibuf), idx.data(), idx.size() * 2);
    BufferFlushMappedRange(&vbuf, 0, 0x4000); BufferFlushMappedRange(&ibuf, 0, 0x1000);
  }

  // ---- procedural texture: 256x256 RGBA8 with a full mip chain, uploaded through CopyBufferToTexture ---------------------------
  const int TW = 256, TH = 256, LEVELS = 9;
  std::vector<std::vector<unsigned char>> mips(LEVELS);
  {
    mips[0].resize((size_t)TW * TH * 4);
    for (int y = 0; y < TH; y++)
      for (int x = 0; x < TW; x++) {
        const bool chk = ((x / 32) + (y / 32)) & 1;
        const float gx = x / (float)TW, gy = y / (float)TH;
        unsigned char* p = &mips[0][((size_t)y * TW + x) * 4];
        p[0] = (unsigned char)(chk ? 230 : 40 + 80 * gx);
        p[1] = (unsigned char)(chk ? 150 + 60 * gy : 60);
        p[2] = (unsigned char)(chk ? 40 : 200 - 100 * gy);
        p[3] = 255;
        const int cx = x - TW / 2, cy = y - TH / 2, rr = cx * cx + cy * cy;
        if (rr > 90 * 90 && rr < 100 * 100) { p[0] = 250; p[1] = 250; p[2] = 250; }   // a ring, so orientation is visible
      }
    for (int l = 1; l < LEVELS; l++) {
      const int w = std::max(TW >> l, 1), h = std::max(TH >> l, 1), pw = std::max(TW >> (l - 1), 1);
      mips[l].resize((size_t)w * h * 4);
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          for (int c = 0; c < 4; c++) {
            int s = 0;
            for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) s += mips[l - 1][((size_t)(2 * y + dy) * pw + 2 * x + dx) * 4 + c];
            mips[l][((size_t)y * w + x) * 4 + c] = (unsigned char)(s / 4);
          }
    }
  }
  {
    // upload staging lives in the data pool at 0xC000.. (levels packed back to back)
    size_t total = 0; for (auto& m : mips) total += m.size();
    unsigned char* st = static_cast<unsigned char*>(MemoryPoolMap(&dataPool)) + 0xC000;
    (void)total;
    size_t off = 0;
    for (auto& m : mips) { memcpy(st + off, m.data(), m.size()); off += m.size(); }
  }
  {
    // texture storage in its own (GPU) pool, then the texture object
    unsigned char* pbb = pb;
    TextureBuilderSetDefaults(tb); TextureBuilderSetDevice(tb, &device); TextureBuilderSetFlags(tb, 0); TextureBuilderSetTarget(tb, 1);
    TextureBuilderSetFormat(tb, 0x25 /*RGBA8*/); TextureBuilderSetSamples(tb, 0); TextureBuilderSetLevels(tb, LEVELS); TextureBuilderSetSize2D(tb, TW, TH);
    size_t sz = (TextureBuilderGetStorageSize(tb) + 0xfff) & ~(size_t)0xfff;
    texMem = _aligned_malloc(sz, 0x1000);
    MemoryPoolBuilderSetDefaults(pbb); MemoryPoolBuilderSetDevice(pbb, &device); MemoryPoolBuilderSetFlags(pbb, 0x29); MemoryPoolBuilderSetStorage(pbb, texMem, sz);
    if (!MemoryPoolInitialize(&texStoragePool, pbb)) { fprintf(stderr, "texture storage pool failed\n"); return 1; }
    TextureBuilderSetStorage(tb, &texStoragePool, 0);
    if (!TextureInitialize(&meshTex, tb)) { fprintf(stderr, "mesh texture init failed\n"); return 1; }
  }

  // ---- sampler + descriptor pools + handle -------------------------------------------------------------------------------------------
  {
    unsigned char sb[0x60];
    SamplerBuilderSetDefaults(sb); SamplerBuilderSetDevice(sb, &device);
    SamplerBuilderSetMinMagFilter(sb, 5 /*LINEAR_MIPMAP_LINEAR*/, 1 /*LINEAR*/);
    SamplerBuilderSetWrapMode(sb, 1, 1, 1);   // REPEAT
    SamplerBuilderSetMaxAnisotropy(sb, 8.0f);
    if (!SamplerInitialize(&sampler, sb)) { fprintf(stderr, "SamplerInitialize failed\n"); return 1; }
  }
  const int kTexId = 300, kSampId = 7;       // ids chosen above the reserved descriptor ids
  if (!TexturePoolInitialize(&texDesc, &descPool, 0, 4096) || !SamplerPoolInitialize(&sampDesc, &descPool, 0x8000, 256)) { fprintf(stderr, "pool init failed\n"); return 1; }
  {
    unsigned char view[0x10]; TextureViewSetDefaults(view); TextureViewSetLevels(view, 0, LEVELS);
    TexturePoolRegisterTexture(&texDesc, kTexId, &meshTex, view);
    SamplerPoolRegisterSampler(&sampDesc, kSampId, &sampler);
  }
  const Handle texHandle = DeviceGetTextureHandle(&device, kTexId, kSampId);
  if (texHandle != (((Handle)kSampId << 20) | (Handle)kTexId)) { fprintf(stderr, "texture handle packing differs from texId | samplerId << 20\n"); return 1; }

  // ---- state objects ---------------------------------------------------------------------------------------------------------------------
  BlendStateSetDefaults(&blendSt); BlendStateSetBlendFunc(&blendSt, 5, 6, 2, 1); BlendStateSetBlendEquation(&blendSt, 1, 1);
  ColorStateSetDefaults(&colorSt); ColorStateSetBlendEnable(&colorSt, 0, 0);
  ChannelMaskStateSetDefaults(&chmaskSt); ChannelMaskStateSetChannelMask(&chmaskSt, 0, 1, 1, 1, 1);
  MultisampleStateSetDefaults(&msSt); MultisampleStateSetMultisampleEnable(&msSt, 1); MultisampleStateSetSamples(&msSt, 0);
  PolygonStateSetDefaults(&polySt); PolygonStateSetCullFace(&polySt, 2 /*BACK*/); PolygonStateSetFrontFace(&polySt, 1 /*CCW*/); PolygonStateSetPolygonMode(&polySt, 2 /*FILL*/);
  PolygonStateSetDefaults(&polyNoCullSt); PolygonStateSetCullFace(&polyNoCullSt, 0 /*NONE*/);
  DepthStencilStateSetDefaults(&dsSt); DepthStencilStateSetDepthTestEnable(&dsSt, 1); DepthStencilStateSetDepthWriteEnable(&dsSt, 1);
  DepthStencilStateSetDepthFunc(&dsSt, 2 /*LESS*/);
  VertexAttribStateSetDefaults(&attrSt[0]); VertexAttribStateSetFormat(&attrSt[0], 0x22 /*RGB32F*/, 0); VertexAttribStateSetStreamIndex(&attrSt[0], 0);
  VertexAttribStateSetDefaults(&attrSt[1]);                                                       // attribute 1 unused
  VertexAttribStateSetDefaults(&attrSt[2]); VertexAttribStateSetFormat(&attrSt[2], 0x16 /*RG32F*/, 12); VertexAttribStateSetStreamIndex(&attrSt[2], 0);
  VertexStreamStateSetDefaults(&streamSt); VertexStreamStateSetStride(&streamSt, sizeof(Vtx));
  // the attribute array handed to BindVertexAttribState is contiguous 4-byte states
  unsigned char attrArr[3][4]; for (int i = 0; i < 3; i++) memcpy(attrArr[i], &attrSt[i], 4);

  // ---- frame loop --------------------------------------------------------------------------------------------------------------------------
  LARGE_INTEGER qf, t0; QueryPerformanceFrequency(&qf);
  Obj fences[2]; for (auto& f : fences) SyncInitialize(&f, &device);
  const float bg[4] = {0.05f, 0.07f, 0.12f, 1.0f};
  bool shotDone = false; double shotCover = -1; int shotOk = -1;
  QueryPerformanceCounter(&t0);
  bool quit = false;
  const Addr uboBase = BufferGetAddress(&ubuf), vbBase = BufferGetAddress(&vbuf), ibBase = BufferGetAddress(&ibuf), stageBase = BufferGetAddress(&ubuf) - 0x8000 + 0xC000;
  for (int f = 0; f < frames && !quit; f++) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) quit = true; TranslateMessage(&msg); DispatchMessageW(&msg); }
    const float t = f / 60.0f;

    // per-frame uniform slices (ring of 3, so two frames in flight never see a rewrite)
    unsigned char* ub = static_cast<unsigned char*>(BufferMap(&ubuf)) + (f % 3) * 0x1000;
    const Addr slice = uboBase + (Addr)(f % 3) * 0x1000;
    const M4 proj = Persp(1.0f, (float)W / H, 0.1f, 50.0f), view = Trans(0, 0, -5.0f);
    const M4 mA = Mul(Trans(-0.7f, 0, 0), Mul(RotY(t * 1.1f), RotX(0.45f + t * 0.5f)));
    const M4 mB = Mul(Trans(0.55f, 0.1f, 0.3f), Mul(Scale(0.8f), Mul(RotY(-t * 0.7f + 1.0f), RotX(t * 0.9f))));
    const M4 wvpA = Mul(proj, Mul(view, mA)), wvpB = Mul(proj, Mul(view, mB));
    memset(ub, 0, 0x1000);
    memcpy(ub + 0x000 + 128, wvpA.m, 64);                 // draw A: nuPerWorldCBuffer (256 B), WVP at +128
    memcpy(ub + 0x100 + 128, wvpB.m, 64);                 // draw B
    const float uvT[8] = {2, 0, 0, 0, 0, 2, 0, 0};        // nuUVTransformCBuffer: u = 2*u, v = 2*v (the wrap mode repeats the texture)
    memcpy(ub + 0x200, uvT, sizeof uvT);
    const float diffuse[4] = {1.0f, 0.97f, 0.92f, 1.0f};   // nuPerMaterialCBuffer
    memcpy(ub + 0x300, diffuse, sizeof diffuse);
    BufferFlushMappedRange(&ubuf, (ptrdiff_t)(f % 3) * 0x1000, 0x1000);

    CommandBufferBeginRecording(&cmdbuf);
    void* rt[1] = {&tex[cur]};
    CommandBufferSetRenderTargets(&cmdbuf, 1, rt, nullptr, &depthTex, nullptr);
    const float vp[4] = {0, 0, (float)W, (float)H}; const int sc[4] = {0, 0, W, H};
    CommandBufferSetViewports(&cmdbuf, 0, 1, vp);
    CommandBufferSetScissors(&cmdbuf, 0, 1, sc);
    CommandBufferClearColor(&cmdbuf, 0, bg, 0xf);
    CommandBufferClearDepthStencil(&cmdbuf, 1.0f, 1, 0, 0xff);
    if (f == 0) {   // first frame: upload every mip level (CopyBufferToTexture with a per-level view)
      size_t off = 0;
      for (int l = 0; l < LEVELS; l++) {
        unsigned char lv[0x10]; TextureViewSetDefaults(lv); TextureViewSetLevels(lv, l, 1);
        const int region[6] = {0, 0, 0, std::max(TW >> l, 1), std::max(TH >> l, 1), 1};
        CommandBufferCopyBufferToTexture(&cmdbuf, stageBase + off, &meshTex, lv, region, 0);
        off += mips[l].size();
      }
      CommandBufferBarrier(&cmdbuf, 0x10 /*INVALIDATE_TEXTURE*/);
    }
    CommandBufferSetShaderScratchMemory(&cmdbuf, &dataPool, 0, 0);
    CommandBufferBindBlendState(&cmdbuf, &blendSt);
    CommandBufferBindColorState(&cmdbuf, &colorSt);
    CommandBufferBindChannelMaskState(&cmdbuf, &chmaskSt);
    CommandBufferBindMultisampleState(&cmdbuf, &msSt);
    CommandBufferBindDepthStencilState(&cmdbuf, &dsSt);
    CommandBufferSetStencilValueMask(&cmdbuf, 3, 0xff); CommandBufferSetStencilMask(&cmdbuf, 3, 0xff); CommandBufferSetStencilRef(&cmdbuf, 3, 0);
    CommandBufferSetPolygonOffsetClamp(&cmdbuf, 0.0f, 0.0f, 0.0f);
    CommandBufferBindProgram(&cmdbuf, &prog, 0x3);
    CommandBufferSetTexturePool(&cmdbuf, &texDesc);
    CommandBufferSetSamplerPool(&cmdbuf, &sampDesc);
    CommandBufferBindTexture(&cmdbuf, 1 /*fragment*/, 0 /*GLSL sampler binding*/, texHandle);
    CommandBufferBindVertexAttribState(&cmdbuf, 3, attrArr);
    CommandBufferBindVertexStreamState(&cmdbuf, 1, &streamSt);
    struct Range { Addr a; size_t s; };
    const Range vbr = {vbBase, 0x4000};
    CommandBufferBindVertexBuffers(&cmdbuf, 0, 1, &vbr);
    // VS binding 6 (UV transform) and PS binding 6 (material) do not change between the two draws
    { const Range r6 = {slice + 0x200, 0x60}; CommandBufferBindUniformBuffers(&cmdbuf, 0, 6, 1, &r6); }
    { const Range r6 = {slice + 0x300, 0x10}; CommandBufferBindUniformBuffers(&cmdbuf, 1, 6, 1, &r6); }
    // draw A: back-face culling on
    CommandBufferBindPolygonState(&cmdbuf, &polySt);
    { const Range r1 = {slice + 0x000, 0x100}; CommandBufferBindUniformBuffers(&cmdbuf, 0, 1, 1, &r1); }
    CommandBufferDrawElementsBaseVertex(&cmdbuf, 4 /*TRIANGLES*/, 1 /*U16*/, 36, ibBase, 0);
    // draw B: same index data, base vertex 24, rebinding the world cbuffer; depth test resolves the intersection
    { const Range r1 = {slice + 0x100, 0x100}; CommandBufferBindUniformBuffers(&cmdbuf, 0, 1, 1, &r1); }
    CommandBufferDrawElementsBaseVertex(&cmdbuf, 4, 1, 36, ibBase, nVerts);
    CommandBufferBarrier(&cmdbuf, 0x28);
    Handle h = CommandBufferEndRecording(&cmdbuf);
    QueueSubmitCommands(&queue, 1, &h);
    QueueFenceSync(&queue, &fences[f & 1], 0, 0);
    QueueFlush(&queue);
    QueuePresentTexture(&queue, &window, cur);
    WindowAcquireTexture(&window, &acquireSync, &cur);
    SyncWait(&acquireSync, ~0ull);
    SyncWait(&fences[(f + 1) & 1], ~0ull);

    if (shot && !shotDone && f == 160 && !hidden) {
      Sleep(100);
      POINT p{0, 0}; ClientToScreen(hwnd, &p);
      HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
      HBITMAP bm = CreateCompatibleBitmap(sdc, W, H); SelectObject(mdc, bm);
      BitBlt(mdc, 0, 0, W, H, sdc, p.x, p.y, SRCCOPY);
      BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
      std::vector<unsigned char> px((size_t)W * H * 4);
      GetDIBits(mdc, bm, 0, H, px.data(), &bi, DIB_RGB_COLORS);
      SaveBmp(shot, px, W, H);
      // the clear color is (0.05, 0.07, 0.12) -> about (63, 74, 98) after the sRGB swapchain encode; count the pixels that differ
      const unsigned char* c0 = &px[0];
      size_t diff = 0;
      for (size_t i = 0; i < (size_t)W * H; i++) {
        const unsigned char* c = &px[i * 4];
        if (abs((int)c[0] - c0[0]) + abs((int)c[1] - c0[1]) + abs((int)c[2] - c0[2]) > 24) diff++;
      }
      shotCover = 100.0 * (double)diff / ((double)W * H);
      shotOk = (shotCover > 3.0 && shotCover < 60.0) ? 1 : 0;
      shotDone = true;
      DeleteObject(bm); DeleteDC(mdc); ReleaseDC(nullptr, sdc);
    }
  }
  QueueFinish(&queue);
  if (shot) printf("screenshot=%s covered=%.1f%% of pixels differ from the background -> %s\n", shot, shotCover, shotOk == 1 ? "PASS" : (shotOk == 0 ? "FAIL" : "n/a"));

  // ---- teardown --------------------------------------------------------------------------------------------------------------------------------
  for (auto& f : fences) SyncFinalize(&f);
  SyncFinalize(&acquireSync);
  WindowFinalize(&window);
  ProgramFinalize(&prog);
  SamplerPoolFinalize(&sampDesc); TexturePoolFinalize(&texDesc);
  SamplerFinalize(&sampler);
  TextureFinalize(&meshTex);
  for (Obj* b : {&vbuf, &ibuf, &ubuf, &codeBufVs, &codeBufPs}) BufferFinalize(b);
  for (auto& t : tex) TextureFinalize(&t);
  TextureFinalize(&depthTex);
  for (auto& p : texPool) MemoryPoolFinalize(&p);
  MemoryPoolFinalize(&texStoragePool); MemoryPoolFinalize(&descPool); MemoryPoolFinalize(&codePool); MemoryPoolFinalize(&dataPool);
  QueueFinalize(&queue);
  CommandBufferFinalize(&cmdbuf);
  MemoryPoolFinalize(&cmdPool);
  DeviceFinalize(&device);
  DestroyWindow(hwnd);
  unsigned errs = nvnHostValidationErrors(), warns = nvnHostValidationWarnings();
  printf("validation: errors=%u warnings=%u (layers %s)\n", errs, warns, validation ? "on" : "off");
  return (errs || shotOk == 0) ? 1 : 0;
}
