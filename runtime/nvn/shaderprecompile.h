/* SPDX-License-Identifier: MIT
 * Shader / pipeline precompile control API (runtime/nvn/nvn_pipeline.cpp). Plain C so the UI layer can call it without NVN headers.
 * Thread-safe; every function may be called from any thread at any time, also before the Vulkan device exists (a begin() request made
 * early is started as soon as the device is up). Details: analysis/perf/shader_precompile_api.md. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  SHADERPRECOMPILE_MODE_UNSET = 0,     /* first launch: the user has not chosen yet (behaves like PLAYING, never holds the game) */
  SHADERPRECOMPILE_MODE_NOW = 1,       /* compile everything known at startup; the game is held on its present loop until done */
  SHADERPRECOMPILE_MODE_PLAYING = 2    /* compile on background threads while the game runs; unseen pipelines compile asynchronously */
};
enum {
  SHADERPRECOMPILE_IDLE = 0,           /* nothing running (never started, or nothing to do) */
  SHADERPRECOMPILE_RUNNING = 1,
  SHADERPRECOMPILE_DONE = 2,
  SHADERPRECOMPILE_CANCELLED = 3
};

/* Queues every known pipeline (recorded keys plus every translated shader) that is not compiled yet. Returns the total queued, 0 when
 * there is nothing to do. Safe to call again after DONE/CANCELLED. */
int shaderprecompile_begin(void);
int shaderprecompile_state(void);
/* done/total of the current or last run and an ETA in seconds (-1 until a rate is known). Any pointer may be NULL. */
void shaderprecompile_progress(uint32_t* done, uint32_t* total, double* eta_seconds);
/* Stops handing out work; jobs already compiling finish. The game is released immediately. */
void shaderprecompile_cancel(void);
int shaderprecompile_get_mode(void);
/* Persists the choice in <shadercache dir>\mode.txt; MODE_NOW starts a run (and holds the game) when the device exists. */
void shaderprecompile_set_mode(int mode);
/* Estimated wall-clock seconds a full run would take on this PC (from the last measured rate), for the "about N min" label. */
double shaderprecompile_estimate_seconds(void);
/* 1 while the game thread is being held on its present loop (mode NOW and a run in progress); the UI draws the progress screen then. */
int shaderprecompile_is_holding_game(void);
/* Pipelines the game asked for that are still compiling in the background (draws using them are skipped until ready). */
uint32_t shaderprecompile_background_pending(void);

#ifdef __cplusplus
}
#endif
