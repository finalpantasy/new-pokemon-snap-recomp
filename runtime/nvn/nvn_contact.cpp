// SPDX-License-Identifier: MIT
// Screen-space contact shadows (SNAP_CONTACT_SHADOWS=1, off by default), an Fx pre-tonemap effect (project code, ours). See nvn_fx.cpp, shaders/contact.frag.
//   * Runs once per scene frame, at render resolution, before the upscaler / tonemap read the scene colour, on the linear HDR colour (multiplied in place).
//   * Needs the sun: the caster matrix is only bound during the ShadowCast draws, so nvn_fx.cpp captures it per present. No dynamic shadow pass in the
//     present (static-only course, night) = the effect skips that frame.
//   * Tunables (env, test aids): SNAP_CONTACT_STRENGTH (0.6), SNAP_CONTACT_LEN world units (0.6), SNAP_CONTACT_THICK (0.25), SNAP_CONTACT_FADE view distance (45),
//     SNAP_CONTACT_LSIGN (+1 / -1: force the sign of the light direction; default: the sign that points the light upward in world space).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nvn_contact_spv.h"
#include "nvn_fx.h"
#include "nvn_internal.h"

namespace nvn {
namespace {
struct G {
  bool failed = false, ready = false;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkShaderModule fs = VK_NULL_HANDLE;
  VkSampler samp = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE, pipeDbg = VK_NULL_HANDLE; VkFormat fmt = VK_FORMAT_UNDEFINED;
} g;

float EnvF(const char* n, float def) { const char* e = getenv(n); return e && *e ? (float)atof(e) : def; }
bool Enabled() { static const bool on = [] { const char* e = getenv("SNAP_CONTACT_SHADOWS"); return e && *e && atoi(e) != 0; }(); return on; }

struct Pc { float ivp[16]; float dclip[4]; float light[4]; float size[4]; float q[4]; };
static_assert(sizeof(Pc) == 128, "push constants must fit the 128 byte guarantee");

bool Setup(Host* h, VkFormat fmt) {
  if (!g.ready) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(h->device, &si, nullptr, &g.samp) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 1; dl.pBindings = &b;
    if (vkCreateDescriptorSetLayout(h->device, &dl, nullptr, &g.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Pc)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &g.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(h->device, &pli, nullptr, &g.pl) != VK_SUCCESS) return false;
    g.fs = FxMakeModule(h, kContactFrag, sizeof kContactFrag);
    if (!g.fs) return false;
    g.ready = true;
  }
  if (g.fmt != fmt || !g.pipe) {   // the scene colour format is fixed (RGBA16F), so this runs once
    g.pipe = FxMakeFullscreenPipeline(h, g.pl, g.fs, fmt, true);
    g.pipeDbg = FxMakeFullscreenPipeline(h, g.pl, g.fs, fmt, false);   // SNAP_CONTACT_DEBUG=7: the shadow factor replaces the picture
    g.fmt = fmt;
  }
  return g.pipe != VK_NULL_HANDLE;
}

void Pre(ReplayCtx& cx) {
  if (g.failed) return;
  const FxCam* cam = FxCamera(); const FxCaster* cs = FxCasterNow();
  TextureImpl* c = FxSceneColor(); TextureImpl* z = FxSceneDepth();
  static unsigned long long nRan = 0, nNoCaster = 0, nTot = 0;   // stats line every 600 scene frames (SNAP_FX_LOG=1)
  if (++nTot % 600 == 0 && getenv("SNAP_FX_LOG")) fprintf(stderr, "contact: p%d stats: %llu scene frames, %llu ran, %llu skipped for no dynamic shadow pass\n", g_presentCount, nTot, nRan, nNoCaster);
  if (cam && nTot % 600 == 1 && getenv("SNAP_FX_LOG")) {
    const double d0 = cam->nearZ * 0.1; double h[4];   // ndc (0, 0, near / 10): clip w must be 10 for reversed infinite z
    for (int r = 0; r < 4; r++) h[r] = cam->ivp[r][2] * d0 + cam->ivp[r][3];
    fprintf(stderr, "contact: p%d camera eye (%.2f %.2f %.2f) near %.4f fovY %.1f, ndc(0,0,near/10) -> clip w %.3f (expect 10), world (%.2f %.2f %.2f)\n", g_presentCount, cam->eye[0], cam->eye[1], cam->eye[2], cam->nearZ, cam->fovY * 57.29578f, 1.0 / h[3], h[0] / h[3], h[1] / h[3], h[2] / h[3]);
  }
  if (!cam || !cs || !c || !z) {
    if (!cs) nNoCaster++;
    return;
  }
  nRan++;
  if (c->HW() != z->HW() || c->HH() != z->HH() || !FxWritable(c)) return;
  if (!Setup(cx.h, c->fi->vk)) { g.failed = true; fprintf(stderr, "contact: setup failed, disabled\n"); return; }
  // toward the light: the caster matrix' depth axis points away from the light (or toward it, if the game reverses the shadow depth): take the sign that points up
  float sgn = EnvF("SNAP_CONTACT_LSIGN", 0.f);
  float L[3] = {cs->axis[0], cs->axis[1], cs->axis[2]};
  if (sgn == 0.f) sgn = L[1] > 0.f ? 1.f : -1.f;
  for (float& v : L) v *= sgn < 0 ? -1.f : 1.f;
  Pc pc{};
  for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) pc.ivp[k * 4 + r] = (float)cam->ivp[r][k];
  for (int r = 0; r < 4; r++) pc.dclip[r] = (float)(cam->vp[r][0] * L[0] + cam->vp[r][1] * L[1] + cam->vp[r][2] * L[2]);
  pc.light[0] = L[0]; pc.light[1] = L[1]; pc.light[2] = L[2]; pc.light[3] = EnvF("SNAP_CONTACT_STRENGTH", 0.6f);
  int dbgMode = (int)EnvF("SNAP_CONTACT_DEBUG", 0.f);   // debug views of contact.frag: 2 tints, 3 view distance, 4 normal, 5 sun facing, 6 depth, 7 the factor itself; 100 = cycle 2..7 every 120 presents
  if (dbgMode >= 100) dbgMode = 2 + (g_presentCount / 120) % 6;
  if (dbgMode >= 2) pc.light[3] += 10.f * (float)dbgMode;
  float jx, jy; FxJitter(&jx, &jy);
  pc.size[0] = (float)c->HW(); pc.size[1] = (float)c->HH(); pc.size[2] = jx; pc.size[3] = jy;
  pc.q[0] = EnvF("SNAP_CONTACT_LEN", 0.6f); pc.q[1] = EnvF("SNAP_CONTACT_THICK", 0.25f); pc.q[2] = EnvF("SNAP_CONTACT_FADE", 45.f); pc.q[3] = (float)(g_presentCount & 1023);
  static int logged; if (logged++ < 3) fprintf(stderr, "contact: p%d light toward (%.3f %.3f %.3f) map %u, %ux%u jitter (%.2f %.2f)\n", g_presentCount, L[0], L[1], L[2], cs->mapSize, c->HW(), c->HH(), jx, jy);
  VkImageLayout prev;
  VkImageView dv = FxDepthRead(cx, z, &prev);
  VkDescriptorSet set = FxAllocSet(cx, g.dsl);
  if (set && dv) {
    VkDescriptorImageInfo ii{g.samp, dv, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(cx.h->device, 1, &w, 0, nullptr);
    FxDrawFullscreen(cx, c, dbgMode == 7 && g.pipeDbg ? g.pipeDbg : g.pipe, g.pl, set, &pc, sizeof pc);
  }
  FxDepthDone(cx, z, prev);
}

const FxEffect kEffect = {"contact_shadows", Enabled, Pre, nullptr};
struct Reg { Reg() { FxRegister(&kEffect); } } reg;
}  // namespace
}  // namespace nvn
