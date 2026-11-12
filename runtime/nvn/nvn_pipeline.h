// SPDX-License-Identifier: MIT
// Persistent, asynchronous pipeline compilation (nvn_pipeline.cpp). A graphics pipeline is identified by a PipeDesc: the stable
// translation-cache keys of its shader stages plus every piece of state the pipeline depends on. The descriptor is plain data,
// so it can be recorded to disk and replayed at startup on a worker pool; runtime misses compile on the same pool while the draw
// is skipped (or the replay thread compiles inline when SNAP_PIPELINE_ASYNC=0).
#pragma once
#include <type_traits>

#include "nvn_internal.h"
#include "nvn_state.h"

namespace nvn {

struct PipeDesc {                 // zero-initialised, then filled: memcmp / hash equality is identity
  char stage[5][16];              // translation cache key prefix of the VS, FS, GS, TCS, TES (index = NVN stage); empty = none
  int32_t topo, nattr, nstream, nColor, alphaFunc, patchSize, samples, depthFmt, stencilFmt;
  int32_t colorFmt[8];
  uint32_t blendEn, chmask;
  float alphaRef;
  uint32_t pad0, pad1;
  uint64_t dsBits;
  MsSt ms;
  PolySt poly;
  BlendSt blend[8];
  VaSt attr[16];
  VsSt stream[16];
  uint32_t pad2;
};
static_assert(std::is_trivially_copyable_v<PipeDesc> && sizeof(PipeDesc) == 464, "PipeDesc must have no padding (it is hashed and compared bytewise)");

// ---- implemented in nvn_draw.cpp (needs the replay helpers) -------------------------------------------------------------
// Builds the Vulkan pipeline for `pd` from the stage objects in `st[0..4]` (nullptr = absent) and `layout`. Thread-safe.
VkPipeline CreateGraphicsPipeline(Host* h, const PipeDesc& pd, StageProg* const st[6], VkPipelineLayout layout);

// ---- implemented in nvn_shader.cpp --------------------------------------------------------------------------------------
bool LoadStageFromCache(Host* h, const std::string& fullKey, StageProg* out);   // module + reflection from the translation cache
const char* TranslatorVersion();

// ---- implemented in nvn_pipeline.cpp ------------------------------------------------------------------------------------
// Returns the compiled pipeline for `pd`, or VK_NULL_HANDLE while it is still compiling (*pending = true) or when it cannot be built.
// `p` supplies the stage objects/layout for an inline (synchronous) compile and the full cache keys of the stages for background jobs.
// `mustHave`: the draw cannot be dropped (one-shot passes such as the photo thumbnail, evaluation and capture targets): wait for a pending
// background compile, or compile inline, instead of returning VK_NULL_HANDLE.
VkPipeline PipelineAcquire(Host* h, const PipeDesc& pd, ProgramImpl* p, bool* pending, bool mustHave = false);
void PipelineServiceInit(Host* h);       // after the device and cacheDir exist: loads the key list, starts the workers
void PipelineServiceShutdown(Host* h);   // joins workers, saves the cache
bool PipelineHoldGame();                 // true while "compile now" is running and the game must stay on its present loop
void PipelineRegisterStageKey(const char* key12, const char* fullKey);   // BuildStage: prefix -> cache file key for background loads

}  // namespace nvn
