// Unit test for runtime/audio/audio_mix.h: 5.1 -> stereo downmix, 5.1 -> 7.1 and 5.1 -> 5.1 mapping, spatial bed order.
#include <cmath>
#include <cstdio>
#include <vector>
#include "audio_mix.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static bool near(float a, float b, float e = 1e-5f) { return std::fabs(a - b) <= e; }

// One frame with a single channel at full scale.
static void one(int ch, uint32_t sc, int16_t* f) { for (uint32_t i = 0; i < sc; ++i) f[i] = 0; f[ch] = 16384; }

int main() {
  using namespace snap_audio::mix;
  const float h = 16384 / 32768.0f;  // 0.5
  // 5.1 -> stereo
  {
    float d[2]; int16_t f[6];
    one(FL, 6, f); d[0] = d[1] = 0; accumulate_s16(d, 2, f, 6, 1);
    CHECK(near(d[0], h * kDownmixNorm) && d[1] == 0, "FL -> L only: %f %f", d[0], d[1]);
    one(FR, 6, f); d[0] = d[1] = 0; accumulate_s16(d, 2, f, 6, 1);
    CHECK(d[0] == 0 && near(d[1], h * kDownmixNorm), "FR -> R only");
    one(FC, 6, f); d[0] = d[1] = 0; accumulate_s16(d, 2, f, 6, 1);
    CHECK(near(d[0], h * kDownmixCenter * kDownmixNorm) && near(d[0], d[1]), "FC -> both at -3 dB: %f %f", d[0], d[1]);
    one(LFE, 6, f); d[0] = d[1] = 0; accumulate_s16(d, 2, f, 6, 1);
    CHECK(d[0] == 0 && d[1] == 0, "LFE dropped");
    one(BL, 6, f); d[0] = d[1] = 0; accumulate_s16(d, 2, f, 6, 1);
    CHECK(near(d[0], h * kDownmixCenter * kDownmixNorm) && d[1] == 0, "BL -> L only");
    one(BR, 6, f); d[0] = d[1] = 0; accumulate_s16(d, 2, f, 6, 1);
    CHECK(d[0] == 0 && near(d[1], h * kDownmixCenter * kDownmixNorm), "BR -> R only");
    // correlated full-scale FL+FC+BL sums to about 1.414 (the core's final clip catches it); real mixes are far below
    for (int i = 0; i < 6; ++i) f[i] = 32767;
    d[0] = d[1] = 0; accumulate_s16(d, 2, f, 6, 1);
    CHECK(near(d[0], 1.41417f, 1e-3f), "full-scale sum: %f", d[0]);
  }
  // 5.1 -> 5.1 copy, each channel lands in its own slot
  for (int c = 0; c < 6; ++c) {
    float d[6] = {}; int16_t f[6]; one(c, 6, f);
    accumulate_s16(d, 6, f, 6, 1);
    for (int o = 0; o < 6; ++o) CHECK(near(d[o], o == c ? h : 0), "5.1 copy ch%d out%d = %f", c, o, d[o]);
  }
  // 5.1 -> 7.1 (FL FR FC LFE BL BR SL SR): surrounds feed the side speakers, backs stay empty
  {
    const int expect[6] = {0, 1, 2, 3, 6, 7};
    for (int c = 0; c < 6; ++c) {
      float d[8] = {}; int16_t f[6]; one(c, 6, f);
      accumulate_s16(d, 8, f, 6, 1);
      for (int o = 0; o < 8; ++o) CHECK(near(d[o], o == expect[c] ? h : 0), "7.1 ch%d out%d = %f", c, o, d[o]);
    }
  }
  // stereo into 5.1 / 7.1: FL FR only; mono: front L/R
  {
    float d[6] = {}; int16_t f[2] = {16384, -16384};
    accumulate_s16(d, 6, f, 2, 1);
    CHECK(near(d[0], h) && near(d[1], -h) && d[2] == 0 && d[3] == 0 && d[4] == 0 && d[5] == 0, "stereo into 5.1");
    float m[2] = {}; int16_t mo[1] = {16384};
    accumulate_s16(m, 2, mo, 1, 1);
    CHECK(near(m[0], h) && near(m[1], h), "mono to L/R");
  }
  // multi-frame stride and accumulate (adds to existing content)
  {
    float d[12] = {}; int16_t f[12];
    for (int i = 0; i < 12; ++i) f[i] = (int16_t)((i % 6 == 4) ? 8192 : 0);
    accumulate_s16(d, 6, f, 6, 2); accumulate_s16(d, 6, f, 6, 2);
    CHECK(near(d[4], 0.5f) && near(d[10], 0.5f) && d[3] == 0, "two frames, accumulated twice: %f", d[4]);
  }
  // spatial bed: indices follow FL FR FC LFE BL BR and surrounds map to side objects
  CHECK(kBed51[FL] == BedObject::FrontLeft && kBed51[FR] == BedObject::FrontRight && kBed51[FC] == BedObject::FrontCenter &&
        kBed51[LFE] == BedObject::LowFrequency && kBed51[BL] == BedObject::SideLeft && kBed51[BR] == BedObject::SideRight, "bed map");
  // upmixer: a stereo-only 5.1 block gets center, LFE and surrounds; a block with real surround content is left alone
  {
    Upmixer u; std::vector<float> b(6 * 48000, 0.0f);
    for (int i = 0; i < 48000; ++i) { b[i * 6] = 0.3f * std::sin(i * 0.05f); b[i * 6 + 1] = 0.3f * std::sin(i * 0.05f + 1.0f); }
    u.process(b.data(), 6, 48000);
    double e[6] = {};
    for (int i = 24000; i < 48000; ++i) for (int c = 0; c < 6; ++c) e[c] += b[i * 6 + c] * b[i * 6 + c];
    CHECK(e[2] > 1.0 && e[3] > 0.0 && e[4] > 1.0 && e[5] > 1.0, "upmix fills FC/LFE/BL/BR: %f %f %f %f", e[2], e[3], e[4], e[5]);
    Upmixer v; std::vector<float> c2(6 * 4800, 0.0f);
    for (int i = 0; i < 4800; ++i) { c2[i * 6] = 0.3f; c2[i * 6 + 2] = 0.3f; c2[i * 6 + 4] = 0.3f; }
    std::vector<float> ref = c2; v.process(c2.data(), 6, 4800);
    CHECK(c2 == ref, "real surround content untouched");
    std::vector<float> d(8 * 4800, 0.0f); Upmixer w;
    for (int k = 0; k < 20; ++k) { for (int i = 0; i < 4800; ++i) { d[i * 8] = 0.3f; d[i * 8 + 1] = 0.3f; d[i * 8 + 2] = d[i*8+3] = d[i*8+6] = d[i*8+7] = 0; } w.process(d.data(), 8, 4800); }
    CHECK(d[4799 * 8 + 6] > 0.0f && d[4799 * 8 + 4] == 0.0f, "7.1: ambience in the side speakers, backs empty");
  }
  std::printf(fails ? "mix_test: %d FAILED\n" : "mix_test: all passed\n", fails);
  return fails ? 1 : 0;
}
