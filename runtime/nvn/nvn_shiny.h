// SPDX-License-Identifier: MIT
// Shiny Pokemon mod (docs/shiny-mod-plan.md): per-draw texture twin swap. Off unless SNAP_SHINY=1 (or SNAP_SHINY_TRACE=1 for the log only).
#pragma once
#include <cstdint>
struct ShinyDraw { uint64_t vb0 = 0, vb1 = 0; const void* uboPool[8] = {}; uint64_t uboOff[8] = {}, uboRange[8] = {}; int rtW = 0, rtH = 0, prog = -1; };
namespace nvn { struct TextureImpl; void ShinyBufferInit(uint64_t va, uint64_t size); void ShinyBufferFinalize(uint64_t va, uint64_t size); bool ShinyActive(); TextureImpl* ShinySubstitute(TextureImpl* t, const ShinyDraw& d); }
