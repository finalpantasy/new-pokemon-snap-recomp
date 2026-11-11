// SPDX-License-Identifier: MIT
// NVN entry points, milestone 2: programs, buffers, textures views, samplers, texture/sampler pools, state objects,
// binds and draws. Calls record host data only (commands go to the recorded stream, objects to Impl structs);
// Vulkan work happens in nvn_draw.cpp at submit.
// Signatures: the game's call sites (see analysis/runtime/nvn_usage.md for the census; BindTexture/GetTextureHandle
// were found by reading the sampler-bind loop at 0x915e80) cross-checked against tools/ref/totk/runtime
// nvn_api_*.c (MIT community reconstruction, [TOTK-NVN]); state-object defaults follow the same reference.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "nvn_internal.h"
#include "nvn_state.h"

namespace nvn {
namespace {

Host* H(const void* dev) {
  auto* h = Impl<Host>(dev, MAGIC_DEVICE);
  if (!h) LogError("invalid Device object");
  return h;
}
Cmd* Rec(void* cb, Op op) {
  auto* c = Impl<CommandBufferImpl>(cb, MAGIC_CB);
  if (!c) return nullptr;
  if (!c->recording) { LogError("command recorded outside Begin/EndRecording"); return nullptr; }
  c->cmds.emplace_back();
  c->cmds.back().op = op;
  return &c->cmds.back();
}

// ---- Program --------------------------------------------------------------------------------------------------------
Boolean ProgramInitialize(void* prog, void* dev) {   // [CALL]: (NVNprogram*, NVNdevice*) -> bool
  Host* h = H(dev);
  if (!h) return 0;
  auto* p = new ProgramImpl();
  p->host = h;
  Bind(prog, MAGIC_PROGRAM, p);
  return 1;
}
void ProgramFinalize(void* prog) {
  if (auto* p = Impl<ProgramImpl>(prog, MAGIC_PROGRAM)) { ProgramDestroy(p); delete p; }
  Unbind(prog);
}
Boolean ProgramSetShaders(void* prog, int n, const ShaderData* sd) {   // (program, count, NVNshaderData{addr, control}*)
  auto* p = Impl<ProgramImpl>(prog, MAGIC_PROGRAM);
  return p && ProgramSetShadersImpl(p, n, sd);
}

// ---- Pools of descriptors ----------------------------------------------------------------------------------------------
// The real pools live in GPU memory (descriptor blocks); ours are host tables: id -> (texture, view) and id -> sampler.
// The memory pool + offset arguments only matter for the size, which the tables do not need.
Boolean TexturePoolInitialize(void* tp, const void* /*pool*/, ptrdiff_t /*offset*/, int /*n*/) {
  auto* p = new TexPoolImpl();
  p->host = g_host;
  Bind(tp, MAGIC_TEXPOOL, p);
  return 1;
}
void TexturePoolFinalize(void* tp) { delete Impl<TexPoolImpl>(tp, MAGIC_TEXPOOL); Unbind(tp); }
void TexturePoolRegisterTexture(const void* tp, int id, const void* tex, const void* view) {
  auto* p = Impl<TexPoolImpl>(tp, MAGIC_TEXPOOL);
  auto* t = Impl<TextureImpl>(tex, MAGIC_TEXTURE);
  if (!p || !t) { LogError("TexturePoolRegisterTexture: bad pool or texture"); return; }
  TexSlot s;
  s.tex = t;
  if (view) { memcpy(&s.view, view, sizeof(TexView)); s.hasView = true; }
  p->slots[id] = s;
}
void TexturePoolRegisterImage(const void* tp, int id, const void* tex, const void* view) { TexturePoolRegisterTexture(tp, id, tex, view); }
void TexturePoolSetDebugLabel(void*, const char*) {}
Boolean SamplerPoolInitialize(void* sp, const void* /*pool*/, ptrdiff_t /*offset*/, int /*n*/) {
  auto* p = new SampPoolImpl();
  p->host = g_host;
  Bind(sp, MAGIC_SAMPPOOL, p);
  return 1;
}
void SamplerPoolFinalize(void* sp) { delete Impl<SampPoolImpl>(sp, MAGIC_SAMPPOOL); Unbind(sp); }
void SamplerPoolRegisterSampler(const void* sp, int id, const void* samp) {
  auto* p = Impl<SampPoolImpl>(sp, MAGIC_SAMPPOOL);
  auto* s = Impl<SamplerImpl>(samp, MAGIC_SAMPLER);
  if (!p || !s) { LogError("SamplerPoolRegisterSampler: bad pool or sampler"); return; }
  p->slots[id] = s;
}
void SamplerPoolSetDebugLabel(void*, const char*) {}
uint64_t DeviceGetImageHandle(const void*, int id) { return (uint64_t)(uint32_t)id; }   // [TOTK-NVN] image handle = pool index

// ---- Buffer ---------------------------------------------------------------------------------------------------------
BufferBuilder* BB(void* b) { return reinterpret_cast<BufferBuilder*>(b); }
void BufferBuilderSetDevice(void* b, void* dev) { BB(b)->device = dev; }
void BufferBuilderSetDefaults(void* b) { void* d = BB(b)->device; *BB(b) = {}; BB(b)->device = d; }
void BufferBuilderSetStorage(void* b, void* pool, ptrdiff_t off, size_t size) { BB(b)->pool = pool; BB(b)->offset = off; BB(b)->size = size; }
Boolean BufferInitialize(void* buf, const void* builder) {
  auto* b = reinterpret_cast<const BufferBuilder*>(builder);
  auto* p = Impl<PoolImpl>(b->pool, MAGIC_POOL);
  if (!p) { LogError("BufferInitialize: builder has no memory pool"); return 0; }
  auto* i = new BufferImpl();
  i->pool = p; i->offset = b->offset; i->size = b->size;
  Bind(buf, MAGIC_BUFFER, i);
  return 1;
}
void BufferFinalize(void* buf) { delete Impl<BufferImpl>(buf, MAGIC_BUFFER); Unbind(buf); }
void* BufferMap(const void* buf) {
  auto* i = Impl<BufferImpl>(buf, MAGIC_BUFFER);
  return (i && i->pool->Cpu() && i->pool->storage) ? static_cast<uint8_t*>(i->pool->storage) + i->offset : nullptr;
}
uint64_t BufferGetAddress(const void* buf) {
  auto* i = Impl<BufferImpl>(buf, MAGIC_BUFFER);
  return i ? i->pool->va + (uint64_t)i->offset : 0;
}
void BufferFlushMappedRange(const void* buf, ptrdiff_t off, size_t size) {
  auto* i = Impl<BufferImpl>(buf, MAGIC_BUFFER);
  if (i) SyncPoolRange(i->pool, (uint64_t)(i->offset + off), size);
}
void BufferInvalidateMappedRange(const void*, ptrdiff_t, size_t) { if (g_host) FlushReadbacks(g_host, false); }   // GPU->guest transfers land in FlushReadbacks
void BufferSetDebugLabel(void*, const char*) {}
void* BufferGetMemoryPool(const void* buf) { auto* i = Impl<BufferImpl>(buf, MAGIC_BUFFER); return i ? i->pool->guest : nullptr; }
ptrdiff_t BufferGetMemoryOffset(const void* buf) { auto* i = Impl<BufferImpl>(buf, MAGIC_BUFFER); return i ? (ptrdiff_t)i->offset : 0; }
size_t BufferGetSize(const void* buf) { auto* i = Impl<BufferImpl>(buf, MAGIC_BUFFER); return i ? (size_t)i->size : 0; }

// ---- Texture builder extras + views ----------------------------------------------------------------------------------
TextureBuilder* TB(void* b) { return reinterpret_cast<TextureBuilder*>(b); }
void TextureBuilderSetSize1D(void* b, int w) { TB(b)->width = w; TB(b)->height = 1; TB(b)->depth = 1; }
void TextureBuilderSetSize3D(void* b, int w, int h, int d) { TB(b)->width = w; TB(b)->height = h; TB(b)->depth = d; }
void TextureBuilderSetStride(void* b, ptrdiff_t s) { TB(b)->stride = (int)s; }
// Packaged data is the GPU-native (block-linear) layout; deswizzling it is phase 3. The pointer is only recorded.
void TextureBuilderSetPackagedTextureData(void* b, const void* d) { TB(b)->packaged = d; }
int TextureGetDepth(const void* t);   // implemented in nvn_entry.cpp (kept there)

TexView* TV(void* v) { return reinterpret_cast<TexView*>(v); }
void TextureViewSetDefaults(void* v) { memset(v, 0, sizeof(TexView)); }
void TextureViewSetLevels(void* v, int base, int n) { TV(v)->flags |= 1; TV(v)->lvlBase = (uint8_t)base; TV(v)->lvlNum = (uint8_t)n; }
void TextureViewSetLayers(void* v, int first, int n) { TV(v)->flags |= 2; TV(v)->layerBase = (uint16_t)first; TV(v)->layerNum = (uint16_t)n; }
void TextureViewSetFormat(void* v, int f) { TV(v)->flags |= 4; TV(v)->format = (uint8_t)f; }
void TextureViewSetTarget(void* v, int t) { TV(v)->flags |= 8; TV(v)->target = (uint8_t)t; }
void TextureViewSetDepthStencilMode(void* v, int m) { TV(v)->flags |= 0x10; TV(v)->dsMode = (uint8_t)m; }

// ---- Sampler ---------------------------------------------------------------------------------------------------------
SamplerBuilder* SB(void* b) { return reinterpret_cast<SamplerBuilder*>(b); }
void SamplerBuilderSetDevice(void* b, void* dev) { SB(b)->device = dev; }
void SamplerBuilderSetDefaults(void* b) {   // GL defaults [TOTK-NVN nvnSamplerBuilderSetDefaults]
  auto* s = SB(b);
  void* d = s->device;
  *s = {};
  s->device = d;
  s->minFilter = 4;  // NEAREST_MIPMAP_LINEAR
  s->magFilter = 1;  // LINEAR
  s->wrap[0] = s->wrap[1] = s->wrap[2] = 1;  // REPEAT
  s->lodMin = -1000.f; s->lodMax = 1000.f;
  s->compareFunc = CMP_LESS;
  s->aniso = 1.f;
}
void SamplerBuilderSetMinMagFilter(void* b, int mn, int mg) { SB(b)->minFilter = mn; SB(b)->magFilter = mg; }
void SamplerBuilderSetWrapMode(void* b, int s, int t, int r) { SB(b)->wrap[0] = s; SB(b)->wrap[1] = t; SB(b)->wrap[2] = r; }
void SamplerBuilderSetLodClamp(void* b, float mn, float mx) { SB(b)->lodMin = mn; SB(b)->lodMax = mx; }
void SamplerBuilderSetLodBias(void* b, float v) { SB(b)->lodBias = v; }
void SamplerBuilderSetCompare(void* b, int mode, int func) { SB(b)->compareMode = mode; SB(b)->compareFunc = func; }
void SamplerBuilderSetBorderColor(void* b, const float* c) { memcpy(SB(b)->border, c, 16); }
void SamplerBuilderSetMaxAnisotropy(void* b, float a) { SB(b)->aniso = a; }

VkSamplerAddressMode Wrap(int w) {
  switch (w) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;            // CLAMP (GL clamp ~ clamp to edge on this hardware path)
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;   // MIRROR_CLAMP, MIRROR_CLAMP_TO_EDGE
    case 4: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;  // MIRROR_CLAMP_TO_BORDER (approximated), CLAMP_TO_BORDER
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;           // 7 CLAMP_TO_EDGE
  }
}
Boolean SamplerInitialize(void* samp, const void* builder) {
  auto* b = reinterpret_cast<const SamplerBuilder*>(builder);
  Host* h = H(b->device);
  if (!h) return 0;
  VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  const int mn = b->minFilter;   // 0 N, 1 L, 2 N_MIP_N, 3 L_MIP_N, 4 N_MIP_L, 5 L_MIP_L
  ci.minFilter = (mn == 1 || mn == 3 || mn == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  ci.magFilter = b->magFilter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  const bool mips = mn >= 2;
  ci.mipmapMode = (mn == 4 || mn == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  ci.addressModeU = Wrap(b->wrap[0]); ci.addressModeV = Wrap(b->wrap[1]); ci.addressModeW = Wrap(b->wrap[2]);
  ci.mipLodBias = b->lodBias;
  float aniso = b->aniso;
  if (g_anisoOverride > 1.f && mips && b->compareMode == 0) aniso = std::max(aniso, g_anisoOverride);
  if (aniso > 1.f && h->props.limits.maxSamplerAnisotropy > 1.f) {
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(h->phys, &f);
    if (f.samplerAnisotropy) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = std::min(aniso, h->props.limits.maxSamplerAnisotropy); }
  }
  if (b->compareMode != 0) { ci.compareEnable = VK_TRUE; ci.compareOp = (VkCompareOp)std::clamp(b->compareFunc - 1, 0, 7); }
  ci.minLod = mips ? std::max(b->lodMin, 0.f) : 0.f;
  ci.maxLod = mips ? std::max(b->lodMax, ci.minLod) : 0.f;
  const float* c = b->border;
  if (c[3] == 0.f) ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  else if (c[0] > 0.5f) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  else ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  auto* s = new SamplerImpl();
  s->host = h; s->b = *b;
  if (vkCreateSampler(h->device, &ci, nullptr, &s->sampler) != VK_SUCCESS) { delete s; LogError("vkCreateSampler failed"); return 0; }
  Bind(samp, MAGIC_SAMPLER, s);
  return 1;
}
void SamplerFinalize(void* samp) {
  if (auto* s = Impl<SamplerImpl>(samp, MAGIC_SAMPLER)) {
    if (s->host && s->host->device && s->sampler) vkDestroySampler(s->host->device, s->sampler, nullptr);
    delete s;
  }
  Unbind(samp);
}

// ---- State objects ---------------------------------------------------------------------------------------------------
template <class T> T* ST(void* p) { return reinterpret_cast<T*>(p); }
template <class T> const T* ST(const void* p) { return reinterpret_cast<const T*>(p); }

void BlendStateSetDefaults(void* s) { *ST<BlendSt>(s) = {BF_ONE, BF_ZERO, BF_ONE, BF_ZERO, BE_ADD, BE_ADD, 0, 0}; }
void BlendStateSetBlendTarget(void* s, int t) { ST<BlendSt>(s)->target = (uint8_t)t; }
void BlendStateSetBlendFunc(void* s, int a, int b, int c, int d) {
  auto* o = ST<BlendSt>(s);
  o->srcRgb = (uint8_t)a; o->dstRgb = (uint8_t)b; o->srcA = (uint8_t)c; o->dstA = (uint8_t)d;
}
void BlendStateSetBlendEquation(void* s, int rgb, int a) { ST<BlendSt>(s)->eqRgb = (uint8_t)rgb; ST<BlendSt>(s)->eqA = (uint8_t)a; }

void ColorStateSetDefaults(void* s) { *ST<ColorSt>(s) = {0, LOGIC_COPY, CMP_ALWAYS, 0}; }
void ColorStateSetBlendEnable(void* s, int i, Boolean en) {
  if (i < 0 || i > 7) return;
  auto* o = ST<ColorSt>(s);
  o->blendEn = en ? (uint8_t)(o->blendEn | (1u << i)) : (uint8_t)(o->blendEn & ~(1u << i));
}
void ColorStateSetAlphaTest(void* s, int func) { ST<ColorSt>(s)->alphaFunc = (uint8_t)func; }   // phase 3: shader specialization
int ColorStateGetAlphaTest(const void* s) { return ST<ColorSt>(s)->alphaFunc; }

void ChannelMaskStateSetDefaults(void* s) { ST<ChMaskSt>(s)->mask = 0xFFFFFFFFu; }
void ChannelMaskStateSetChannelMask(void* s, int i, Boolean r, Boolean g, Boolean b, Boolean a) {
  if (i < 0 || i > 7) return;
  uint32_t m = (r ? 1u : 0) | (g ? 2u : 0) | (b ? 4u : 0) | (a ? 8u : 0);
  auto* o = ST<ChMaskSt>(s);
  o->mask = (o->mask & ~(0xFu << (4 * i))) | (m << (4 * i));
}

void MultisampleStateSetDefaults(void* s) { memset(s, 0, sizeof(MsSt)); ST<MsSt>(s)->enable = 1; }
void MultisampleStateSetMultisampleEnable(void* s, Boolean v) { ST<MsSt>(s)->enable = v != 0; }
void MultisampleStateSetSamples(void* s, int n) { ST<MsSt>(s)->samples = (uint8_t)n; }
void MultisampleStateSetAlphaToCoverageEnable(void* s, Boolean v) { ST<MsSt>(s)->a2c = v != 0; }
void MultisampleStateSetAlphaToCoverageDither(void* s, Boolean v) { ST<MsSt>(s)->a2cDither = v != 0; }
Boolean MultisampleStateGetMultisampleEnable(const void* s) { return ST<MsSt>(s)->enable; }
int MultisampleStateGetSamples(const void* s) { return ST<MsSt>(s)->samples; }
Boolean MultisampleStateGetAlphaToCoverageEnable(const void* s) { return ST<MsSt>(s)->a2c; }
Boolean MultisampleStateGetAlphaToCoverageDither(const void* s) { return ST<MsSt>(s)->a2cDither; }
int MultisampleStateGetCoverageModulationMode(const void* s) { return ST<MsSt>(s)->covMod; }
Boolean MultisampleStateGetCoverageToColorEnable(const void* s) { return ST<MsSt>(s)->covToColor; }
int MultisampleStateGetCoverageToColorOutput(const void* s) { return ST<MsSt>(s)->covToColorOut; }
Boolean MultisampleStateGetSampleLocationsEnable(const void* s) { return ST<MsSt>(s)->slEn; }
Boolean MultisampleStateGetSampleLocationsGridEnable(const void* s) { return ST<MsSt>(s)->slGrid; }

void PolygonStateSetDefaults(void* s) { *ST<PolySt>(s) = {FACE_NONE, FRONT_CCW, POLY_FILL, 0}; }
void PolygonStateSetCullFace(void* s, int f) { ST<PolySt>(s)->cull = (uint8_t)f; }
void PolygonStateSetFrontFace(void* s, int f) { ST<PolySt>(s)->front = (uint8_t)f; }
void PolygonStateSetPolygonMode(void* s, int m) { ST<PolySt>(s)->mode = (uint8_t)m; }
void PolygonStateSetPolygonOffsetEnables(void* s, int e) { ST<PolySt>(s)->offsetEn = (uint8_t)e; }
int PolygonStateGetPolygonOffsetEnables(const void* s) { return ST<PolySt>(s)->offsetEn; }

void DepthStencilStateSetDefaults(void* s) {
  auto* o = ST<DsSt>(s);
  o->bits = 0;
  DsSet(*o, DS_DW, 1, 1); DsSet(*o, DS_DF, 4, CMP_LESS);
  DsSet(*o, DS_SF_F, 4, CMP_ALWAYS); DsSet(*o, DS_SF_B, 4, CMP_ALWAYS);
  for (int sh : {DS_FAIL_F, DS_ZFAIL_F, DS_ZPASS_F, DS_FAIL_B, DS_ZFAIL_B, DS_ZPASS_B}) DsSet(*o, sh, 4, SOP_KEEP);
}
void DepthStencilStateSetDepthTestEnable(void* s, Boolean v) { DsSet(*ST<DsSt>(s), DS_DT, 1, v != 0); }
void DepthStencilStateSetDepthWriteEnable(void* s, Boolean v) { DsSet(*ST<DsSt>(s), DS_DW, 1, v != 0); }
void DepthStencilStateSetDepthFunc(void* s, int f) { DsSet(*ST<DsSt>(s), DS_DF, 4, (uint32_t)f); }
void DepthStencilStateSetStencilTestEnable(void* s, Boolean v) { DsSet(*ST<DsSt>(s), DS_ST, 1, v != 0); }
void DepthStencilStateSetStencilFunc(void* s, int face, int fn) {
  if (face & FACE_FRONT) DsSet(*ST<DsSt>(s), DS_SF_F, 4, (uint32_t)fn);
  if (face & FACE_BACK) DsSet(*ST<DsSt>(s), DS_SF_B, 4, (uint32_t)fn);
}
void DepthStencilStateSetStencilOp(void* s, int face, int fail, int zfail, int zpass) {
  auto& o = *ST<DsSt>(s);
  if (face & FACE_FRONT) { DsSet(o, DS_FAIL_F, 4, (uint32_t)fail); DsSet(o, DS_ZFAIL_F, 4, (uint32_t)zfail); DsSet(o, DS_ZPASS_F, 4, (uint32_t)zpass); }
  if (face & FACE_BACK) { DsSet(o, DS_FAIL_B, 4, (uint32_t)fail); DsSet(o, DS_ZFAIL_B, 4, (uint32_t)zfail); DsSet(o, DS_ZPASS_B, 4, (uint32_t)zpass); }
}

void VertexAttribStateSetDefaults(void* s) { ST<VaSt>(s)->v = 0; }
void VertexAttribStateSetFormat(void* s, int fmt, ptrdiff_t off) {
  auto* o = ST<VaSt>(s);
  o->v = (o->v & ~0x1FFu & ~(0x7FFFFu << 13)) | ((uint32_t)fmt & 0x1FF) | (((uint32_t)off & 0x7FFFF) << 13);
}
void VertexAttribStateSetStreamIndex(void* s, int i) { auto* o = ST<VaSt>(s); o->v = (o->v & ~(0xFu << 9)) | (((uint32_t)i & 0xF) << 9); }
void VertexStreamStateSetDefaults(void* s) { *ST<VsSt>(s) = {0, 0}; }
void VertexStreamStateSetStride(void* s, ptrdiff_t st) { ST<VsSt>(s)->stride = (uint32_t)st; }

// ---- Command buffer: state binds -------------------------------------------------------------------------------------
template <class T> void BindStateCmd(void* cb, Op op, const void* s) {
  if (Cmd* c = Rec(cb, op)) memcpy(&c->a[0], s, sizeof(T));
}
void CommandBufferBindBlendState(void* cb, const void* s) { BindStateCmd<BlendSt>(cb, Op::BindBlend, s); }
void CommandBufferBindColorState(void* cb, const void* s) { BindStateCmd<ColorSt>(cb, Op::BindColor, s); }
void CommandBufferBindChannelMaskState(void* cb, const void* s) { BindStateCmd<ChMaskSt>(cb, Op::BindChannelMask, s); }
void CommandBufferBindPolygonState(void* cb, const void* s) { BindStateCmd<PolySt>(cb, Op::BindPolygon, s); }
void CommandBufferBindDepthStencilState(void* cb, const void* s) { BindStateCmd<DsSt>(cb, Op::BindDepthStencil, s); }
void CommandBufferBindMultisampleState(void* cb, const void* s) {
  if (Cmd* c = Rec(cb, Op::BindMultisample)) memcpy(&c->a[0], s, sizeof(MsSt));
}
static_assert(sizeof(MsSt) <= sizeof(uint64_t) * 4);
void CommandBufferBindVertexAttribState(void* cb, int n, const void* states) {
  if (Cmd* c = Rec(cb, Op::BindVertexAttribs)) {
    c->i[0] = n;
    c->blob.assign(static_cast<const uint8_t*>(states), static_cast<const uint8_t*>(states) + (size_t)n * sizeof(VaSt));
  }
}
void CommandBufferBindVertexStreamState(void* cb, int n, const void* states) {
  if (Cmd* c = Rec(cb, Op::BindVertexStreams)) {
    c->i[0] = n;
    c->blob.assign(static_cast<const uint8_t*>(states), static_cast<const uint8_t*>(states) + (size_t)n * sizeof(VsSt));
  }
}
void CommandBufferBindProgram(void* cb, const void* prog, int stages) {
  if (Cmd* c = Rec(cb, Op::BindProgram)) { c->p[0] = Impl<ProgramImpl>(prog, MAGIC_PROGRAM); c->i[0] = stages; }
}
struct BufferRange { uint64_t addr, size; };   // NVNbufferRange {address, size}
void CommandBufferBindVertexBuffers(void* cb, int first, int n, const BufferRange* r) {
  if (Cmd* c = Rec(cb, Op::BindVertexBuffers)) {
    c->i[0] = first; c->i[1] = n;
    c->blob.assign(reinterpret_cast<const uint8_t*>(r), reinterpret_cast<const uint8_t*>(r) + (size_t)n * sizeof(BufferRange));
  }
}
void CommandBufferBindVertexBuffer(void* cb, int idx, uint64_t addr, size_t size) {
  BufferRange r{addr, size};
  CommandBufferBindVertexBuffers(cb, idx, 1, &r);
}
void BindBufferCmd(void* cb, Op op, int stage, int idx, uint64_t addr, uint64_t size) {
  if (Cmd* c = Rec(cb, op)) { c->i[0] = stage; c->i[1] = idx; c->a[0] = addr; c->a[1] = size; }
}
void CommandBufferBindUniformBuffer(void* cb, int stage, int idx, uint64_t addr, size_t size) { BindBufferCmd(cb, Op::BindUbo, stage, idx, addr, size); }
void CommandBufferBindUniformBuffers(void* cb, int stage, int first, int n, const BufferRange* r) {
  for (int k = 0; k < n; k++) BindBufferCmd(cb, Op::BindUbo, stage, first + k, r[k].addr, r[k].size);
}
void CommandBufferBindStorageBuffer(void* cb, int stage, int idx, uint64_t addr, size_t size) { BindBufferCmd(cb, Op::BindSsbo, stage, idx, addr, size); }
void CommandBufferBindStorageBuffers(void* cb, int stage, int first, int n, const BufferRange* r) {
  for (int k = 0; k < n; k++) BindBufferCmd(cb, Op::BindSsbo, stage, first + k, r[k].addr, r[k].size);
}
// BindTexture(cb, stage, index, handle): index == the GLSL sampler binding (the game's sampler loop at 0x915e80 passes the
// loop index 0..31 and the handle from GetTextureHandle).
void CommandBufferBindTexture(void* cb, int stage, int idx, uint64_t handle) {
  if (Cmd* c = Rec(cb, Op::BindTexture)) { c->i[0] = stage; c->i[1] = idx; c->a[0] = handle; }
}
void CommandBufferBindTextures(void* cb, int stage, int first, int n, const uint64_t* h) {
  for (int k = 0; k < n; k++) CommandBufferBindTexture(cb, stage, first + k, h[k]);
}
void CommandBufferBindImages(void* cb, int stage, int first, int n, const uint64_t* h) {
  for (int k = 0; k < n; k++)
    if (Cmd* c = Rec(cb, Op::BindImage)) { c->i[0] = stage; c->i[1] = first + k; c->a[0] = h[k]; }
}
void CommandBufferSetTexturePool(void* cb, const void* tp) { if (Cmd* c = Rec(cb, Op::SetTexturePool)) c->p[0] = Impl<TexPoolImpl>(tp, MAGIC_TEXPOOL); }
void CommandBufferSetSamplerPool(void* cb, const void* sp) { if (Cmd* c = Rec(cb, Op::SetSamplerPool)) c->p[0] = Impl<SampPoolImpl>(sp, MAGIC_SAMPPOOL); }
void CommandBufferSetStencilValueMask(void* cb, int face, int v) { if (Cmd* c = Rec(cb, Op::StencilValueMask)) { c->i[0] = face; c->i[1] = v; } }
void CommandBufferSetStencilMask(void* cb, int face, int v) { if (Cmd* c = Rec(cb, Op::StencilMask)) { c->i[0] = face; c->i[1] = v; } }
void CommandBufferSetStencilRef(void* cb, int face, int v) { if (Cmd* c = Rec(cb, Op::StencilRef)) { c->i[0] = face; c->i[1] = v; } }
void CommandBufferSetPolygonOffsetClamp(void* cb, float factor, float units, float clamp) {
  if (Cmd* c = Rec(cb, Op::PolygonOffset)) { c->f[0] = factor; c->f[1] = units; c->f[2] = clamp; }
}
void CommandBufferSetShaderScratchMemory(void*, const void*, ptrdiff_t, size_t) {}   // Maxwell local-memory scratch: not needed on Vulkan

// ---- Command buffer: draws -------------------------------------------------------------------------------------------
void CommandBufferDrawArrays(void* cb, int prim, int first, int count) {
  if (Cmd* c = Rec(cb, Op::Draw)) { c->i[0] = prim; c->i[1] = first; c->i[2] = count; c->i[3] = 0; c->i[4] = 1; }
}
void CommandBufferDrawArraysInstanced(void* cb, int prim, int first, int count, int baseInstance, int instances) {
  if (Cmd* c = Rec(cb, Op::Draw)) { c->i[0] = prim; c->i[1] = first; c->i[2] = count; c->i[3] = baseInstance; c->i[4] = instances; }
}
void CommandBufferDrawElementsBaseVertex(void* cb, int prim, int type, int count, uint64_t indices, int baseVertex) {
  if (Cmd* c = Rec(cb, Op::DrawIndexed)) {
    c->i[0] = prim; c->i[1] = type; c->i[2] = count; c->i[3] = baseVertex; c->i[4] = 0; c->i[5] = 1; c->a[0] = indices;
  }
}
void CommandBufferDrawElements(void* cb, int prim, int type, int count, uint64_t indices) { CommandBufferDrawElementsBaseVertex(cb, prim, type, count, indices, 0); }
void CommandBufferDrawElementsInstanced(void* cb, int prim, int type, int count, uint64_t indices, int baseVertex, int baseInstance, int instances) {
  if (Cmd* c = Rec(cb, Op::DrawIndexed)) {
    c->i[0] = prim; c->i[1] = type; c->i[2] = count; c->i[3] = baseVertex; c->i[4] = baseInstance; c->i[5] = instances; c->a[0] = indices;
  }
}
struct CopyRegion { int x, y, z, w, h, d; };   // NVNcopyRegion
void CommandBufferCopyBufferToTexture(void* cb, uint64_t src, const void* tex, const void* view, const CopyRegion* r, int flags) {
  auto* t = Impl<TextureImpl>(tex, MAGIC_TEXTURE);
  if (!t) { LogError("CopyBufferToTexture: bad texture"); return; }
  if (Cmd* c = Rec(cb, Op::CopyBufferToTexture)) {
    c->tex[0] = t; c->a[0] = src; c->i[0] = view != nullptr; c->i[1] = flags;
    c->blob.resize(sizeof(CopyRegion) + sizeof(TexView));
    memcpy(c->blob.data(), r, sizeof(CopyRegion));
    TexView v{};
    if (view) memcpy(&v, view, sizeof v);
    memcpy(c->blob.data() + sizeof(CopyRegion), &v, sizeof v);
  }
}

void CommandBufferSetAlphaRef(void* cb, float ref) { if (Cmd* c = Rec(cb, Op::SetAlphaRef)) c->f[0] = ref; }
void CommandBufferSetPatchSize(void* cb, int n) { if (Cmd* c = Rec(cb, Op::SetPatchSize)) c->i[0] = n; }
void CommandBufferDrawArraysIndirect(void* cb, int prim, uint64_t indirect) {
  if (Cmd* c = Rec(cb, Op::DrawIndirect)) { c->i[0] = prim; c->a[1] = indirect; }
}
void CommandBufferDrawElementsIndirect(void* cb, int prim, int type, uint64_t indices, uint64_t indirect) {
  if (Cmd* c = Rec(cb, Op::DrawIndexedIndirect)) { c->i[0] = prim; c->i[1] = type; c->a[0] = indices; c->a[1] = indirect; }
}
void CommandBufferDispatchCompute(void* cb, int x, int y, int z) { if (Cmd* c = Rec(cb, Op::Dispatch)) { c->i[0] = x; c->i[1] = y; c->i[2] = z; } }
void CommandBufferReportCounter(void* cb, int type, uint64_t addr) {
  if (type == 1 && g_host) g_host->wantSamples = true;   // from now on rendering scopes are wrapped in occlusion queries
  if (Cmd* c = Rec(cb, Op::ReportCounter)) { c->i[0] = type; c->a[0] = addr; }
}
void CommandBufferCopyTextureToBuffer(void* cb, const void* tex, const void* view, const CopyRegion* r, uint64_t dst, int flags) {
  auto* t = Impl<TextureImpl>(tex, MAGIC_TEXTURE);
  if (!t) { LogError("CopyTextureToBuffer: bad texture"); return; }
  if (Cmd* c = Rec(cb, Op::CopyTextureToBuffer)) {
    c->tex[0] = t; c->a[0] = dst; c->i[0] = view != nullptr; c->i[1] = flags;
    c->blob.resize(sizeof(CopyRegion) + sizeof(TexView));
    memcpy(c->blob.data(), r, sizeof(CopyRegion));
    TexView v{}; if (view) memcpy(&v, view, sizeof v);
    memcpy(c->blob.data() + sizeof(CopyRegion), &v, sizeof v);
  }
}
void CommandBufferCopyTextureToTexture(void* cb, const void* src, const void* sview, const CopyRegion* sr, const void* dst, const void* dview, const CopyRegion* dr, int flags) {
  auto* s = Impl<TextureImpl>(src, MAGIC_TEXTURE); auto* d = Impl<TextureImpl>(dst, MAGIC_TEXTURE);
  if (!s || !d) { LogError("CopyTextureToTexture: bad texture"); return; }
  if (Cmd* c = Rec(cb, Op::CopyTextureToTexture)) {
    c->tex[0] = s; c->tex[1] = d; c->i[0] = (sview ? 1 : 0) | (dview ? 2 : 0); c->i[1] = flags;
    c->blob.resize(2 * (sizeof(CopyRegion) + sizeof(TexView)));
    uint8_t* b = c->blob.data();
    TexView v{};
    memcpy(b, sr, sizeof(CopyRegion)); if (sview) memcpy(&v, sview, sizeof v);
    memcpy(b + sizeof(CopyRegion), &v, sizeof v); b += sizeof(CopyRegion) + sizeof v;
    memcpy(b, dr, sizeof(CopyRegion)); v = TexView{}; if (dview) memcpy(&v, dview, sizeof v);
    memcpy(b + sizeof(CopyRegion), &v, sizeof v);
  }
}
uint64_t DeviceGetTimestampInNanoseconds(const void*, const void* counterData) {   // NVNcounterData{counter, timestamp}: the timestamp is already in ns
  uint64_t v[2] = {};
  if (counterData) memcpy(v, counterData, 16);
  return v[1];
}

// ---- table ----------------------------------------------------------------------------------------------------------------
#define X(n) {"nvn" #n, reinterpret_cast<GenericProc>(&n)}
const Entry kGfx[] = {
    X(DeviceGetImageHandle),
    X(ProgramInitialize), X(ProgramFinalize), X(ProgramSetShaders),
    X(TexturePoolInitialize), X(TexturePoolFinalize), X(TexturePoolRegisterTexture), X(TexturePoolRegisterImage), X(TexturePoolSetDebugLabel),
    X(SamplerPoolInitialize), X(SamplerPoolFinalize), X(SamplerPoolRegisterSampler), X(SamplerPoolSetDebugLabel),
    X(BufferBuilderSetDevice), X(BufferBuilderSetDefaults), X(BufferBuilderSetStorage), X(BufferInitialize), X(BufferFinalize), X(BufferMap),
    X(BufferGetAddress), X(BufferFlushMappedRange), X(BufferInvalidateMappedRange), X(BufferGetMemoryPool), X(BufferGetMemoryOffset),
    X(BufferGetSize), X(BufferSetDebugLabel),
    X(TextureBuilderSetSize1D), X(TextureBuilderSetSize3D), X(TextureBuilderSetPackagedTextureData), X(TextureBuilderSetStride),
    X(TextureViewSetDefaults), X(TextureViewSetLevels), X(TextureViewSetLayers), X(TextureViewSetFormat), X(TextureViewSetDepthStencilMode),
    X(TextureViewSetTarget),
    X(SamplerBuilderSetDevice), X(SamplerBuilderSetDefaults), X(SamplerBuilderSetMinMagFilter), X(SamplerBuilderSetWrapMode),
    X(SamplerBuilderSetLodClamp), X(SamplerBuilderSetLodBias), X(SamplerBuilderSetCompare), X(SamplerBuilderSetBorderColor),
    X(SamplerBuilderSetMaxAnisotropy), X(SamplerInitialize), X(SamplerFinalize),
    X(BlendStateSetDefaults), X(BlendStateSetBlendTarget), X(BlendStateSetBlendFunc), X(BlendStateSetBlendEquation),
    X(ColorStateSetDefaults), X(ColorStateSetBlendEnable), X(ColorStateSetAlphaTest), X(ColorStateGetAlphaTest),
    X(ChannelMaskStateSetDefaults), X(ChannelMaskStateSetChannelMask),
    X(MultisampleStateSetDefaults), X(MultisampleStateSetMultisampleEnable), X(MultisampleStateSetSamples),
    X(MultisampleStateSetAlphaToCoverageEnable), X(MultisampleStateSetAlphaToCoverageDither), X(MultisampleStateGetMultisampleEnable),
    X(MultisampleStateGetSamples), X(MultisampleStateGetAlphaToCoverageEnable), X(MultisampleStateGetAlphaToCoverageDither),
    X(MultisampleStateGetCoverageModulationMode), X(MultisampleStateGetCoverageToColorEnable), X(MultisampleStateGetCoverageToColorOutput),
    X(MultisampleStateGetSampleLocationsEnable), X(MultisampleStateGetSampleLocationsGridEnable),
    X(PolygonStateSetDefaults), X(PolygonStateSetCullFace), X(PolygonStateSetFrontFace), X(PolygonStateSetPolygonMode),
    X(PolygonStateSetPolygonOffsetEnables), X(PolygonStateGetPolygonOffsetEnables),
    X(DepthStencilStateSetDefaults), X(DepthStencilStateSetDepthTestEnable), X(DepthStencilStateSetDepthWriteEnable),
    X(DepthStencilStateSetDepthFunc), X(DepthStencilStateSetStencilTestEnable), X(DepthStencilStateSetStencilFunc),
    X(DepthStencilStateSetStencilOp),
    X(VertexAttribStateSetDefaults), X(VertexAttribStateSetFormat), X(VertexAttribStateSetStreamIndex),
    X(VertexStreamStateSetDefaults), X(VertexStreamStateSetStride),
    X(CommandBufferBindBlendState), X(CommandBufferBindChannelMaskState), X(CommandBufferBindColorState),
    X(CommandBufferBindMultisampleState), X(CommandBufferBindPolygonState), X(CommandBufferBindDepthStencilState),
    X(CommandBufferBindVertexAttribState), X(CommandBufferBindVertexStreamState), X(CommandBufferBindProgram),
    X(CommandBufferBindVertexBuffers), X(CommandBufferBindVertexBuffer), X(CommandBufferBindUniformBuffer),
    X(CommandBufferBindUniformBuffers), X(CommandBufferBindStorageBuffer), X(CommandBufferBindStorageBuffers),
    X(CommandBufferBindTexture), X(CommandBufferBindTextures), X(CommandBufferBindImages),
    X(CommandBufferDrawArrays), X(CommandBufferDrawElementsBaseVertex), X(CommandBufferDrawElements), X(CommandBufferDrawArraysInstanced),
    X(CommandBufferDrawElementsInstanced),
    X(CommandBufferSetAlphaRef), X(CommandBufferSetPatchSize), X(CommandBufferDrawArraysIndirect), X(CommandBufferDrawElementsIndirect),
    X(CommandBufferDispatchCompute), X(CommandBufferReportCounter), X(CommandBufferCopyTextureToBuffer), X(CommandBufferCopyTextureToTexture),
    X(DeviceGetTimestampInNanoseconds),
    X(CommandBufferSetStencilValueMask), X(CommandBufferSetStencilMask), X(CommandBufferSetStencilRef),
    X(CommandBufferSetPolygonOffsetClamp), X(CommandBufferCopyBufferToTexture), X(CommandBufferSetTexturePool),
    X(CommandBufferSetSamplerPool), X(CommandBufferSetShaderScratchMemory),
};
#undef X

}  // namespace

const Entry* GfxEntries(int* count) {
  *count = (int)(sizeof kGfx / sizeof *kGfx);
  return kGfx;
}

}  // namespace nvn
