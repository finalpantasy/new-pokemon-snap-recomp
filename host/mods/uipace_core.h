// SPDX-License-Identifier: MIT
// Interface pacing above 60 fps: the pure decision and pad-sampling logic (no guest, host or time access), so the native test
// (tests/mods/uipace_test.cpp) can drive it directly. The hooks that use it are in host/mods/uipace.cpp.
//
// The game's interface counts game frames, not seconds, so it runs at the frame rate. Above 60 fps it should take
// 60 steps a second of real time; the other frames only draw. Method and layout facts follow NewSnapRecomp's UI-HZ-01 /
// INPUT-HZ-01 (MIT, see THIRD_PARTY_NOTICES.md).
#pragma once
#include <cstdint>
#include <cstring>

namespace uipace {

constexpr uint64_t kStepNs = 16666667ull;     // one interface step as the game paces it
constexpr uint64_t kSlackNs = 250000ull;      // a frame this close to a whole step still takes it
constexpr uint64_t kMaxDtNs = 100000000ull;   // a hitch never banks steps
// Real time kept for later steps after a step (2026-11-23, hifps2): one step more than the one taken. Uneven frames (an unlocked
// 80 fps of 3, 3, 3 and 40 ms frames) lost a step in every long frame with no backlog (41 steps a second); with one step of backlog the
// short frames that follow take it (61 a second). A hitch still never banks more than one extra step.
constexpr uint64_t kBacklogNs = 2 * kStepNs - 1;

// True when frames shorter than a step are possible: the pace period is known and clearly below 1/60 s.
inline bool above60(uint64_t period_ns) { return period_ns != 0 && period_ns + kSlackNs < kStepNs; }

struct Pace {
    uint64_t frame = 0;     // frame counter of the last decision, plus 1 (0 = none yet)
    uint64_t last_ns = 0;   // host time of the last decision
    uint64_t acc_ns = 0;    // real time not yet turned into a step
    int step = 1;           // the decision in force: 1 = run the interface update, 0 = draw only
};

// One decision per frame id. Every call with the same frame id returns the decision already made. At 60 fps and below
// (period_ns == 0 or not above60) every frame steps. Above 60, a frame steps when a whole step of real time has accrued; at most
// one step per frame, and a long frame adds at most kMaxDtNs.
inline int decide(Pace& p, uint64_t frame, uint64_t now_ns, uint64_t period_ns) {
    if (p.frame == frame + 1) return p.step;
    const bool first = p.frame == 0;
    p.frame = frame + 1;
    if (!above60(period_ns) || first || now_ns <= p.last_ns) {
        p.last_ns = now_ns;
        p.acc_ns = 0;
        return p.step = 1;
    }
    const uint64_t dt = now_ns - p.last_ns;
    p.last_ns = now_ns;
    p.acc_ns += dt > kMaxDtNs ? kMaxDtNs : dt;
    if (p.acc_ns + kSlackNs < kStepNs) return p.step = 0;
    p.acc_ns = p.acc_ns > kStepNs ? p.acc_ns - kStepNs : 0;
    if (p.acc_ns > kBacklogNs) p.acc_ns = kBacklogNs;
    return p.step = 1;
}

// ---- pointer time (FPS-02)
// Frame time in seconds for the pointer update when the game passes 0: the last game-frame time (real_ns; 0 or over kMaxDtNs = the
// pace period), averaged over about 16 frames; a frame more than twice the average counts at its own length. Result 1 to 100 ms.
inline float pointer_dt(double& avg_ns, uint64_t real_ns, uint64_t period_ns) {
    if (!real_ns || real_ns > kMaxDtNs) real_ns = period_ns ? period_ns : kStepNs;
    avg_ns = avg_ns > 0 ? avg_ns + ((double)real_ns - avg_ns) / 16.0 : (double)real_ns;
    double dt = (double)real_ns > 2.0 * avg_ns ? (double)real_ns : avg_ns;
    if (dt < 1e6) dt = 1e6;
    if (dt > (double)kMaxDtNs) dt = (double)kMaxDtNs;
    return (float)(dt / 1e9);
}

// ---- pad sampling
// The game's pad object (update at main+0x925b60): +0x44 four 32-bit masks [held, pressed, released, previous], +0x54 one byte
// per button (current), +0x74 the same bytes of the previous update. Held buttons are read from the bytes, new presses
// by comparing the bytes with the previous copy; the masks are kept by the update but not read by the game's queries.
constexpr int kPadBytes = 32;
constexpr unsigned kPadMaskOff = 0x44, kPadBytesOff = 0x54;

struct PadState {
    bool seen = false;                 // the first update has run: last* describe what the pad showed at the last step
    uint32_t last_mask = 0;
    uint32_t between_mask = 0;         // buttons held on frames since the last step
    uint8_t last_bytes[kPadBytes] = {};
    uint8_t between[kPadBytes] = {};
};

// Applied at the end of each pad update. mask[4], bytes[2 * kPadBytes] (current, then previous) are the pad's fields.
// every_frame: 60 fps or below, the pad stays as the game wrote it (only remembered). Otherwise on a step the pad shows
// the buttons of this update compared with the last step's (a press and release in between still counts as pressed once),
// and on the other frames the buttons of the last step and no new edges. Sticks are not part of these fields.
inline void sample_pad(PadState& s, uint32_t* mask, uint8_t* bytes, bool step, bool every_frame) {
    uint8_t* previous = bytes + kPadBytes;
    if (!s.seen) {   // first update: the last step is what the pad shows now
        s.seen = true;
        s.last_mask = mask[0];
        std::memcpy(s.last_bytes, bytes, kPadBytes);
    }
    if (every_frame) {
        s.last_mask = mask[0];
        s.between_mask = 0;
        std::memcpy(s.last_bytes, bytes, kPadBytes);
        std::memset(s.between, 0, kPadBytes);
        return;
    }
    if (step) {
        const uint32_t last = s.last_mask;
        uint32_t held = mask[0];
        const uint32_t taps = s.between_mask & ~last & ~held;   // pressed and released between two steps
        held |= taps;
        mask[0] = held, mask[1] = held & ~last, mask[2] = last & ~held, mask[3] = last;
        for (int i = 0; i < kPadBytes; i++) {
            if (s.between[i] && !s.last_bytes[i] && !bytes[i]) bytes[i] = s.between[i];
            previous[i] = s.last_bytes[i];
        }
        s.last_mask = held;
        s.between_mask = 0;
        std::memcpy(s.last_bytes, bytes, kPadBytes);
        std::memset(s.between, 0, kPadBytes);
    } else {
        s.between_mask |= mask[0];
        for (int i = 0; i < kPadBytes; i++)
            if (bytes[i]) s.between[i] = bytes[i];
        mask[0] = mask[3] = s.last_mask;
        mask[1] = mask[2] = 0;
        std::memcpy(bytes, s.last_bytes, kPadBytes);
        std::memcpy(previous, s.last_bytes, kPadBytes);
    }
}

}  // namespace uipace
