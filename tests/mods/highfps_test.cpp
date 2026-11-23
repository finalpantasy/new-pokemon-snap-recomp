// Native test of the frame rate target (host/mods/highfps_core.h) and of the interface pacing at every rate (host/mods/uipace_core.h).
// No game needed.
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "../../host/mods/highfps_core.h"
#include "../../host/mods/uipace_core.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main() {
    using namespace highfps;
    // parsing: the game's own, any number from 61 to 1000, unlocked, monitor; anything else is invalid (ignored with a message)
    CHECK(parse(nullptr).kind == K_DEFAULT && parse("").kind == K_DEFAULT && parse("default").kind == K_DEFAULT && parse("60").kind == K_DEFAULT);
    CHECK(parse("59.94").kind == K_DEFAULT && parse("Original").kind == K_DEFAULT);
    const double rates[] = {72, 90, 120, 128, 143.988, 144, 165, 175, 180, 200, 240, 280, 300, 360, 480, 500, 1000};
    for (double r : rates) {
        char b[32];
        std::snprintf(b, sizeof b, "%g", r);
        const Target t = parse(b);
        CHECK(t.kind == K_FIXED && near(t.hz, r));
        CHECK(near(pace_hz(t, 0, 143.988), r));                  // no limit: the target
        CHECK(near(pace_hz(t, 2000, 143.988), r));               // a limit above it: the target
    }
    CHECK(parse(" 144 ").kind == K_FIXED);
    CHECK(parse("unlocked").kind == K_UNLOCKED && parse("UNCAPPED").kind == K_UNLOCKED && parse("max").kind == K_UNLOCKED && parse("unlimited").kind == K_UNLOCKED);
    CHECK(parse("monitor").kind == K_MONITOR && parse("Refresh").kind == K_MONITOR && parse("match").kind == K_MONITOR);
    CHECK(parse("30").kind == K_INVALID && parse("1001").kind == K_INVALID && parse("fast").kind == K_INVALID && parse("144fps").kind == K_INVALID && parse("0").kind == K_INVALID);
    // the frame rate limit wins when lower: one clock for the game's vsync and the limiter (perf pass 21: 144 under a saved 120 limit)
    CHECK(near(pace_hz(parse("144"), 120, 143.988), 120));
    CHECK(near(pace_hz(parse("240"), 141, 143.988), 141));      // VRR: limit 3 under the refresh
    CHECK(near(pace_hz(parse("144"), 30, 143.988), 60));        // a limit of 60 or lower keeps the game's own 60 (the limiter slows presents)
    CHECK(near(pace_hz(parse("default"), 120, 143.988), -1));   // default: nothing touched
    // unlocked: limiter off, or the limit when one is set
    CHECK(near(pace_hz(parse("unlocked"), 0, 143.988), 0));
    CHECK(near(pace_hz(parse("unlocked"), 200, 143.988), 200));
    CHECK(near(pace_hz(parse("unlocked"), 45, 143.988), 60));
    // monitor: the panel's exact refresh (locks to vblank), 60 until known or on a 60 Hz panel, the limit when lower
    CHECK(near(pace_hz(parse("monitor"), 0, 143.988), 143.988));
    CHECK(near(pace_hz(parse("monitor"), 0, 0), 60));
    CHECK(near(pace_hz(parse("monitor"), 0, 59.94), 60));
    CHECK(near(pace_hz(parse("monitor"), 0, 239.76), 239.76));
    CHECK(near(pace_hz(parse("monitor"), 120, 239.76), 120));

    // interface pacing: 60 steps a second of real time at every rate, paced or unlocked with uneven frames
    const double check_rates[] = {61, 72, 90, 120, 128, 144, 165, 180, 200, 240, 360, 500, 1000};
    for (double r : check_rates) {
        for (int jitter = 0; jitter < 2; jitter++) {
            uipace::Pace p;
            const uint64_t period = (uint64_t)(1e9 / r);
            uint64_t t = 1000000000, steps = 0;
            const uint64_t frames = (uint64_t)(r * 10);   // 10 s
            for (uint64_t f = 0; f < frames; f++) {
                // jitter: frames alternate 70% and 130% of the period (the average stays the rate)
                t += jitter ? (f & 1 ? period * 13 / 10 : period * 7 / 10) : period;
                steps += (uint64_t)uipace::decide(p, f, t, period);
            }
            const double per_s = (double)steps / ((double)(t - 1000000000) / 1e9);
            if (std::fabs(per_s - 60.0) > 0.6) std::printf("  rate %g jitter %d: %.2f steps/s\n", r, jitter, per_s);
            CHECK(std::fabs(per_s - 60.0) <= 0.6);
        }
    }
    // uneven unlocked frames (3, 3, 3, 40 ms: 81 fps on average) still give 60 steps a second (one step of backlog)
    {
        uipace::Pace p;
        uint64_t t = 1000000000, steps = 0;
        const uint64_t pat[4] = {3000000, 3000000, 3000000, 40000000};
        for (uint64_t f = 0; f < 4000; f++) { t += pat[f & 3]; steps += (uint64_t)uipace::decide(p, f, t, 12250000); }
        const double per_s = (double)steps / ((double)(t - 1000000000) / 1e9);
        if (std::fabs(per_s - 60.0) > 1.0) std::printf("  uneven: %.2f steps/s\n", per_s);
        CHECK(std::fabs(per_s - 60.0) <= 1.0);
    }
    // at 60 and below every frame steps (the game's own behavior is untouched)
    {
        uipace::Pace p;
        uint64_t t = 1000000000, steps = 0;
        for (uint64_t f = 0; f < 600; f++) { t += 16666667; steps += (uint64_t)uipace::decide(p, f, t, 16666666); }
        CHECK(steps == 600);
        CHECK(!uipace::above60(0) && !uipace::above60(16666666) && uipace::above60((uint64_t)(1e9 / 61)));
    }
    std::printf(g_fail ? "highfps_test: %d FAILED\n" : "highfps_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
