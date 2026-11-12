// SPDX-License-Identifier: MIT
// Channel mapping and downmix for the AudioOut core. Pure functions (no OS headers) so tests/audio can check them.
// Guest layout (Switch and Wwise 5.1): FL FR FC LFE BL BR. Device layouts follow WAVEFORMATEXTENSIBLE order:
// 5.1 = FL FR FC LFE BL BR (mask 0x3F), 7.1 = FL FR FC LFE BL BR SL SR (mask 0x63F).
#pragma once
#include <algorithm>
#include <cstdint>

namespace snap_audio {
namespace mix {

enum Ch51 { FL = 0, FR = 1, FC = 2, LFE = 3, BL = 4, BR = 5 };

// 5.1 -> stereo (ITU-style): L = (FL + 0.707 FC + 0.707 BL) / 1.707, same for R. LFE is dropped.
constexpr float kDownmixCenter = 0.70710678f;
constexpr float kDownmixNorm = 1.0f / 1.70710678f;

// Which bed object each 5.1 channel feeds in the Windows spatial path (index into the 6 channels). Names are the
// ISpatialAudioClient AudioObjectType values: the 5.1 surrounds sit at about 110 degrees, so they feed the SIDE
// objects, which is what Windows Sonic and Dolby Atmos for Headphones place there.
enum class BedObject { FrontLeft, FrontRight, FrontCenter, LowFrequency, SideLeft, SideRight };
constexpr BedObject kBed51[6] = {BedObject::FrontLeft, BedObject::FrontRight, BedObject::FrontCenter,
                                 BedObject::LowFrequency, BedObject::SideLeft, BedObject::SideRight};

// Accumulates `frames` frames of interleaved s16 (`sc` channels) into interleaved float `d` (`dc` channels).
inline void accumulate_s16(float* d, uint32_t dc, const int16_t* p, uint32_t sc, uint32_t frames) {
  constexpr float k = 1.0f / 32768.0f;
  if (sc == dc) {
    for (uint32_t i = 0; i < frames * dc; ++i) d[i] += p[i] * k;
  } else if (sc == 1) {  // mono to front L/R
    for (uint32_t i = 0; i < frames; ++i)
      for (uint32_t c = 0; c < (dc < 2 ? dc : 2u); ++c) d[i * dc + c] += p[i] * k;
  } else if (sc == 2) {  // stereo into a multichannel device: FL/FR only
    for (uint32_t i = 0; i < frames; ++i) { d[i * dc] += p[i * 2] * k; d[i * dc + 1] += p[i * 2 + 1] * k; }
  } else if (sc >= 6 && dc == 2) {
    for (uint32_t i = 0; i < frames; ++i) {
      const int16_t* f = p + i * sc;
      d[i * 2] += (f[FL] + kDownmixCenter * f[FC] + kDownmixCenter * f[BL]) * k * kDownmixNorm;
      d[i * 2 + 1] += (f[FR] + kDownmixCenter * f[FC] + kDownmixCenter * f[BR]) * k * kDownmixNorm;
    }
  } else if (sc == 6 && dc == 8) {  // 5.1 on 7.1 speakers: the surrounds go to the SIDE speakers, backs stay empty
    for (uint32_t i = 0; i < frames; ++i) {
      const int16_t* f = p + i * 6;
      float* o = d + i * 8;
      o[0] += f[FL] * k; o[1] += f[FR] * k; o[2] += f[FC] * k; o[3] += f[LFE] * k;
      o[6] += f[BL] * k; o[7] += f[BR] * k;
    }
  } else {
    const uint32_t m = sc < dc ? sc : dc;
    for (uint32_t i = 0; i < frames; ++i)
      for (uint32_t c = 0; c < m; ++c) d[i * dc + c] += p[i * sc + c] * k;
  }
}


// Stereo -> 5.1/7.1 upmixer. The game's Wwise mix is stereo-authored: even with a 5.1 master bus, FC, LFE and the
// surrounds stay (almost) silent because nearly every sound is a 2D stereo source. When a block has no real surround
// content the upmixer derives them from FL/FR:
//   FC  += 0.35355 (L + R)            center: the phantom center
//   LFE += 0.5 * lowpass120Hz(L + R)  bass to the subwoofer
//   BL  += 0.4 * delay15ms(L), BR += 0.4 * delay15ms(R)   ambience in the surrounds (7.1: side speakers)
// FL/FR are left unchanged. The contribution fades in and out (about 100 ms) through a gate that is open while
// FC/LFE/surround carry no energy, so a mix with real surround content (a future 5.1 bank) is never doubled.
class Upmixer {
 public:
  // buf: interleaved float, dc = 6 or 8 channels (FL FR FC LFE BL BR [SL SR]); anything else is left alone.
  void process(float* buf, uint32_t dc, uint32_t frames) {
    if (dc != 6 && dc != 8) return;
    const uint32_t rear0 = dc == 8 ? 6 : 4, rear1 = rear0 + 1;
    double e = 0;
    for (uint32_t i = 0; i < frames; ++i) {
      const float* f = buf + (size_t)i * dc;
      e += (double)f[2] * f[2] + (double)f[3] * f[3] + (double)f[rear0] * f[rear0] + (double)f[rear1] * f[rear1];
    }
    const bool surroundPresent = e / (frames ? frames : 1) > 1e-6;   // about -60 dBFS RMS over four channels
    const float target = surroundPresent ? 0.0f : 1.0f;
    const float k = 1.0f / (0.1f * 48000.0f);                          // 100 ms ramp
    constexpr float lp = 0.0155f;                                      // one pole, about 120 Hz at 48 kHz
    for (uint32_t i = 0; i < frames; ++i) {
      float* f = buf + (size_t)i * dc;
      gate_ += gate_ < target ? std::min(k, target - gate_) : -std::min(k, gate_ - target);
      const float l = f[0], r = f[1];
      const float dl = dly_[0][pos_], dr = dly_[1][pos_];
      dly_[0][pos_] = l; dly_[1][pos_] = r;
      pos_ = (pos_ + 1) % kDelay;
      lpState_ += lp * ((l + r) - lpState_);
      const float g = gate_;
      f[2] += g * 0.35355f * (l + r);
      f[3] += g * 0.5f * lpState_;
      f[rear0] += g * 0.4f * dl;
      f[rear1] += g * 0.4f * dr;
    }
  }
  float gate() const { return gate_; }
 private:
  static constexpr uint32_t kDelay = 720;   // 15 ms at 48 kHz
  float dly_[2][kDelay] = {};
  uint32_t pos_ = 0;
  float lpState_ = 0, gate_ = 0;
};

}  // namespace mix
}  // namespace snap_audio
