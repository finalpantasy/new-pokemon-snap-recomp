// SPDX-License-Identifier: MIT
// Host implementation of the nn::audio AudioOut surface used by the game's Wwise sink, plus host-side controls.
// See docs/audio.md. Guest pointers are host pointers (guest == host address space), so the guest-visible
// structs below are read and written in place.
#pragma once
#include <cstdint>

#if defined(_WIN32)
#define SNAP_AUDIO_API extern "C" __declspec(dllexport)
#else
#define SNAP_AUDIO_API extern "C" __attribute__((visibility("default")))
#endif

// ---- Guest-visible layouts (derived from the call sites in main, see docs/audio.md) -----------------------------
// nn::audio::AudioOutParameter: written by InitializeAudioOutParameter, then sampleRate (+0) and channelCount (+4)
// are set by the caller (0x124780).
struct SnapAudioOutParameter {
  uint32_t sampleRate;    // 0 = device default (48000)
  uint16_t channelCount;  // 0 = device default, 2 or 6 used by the game
  uint16_t reserved0;
  uint32_t reserved1[2];
};
// nn::audio::AudioOutBuffer, 0x28 bytes (the game strides by 0x28).
struct SnapAudioOutBuffer {
  SnapAudioOutBuffer* next;  // +0x00 host-owned link, the guest must not touch it
  void* buffer;              // +0x08 PCM16 interleaved
  uint64_t bufferSize;       // +0x10 capacity (4 KiB aligned)
  uint64_t dataSize;         // +0x18 bytes of valid PCM
  uint64_t dataOffset;       // +0x20
};
static_assert(sizeof(SnapAudioOutBuffer) == 0x28, "AudioOutBuffer must be 0x28 bytes");
// nn::audio::AudioDeviceName: 128-byte NUL-terminated string.
struct SnapAudioDeviceName { char name[128]; };
// nn::audio::AudioOut is an opaque guest object (the game reserves 0x168 bytes). The host keeps a magic at +0
// (the game tests the first dword for "open") and a slot index at +4.
enum { SNAP_AUDIO_STATE_STARTED = 0, SNAP_AUDIO_STATE_STOPPED = 1 };

// ---- nn::audio entry points (nn::Result is 0 on success) ---------------------------------------------------------
SNAP_AUDIO_API void snap_nn_audio_InitializeAudioOutParameter(SnapAudioOutParameter* p);
SNAP_AUDIO_API uint32_t snap_nn_audio_OpenDefaultAudioOut(void* audioOut, const SnapAudioOutParameter* p);
SNAP_AUDIO_API void snap_nn_audio_CloseAudioOut(void* audioOut);
SNAP_AUDIO_API uint32_t snap_nn_audio_StartAudioOut(void* audioOut);
SNAP_AUDIO_API void snap_nn_audio_StopAudioOut(void* audioOut);
SNAP_AUDIO_API int snap_nn_audio_GetAudioOutState(const void* audioOut);
SNAP_AUDIO_API uint32_t snap_nn_audio_GetAudioOutChannelCount(const void* audioOut);
SNAP_AUDIO_API void snap_nn_audio_SetAudioOutBufferInfo(SnapAudioOutBuffer* b, void* data, uint64_t bufferSize, uint64_t dataSize);
SNAP_AUDIO_API void* snap_nn_audio_GetAudioOutBufferDataPointer(const SnapAudioOutBuffer* b);
SNAP_AUDIO_API void snap_nn_audio_AppendAudioOutBuffer(void* audioOut, SnapAudioOutBuffer* b);
SNAP_AUDIO_API SnapAudioOutBuffer* snap_nn_audio_GetReleasedAudioOutBuffer(void* audioOut);
SNAP_AUDIO_API int snap_nn_audio_ListAudioDeviceName(SnapAudioDeviceName* names, int count);
SNAP_AUDIO_API void snap_nn_audio_SetAudioDeviceOutputVolume(const SnapAudioDeviceName* name, float volume);
SNAP_AUDIO_API void snap_nn_audio_GetActiveAudioDeviceName(SnapAudioDeviceName* name);
SNAP_AUDIO_API int snap_nn_audio_GetActiveAudioDeviceChannelCountForOutput(void);
// `systemEvent` is an nn::os::SystemEvent* owned by the guest. It is signaled (through the OS hook below) when the
// default output device changes.
SNAP_AUDIO_API uint32_t snap_nn_audio_AcquireAudioDeviceNotificationForOutput(void* systemEvent);

// Name-keyed override table (Itanium mangled nn:: symbol -> host function), same style as the NVN table.
struct SnapAudioOverride { const char* mangled; void* fn; };
SNAP_AUDIO_API const SnapAudioOverride* snap_audio_overrides(int* count);

// ---- Host controls -------------------------------------------------------------------------------------------------
enum SnapAudioBackend { SNAP_AUDIO_BACKEND_AUTO = 0, SNAP_AUDIO_BACKEND_WASAPI = 1, SNAP_AUDIO_BACKEND_NULL = 2 };
enum SnapAudioChannelMode { SNAP_AUDIO_CH_STEREO = 0, SNAP_AUDIO_CH_AUTO = 1, SNAP_AUDIO_CH_SURROUND51 = 2 };
struct SnapAudioConfig {
  int backend;             // SnapAudioBackend; env SNAP_AUDIO_BACKEND=wasapi|null overrides AUTO
  int channelMode;         // what GetActiveAudioDeviceChannelCountForOutput reports (and the device stream uses)
  uint32_t sampleRate;     // device stream rate, 48000
  uint32_t deviceBufferMs; // requested WASAPI buffer (0 = engine default period)
  int enableTap;           // capture the final mix into a ring (tests)
};
// Volume categories. Only MASTER is applied at this layer; the others are stored for the settings menu and
// take effect once a Wwise-level hook (AK::SoundEngine bus/RTPC) is wired (docs/audio.md, "Volume").
enum SnapAudioVolume { SNAP_VOL_MASTER = 0, SNAP_VOL_BGM = 1, SNAP_VOL_SE = 2, SNAP_VOL_VOICE = 3, SNAP_VOL_COUNT = 4 };

SNAP_AUDIO_API void snap_audio_default_config(SnapAudioConfig* cfg);
SNAP_AUDIO_API void snap_audio_configure(const SnapAudioConfig* cfg);  // before the first Open
SNAP_AUDIO_API void snap_audio_shutdown(void);
SNAP_AUDIO_API void snap_audio_set_volume(int category, float v);
SNAP_AUDIO_API float snap_audio_get_volume(int category);
// Hook run on the device thread when the default device changes: lets runtime\os signal the SystemEvent.
typedef void (*SnapAudioSignalEventFn)(void* systemEvent);
SNAP_AUDIO_API void snap_audio_set_os_hooks(SnapAudioSignalEventFn signal);

struct SnapAudioStats {
  uint64_t framesMixed;        // frames the device pulled
  uint64_t framesFromGuest;    // frames that came from guest buffers
  uint64_t underruns;          // device pulls that found a started AudioOut with no data
  uint64_t underrunFrames;
  uint64_t buffersAppended, buffersReleased;
  uint64_t deviceChanges;
  uint32_t deviceBufferFrames; // WASAPI engine buffer (0 for the null backend)
  double deviceLatencyMs;      // stream latency reported by the device
  uint32_t deviceChannels, deviceRate;
  int backendInUse;            // SnapAudioBackend actually running
  int deviceAttached;          // 0 while falling back to the timer
};
SNAP_AUDIO_API void snap_audio_get_stats(SnapAudioStats* s);
SNAP_AUDIO_API void snap_audio_reset_stats(void);
// Reads up to n interleaved float samples of the final mix (needs enableTap). Returns samples read.
SNAP_AUDIO_API uint64_t snap_audio_tap_read(float* dst, uint64_t n);
SNAP_AUDIO_API uint32_t snap_audio_tap_channels(void);
