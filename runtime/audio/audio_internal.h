// SPDX-License-Identifier: MIT
// Internal interfaces between the AudioOut core (audio_out.cpp) and the device backends.
#pragma once
#include <atomic>
#include <cstdint>
#include "audio_out.h"

namespace snap_audio {

// Device thread entry point into the core: fills `frames` interleaved float frames (device channel layout) with the
// mix of every started AudioOut, applies master volume, feeds the tap, counts underruns. Never blocks.
void core_mix(float* dst, uint32_t frames);
// Device or timer changed: called by the backend thread; signals the guest notification event.
void core_notify_device_change();
// Stats written by backends.
void core_set_device_info(bool attached, uint32_t bufferFrames, double latencyMs, uint32_t channels, uint32_t rate, int mode);

// Parameters the backend opens its stream with.
// mode = SnapAudioOutputMode. channels = core mix channels: 2 stereo, 6 or 8 speakers, 6 for the spatial bed
// (FL FR FC LFE BL BR). A backend that has to fall back reports the new channel count and mode through
// core_set_device_info.
struct DeviceFormat { uint32_t rate; uint32_t channels; uint32_t bufferMs; int mode; };

class Backend {
 public:
  virtual ~Backend() {}
  virtual bool start(const DeviceFormat& fmt) = 0;  // spawns the device thread
  virtual void stop() = 0;                          // joins it
  virtual int kind() const = 0;                     // SnapAudioBackend
  // Channel count of the current default device mix format (2 when unknown).
  virtual uint32_t native_channels() { return 2; }
  // 1 if the default device can render the spatial bed (probe only, nothing is started).
  virtual int spatial_available() { return 0; }
};

Backend* make_null_backend();
#if defined(_WIN32)
Backend* make_wasapi_backend();
#endif

}  // namespace snap_audio
