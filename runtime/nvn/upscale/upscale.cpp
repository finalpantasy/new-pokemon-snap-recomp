// SPDX-License-Identifier: MIT
// Front end of the temporal upscalers (see upscaler.h for the settings). Structure adapted from the Astral Chain PC recomp's
// runtime/vk_dlaa.c (MIT, commit e858fc6, see THIRD_PARTY_NOTICES.md): viewport jitter on the scene's draws, a Halton(2,3)
// sequence of 8 x ratio^2 phases, camera motion vectors from depth, resets on camera cuts. The scene is found with Snap's own
// render graph instead of pass signatures:
//   - scene depth: the 1920x1080 DEPTH32F target of the NU pass graph (the Evaluation target is DEPTH32F_STENCIL8 and
//     tagged by its creation call site, main+0x5c3d3c);
//   - scene colour: the 1920x1080 RGBA16F target drawn with that depth (linear light, after bloom, before the tonemap);
//   - upscale point: the first time the window (DISPLAY) texture is bound after the scene. There the upscaler writes an
//     output-size RGBA16F image and the game's final (tonemap) pass samples it instead of the scene colour, so tonemap and UI
//     run at the output size.
//   - camera: PerViewCBuffer, vertex constant bank c3 (docs/motion-vectors.md): viewProjection at +0x100 (column-major),
//     projection at +0x80, cameraPosition at +0x240. Read at the first scene draw of the frame; last frame's copy is kept.
// Photo safety: a submit whose lists bind the Evaluation depth target or a 360x240 photo/thumbnail target is a photo frame:
// it is drawn unjittered and not upscaled (the game's own final pass reads the scene colour), and the next frames start a new
// history. Check with SNAP_UPSCALE_LOG=1 ("photo" lines).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../nvn_internal.h"
#include "../../hle/input_core.h"
#include "upscale_mv_spv.h"
#include "upscale_nvn.h"
#include "upscaler.h"

// ======================================================================================================================
// settings and backend selection (namespace snap_upscale)
// ======================================================================================================================
namespace snap_upscale {
namespace {
struct Cfg {
  bool parsed = false;
  int want = BK_OFF;
  bool dlaa = false;
  int quality = 1;   // 0 native, 1 quality, 2 balanced, 3 performance
  double ratio = 1.5;
  bool jitter = true, mv = true;
  int log = 0;
  int probe = -1;    // SNAP_UPSCALE_PROBE=<present>: dump the scene draws' vertex constant banks around that present
} C;
const char* const kQName[] = {"Native AA", "Quality", "Balanced", "Performance"};
const double kRatio[] = {1.0, 1.5, 1.7241, 2.0};
const char* const kBkName[] = {"off", "DLSS", "FSR 3.1", "XeSS"};
int g_active = BK_OFF;
bool g_inited[4] = {};
bool g_failed[4] = {};

void Parse() {
  if (C.parsed) return;
  C.parsed = true;
  if (const char* u = getenv("SNAP_UPSCALER")) {
    if (!_stricmp(u, "dlss")) C.want = BK_DLSS;
    else if (!_stricmp(u, "dlaa")) { C.want = BK_DLSS; C.dlaa = true; }
    else if (!_stricmp(u, "fsr")) C.want = BK_FSR;
    else if (!_stricmp(u, "xess")) C.want = BK_XESS;
    else if (*u && _stricmp(u, "off")) Logf("unknown SNAP_UPSCALER '%s' (off, dlss, dlaa, fsr, xess): off", u);
  }
  int q = 1;
  if (const char* e = getenv("SNAP_UPSCALE_QUALITY")) {
    if (!_stricmp(e, "native") || !_stricmp(e, "dlaa")) q = 0;
    else if (!_stricmp(e, "quality")) q = 1;
    else if (!_stricmp(e, "balanced")) q = 2;
    else if (!_stricmp(e, "performance")) q = 3;
    else Logf("unknown SNAP_UPSCALE_QUALITY '%s' (native, quality, balanced, performance): quality", e);
  }
  if (C.dlaa) q = 0;
  C.quality = q; C.ratio = kRatio[q];
  if (const char* e = getenv("SNAP_UPSCALE_JITTER")) C.jitter = atoi(e) != 0;
  if (const char* e = getenv("SNAP_UPSCALE_MV")) C.mv = atoi(e) != 0;
  if (const char* e = getenv("SNAP_UPSCALE_LOG")) C.log = atoi(e);
  if (const char* e = getenv("SNAP_UPSCALE_PROBE")) C.probe = atoi(e);
}

// the wanted backend first, then DLSS -> FSR -> XeSS
int Order(int* o) {
  int n = 0;
  o[n++] = C.want;
  for (int b : {BK_DLSS, BK_FSR, BK_XESS}) if (b != C.want) o[n++] = b;
  return n;
}
bool BkEvaluate(int b, const Eval& e) {
  return b == BK_DLSS ? dlss::Evaluate(e) : b == BK_FSR ? fsr::Evaluate(e) : b == BK_XESS ? xess::Evaluate(e) : false;
}
}  // namespace

void Logf(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  fprintf(stderr, "[upscale] %s\n", buf);
  fflush(stderr);
}

bool Wanted() { Parse(); return C.want != BK_OFF; }
float SceneScale(float outputScale) { Parse(); return (float)(outputScale / C.ratio); }
int ActiveBackend() { return g_active; }
const char* Name() {
  static char buf[64];
  if (g_active == BK_OFF) return "off";
  if (g_active == BK_DLSS && C.quality == 0) return "DLAA";
  snprintf(buf, sizeof buf, "%s %s", kBkName[g_active], kQName[C.quality]);
  return buf;
}

void AddInstanceExtensions(std::vector<const char*>& ext) {
  if (!Wanted()) return;
  if (xess::Load()) xess::InstanceExtensions(ext);
  fsr::Load();
}

void AddDeviceExtensions(VkInstance inst, VkPhysicalDevice pd, std::vector<const char*>& ext, void** chainHead,
                         VkPhysicalDeviceVulkan11Features* f11, VkPhysicalDeviceVulkan12Features* f12,
                         VkPhysicalDeviceVulkan13Features* f13) {
  if (!Wanted()) return;
  if (dlss::LoadAfterInstance()) dlss::DeviceExtensions(pd, ext);
  bool needMut = false;
  xess::DeviceExtensions(inst, pd, ext, &needMut);
  // The backends' shaders use optional 1.1-1.3 features (16-bit storage, int8, dot product, subgroup size control, buffer
  // device address...): enable every one the GPU has (protected memory aside), as the layer already does for 1.0.
  VkPhysicalDeviceVulkan13Features q13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features q12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &q13};
  VkPhysicalDeviceVulkan11Features q11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &q12};
  static VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT md{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
  q13.pNext = needMut ? &md : nullptr;
  VkPhysicalDeviceFeatures2 q2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q11};
  vkGetPhysicalDeviceFeatures2(pd, &q2);
  { void* n = f11->pNext; *f11 = q11; f11->pNext = n; f11->protectedMemory = VK_FALSE; }
  { void* n = f12->pNext; *f12 = q12; f12->pNext = n; }
  { void* n = f13->pNext; *f13 = q13; f13->pNext = n; }
  if (needMut && md.mutableDescriptorType) { md.pNext = *chainHead; *chainHead = &md; }
}

void DeviceCreated(const DeviceCtx& c) {
  if (!Wanted()) return;
  int o[4]; const int n = Order(o);
  for (int i = 0; i < n; i++) {
    const int b = o[i];
    g_inited[b] = b == BK_DLSS ? dlss::Init(c) : b == BK_FSR ? fsr::Init(c) : xess::Init(c);
  }
  for (int i = 0; i < n; i++) {
    if (g_inited[o[i]]) { g_active = o[i]; break; }
    Logf("%s unavailable: falling back", kBkName[o[i]]);
  }
  if (g_active == BK_OFF) Logf("no upscaler available: plain render scale (scene %.3fx per axis below the output)", C.ratio);
  else Logf("active: %s (scene = output / %.3f)", Name(), C.ratio);
}

void Shutdown() {
  if (g_inited[BK_DLSS]) dlss::Release();
  if (g_inited[BK_FSR]) fsr::Release();
  if (g_inited[BK_XESS]) xess::Release();
}

// ---- used by the NVN side below ----
namespace front {
int LogLevel() { Parse(); return C.log; }
int Probe() { Parse(); return C.probe; }
bool JitterOn() { return C.jitter; }
bool MvOn() { return C.mv; }
// Records the active backend; on failure the next initialised backend takes over (history restarts), else off.
bool Evaluate(const Eval& e) {
  while (g_active != BK_OFF) {
    if (BkEvaluate(g_active, e)) return true;
    g_failed[g_active] = true;
    int o[4]; const int n = Order(o), was = g_active;
    g_active = BK_OFF;
    for (int i = 0; i < n; i++) if (g_inited[o[i]] && !g_failed[o[i]]) { g_active = o[i]; break; }
    Logf("%s failed during the run: %s", kBkName[was], g_active == BK_OFF ? "plain render scale from now on" : kBkName[g_active]);
    if (g_active != BK_OFF) return false;   // next frame, with a reset
  }
  return false;
}
// SNAP_UPSCALE_COMPARE=1 (test aid): every initialised backend upscales every frame from the same inputs into its own
// image (each keeps its own history) and the display shows them in turn, one per present, so consecutive dumps compare
// the backends at the same view. The list order is DLSS, FSR, XeSS.
int CompareBackends(int* out) {
  static const bool on = getenv("SNAP_UPSCALE_COMPARE") != nullptr;
  if (!on) return 0;
  int n = 0;
  for (int b : {BK_DLSS, BK_FSR, BK_XESS}) if (g_inited[b] && !g_failed[b]) out[n++] = b;
  return n;
}
bool EvaluateBackend(int b, const Eval& e) { return BkEvaluate(b, e); }
const char* BackendName(int b) { return kBkName[b]; }
}  // namespace front
}  // namespace snap_upscale

// ======================================================================================================================
// the NVN side (namespace nvn)
// ======================================================================================================================
namespace nvn {
// A descriptor set from the command buffer's pools (recycled when it retires), as nvn_draw.cpp's AllocSet does.
static VkDescriptorSet UpAllocSet(ReplayCtx& cx, VkDescriptorSetLayout layout) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (cx.pools->empty() || attempt == 1) cx.pools->push_back(AcquireDescriptorPool(cx.q));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = cx.pools->back(); ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(cx.h->device, &ai, &set) == VK_SUCCESS) return set;
  }
  return VK_NULL_HANDLE;
}

namespace {
using namespace snap_upscale;
constexpr uint32_t kTagEvalDepth = 0x45564450;   // 'EVDP' (nvn_entry.cpp)
constexpr uint32_t kCamBytes = 0x250;            // PerViewCBuffer is 0x2f0 (bound range 0x2e8); the camera fields end at cameraPosition +0x24c
constexpr uint32_t kOffProj = 0x80, kOffViewProj = 0x100, kOffCamPos = 0x240;

struct Mat { double m[4][4]; };   // row-major: clip = M * p

struct State {
  bool wanted = false, on = false;
  Host* h = nullptr;
  float sceneScale = 1, outScale = 1;
  TextureImpl* evalDepth = nullptr;
  TextureImpl* sceneDepth = nullptr;
  TextureImpl* sceneColor = nullptr;
  TextureImpl* out = nullptr;
  TextureImpl* cmpOut[4] = {};   // SNAP_UPSCALE_COMPARE: one output per backend
  TextureImpl* shown = nullptr;  // the image the display samples this present
  // motion vectors
  VkImage mvImg = VK_NULL_HANDLE; VkDeviceMemory mvMem = VK_NULL_HANDLE; VkImageView mvView = VK_NULL_HANDLE;
  uint32_t mvW = 0, mvH = 0;
  VkImageLayout mvLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE;
  VkSampler samp = VK_NULL_HANDLE;
  bool mvOk = false;
  // frame
  int phase = 0;              // 0 before the scene, 1 scene being drawn, 2 upscale point passed (until the present)
  bool frameHold = false;     // photo frame: no jitter, no upscale
  bool submitPhoto = false;   // the current submit holds a photo / Evaluation pass
  int hold = 0;               // presents left without jitter after a photo frame
  bool evaluated = false;     // this present
  bool jitUsed = false;
  float jx = 0, jy = 0;
  uint32_t jframe = 0;
  VkCommandBuffer ecb = VK_NULL_HANDLE; float ejx = 0, ejy = 0;   // jitter the viewports of ecb were last emitted with
  // camera
  Mat cur{}, prev{}; bool haveCur = false, havePrev = false;
  double eye[3] = {}, prevEye[3] = {}, fwd[3] = {}, prevFwd[3] = {};
  float nearZ = 0.1f, farZ = 1000.f, fovY = 0.6f;
  bool invDepth = true, infFar = true;   // from the projection (reversed infinite in the course; checked per frame)
  bool reset = true;
  LARGE_INTEGER lastEval{};
  // probe / stats
  int probeDraws = 0, probePresent = -1;
  uint64_t frames = 0, evals = 0, holds = 0, cuts = 0, noCam = 0, photoSubmits = 0, missed = 0, lateHold = 0, shutterHolds = 0;
  bool prevHeld = false;      // the previous present was rendered native and unjittered (what a start-of-present photo pass reads)
  double evalMsSum = 0, bkMsSum = 0;
  uint64_t resolves = 0;
  int lastEvalPresent = -100;
} U;

double Now() { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / (double)f.QuadPart; }

// SNAP_UPSCALE_AB=<P> (test aid): presents in odd blocks of P are rendered native and unjittered (like a photo frame), so
// frames N and N+1 at a block edge give a same-view native vs upscaled pair; the history is kept across the native block.
int AbPeriod() { static const int p = [] { const char* e = getenv("SNAP_UPSCALE_AB"); return e ? std::max(0, atoi(e)) : 0; }(); return p; }
bool AbNative() { const int p = AbPeriod(); return p > 0 && ((g_presentCount / p) & 1); }

bool IsSceneDepth(const TextureImpl* t) {
  return t && t != U.evalDepth && t->b.format == FMT_DEPTH32F && t->b.width == 1920 && t->b.height == 1080 &&
         t->b.target == TT_2D && t->b.samples <= 1;
}
bool IsSceneColor(const TextureImpl* t) {
  return t && t->b.format == FMT_RGBA16F && t->b.width == 1920 && t->b.height == 1080 && t->b.target == TT_2D && !(t->b.flags & TF_DISPLAY);
}
bool IsDisplay(const TextureImpl* t) { return t && (t->b.flags & TF_DISPLAY); }

float Halton(uint32_t i, uint32_t b) { float f = 1, r = 0; for (; i; i /= b) { f /= (float)b; r += f * (float)(i % b); } return r; }
uint32_t Phases(uint32_t w, uint32_t ow) {
  const double r = w ? (double)ow / w : 1.0;
  const uint32_t p = (uint32_t)std::ceil(8.0 * r * r - 1e-6);
  return std::clamp(p, 16u, 72u);
}
void NextJitter(uint32_t w, uint32_t ow) {
  const uint32_t i = U.jframe++ % Phases(w, ow) + 1;
  U.jx = Halton(i, 2) - 0.5f; U.jy = Halton(i, 3) - 0.5f;
}

bool Invert(const Mat& a, Mat& r) {
  double m[4][8];
  for (int i = 0; i < 4; i++) for (int j = 0; j < 8; j++) m[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
  for (int c = 0; c < 4; c++) {
    int p = c;
    for (int i = c + 1; i < 4; i++) if (std::fabs(m[i][c]) > std::fabs(m[p][c])) p = i;
    if (std::fabs(m[p][c]) < 1e-30) return false;
    if (p != c) for (int j = 0; j < 8; j++) std::swap(m[p][j], m[c][j]);
    const double d = m[c][c];
    for (int j = 0; j < 8; j++) m[c][j] /= d;
    for (int i = 0; i < 4; i++) if (i != c) { const double f = m[i][c]; for (int j = 0; j < 8; j++) m[i][j] -= f * m[c][j]; }
  }
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r.m[i][j] = m[i][j + 4];
  return true;
}
Mat Mul(const Mat& a, const Mat& b) {
  Mat r{};
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += a.m[i][k] * b.m[k][j]; r.m[i][j] = s; }
  return r;
}
// column-major float4x4 (four column vec4s) -> row-major
Mat FromColumns(const float* f) {
  Mat r{};
  for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) r.m[rr][c] = f[c * 4 + rr];
  return r;
}

// ---- Vulkan objects -------------------------------------------------------------------------------------------------
bool MakeMvPipeline(Host* h) {
  VkDevice dev = h->device;
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(dev, &si, nullptr, &U.samp) != VK_SUCCESS) return false;
  VkDescriptorSetLayoutBinding b[2] = {};
  b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  di.bindingCount = 2; di.pBindings = b;
  if (vkCreateDescriptorSetLayout(dev, &di, nullptr, &U.dsl) != VK_SUCCESS) return false;
  VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 96};
  VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  li.setLayoutCount = 1; li.pSetLayouts = &U.dsl; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
  if (vkCreatePipelineLayout(dev, &li, nullptr, &U.pl) != VK_SUCCESS) return false;
  VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  mi.codeSize = sizeof kUpscaleMvComp; mi.pCode = kUpscaleMvComp;
  VkShaderModule mod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(dev, &mi, nullptr, &mod) != VK_SUCCESS) return false;
  VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
  pi.layout = U.pl;
  const VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &U.pipe);
  vkDestroyShaderModule(dev, mod, nullptr);
  return r == VK_SUCCESS;
}

void FreeMv() {
  VkDevice dev = U.h->device;
  if (U.mvView) vkDestroyImageView(dev, U.mvView, nullptr);
  if (U.mvImg) vkDestroyImage(dev, U.mvImg, nullptr);
  if (U.mvMem) vkFreeMemory(dev, U.mvMem, nullptr);
  U.mvView = VK_NULL_HANDLE; U.mvImg = VK_NULL_HANDLE; U.mvMem = VK_NULL_HANDLE; U.mvW = U.mvH = 0;
}
bool EnsureMv(uint32_t w, uint32_t h) {
  if (U.mvImg && U.mvW == w && U.mvH == h) return true;
  VkDevice dev = U.h->device;
  if (U.mvImg) { vkDeviceWaitIdle(dev); FreeMv(); }
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R16G16_SFLOAT; ci.extent = {w, h, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (vkCreateImage(dev, &ci, nullptr, &U.mvImg) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, U.mvImg, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = U.h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(dev, &ai, nullptr, &U.mvMem) != VK_SUCCESS) return false;
  vkBindImageMemory(dev, U.mvImg, U.mvMem, 0);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = U.mvImg; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(dev, &vi, nullptr, &U.mvView) != VK_SUCCESS) return false;
  U.mvW = w; U.mvH = h; U.mvLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return true;
}

TextureImpl* EnsureOut(TextureImpl* c, uint32_t ow, uint32_t oh) {
  if (U.out && U.out->HW() == ow && U.out->HH() == oh) return U.out;
  if (U.out) { vkDeviceWaitIdle(U.h->device); DestroyTexture(U.out); U.out = nullptr; }
  auto* t = new TextureImpl();
  t->host = c->host; t->b = c->b; t->fi = c->fi;
  t->hostW = ow; t->hostH = oh; t->scale = (float)ow / (float)c->b.width;
  if (!t->EnsureImage()) { delete t; return nullptr; }
  U.out = t;
  Logf("output image %ux%u RGBA16F", ow, oh);
  return t;
}

void MvBarrier(VkCommandBuffer cb, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
  b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = U.mvLayout; b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = U.mvImg; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
  U.mvLayout = to;
}

// ---- camera ---------------------------------------------------------------------------------------------------------
void TakeCamera(const uint8_t* p) {
  float f[16];
  memcpy(f, p + kOffViewProj, 64);
  U.cur = FromColumns(f);
  memcpy(f, p + kOffProj, 64);
  const Mat P = FromColumns(f);
  float e[3]; memcpy(e, p + kOffCamPos, 12);
  for (int i = 0; i < 3; i++) U.eye[i] = e[i];
  // forward: the w row of viewProjection is minus the view direction (scaled by 1 for a perspective P)
  double n = 0;
  for (int i = 0; i < 3; i++) { U.fwd[i] = -U.cur.m[3][i]; n += U.fwd[i] * U.fwd[i]; }
  n = std::sqrt(n);
  if (n > 0) for (double& v : U.fwd) v /= n;
  // near / far / fov from P (near-is-zero depth): at view distance d, z_ndc = (P22 (-d) + P23) / d = -P22 + P23 / d, so ndc 0 lies at
  // d0 = P23 / P22 and ndc 1 at d1 = P23 / (1 + P22). The game's course camera has P22 = 0: reversed, infinite far (ndc 1 = near).
  const double a = P.m[2][2], b = P.m[2][3];
  if (std::fabs(P.m[3][2]) > 0.5 && std::fabs(P.m[1][1]) > 1e-6) {
    const double d0 = std::fabs(a) > 1e-12 ? b / a : INFINITY, d1 = std::fabs(1.0 + a) > 1e-12 ? b / (1.0 + a) : INFINITY;
    const double dn = std::min(std::fabs(d0), std::fabs(d1)), df = std::max(std::fabs(d0), std::fabs(d1));
    U.invDepth = std::fabs(d1) < std::fabs(d0);
    U.nearZ = (float)dn; U.infFar = !std::isfinite(df) || df > 1e7; U.farZ = U.infFar ? 1e7f : (float)df;
    U.fovY = (float)(2.0 * std::atan(1.0 / std::fabs(P.m[1][1])));
  }
  U.haveCur = true;
  static int logged = 0;
  if (front::LogLevel() && (logged++ < 2 || front::LogLevel() >= 3))
    Logf("p%d camera: eye (%.2f %.2f %.2f) forward (%.3f %.3f %.3f) near %.3f far %.1f fovY %.1f deg; P row3 (%.3f %.3f %.3f %.3f)", g_presentCount,
         U.eye[0], U.eye[1], U.eye[2], U.fwd[0], U.fwd[1], U.fwd[2], U.nearZ, U.farZ, U.fovY * 57.29578f, P.m[3][0], P.m[3][1], P.m[3][2], P.m[3][3]);
}

}  // namespace

// ---- NVN entry points -----------------------------------------------------------------------------------------------
static int WindowHeightPx() {
  if (const char* e = getenv("SNAP_WINDOW_H")) if (atoi(e) > 0) return atoi(e);
  std::string dir;
  if (const char* c = getenv("SNAP_CONFIG_DIR")) dir = c; else if (const char* a = getenv("APPDATA")) dir = std::string(a) + "\\SnapRecomp";
  if (!dir.empty()) {
    if (FILE* f = fopen((dir + "\\window.json").c_str(), "rb")) {
      char buf[4096]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
      if (const char* p = strstr(buf, "\"height\"")) { p = strchr(p, ':'); if (p && atoi(p + 1) > 0) return atoi(p + 1); }
    }
  }
  return 1080;
}

bool UpscaleApplyPolicy(float* sceneScale, float* outScale) {
  if (!snap_upscale::Wanted()) return false;
  U.wanted = true;
  U.outScale = std::clamp((float)WindowHeightPx() / 1080.0f, 0.5f, 4.0f);
  U.sceneScale = snap_upscale::SceneScale(U.outScale);
  *sceneScale = U.sceneScale; *outScale = U.outScale;
  Logf("scene %ux%u -> output %ux%u (scene scale %.4f, window scale %.4f)", (unsigned)std::lround(1920 * U.sceneScale),
       (unsigned)std::lround(1080 * U.sceneScale), (unsigned)std::lround(1920 * U.outScale), (unsigned)std::lround(1080 * U.outScale),
       U.sceneScale, U.outScale);
  return true;
}

void UpscaleTagTexture(TextureImpl* t, const uint64_t* chainRel, int n, uint32_t tag) {
  if (!U.wanted || !t) return;
  if (tag == kTagEvalDepth) U.evalDepth = t;
  if (front::LogLevel() && (tag == kTagEvalDepth || IsSceneDepth(t) || IsSceneColor(t))) {
    char cs[160] = "";
    for (int i = 0, k = 0; i < n && k < (int)sizeof cs - 16; i++) k += snprintf(cs + k, sizeof cs - k, " %llx", (unsigned long long)chainRel[i]);
    Logf("%s %p %dx%d fmt 0x%x scale %.3f; main+%s", tag == kTagEvalDepth ? "evaluation depth" : IsSceneDepth(t) ? "scene depth candidate" : "scene colour candidate",
         (void*)t, t->b.width, t->b.height, t->b.format, t->scale, cs);
  }
}
void UpscaleForgetTexture(TextureImpl* t) {
  if (!U.wanted) return;
  if (U.evalDepth == t) U.evalDepth = nullptr;
  if (U.sceneDepth == t) { U.sceneDepth = nullptr; U.reset = true; }
  if (U.sceneColor == t) { U.sceneColor = nullptr; U.reset = true; }
}

void UpscaleInstanceExtensions(std::vector<const char*>& ext) { snap_upscale::AddInstanceExtensions(ext); }
void UpscaleDeviceExtensions(Host* h, std::vector<const char*>& ext, void** chainHead, VkPhysicalDeviceVulkan11Features* f11,
                             VkPhysicalDeviceVulkan12Features* f12, VkPhysicalDeviceVulkan13Features* f13) {
  snap_upscale::AddDeviceExtensions(h->instance, h->phys, ext, chainHead, f11, f12, f13);
}
void UpscaleDeviceCreated(Host* h) {
  if (!U.wanted && !snap_upscale::Wanted()) return;
  U.h = h;
  snap_upscale::DeviceCtx c;
  c.instance = h->instance; c.phys = h->phys; c.device = h->device; c.queue = h->queue; c.family = h->family;
  snap_upscale::DeviceCreated(c);
  U.mvOk = MakeMvPipeline(h);
  if (!U.mvOk) Logf("motion-vector pipeline creation failed: zero motion");
  U.on = snap_upscale::ActiveBackend() != BK_OFF;
}
void UpscaleDestroy(Host* h) {
  if (!U.h || U.h != h) return;
  if (U.frames) {
    Logf("summary: %llu scene frames, %llu upscaled (%.2f ms CPU record avg), %llu held for photos (%llu photo submits, %llu late, %llu shutter presses), %llu camera cuts, "
         "%llu without camera, %llu scenes without an upscale point",
         (unsigned long long)U.frames, (unsigned long long)U.evals, U.evals ? U.evalMsSum / (double)U.evals : 0.0, (unsigned long long)U.holds,
         (unsigned long long)U.photoSubmits, (unsigned long long)U.lateHold, (unsigned long long)U.shutterHolds, (unsigned long long)U.cuts, (unsigned long long)U.noCam,
         (unsigned long long)U.missed);
  }
  snap_upscale::Shutdown();
  VkDevice dev = h->device;
  FreeMv();
  if (U.pipe) vkDestroyPipeline(dev, U.pipe, nullptr);
  if (U.pl) vkDestroyPipelineLayout(dev, U.pl, nullptr);
  if (U.dsl) vkDestroyDescriptorSetLayout(dev, U.dsl, nullptr);
  if (U.samp) vkDestroySampler(dev, U.samp, nullptr);
  if (U.out) DestroyTexture(U.out);
  for (TextureImpl*& t : U.cmpOut) if (t) { DestroyTexture(t); t = nullptr; }
  U = State{};
}

static void Resolve(ReplayCtx& cx) {
  TextureImpl* c = U.sceneColor; TextureImpl* z = U.sceneDepth; TextureImpl* disp = cx.rt[0];
  if (!c || !z || !c->EnsureImage() || !z->EnsureImage()) {
    static int n = 0;
    if (n++ < 4) Logf("upscale point without a scene colour (%p) or depth (%p): skipped", (void*)c, (void*)z);
    return;
  }
  const double t0 = Now();
  const uint32_t w = c->HW(), h = c->HH(), ow = disp->HW(), oh = disp->HH();
  TextureImpl* o = EnsureOut(c, ow, oh);
  if (!o || !EnsureMv(w, h)) { Logf("output or motion image creation failed: upscaler off"); U.on = false; return; }
  VkCommandBuffer cb = cx.cb;
  Transition(cb, z, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
  Transition(cb, c, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  const float ujx = U.jitUsed ? U.jx : 0.f, ujy = U.jitUsed ? U.jy : 0.f;
  // camera cut: a jump of the eye or a turn the vectors cannot follow
  bool cut = false;
  if (U.haveCur && U.havePrev) {
    double d2 = 0, dot = 0;
    for (int i = 0; i < 3; i++) { const double d = U.eye[i] - U.prevEye[i]; d2 += d * d; dot += U.fwd[i] * U.prevFwd[i]; }
    if (std::sqrt(d2) > 5.0 || dot < std::cos(30.0 * 3.14159265358979 / 180.0)) { cut = true; U.cuts++; }
  }
  if (!U.haveCur) U.noCam++;
  // motion vectors
  if (U.mvOk) {
    MvBarrier(cb, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    Mat R{};
    for (int i = 0; i < 4; i++) R.m[i][i] = 1.0;   // zero motion
    Mat inv;
    if (front::MvOn() && U.haveCur && U.havePrev && !cut && Invert(U.cur, inv)) R = Mul(U.prev, inv);
    struct { float r[4][4]; float size[2]; float jit[2]; float mode[4]; } pc{};
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) pc.r[i][j] = (float)R.m[i][j];
    pc.size[0] = (float)w; pc.size[1] = (float)h; pc.jit[0] = ujx; pc.jit[1] = ujy;
    VkDescriptorSet set = UpAllocSet(cx, U.dsl);
    if (set) {
      VkDescriptorImageInfo ii[2] = {{U.samp, z->View(nullptr, true), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                                     {VK_NULL_HANDLE, U.mvView, VK_IMAGE_LAYOUT_GENERAL}};
      VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
      wr[0].dstSet = set; wr[0].dstBinding = 0; wr[0].descriptorCount = 1; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = &ii[0];
      wr[1].dstSet = set; wr[1].dstBinding = 1; wr[1].descriptorCount = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[1].pImageInfo = &ii[1];
      vkUpdateDescriptorSets(U.h->device, 2, wr, 0, nullptr);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, U.pipe);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, U.pl, 0, 1, &set, 0, nullptr);
      vkCmdPushConstants(cb, U.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
      vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    }
    MvBarrier(cb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  }
  Transition(cb, o, VK_IMAGE_LAYOUT_GENERAL);
  Eval e;
  e.cb = cb;
  e.color = {c->image, c->View(nullptr, true), c->fi->vk};
  e.depth = {z->image, z->View(nullptr, true), z->fi->vk};
  e.mv = {U.mvImg, U.mvView, VK_FORMAT_R16G16_SFLOAT};
  e.out = {o->image, o->View(nullptr, true), o->fi->vk};
  e.w = w; e.h = h; e.ow = ow; e.oh = oh;
  e.jx = ujx; e.jy = ujy;
  e.phases = Phases(w, ow);
  e.reset = U.reset || cut;
  e.hdr = true;
  e.invDepth = U.invDepth; e.infFar = U.infFar;   // read from the projection: reversed infinite z in the game (ndc 1 = near)
  LARGE_INTEGER now; QueryPerformanceCounter(&now);
  { LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    const double dt = U.lastEval.QuadPart ? (double)(now.QuadPart - U.lastEval.QuadPart) * 1000.0 / (double)f.QuadPart : 0;
    e.dtMs = dt > 0 && dt < 100 ? (float)dt : 16.67f; }
  U.lastEval = now;
  e.nearZ = U.nearZ; e.farZ = U.farZ; e.fovY = U.fovY;
  // the barrier the backends need after their own work is the replay's: `out` goes GENERAL -> sampled at the final pass
  static const bool noEval = getenv("SNAP_UPSCALE_NOEVAL") != nullptr;   // test: everything but the backend call
  const double tb = Now();
  int cmp[4]; const int ncmp = front::CompareBackends(cmp);
  bool evOk = false;
  U.shown = o;
  if (ncmp > 0 && !noEval) {   // test aid: all backends, shown in turn
    const int pick = cmp[g_presentCount % ncmp];
    for (int i = 0; i < ncmp; i++) {
      TextureImpl*& co = U.cmpOut[cmp[i]];
      if (!co) {
        co = new TextureImpl(); co->host = o->host; co->b = o->b; co->fi = o->fi; co->hostW = ow; co->hostH = oh; co->scale = o->scale;
        if (!co->EnsureImage()) { delete co; co = nullptr; continue; }
        if (i == 0) Logf("compare: %d backends upscale every frame; present %% %d picks the shown one (0 = %s)", ncmp, ncmp, front::BackendName(cmp[0]));
      }
      Transition(cb, co, VK_IMAGE_LAYOUT_GENERAL);
      Eval ei = e; ei.out = {co->image, co->View(nullptr, true), co->fi->vk};
      const bool ok = front::EvaluateBackend(cmp[i], ei);
      co->uploaded = true;
      if (cmp[i] == pick && ok) { U.shown = co; evOk = true; }
    }
  } else {
    evOk = noEval ? false : front::Evaluate(e);
  }
  U.bkMsSum += Now() - tb;
  if (front::LogLevel() && ++U.resolves % 300 == 0) {
    static double lastT = 0; static int lastP = 0; const double t = Now();
    Logf("p%d stats: %llu upscaled of %llu, record %.2f ms avg (backend %.2f ms), %.1f presents/s", g_presentCount, (unsigned long long)U.evals, (unsigned long long)U.resolves,
         U.evalMsSum / (double)U.resolves, U.bkMsSum / (double)U.resolves, lastT > 0 ? (g_presentCount - lastP) * 1000.0 / (t - lastT) : 0.0);
    lastT = t; lastP = g_presentCount;
  }
  if (evOk) {
    U.evaluated = true; U.evals++; U.reset = false;
    o->uploaded = true;
    if (front::LogLevel() >= 2 || (front::LogLevel() && U.evals <= 3))
      Logf("p%d upscaled %ux%u -> %ux%u jitter (%.3f %.3f)%s%s%s", g_presentCount, w, h, ow, oh, ujx, ujy, e.reset ? " reset" : "",
           cut ? " (camera cut)" : "", U.haveCur ? "" : " (no camera)");
  } else {
    U.reset = true;
    U.on = snap_upscale::ActiveBackend() != BK_OFF;
  }
  if (U.haveCur) { U.prev = U.cur; memcpy(U.prevEye, U.eye, sizeof U.eye); memcpy(U.prevFwd, U.fwd, sizeof U.fwd); U.havePrev = true; }
  else U.havePrev = false;
  NextJitter(w, ow);
  U.evalMsSum += Now() - t0;
}

static void CurJitter(const ReplayCtx& cx, float* jx, float* jy) {
  *jx = *jy = 0;
  if (!U.on || U.phase != 1 || U.frameHold || !front::JitterOn() || !cx.rt[8] || cx.rt[8] != U.sceneDepth) return;
  *jx = U.jx; *jy = U.jy;
}

bool UpscaleOnSetRenderTargets(ReplayCtx& cx) {
  if (!U.on) return false;
  TextureImpl* d = cx.rt[8]; TextureImpl* c0 = cx.rt[0];
  if (d && IsSceneDepth(d)) {
    if (U.phase == 0) {   // a new frame's scene
      U.phase = 1; U.sceneDepth = d; U.sceneColor = nullptr; U.haveCur = false; U.jitUsed = false;
      U.frameHold = U.submitPhoto || U.hold > 0 || AbNative();
      U.frames++;
      if (U.frameHold) U.holds++;   // a short hold keeps the history: the next vectors span the gap (prev = last upscaled frame)
    }
    if (U.phase == 1 && d == U.sceneDepth && IsSceneColor(c0)) U.sceneColor = c0;
  } else if (U.phase == 1 && IsDisplay(c0)) {
    U.phase = 2;
    if (!U.frameHold) Resolve(cx);
  }
  if (front::LogLevel() && c0 && c0->b.width == 360 && c0->b.height == 240) {
    static int lastP = -1;
    if (lastP != g_presentCount) {
      lastP = g_presentCount;
      Logf("p%d %s bound: phase %d, frame held %d, upscaled this present %d, scene drawn %s", g_presentCount, d && d == U.evalDepth ? "Evaluation depth" : "360x240 photo target",
           U.phase, (int)U.frameHold, (int)U.evaluated, U.phase == 0 ? (U.prevHeld ? "not yet (display holds the previous present, which was held: native, unjittered)" : "not yet (display holds the previous present, which was UPSCALED)") : "yes");
    }
  }
  float jx, jy; CurJitter(cx, &jx, &jy);
  return cx.cb != U.ecb ? (jx != 0 || jy != 0) : (jx != U.ejx || jy != U.ejy);
}

void UpscaleViewportJitter(const ReplayCtx& cx, float* jx, float* jy) {
  CurJitter(cx, jx, jy);
  U.ecb = cx.cb; U.ejx = *jx; U.ejy = *jy;
  if (*jx != 0 || *jy != 0) U.jitUsed = true;
}

TextureImpl* UpscaleSubstitute(const ReplayCtx& cx, TextureImpl* t) {
  if (!U.evaluated || U.phase != 2 || !t || t != U.sceneColor || !U.shown || !IsDisplay(cx.rt[0])) return nullptr;
  return U.shown;
}

void UpscaleBeforeSubmit(int n, CmdList* const* lists) {
  if (!U.on) return;
  bool photo = false;
  const char* why = "";
  for (int k = 0; k < n && !photo; k++) {
    if (!lists[k]) continue;
    int bound = 0;   // 1: the Evaluation depth target, 2: a 360x240 photo target is bound (the game binds them without drawing too)
    for (const Cmd& c : lists[k]->cmds) {
      if (c.op == Op::SetRenderTargets) {
        const TextureImpl* d = c.tex[8]; const TextureImpl* c0 = c.tex[0];
        bound = (d && d == U.evalDepth) ? 1 : (c0 && c0->b.width == 360 && c0->b.height == 240) ? 2 : 0;
      } else if (bound && (c.op == Op::Draw || c.op == Op::DrawIndexed || c.op == Op::DrawIndirect || c.op == Op::DrawIndexedIndirect)) {
        photo = true; why = bound == 1 ? "draws into the Evaluation depth target" : "draws into a 360x240 photo target";
        break;
      }
    }
  }
  U.submitPhoto = photo;
  if (!photo) return;
  U.photoSubmits++;
  U.hold = std::max(U.hold, 2);
  if (U.phase == 1 && !U.frameHold) {
    // the scene of this frame started in an earlier submit: stop its jitter and do not upscale it
    if (U.jitUsed) { U.lateHold++; Logf("p%d photo pass found after jittered scene draws of the same frame", g_presentCount); }
    U.frameHold = true; U.holds++;
  }
  if (front::LogLevel()) Logf("p%d photo submit (%d lists, %s): frame unjittered, not upscaled", g_presentCount, n, why);
}

void UpscaleOnDraw(ReplayCtx& cx, const uint8_t* const* vsUbo, const uint64_t* vsRange, int n) {
  if (!U.on || U.phase != 1 || !cx.rt[8] || cx.rt[8] != U.sceneDepth) return;
  const int probe = front::Probe();
  static int probeFrom = -1;
  if (probe >= 0 && probeFrom < 0 && g_presentCount >= probe) probeFrom = g_presentCount;   // the first 3 scene frames from that present
  if (probeFrom >= 0 && g_presentCount < probeFrom + 3) {
    if (U.probePresent != g_presentCount) { U.probePresent = g_presentCount; U.probeDraws = 0; }
    if (U.probeDraws++ < 3) {
      for (int i = 0; i < n && i < 2; i++) {
        if (!vsUbo[i]) continue;
        const float* f = (const float*)vsUbo[i];
        const int nf = (int)std::min<uint64_t>(vsRange[i], 0x180) / 4;
        char line[4096]; int k = snprintf(line, sizeof line, "probe p%d draw%d c%d range %llu:", g_presentCount, U.probeDraws, i + 3, (unsigned long long)vsRange[i]);
        for (int j = 0; j < nf && k < (int)sizeof line - 24; j++) k += snprintf(line + k, sizeof line - k, "%s%.5g", j % 16 == 0 ? " |" : " ", f[j]);
        Logf("%s", line);
      }
    }
  }
  if (!U.haveCur && n > 0 && vsUbo[0] && vsRange[0] >= kCamBytes) TakeCamera(vsUbo[0]);
  else if (!U.haveCur && front::LogLevel()) {
    static int logged = 0;
    if (logged++ < 6) Logf("p%d scene draw without a camera bank: c3 %p range %llu, c4 %p range %llu", g_presentCount, (const void*)vsUbo[0],
                           (unsigned long long)vsRange[0], n > 1 ? (const void*)vsUbo[1] : nullptr, n > 1 ? (unsigned long long)vsRange[1] : 0ull);
  }
}

void UpscaleOnPresent() {
  if (!U.on && !U.evaluated) return;
  if (U.phase == 1) U.missed++;
  if (U.evaluated) U.lastEvalPresent = g_presentCount;
  else if (g_presentCount - U.lastEvalPresent > 4 + 2 * AbPeriod()) U.reset = true;   // a longer gap (menus, photo select, loads) starts a new history
  U.prevHeld = !U.evaluated && (U.frameHold || U.phase == 0);
  U.phase = 0; U.evaluated = false;
  if (U.hold > 0 && !U.submitPhoto) U.hold--;
  // Shutter look-ahead: the photo pass often runs at the start of a present, before that present's scene, and downscales the
  // display texture, i.e. the PREVIOUS present. Florio route: A at f6000+24k, photo pass at p6003+24k (3 presents later), so
  // holding from the press edge keeps every present the photo can read native and unjittered. A and ZR shoot; R is included
  // as a cheap margin. Menu presses only cost a few native frames (a short hold keeps the history).
  static uint64_t lastBtn = 0;
  const uint64_t btn = hle::input::hub().pad().buttons;
  const uint64_t shoot = (1ull << hle::input::kA) | (1ull << hle::input::kZR) | (1ull << hle::input::kR);
  if ((btn & ~lastBtn) & shoot) { U.hold = std::max(U.hold, 6); U.shutterHolds++; }
  lastBtn = btn;
}
}  // namespace nvn
