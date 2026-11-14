// SPDX-License-Identifier: MIT
// Anti-aliasing and sharpening for the game's tonemap pass (project code, ours). See nvn_aa.cpp.
//   SNAP_AA=fxaa|off|smaa|ssaa2 (combine with + or ,  e.g. ssaa2+smaa)   SNAP_SHARPEN=0..1
#pragma once
#include <cstdint>
#include <vector>

namespace nvn {
struct ReplayCtx;
struct TextureImpl;
enum { AA_FXAA = 0, AA_OFF = 1, AA_SMAA = 2 };
extern int g_aaMode;        // what replaces the game's FXAA
extern float g_aaSharpen;   // contrast adaptive sharpening strength, 0 = off
// Parses SNAP_AA / SNAP_SHARPEN once. Returns the render scale SNAP_AA=ssaa2 asks for (2x the window height / 1080, capped at 3.0 and 8192 px), or 0.
float AaParseEnv();
// Window client height in pixels (SNAP_WINDOW_H, window.json, else 720). Used by ssaa2 and by the upscaler.
int HostWindowHeight();
// Called from DestroyTexture (nvn_vk.cpp): drops the cached UNORM views of this texture image.
void AaForgetTexture(const struct TextureImpl* t);
// Fragment shader fixups, called on a translated fragment module (key = first 12 hex digits of its cache key): turns the game's FXAA off by
// forcing its edge test to fail, so the tonemap shaders write the plain tonemapped color. No-op for other shaders and for SNAP_AA=fxaa.
void AaPatchFragment(std::vector<uint32_t>& words, const char* key12);
// Called after every draw; runs SMAA and/or CAS in place on render target 0 when the draw used one of the game's tonemap programs.
void AaAfterDraw(ReplayCtx& cx, const char* fragKey12);
// True for the game's tonemap programs (the four FXAA ones and the plain / LUT ones); the Fx post-tonemap slot (nvn_fx.cpp) uses it.
bool AaIsTonemapKey(const char* fragKey12);
}  // namespace nvn
