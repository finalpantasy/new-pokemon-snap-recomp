// SPDX-License-Identifier: MIT
// DLSS / DLAA through the driver's NGX core (_nvngx.dll, beside NVIDIA's Vulkan ICD). No NVIDIA SDK code, header or binary is
// compiled in or shipped: the few NGX types used are declared here from the SDK's documented ABI. The DLSS model
// (nvngx_dlss.dll) is loaded by NGX from the folder of the exe; locally it comes from tools\nvidia\ (git-ignored), copied by
// host/build_snap.py.
// Adapted from the Astral Chain PC recomp, runtime/vk_dlaa.c (MIT, Copyright (c) 2026 Valiantsin Dzerakh, commit e858fc6;
// see THIRD_PARTY_NOTICES.md): core_load, the parameter vtable layout and its round-trip check, and the evaluate parameters.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

#include "upscaler.h"

namespace snap_upscale::dlss {
namespace {

typedef int NgxResult;   // NVSDK_NGX_Result: 1 success, 0xBAD00000 | n failures
inline bool NgxOk(NgxResult r) { return ((unsigned)r & 0xFFF00000u) != 0xBAD00000u; }
constexpr int kNgxVersionApi = 0x15;
constexpr int kFeatureSuperSampling = 1;
constexpr int kPerfQDlaa = 5;   // NVSDK_NGX_PerfQuality_Value: 0 performance, 1 balanced, 2 quality, 3 ultra performance, 5 DLAA
struct NgxParamVtbl { void* slot[17]; };
struct NgxParam { const NgxParamVtbl* vt; };
struct NgxHandle;
struct NgxImageView {   // NVSDK_NGX_ImageViewInfo_VK
  VkImageView view; VkImage image; VkImageSubresourceRange range; VkFormat format; unsigned width, height;
};
struct NgxResourceVK {   // NVSDK_NGX_Resource_VK
  union { NgxImageView img; struct { VkBuffer buffer; unsigned bytes; } buf; } r;
  int type;   // 0 image view, 1 buffer
  bool rw;
};
typedef NgxResult (*PFN_Init)(unsigned long long app, const wchar_t* path, VkInstance, VkPhysicalDevice, VkDevice,
                              PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int version, const void* info);
typedef NgxResult (*PFN_ReqExt)(unsigned* ni, const char*** ie, unsigned* nd, const char*** de);
typedef NgxResult (*PFN_Params)(NgxParam**);
typedef NgxResult (*PFN_DestroyParams)(NgxParam*);
typedef NgxResult (*PFN_Create)(VkCommandBuffer, int feature, NgxParam*, NgxHandle**);
typedef NgxResult (*PFN_Evaluate)(VkCommandBuffer, const NgxHandle*, const NgxParam*, void* callback);
typedef NgxResult (*PFN_Release)(NgxHandle*);
typedef NgxResult (*PFN_Shutdown)(VkDevice);
typedef void (*SetI)(NgxParam*, const char*, int);
typedef void (*SetU)(NgxParam*, const char*, unsigned);
typedef void (*SetF)(NgxParam*, const char*, float);
typedef void (*SetP)(NgxParam*, const char*, void*);
typedef NgxResult (*GetI)(const NgxParam*, const char*, int*);
typedef NgxResult (*GetU)(const NgxParam*, const char*, unsigned*);
typedef NgxResult (*GetF)(const NgxParam*, const char*, float*);
typedef NgxResult (*GetP)(const NgxParam*, const char*, void**);
// MSVC vtable of NVSDK_NGX_Parameter: the Set overloads in slots 0-7, the Get overloads in 8-15 (reverse declaration order).
enum { S_P = 0, S_D12, S_D11, S_I, S_U32, S_D, S_F, S_U64, G_P, G_D12, G_D11, G_I, G_U32, G_D, G_F, G_U64 };
void SetIv(NgxParam* p, const char* n, int v) { ((SetI)p->vt->slot[S_I])(p, n, v); }
void SetUv(NgxParam* p, const char* n, unsigned v) { ((SetU)p->vt->slot[S_U32])(p, n, v); }
void SetFv(NgxParam* p, const char* n, float v) { ((SetF)p->vt->slot[S_F])(p, n, v); }
void SetPv(NgxParam* p, const char* n, void* v) { ((SetP)p->vt->slot[S_P])(p, n, v); }
bool GetIv(const NgxParam* p, const char* n, int* v) { return NgxOk(((GetI)p->vt->slot[G_I])(p, n, v)); }
bool GetUv(const NgxParam* p, const char* n, unsigned* v) { return NgxOk(((GetU)p->vt->slot[G_U32])(p, n, v)); }
bool GetFv(const NgxParam* p, const char* n, float* v) { return NgxOk(((GetF)p->vt->slot[G_F])(p, n, v)); }
bool GetPv(const NgxParam* p, const char* n, void** v) { return NgxOk(((GetP)p->vt->slot[G_P])(p, n, v)); }

struct State {
  HMODULE core = nullptr;
  PFN_Init init = nullptr; PFN_ReqExt reqExt = nullptr; PFN_Params caps = nullptr, allocParams = nullptr;
  PFN_DestroyParams destroyParams = nullptr; PFN_Create create = nullptr; PFN_Evaluate evaluate = nullptr;
  PFN_Release release = nullptr; PFN_Shutdown shutdown = nullptr;
  bool tried = false, loaded = false, inited = false, available = false, failed = false;
  VkDevice device = VK_NULL_HANDLE;
  NgxParam* params = nullptr;
  NgxHandle* feat = nullptr;
  uint32_t fw = 0, fh = 0, fow = 0, foh = 0; bool fhdr = false, finv = false;
  uint64_t evals = 0;
} N;

NgxResourceVK ResImage(const Image& m, uint32_t w, uint32_t h, VkImageAspectFlags a, bool rw) {
  NgxResourceVK r; memset(&r, 0, sizeof r);
  r.r.img = NgxImageView{m.view, m.image, {a, 0, 1, 0, 1}, m.format, w, h};
  r.type = 0; r.rw = rw;
  return r;
}

bool VtableOk() {
  NgxParam* p = nullptr;
  if (!NgxOk(N.allocParams(&p)) || !p) return false;
  int i = 0; unsigned u = 0; float f = 0; void* v = nullptr;
  SetIv(p, "Snap.Test.I", -12345); SetUv(p, "Snap.Test.U", 4000000000u); SetFv(p, "Snap.Test.F", 1.25f); SetPv(p, "Snap.Test.P", (void*)p);
  const bool ok = GetIv(p, "Snap.Test.I", &i) && i == -12345 && GetUv(p, "Snap.Test.U", &u) && u == 4000000000u &&
                  GetFv(p, "Snap.Test.F", &f) && f == 1.25f && GetPv(p, "Snap.Test.P", &v) && v == (void*)p;
  if (N.destroyParams) N.destroyParams(p);
  if (!ok) Logf("dlss: NGX parameter layout check failed: DLSS off");
  return ok;
}

bool Fail(const char* what, NgxResult r) {
  Logf("dlss: %s failed (%08x): DLSS off for this run", what, (unsigned)r);
  N.failed = true;
  return false;
}

}  // namespace

// The core is the _nvngx.dll in the folder of NVIDIA's Vulkan ICD (loaded once the instance exists); the NGXCore registry
// key that older drivers write is the fallback.
bool LoadAfterInstance() {
  if (N.tried) return N.loaded;
  N.tried = true;
  wchar_t path[MAX_PATH * 2 + 32] = {0};
  HMODULE icd = GetModuleHandleW(L"nvoglv64.dll");
  DWORD len = icd ? GetModuleFileNameW(icd, path, MAX_PATH * 2) : 0;
  while (len && path[len - 1] != L'\\') len--;
  if (len) { wcscpy(path + len, L"_nvngx.dll"); N.core = LoadLibraryW(path); }
  if (!N.core) {
    DWORD n = MAX_PATH * 2 * sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ, nullptr, path, &n) == ERROR_SUCCESS) {
      wcscat(path, L"\\_nvngx.dll");
      N.core = LoadLibraryW(path);
    }
  }
  if (!N.core) { Logf("dlss: no NGX core (_nvngx.dll)%s: DLSS unavailable", icd ? "" : " (no NVIDIA Vulkan driver loaded)"); return false; }
#define FN(f, name) N.f = (decltype(N.f))(void*)GetProcAddress(N.core, name)
  FN(init, "NVSDK_NGX_VULKAN_Init_Ext2");
  FN(reqExt, "NVSDK_NGX_VULKAN_RequiredExtensions");
  FN(caps, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
  FN(allocParams, "NVSDK_NGX_VULKAN_AllocateParameters");
  FN(destroyParams, "NVSDK_NGX_VULKAN_DestroyParameters");
  FN(create, "NVSDK_NGX_VULKAN_CreateFeature");
  FN(evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature");
  FN(release, "NVSDK_NGX_VULKAN_ReleaseFeature");
  FN(shutdown, "NVSDK_NGX_VULKAN_Shutdown1");
#undef FN
  if (!N.init || !N.reqExt || !N.caps || !N.allocParams || !N.create || !N.evaluate || !N.release) {
    Logf("dlss: the NGX core lacks Vulkan entry points: DLSS unavailable");
    return false;
  }
  N.loaded = true;
  Logf("dlss: NGX core %ls", path);
  return true;
}

void DeviceExtensions(VkPhysicalDevice pd, std::vector<const char*>& ext) {
  if (!N.loaded) return;
  unsigned ni = 0, nd = 0;
  const char** ie = nullptr; const char** de = nullptr;
  if (!NgxOk(N.reqExt(&ni, &ie, &nd, &de))) { Logf("dlss: no NGX extension list"); return; }
  for (unsigned i = 0; i < ni; i++)
    if (strcmp(ie[i], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) Logf("dlss: NGX asks for instance extension %s (not enabled)", ie[i]);
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> have(n);
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, have.data());
  for (unsigned i = 0; i < nd; i++) {
    bool ok = false;
    for (auto& h : have) if (!strcmp(h.extensionName, de[i])) { ok = true; break; }
    if (!ok) { Logf("dlss: the device lacks %s, which NGX asks for", de[i]); continue; }
    bool listed = false;
    for (auto* e : ext) if (!strcmp(e, de[i])) { listed = true; break; }
    if (!listed) ext.push_back(de[i]);
  }
}

bool Init(const DeviceCtx& c) {
  if (!N.loaded || N.inited) return N.available;
  wchar_t wdir[MAX_PATH];
  DWORD len = GetModuleFileNameW(nullptr, wdir, MAX_PATH);
  while (len && wdir[len - 1] != L'\\') len--;
  wdir[len] = 0;
  // 231313132: the application id NVIDIA's DLSS samples use for builds without an NVIDIA-issued id
  NgxResult r = N.init(231313132ull, wdir, c.instance, c.phys, c.device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, kNgxVersionApi, nullptr);
  if (!NgxOk(r)) { Logf("dlss: NGX init failed (%08x): DLSS unavailable", (unsigned)r); return false; }
  N.inited = true;
  N.device = c.device;
  if (!VtableOk()) return false;
  NgxParam* caps = nullptr;
  int avail = 0, driver = 0;
  if (NgxOk(N.caps(&caps)) && caps) { GetIv(caps, "SuperSampling.Available", &avail); GetIv(caps, "SuperSampling.NeedsUpdatedDriver", &driver); }
  N.available = avail != 0;
  Logf("dlss: NGX up; DLSS %s%s", avail ? "available" : "NOT available (nvngx_dlss.dll next to the exe?)", driver ? "; the driver needs an update" : "");
  return N.available;
}

bool Evaluate(const Eval& e) {
  if (!N.available || N.failed) return false;
  if (!N.params && !NgxOk(N.allocParams(&N.params))) return Fail("parameter allocation", 0);
  NgxParam* p = N.params;
  if (N.feat && (N.fw != e.w || N.fh != e.h || N.fow != e.ow || N.foh != e.oh || N.fhdr != e.hdr || N.finv != e.invDepth)) {
    vkDeviceWaitIdle(N.device);   // a size change: rare (once at start), so a wait is fine
    N.release(N.feat); N.feat = nullptr;
  }
  bool reset = e.reset;
  if (!N.feat) {
    const double k = (double)e.w / e.ow;
    const int q = e.w >= e.ow ? kPerfQDlaa : k >= 0.625 ? 2 : k >= 0.54 ? 1 : k >= 0.42 ? 0 : 3;
    SetUv(p, "CreationNodeMask", 1); SetUv(p, "VisibilityNodeMask", 1);
    SetUv(p, "Width", e.w); SetUv(p, "Height", e.h); SetUv(p, "OutWidth", e.ow); SetUv(p, "OutHeight", e.oh);
    SetIv(p, "PerfQualityValue", q);
    // MVLowRes (motion vectors at render size); linear scene light: IsHDR | AutoExposure
    SetIv(p, "DLSS.Feature.Create.Flags", (1 << 1) | (e.invDepth ? (1 << 3) : 0) | (e.hdr ? (1 << 0) | (1 << 6) : 0));   // MVLowRes, DepthInverted, IsHDR, AutoExposure
    SetIv(p, "DLSS.Enable.Output.Subrects", 0);
    // NVSDK_NGX_DLSS_Hint_Render_Preset: 11 K (transformer) for DLAA/Quality/Balanced, 13 M for Performance, 12 L for Ultra Perf
    SetUv(p, "DLSS.Hint.Render.Preset.DLAA", 11); SetUv(p, "DLSS.Hint.Render.Preset.UltraQuality", 11);
    SetUv(p, "DLSS.Hint.Render.Preset.Quality", 11); SetUv(p, "DLSS.Hint.Render.Preset.Balanced", 11);
    SetUv(p, "DLSS.Hint.Render.Preset.Performance", 13); SetUv(p, "DLSS.Hint.Render.Preset.UltraPerformance", 12);
    NgxResult r = N.create(e.cb, kFeatureSuperSampling, p, &N.feat);
    if (!NgxOk(r) || !N.feat) return Fail("DLSS feature creation", r);
    N.fw = e.w; N.fh = e.h; N.fow = e.ow; N.foh = e.oh; N.fhdr = e.hdr; N.finv = e.invDepth;
    reset = true;
    Logf("dlss: feature created %ux%u -> %ux%u, quality value %d (%s), %s colour", e.w, e.h, e.ow, e.oh, q, q == kPerfQDlaa ? "DLAA" : "upscale", e.hdr ? "linear HDR" : "LDR");
  }
  NgxResourceVK rc = ResImage(e.color, e.w, e.h, VK_IMAGE_ASPECT_COLOR_BIT, false);
  NgxResourceVK ro = ResImage(e.out, e.ow, e.oh, VK_IMAGE_ASPECT_COLOR_BIT, true);
  NgxResourceVK rz = ResImage(e.depth, e.w, e.h, VK_IMAGE_ASPECT_DEPTH_BIT, false);
  NgxResourceVK rm = ResImage(e.mv, e.w, e.h, VK_IMAGE_ASPECT_COLOR_BIT, false);
  SetPv(p, "Color", &rc); SetPv(p, "Output", &ro); SetPv(p, "Depth", &rz); SetPv(p, "MotionVectors", &rm);
  SetFv(p, "Jitter.Offset.X", e.jx); SetFv(p, "Jitter.Offset.Y", e.jy);
  SetFv(p, "Sharpness", 0.0f);
  SetIv(p, "Reset", reset ? 1 : 0);
  SetFv(p, "MV.Scale.X", 1.0f); SetFv(p, "MV.Scale.Y", 1.0f);
  SetUv(p, "DLSS.Render.Subrect.Dimensions.Width", e.w); SetUv(p, "DLSS.Render.Subrect.Dimensions.Height", e.h);
  SetFv(p, "DLSS.Pre.Exposure", 1.0f); SetFv(p, "DLSS.Exposure.Scale", 1.0f);
  SetFv(p, "FrameTimeDeltaInMsec", e.dtMs);
  NgxResult r = N.evaluate(e.cb, N.feat, p, nullptr);
  if (!NgxOk(r)) return Fail("DLSS evaluation", r);
  N.evals++;
  return true;
}

void Release() {
  if (N.feat) { N.release(N.feat); N.feat = nullptr; }
  if (N.params && N.destroyParams) { N.destroyParams(N.params); N.params = nullptr; }
  if (N.inited && N.shutdown) N.shutdown(N.device);
  N.inited = false; N.available = false;
}

}  // namespace snap_upscale::dlss
