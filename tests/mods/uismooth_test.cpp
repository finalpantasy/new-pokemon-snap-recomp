// Native test of the smooth interface above 60 fps (runtime/nvn/nvn_uismooth.h). No game needed.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
namespace nvn { double g_paceHz = 144.0, g_paceEffHz = 144.0; int g_presentCount = 0; }
#include "../../runtime/nvn/nvn_uismooth.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// One element (a 6-vertex box, 40 x 40) that the game moves by (dx, dy) on each step; steps happen on presents where step_at(p) is true.
struct Rig {
    nvn::UiSmoothState st;
    float x = 100, y = 200, w = 40;
    float s4[4];
    // sx/sy: the shift of the left/top edge; s4 holds all four
    void present(int p, bool step, float dx, float dy, float* sx, float* sy, int n = 6, float dw = 0.0f) {
        if (step) { x += dx; y += dy; w += dw; }
        const float box[4] = {x, y, x + w, y + 40};
        nvn::UiSmoothOnDraw(st, p, n, box, s4);
        *sx = s4[0]; *sy = s4[1];
    }
};

int main() {
    // 144 fps, steps on presents 0, 2, 5, 7, 10, ... (2.4 presents per 60 Hz step): 6 px a step to the right.
    {
        Rig r; float sx = 0, sy = 0; int shifted = 0; float maxShift = 0;
        for (int p = 0; p < 240; p++) {
            const bool step = (int)std::floor(p / 2.4) != (int)std::floor((p - 1) / 2.4) || p == 0;
            r.present(p, step, 6.0f, 0.0f, &sx, &sy);
            if (step) CHECK(sx == 0.0f && sy == 0.0f);                         // a step lands on the game's own position
            if (p > 30 && sx != 0.0f) shifted++;
            CHECK(sx <= 6.0f + 1e-3f && sx >= 0.0f && sy == 0.0f);            // never more than one step ahead, never behind
            maxShift = std::fmax(maxShift, sx);
        }
        CHECK(shifted > 100);                                                  // the held presents are drawn ahead
        CHECK(maxShift > 2.0f);
    }
    // The element stops: the held presents go back to the game's position after at most one step of time (3 presents at 144 fps).
    {
        Rig r; float sx = 0, sy = 0;
        int p = 0;
        for (; p < 60; p++) r.present(p, (p % 2) == 0, 4.0f, 0.0f, &sx, &sy);
        for (int k = 0; k < 12; k++, p++) r.present(p, false, 0, 0, &sx, &sy);
        CHECK(sx == 0.0f);
    }
    // An element that is not smooth (a jump, a different size) is left alone.
    {
        Rig r; float sx = 0, sy = 0;
        r.present(0, true, 5, 0, &sx, &sy); r.present(1, false, 0, 0, &sx, &sy); CHECK(sx == 0.0f);
        r.present(2, true, 5, 0, &sx, &sy); r.present(3, false, 0, 0, &sx, &sy); CHECK(sx == 0.0f);     // one delta is not enough
        r.present(4, true, 5, 0, &sx, &sy); r.present(5, false, 0, 0, &sx, &sy); CHECK(sx > 0.0f);      // two equal ones are
        r.present(6, true, 100, 0, &sx, &sy); r.present(7, false, 0, 0, &sx, &sy); CHECK(sx == 0.0f);   // a jump of 100 px (no match within reach: a new element)
    }
    // Two identical elements in a different draw order each present keep their own motion.
    {
        nvn::UiSmoothState st; float sy = 0; float a = 100, b = 500; int ok = 0;
        for (int p = 0; p < 120; p++) {
            const bool step = (p % 2) == 0;
            if (step) { a += 5; b -= 5; }
            const float ba[4] = {a, 100, a + 40, 140}, bb[4] = {b, 300, b + 40, 340};
            float sa = 0, sb = 0, t4[4];
            if (p % 3 == 0) { nvn::UiSmoothOnDraw(st, p, 6, ba, t4); sa = t4[0]; nvn::UiSmoothOnDraw(st, p, 6, bb, t4); sb = t4[0]; }
            else            { nvn::UiSmoothOnDraw(st, p, 6, bb, t4); sb = t4[0]; nvn::UiSmoothOnDraw(st, p, 6, ba, t4); sa = t4[0]; }
            if (p > 20 && !step) { CHECK(sa >= 0.0f && sb <= 0.0f); if (sa > 0.0f && sb < 0.0f) ok++; }
        }
        CHECK(ok > 40);
    }
    // An element that grows (a zoom or an iris): the right edge goes ahead of the left edge, the width stays positive.
    {
        Rig r; float sx = 0, sy = 0; int grown = 0;
        for (int p = 0; p < 120; p++) {
            r.present(p, (p % 2) == 0, -8.0f, 0.0f, &sx, &sy, 6, 16.0f);   // left edge -8, right edge +8 a step
            if (p > 20 && (p % 2) == 1) { CHECK(r.s4[0] < 0.0f && r.s4[2] > 0.0f); if (r.s4[2] - r.s4[0] > 0.0f) grown++; }
        }
        CHECK(grown > 40);
    }
    // Measured time (nvn_draw.cpp passes the present's time): unlocked at an uneven 170-230 fps, 6 px steps every 16.67 ms of real time.
    // Every held present is drawn its elapsed share of the step ahead, never past the next step and never behind the last one.
    {
        nvn::g_paceHz = 0.0;   // unlocked: no pace rate at all, the time alone decides
        nvn::UiSmoothState st; float x = 100, s4[4]; int64_t t = 1000000000, lastStep = t; double worst = 0; int shifted = 0;
        for (int p = 0; p < 600; p++) {
            t += (p % 3 == 0) ? 5900000 : 4300000;   // 5.9 / 4.3 / 4.3 ms
            const bool step = t - lastStep >= 16666667 - 250000 || p == 0;
            if (step) { x += 6.0f; lastStep = t; }
            const float box[4] = {x, 200, x + 40, 240};
            nvn::UiSmoothOnDraw(st, p, 6, box, s4, t);
            if (p > 30 && !step) {
                const double want = 6.0 * (double)(t - lastStep) / 16666667.0;
                CHECK(s4[0] >= 0.0f && s4[0] <= 6.0f);
                worst = std::fmax(worst, std::fabs(s4[0] - want));
                shifted += s4[0] > 0.0f;
            }
        }
        CHECK(worst < 0.01);
        CHECK(shifted > 200);
        nvn::g_paceHz = 144.0;
    }
    // At 60 fps and below, and with no pace, nothing is shifted (UiSmoothOn reads the effective rate: the limit applied, measured when unlocked).
    nvn::g_paceEffHz = 60.0;
    CHECK(!nvn::UiSmoothOn());
    nvn::g_paceEffHz = 0.0;
    CHECK(!nvn::UiSmoothOn());
    nvn::g_paceEffHz = 144.0;
    CHECK(nvn::UiSmoothOn());
    nvn::g_paceEffHz = 120.0;   // a 144 target under a 120 limit, or an unlocked 120
    CHECK(nvn::UiSmoothOn());
    nvn::UiSmoothUser().store(0);
    CHECK(!nvn::UiSmoothOn());
    std::printf(g_fail ? "uismooth_test: %d FAILED\n" : "uismooth_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
