// [uismooth] Smooth interface above 60 fps (SNAP_UI_SMOOTH=0 turns it off; F1 > Frame rate > Smooth interface).
//
// The game's interface counts frames, so above 60 fps host/mods/uipace.cpp lets it step 60 times a second: a moving element (the frame over a
// Pokemon, the reticle, a sliding menu) keeps its position for 1 or 2 displayed frames and then jumps one step. This file hides the jumps with
// no delay: for each interface draw that moved by the same amount in the last two steps it draws the element where it would be now, a
// fraction of the step ahead along its own motion (dead reckoning), by moving the viewport of that draw. The next step lands on the real
// position, so the drawn path is the game's path; nothing is drawn behind the game and the game's state, scoring and photo are untouched.
// Draws are matched across presents by vertex count, size and nearest last position; the four edges of the box move on their own, so an element
// that grows or shrinks (a zoom) is smoothed too. An element that stops is drawn at most one step past its last position for the 1-2 presents
// the missing step takes to show; a draw that does not match the last two steps is left alone.
#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace nvn {
extern double g_paceHz;   // nvn_vk.cpp: rate of present interval 1, 0 = display-paced only
extern double g_paceEffHz;   // nvn_vk.cpp: the rate the presents follow (frame rate limit applied; measured when unlocked), 0 = unknown
extern int g_presentCount;   // nvn_tex_vk.cpp

inline std::atomic<int>& UiSmoothUser() { static std::atomic<int> v{[] { const char* e = getenv("SNAP_UI_SMOOTH"); return (e && e[0] == '0') ? 0 : 1; }()}; return v; }
// SNAP_UI_SMOOTH_AB=<P> (test aid, off when unset): smoothing is on only while (present count / P) is odd, so one run holds both sides of an A/B
inline int UiSmoothAbPeriod() { static const int p = [] { const char* e = getenv("SNAP_UI_SMOOTH_AB"); const int v = e ? atoi(e) : 0; return v > 0 ? v : 0; }(); return p; }
// on only while the display runs faster than the 60 Hz interface steps
inline bool UiSmoothOn() {
  if (UiSmoothUser().load(std::memory_order_relaxed) == 0 || !(g_paceEffHz > 60.5)) return false;
  const int ab = UiSmoothAbPeriod();
  return ab == 0 || ((g_presentCount / ab) & 1) != 0;
}

struct UiSmoothState {
  struct Slot { int n = 0, seen = -1, claimed = -1, updAt = -1, conf = 0; bool has = false; float b[4] = {0, 0, 0, 0}, d[4] = {0, 0, 0, 0}; };
  static constexpr int kSlots = 1024;
  Slot s[kSlots];
  int used = 0, present = -1, ord = 0;
  int64_t presentNs = 0;   // time of the present being drawn (first draw seen), when the caller passes one
  std::vector<int64_t> stepNs = std::vector<int64_t>(kSlots, 0);   // per slot: time of the present of its last step
};
// The interface's draws are replayed on more than one thread; one shared state (under this mutex) sees all of a present's draws.
inline std::mutex& UiSmoothMutex() { static std::mutex m; return m; }
inline UiSmoothState& UiSmoothShared() { static UiSmoothState s; return s; }

// One interface draw of n vertices covering box (min x, min y, max x, max y; guest pixels) in present number `present`; the shift of each of the
// box's four edges (x0, y0, x1, y1) to draw it with: all four equal for an element that slides, different for one that grows or shrinks.
// The draw is matched to the element it was in the last presents: same vertex count, similar size, and the box where the element was or one step on from there.
// (The order of the draws within a frame is not stable: frames the interface only draws, without a step, draw a different set.)
// nowNs: the time of this present on the steady clock (nvn_draw.cpp passes it, so the fraction of a step is measured, right at any frame rate,
// also with a frame rate limit or when the PC does not reach the target); -1 = count presents at g_paceHz (the unit test).
inline void UiSmoothOnDraw(UiSmoothState& st, int present, int n, const float* box, float* shift, int64_t nowNs = -1) {
  for (int k = 0; k < 4; k++) shift[k] = 0.0f;
  if (present != st.present) { st.present = present; st.ord = 0; st.presentNs = nowNs; }
  st.ord++;
  const float w = box[2] - box[0], h = box[3] - box[1];
  UiSmoothState::Slot* s = nullptr; float best = 256.0f + 1.0f;
  for (int i = 0; i < st.used; i++) {
    UiSmoothState::Slot& c = st.s[i];
    if (c.n != n || c.claimed == present || c.seen < present - 2 || c.seen > present) continue;
    const float cw = c.b[2] - c.b[0], ch = c.b[3] - c.b[1];
    if (std::fabs(w - cw) > 0.3f * cw + 2.0f || std::fabs(h - ch) > 0.3f * ch + 2.0f) continue;
    // the draw is this element if its box is where the element was (held) or one step on from there (stepped): the sum of the four edges' distances
    float held = 0.0f, stepped = 0.0f;
    for (int k = 0; k < 4; k++) { held += std::fabs(box[k] - c.b[k]); stepped += std::fabs(box[k] - (c.b[k] + c.d[k])); }
    const float cost = c.has && stepped < held ? stepped : held;
    if (cost < best) { best = cost; s = &c; if (cost == 0.0f) break; }
  }
  if (!s) {   // a new element (or one that jumped or changed a lot): start over
    for (int i = 0; i < st.used && !s; i++) if (st.s[i].seen < present - 2 || st.s[i].seen > present) s = &st.s[i];
    if (!s && st.used < UiSmoothState::kSlots) s = &st.s[st.used++];
    if (!s) return;
    s->n = n; s->seen = s->claimed = present; s->updAt = present; s->has = false; s->conf = 0;
    st.stepNs[s - st.s] = st.presentNs;
    for (int k = 0; k < 4; k++) s->b[k] = box[k];
    return;
  }
  s->claimed = s->seen = present;
  float d[4]; bool moved = false;
  for (int k = 0; k < 4; k++) { d[k] = box[k] - s->b[k]; if (d[k] != 0.0f) moved = true; }
  if (moved) {   // the game stepped this element: its real position
    const float mag = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + d[3] * d[3]);
    if (present - s->updAt <= 8) {
      if (s->has) {
        const float m0 = std::sqrt(s->d[0] * s->d[0] + s->d[1] * s->d[1] + s->d[2] * s->d[2] + s->d[3] * s->d[3]);
        const float dot = d[0] * s->d[0] + d[1] * s->d[1] + d[2] * s->d[2] + d[3] * s->d[3];
        s->conf = (m0 > 0.0f && dot > 0.0f && mag >= 0.5f * m0 && mag <= 2.0f * m0) ? (s->conf < 2 ? s->conf + 1 : 2) : 0;
      }
      for (int k = 0; k < 4; k++) s->d[k] = d[k];
      s->has = true;
    } else { s->has = false; s->conf = 0; }
    for (int k = 0; k < 4; k++) s->b[k] = box[k];
    s->updAt = present;
    st.stepNs[s - st.s] = st.presentNs;
    return;
  }
  if (!s->has || s->conf < 1) return;
  // held since the last step: the part of the step that has passed, up to one step (the next step is due by then; if it is not, it was the last)
  double frac;
  const int64_t t0 = st.stepNs[s - st.s];
  if (st.presentNs >= 0 && t0 > 0 && st.presentNs >= t0) {
    frac = (double)(st.presentNs - t0) / (1e9 / 60.0);
    if (frac > 1.25) return;           // no step for a step and a quarter: it stopped
    if (frac > 1.0) frac = 1.0;        // the step is a little late (the replay's clock is not the game frame's): hold at the next step, never jump back
  } else if (g_paceHz > 0.0) {
    frac = (double)(present - s->updAt) * (1000.0 / g_paceHz) / (1000.0 / 60.0);
    if (frac > 1.0001) return;
  } else return;
  for (int k = 0; k < 4; k++) shift[k] = (float)(s->d[k] * frac);
  if (box[2] + shift[2] - (box[0] + shift[0]) < 1.0f || box[3] + shift[3] - (box[1] + shift[1]) < 1.0f) for (int k = 0; k < 4; k++) shift[k] = 0.0f;   // never collapse a quad
}
}  // namespace nvn
