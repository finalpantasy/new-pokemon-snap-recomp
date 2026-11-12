# Audio

Code: `runtime/audio/`, test: `tests/audio/`. Proof: `analysis/audio/sink_test.wav` (10 s sweep) and the `.log` files.

## Surface the game uses

**AudioOut only. No `nn::audio` AudioRenderer.** `main` imports exactly 16 `nn::audio` functions
(`analysis/runtime/imports_usage.tsv`), all AudioOut or device queries: `InitializeAudioOutParameter`,
`OpenDefaultAudioOut`, `GetAudioOutChannelCount`, `SetAudioOutBufferInfo`, `GetAudioOutBufferDataPointer`,
`AppendAudioOutBuffer`, `GetReleasedAudioOutBuffer`, `StartAudioOut`, `StopAudioOut`, `GetAudioOutState`,
`CloseAudioOut`, `GetActiveAudioDeviceName`, `GetActiveAudioDeviceChannelCountForOutput`,
`AcquireAudioDeviceNotificationForOutput`, `ListAudioDeviceName`, `SetAudioDeviceOutputVolume`. There is no
`OpenAudioRenderer`, voice, mix, effect or sink call and no `WaitAudioOutBufferEvent`: Wwise mixes everything itself
(statically linked, lifted) and the game only hands finished PCM to the OS. The 1,410 `nn::audio` exports in `sdk`
(renderer, Opus decoder, etc.) are never reached from `main`.

Call-site evidence (module offsets, v262144 `main`). The sink is a Wwise platform sink with vtable `0xc68b20`:

| Offset | Role |
|---|---|
| `0x124640` | ctor: 0x1f8-byte object, `numBuffers = AkPlatformInitSettings+0xac (u16)`, `sampleRate = +0xa8 (u32)`, memset of the 0x168-byte `AudioOut`, `nn::mem::StandardAllocator` over 128 KiB |
| `0x1231e0` | default platform settings: `0x0004'0000'BB80` stored at +0xa8: **sample rate 48000, 4 buffers** (`0xa9d94` forces 4 if the field is 0) |
| `0x124780` | Open: `InitializeAudioOutParameter`, sets `sampleRate`, `channelCount` (2, or 6 when the speaker config asks for surround), `OpenDefaultAudioOut`, `GetAudioOutChannelCount`, then per buffer `StandardAllocator::Allocate(size, 0x1000)` and `SetAudioOutBufferInfo(buf, mem, cap, dataSize)`, then 4 silent prefill submits through vtable slot 15 (`0x124c30`) |
| `0x124b10` | submit: converts Wwise's interleaved float mix to s16 (`fcvtzs #15` + saturating narrow) into the next free `AudioOutBuffer` and calls `AppendAudioOutBuffer`; ring index modulo `numBuffers` |
| `0x124ce0` | poll: drains `GetReleasedAudioOutBuffer` until null (non-blocking), reports `numBuffers - pending` free slots |
| `0x124d60` / `0x124a10` | Start / (state check, Stop, Close, free buffers) |
| `0x123480`, `0x1232e0` | device notification: `TryWaitSystemEvent` on the acquired event, re-query channel count, reopen the sink if it changed |
| `0x67ffa0` | settings path: `ListAudioDeviceName` + `SetAudioDeviceOutputVolume` (per-device volume) |

Format: **48000 Hz, signed 16-bit PCM, interleaved, 2 channels (6 if the device query says surround)**.
Frames per buffer: `uNumSamplesPerFrame` global at `0xc66388` = **0x400 = 1024 frames** (21.3 ms), so a buffer is
4096 bytes (stereo) or 12288 bytes (5.1, 0x3000, 4 KiB aligned capacity). Queue depth 4 buffers = 85.3 ms.
`AudioOutBuffer` is 0x28 bytes (`next, buffer, capacity, dataSize, offset`), `AudioOut` storage is 0x168 bytes.
Device names compared in `0x1235xx`: `AudioStereoJackOutput`, `AudioBuiltInSpeakerOutput`, `AudioTvOutput`,
`AudioUsbOutput` (the `strcmp` table at GOT `0xcc8340`; the exact mapping to indices is unverified).

## Boundary decision: override at the `nn::audio` API

Override the 16 imports by mangled name; do not lift sdk's audio client. Reasons: (1) the surface is small and
fully visible at call sites; (2) lifting sdk's AudioOut would run the guest side of the `audout:u` IPC and need a
fake audio service, which is the pure-HLE route that `runtime-boundary.md` rejects; (3) there is no renderer, so the
"hard" rating (voices, mempools, effects, mixing) does not apply. Ryujinx's AudioRenderer is therefore not needed;
only its AudioOut semantics were used as a reference (buffers queue FIFO, are released when consumed, release is
polled).

## Design

```
Wwise sink thread (guest)            device thread (host)
  Append ----> SPSC submit ring ---->  core_mix(): pop, s16->float, remap, master gain, clip
  GetReleased <- SPSC released ring <-  buffer released once fully consumed
                                        -> WASAPI event buffer / timer
                                        -> optional tap ring (tests)
```

- **Zero copy until the device:** guest memory is host memory, so the submit ring carries `AudioOutBuffer*` and the
  device thread reads PCM in place. A buffer belongs to the host from Append until it comes out of GetReleased, as on
  hardware.
- **Rings:** both are wait-free SPSC rings (`spsc_ring.h`, 64 entries). The device thread takes no lock. A buffer
  is released after the device pulled its last frame (device latency is the only difference from hardware).
- **Close safety:** Close clears `alive`, then spins until the device thread leaves the slot (`inUse` handshake,
  Dekker-style, seq_cst) so the guest can free the buffers right after.
- **Up to 4 AudioOuts** are mixed additively; the game uses one. Rate must match the device rate (48000).
- **Channel mapping:** same count: copy. Mono: to L/R. Stereo into a 5.1 device: FL/FR. 5.1 into stereo:
  `L = (FL + 0.707 FC + 0.707 BL) / 1.707` (LFE dropped).
- **Underruns:** a pull where a started AudioOut had less than the requested frames counts as an underrun (counter,
  missing frames, stderr log). The device is fed silence for the gap.
- **Stats/latency:** `snap_audio_get_stats`: frames mixed, appended, released, underruns, WASAPI buffer, latency.

### Backend: WASAPI shared mode (SDL3 is not vendored)

`runtime/hle` does not exist yet and no SDL3 is vendored anywhere in the repo, so WASAPI is used directly rather than
adding a dependency (SDL3 can replace `audio_wasapi.cpp` behind the same `Backend` interface if the HLE layer
vendors it). Event-driven, float32 stream at the guest rate and channel count with `AUTOCONVERTPCM |
SRC_DEFAULT_QUALITY` (Windows converts to the endpoint). It tries `IAudioClient3::InitializeSharedAudioStream` at the
engine's minimum period first, then falls back to `IAudioClient::Initialize` (default period). Measured here
(`sink_test`): **1056-frame engine buffer (22.0 ms), reported stream latency 22.0 ms**, plus up to 85 ms of guest
queue (4 x 1024 frames), so worst-case game-to-speaker delay is about 107 ms; the typical figure is about 22 ms plus
about 43 ms. The device thread uses MMCSS "Pro Audio".

**Device changes:** `IMMNotificationClient` watches the default render endpoint. On a default-device change, or on
`DEVICE_INVALIDATED`, unplug or a failed `GetBuffer`, the stream is rebuilt on the new default endpoint, and the guest
notification event is signaled (`core_notify_device_change` -> `snap_audio_set_os_hooks`), which makes the game
re-query the channel count. While there is no endpoint, a 10 ms timer keeps consuming at stream rate so the game
never stalls, and the stream is retried every 500 ms. This path was written but not exercised: the test machine had
one endpoint and the test did not switch devices (see Risks).

A `null` backend (`SNAP_AUDIO_BACKEND=null` or `cfg.backend`) consumes on a real-time timer and is what the unit test
uses for exact comparison.

### Volume

Findings (names and IDs only; the soundbank data stays local). `Init.bnk` (Wwise bank version 134, 40 buses) has this
bus tree; IDs are the FNV-1 32-bit hash of the lower-case name and the three category names were confirmed by hash:

| Bus | ID | Parent |
|---|---|---|
| `Master Audio Bus` | 3803692087 | none |
| (unnamed mixer under master) | 508372457 | 3803692087 |
| `bgm` | 412724365 | 508372457 (6 child buses) |
| `se` | 1584861537 | 508372457 (about 25 child buses) |
| `voice` | 3170124113 | 508372457 (4 child buses) |
| (second root, unnamed) | 985987111 | none |

`Init.bnk` has no game parameters bound to bus volume, so there is no RTPC to set. The game does it with events: its
own options menu has BGM/SE/voice levels 0..10, and `BANK_MAIN.bnk` holds 33 events `Set_volume_bgm_NN`,
`Set_volume_se_NN`, `Set_volume_voice_NN` (NN = 00..10, e.g. `Set_volume_bgm_00` = 129505574, `_10` = 146283161,
`Set_volume_se_00` = 1152448242, `Set_volume_voice_00` = 2616401070) that set the matching bus volume.
We do not call `AK::SoundEngine` (stripped, no symbol) and do not add our own events; we drive the game's setter.

How the calls are made:

- Game setter: `main+0x67c610(AudioManager* this, int cat, uint8 level)`, `cat` 0 = bgm, 1 = se, 2 = voice. It builds
  `Set_volume_<cat>_<2-digit level>` and posts it by name (`this->[8]` vtable +0x78). Its callers are the game's startup
  apply (`0x444498`) and the options-menu setter (`0x737090`).
- Guest objects: audio manager pointer at `main+0x12fef28`; the game's saved levels are bytes `+0x6f` (bgm), `+0x70` (se),
  `+0x71` (voice) of the settings object whose address sits in GOT slot `main+0xcfab68`; manager `+0x128` = 2 forces
  voice to 0.
- `host/abi_audio.cpp` `apply_category_volumes()` runs from the `GetReleasedAudioOutBuffer` bridge, so it executes on
  the Wwise sink thread (a guest thread with its own `Cpu`) and uses `loader::call_guest_nested`. Level posted =
  `round(gameLevel * userVolume)`, so 1.0 is exactly the original mix. It re-posts when the user volume changes, when the
  game writes its own level (that post would override ours), and once a second while a volume is below 1.0. A category never
  moved off 1.0 is never touched.
- Resolution is the game's 11 steps (0.1 each); a user volume of 0 is silence for that bus. Master is applied in the host
  mixer (continuous, ramped per block) times the per-device volume.

Debug aids: `SNAP_AUDIO_LEVELS=1` prints `[audio-level] t=Ns rms= peak=` of the final mix each second;
`SNAP_AUDIO_DUMP=<file>` writes the final mix as raw s16 (device rate and channels); `SNAP_AUDIO_TRACE=1` logs the
game levels and each category post.

### Volume API for UI

```c
#include "audio/audio_out.h"
enum { SNAP_VOL_MASTER = 0, SNAP_VOL_BGM = 1, SNAP_VOL_SE = 2, SNAP_VOL_VOICE = 3 };
void  snap_audio_set_volume(int category, float v);  // v clamped to 0..1; applies live and persists
float snap_audio_get_volume(int category);           // current value, 1.0 default
```

- Callable from any thread, at any time (before or after boot). Setting is lock-free for the audio path; persisting
  writes a small file, so call it when a slider is released (or at most a few times per second), not per frame.
- Persistence: `audio.json` (`{"master":..,"bgm":..,"se":..,"voice":..}`) in `%SNAP_CONFIG_DIR%`, else
  `%APPDATA%\SnapRecomp`. Loaded when the audio layer starts; defaults to 1.0 everywhere (original mix). It is a
  separate file, not `hle.json`, so the UI needs no config plumbing: just call `snap_audio_set_volume`.
- Env overrides, not persisted (tests/scripts): `SNAP_VOL_MASTER`, `SNAP_VOL_BGM`, `SNAP_VOL_SE`, `SNAP_VOL_VOICE`.
- BGM/SE/VOICE take effect within about a second at most, in 0.1 steps, and only once the game's audio manager exists.
  The game's own BGM/SE/voice options still work; the user volume scales them.

## Surround and spatial output

**The switch.** Wwise's Switch sink asks `nn::audio::GetActiveAudioDeviceChannelCountForOutput` (callers `0x1232e0`,
`0x123480`, `0x123510`) and opens `AudioOut` with 2 or 6 channels (`0x124780`). The host answers with the "game
channels" value below, so the game itself switches Wwise to 5.1 (no patching). `SNAP_AUDIO_TRACE=1` logs the query and the
Open (`[audio-trace] ...`). The game never reads `nn::settings` or `nn::oe` for audio.

**Guest layout:** 48 kHz s16 interleaved, 6 channels in the Switch and Wwise order FL FR FC LFE BL BR (the downmix and
mapping are in `runtime/audio/audio_mix.h`, tested by `tests/audio/mix_test.cpp`; whether the lab and course scenes
really put content in the rear and center channels is in the evidence section below).

**Output modes** (env `SNAP_AUDIO_OUTPUT`, or `audio.json` key `output`; restart required, the game opens its sink once):

| Mode | Device stream | Notes |
|---|---|---|
| `stereo` (default) | 2 ch float | 5.1 is downmixed `L = (FL + .707 FC + .707 BL) / 1.707`, LFE dropped. Game channels default to 2. |
| `speakers` | WASAPI 5.1 (mask 0x3F) or 7.1 (0x63F) | `speakers` key / `SNAP_AUDIO_SPEAKERS` = 6, 8, or 0 = 8 if the device mix format has 8 or more channels, else 6. On 7.1 the surrounds feed the SIDE speakers, the back pair stays empty. Windows converts if the device has fewer. |
| `spatial` | `ISpatialAudioClient`, 6 static bed objects | FL FR FC LFE and the surrounds on SideLeft/SideRight, rendered by Windows Sonic, Dolby Atmos for Headphones or DTS. Needs the spatial format enabled on the default device. If not available it logs why and falls back to 5.1 speakers, then stereo. |

Game channels: env `SNAP_AUDIO_CHANNELS` (2 or 6) > `audio.json` `channels` (0 = auto) > auto (6 for speakers and spatial, 2 for
stereo). Debug and tests: `SNAP_AUDIO_CAPTURE=<dir>` writes `guest_<n>ch.wav` (what Wwise produced) and
`device_<mode>_<n>ch.wav` (final mix) starting at the first non-silent frame, `SNAP_AUDIO_CAPTURE_SECS` (default 30), and
logs `[audio-ch] guest|device ... rms: FL=.. FR=..` every 5 s.

`audio.json` keys: `"output": "stereo"|"speakers"|"spatial"`, `"channels": 0|2|6`, `"speakers": 0|6|8`, next to the
volume keys (the volume writer preserves them).

### Output API for the UI

```c
void snap_audio_set_output(int mode /*0 stereo,1 speakers,2 spatial; -1 keep*/, int gameChannels /*0,2,6; -1 keep*/,
                           int speakerChannels /*0,6,8; -1 keep*/);   // persists to audio.json; applies on next start
int  snap_audio_get_output_mode(void);      // configured (env wins)
int  snap_audio_active_output_mode(void);   // running, after fallbacks (also SnapAudioStats.outputModeActive)
int  snap_audio_spatial_available(void);    // 1 if the default device can render the bed (probe, starts nothing)
```

The UI should say "restart required" next to the control, and may grey out Spatial when
`snap_audio_spatial_available()` is 0 (tell the user to enable Windows Sonic or Dolby Atmos for Headphones in Sound settings).

### Resampler

There is none: the stream is opened at 48 kHz float with `AUTOCONVERTPCM | SRC_DEFAULT_QUALITY`, so Windows resamples
when the device runs at 44.1 or 96 kHz (spatial streams are 48 kHz and Windows handles the device side). Not replaced:
it is a quality choice only and nothing measured is wrong.

### Getting real 5.1 out of Wwise (2026-11-12)

Three things stack, all needed:

1. **Settings word:** the game builds its `AkOutputSettings` in `main+0x67ffa0` with channel config `0x3102` (stereo). `host/abi_audio.cpp`
   rewrites it to `0x60f106` (5.1) at the `ListAudioDeviceName` call (`main+0x680270`) and again at the sink Open
   (`main+0x1247d8`), when the host wants 6 channels. The sink then opens 6 channels.
2. **Bank:** `Init.bnk` fixes the mixer bus under the Master Audio Bus (id 508372457, parent of bgm/se/voice) at stereo
   (channel config field `0x3102`; the other 39 buses are 0 = inherit). Without a change Wwise mixes stereo no matter what the
   sink is. `python -I scriptsudio_make_surround_pack.py` writes the local mod pack `surround-bus` (config 0 = same as
   parent) to `%APPDATA%\SnapRecomp\mods`; with it the sink buffers carry a real 5.1 mix. Stereo output is unaffected (the
   bus follows the 2-channel device). Never commit the pack (it is game data).
3. **Upmixer:** the game's content is stereo-authored, so even on a 5.1 bus FC, LFE and the surrounds stay near 0 (RMS
   0.0000 to 0.0001 for 130 s of the lab). The host upmixer (`Upmixer` in `audio_mix.h`) fills them from FL/FR when a block
   has no real surround content: center 0.354 (L+R), LFE 0.5 x lowpass 120 Hz (L+R), surrounds (7.1: sides) 0.4 x the fronts
   delayed 15 ms. FL/FR are unchanged. `SNAP_AUDIO_UPMIX=0` turns it off.

Lab capture (`florio_photos`, 130 s, speakers, per-channel RMS FL FR FC LFE BL BR): game mix
0.021 0.021 0.0000 0.0000 0.0001 0.0001; device mix after the upmixer 0.021 0.021 0.014 0.009 0.008 0.008.

## Listening checklist (owner; the output could not be heard while building this)

1. `stereo` mode, play a course: sound is unchanged from before, no new hiss or clipping in busy scenes (the 5.1 downmix only runs if you force 6 channels).
2. `speakers` on 5.1 speakers: each speaker works; in a course, turning the camera or a Pokemon flying past moves effects between front and rear; voices and menu sounds sit in the center speaker; the subwoofer carries bass hits only.
3. `speakers` on 7.1: the rear (back) pair is silent by design, the sides carry the surrounds.
4. `spatial` with Windows Sonic or Dolby Atmos for Headphones on headphones: the sound is outside the head and wider than stereo; the log says `spatial audio stream: 6 static bed objects`. If it says "unavailable" it fell back to 5.1 speakers.
5. Volume sliders (master, BGM, SE, voice) still work in every mode, and unplugging or switching the default device recovers without a freeze.

## Threading

- Guest side: the Wwise sink thread (created via `nn::os::CreateThread`, name not in the 5 known `AK::` threads, so
  it is probably the sound-engine audio thread) is the only caller of Append/GetReleased. Both are single-thread-safe
  and wait-free. Open/Close/queries take one init mutex.
- Host side: one device thread (WASAPI or timer). It never blocks on the guest and the guest never blocks on it,
  except Close waiting for `inUse` (one device period at worst, a few ms).
- `nn::os` interplay: only the notification event needs `nn::os` (hook, `PROPOSED_SHARED_CHANGES.md` item 2). The
  `runtime\os` docs did not exist when this was written, so that is not yet coordinated.

## Test (`tests/audio/sink_test.cpp`, `tests/audio/build.bat`)

Drives the API in the sink's order (query device, Open with rate 48000, `numBuffers = 4` x 1024 frames, 4 silent
prefill buffers, Start, then poll/submit loop with the same float->s16 conversion, Close) with a 10 s log sweep
(100 Hz to 6 kHz plus 440 Hz), then reopens as 5.1 and checks the downmix. Checks: no underruns while feeding,
every buffer accounted for, the captured final mix equals the submitted PCM sample for sample (null: max error 0;
WASAPI at volume 0.05: 1e-9), downmix level, slot reuse. Result at the time of writing: null and WASAPI both pass
(483,328 frames in, 0 underruns while feeding, 472 buffers appended and released). Capture: `analysis/audio/sink_test.wav`.

Note: a normal-priority sink thread inside a BelowNormal process underran heavily when the machine was at 77% CPU;
the test raises its own thread to HIGHEST (the process stays BelowNormal). The real sink thread needs the same care.

## Risks

1. **Struct layouts are inferred** (`AudioOutParameter`, `AudioOutBuffer`, `AudioDeviceName`) from call-site offsets and
   the public SDK shape; sizes match (0x28 stride) but the field order of `AudioOutBuffer` past `buffer` is by
   convention. Only `buffer`, `dataSize` and `dataOffset` are read by the host, and `SetAudioOutBufferInfo` writes
   them, so a different order would still work as long as the game never reads them directly (it uses the getter).
2. **Device switching is untested** (no second endpoint here). The rebuild path is straightforward but unproven.
3. **Wwise sink thread priority/cadence**: if lifted `nn::os` thread priorities map low, 85 ms of queue is thin.
   Raising `numBuffers` is a one-line settings change (`AkPlatformInitSettings+0xac`) if needed.
4. **Category volumes** use the game's 11-step events (no finer than 0.1) and are posted from the sink thread.
5. **Device-name semantics** (which of the 4 names selects which game behavior) unverified; the host reports
   `AudioTvOutput` and 2 channels by default (`cfg.channelMode` can report 5.1 or follow the device mix format).
6. **Release timing:** buffers are released when consumed by the device thread, not when heard (up to one WASAPI
   period early), which only makes the game's queue accounting slightly optimistic.
7. **Mixed-rate AudioOuts** are rejected (only 48000 is used by the game).

### Volume verification (hidden run, `-SaveFixture post_tutorial`, 100 s, `SNAP_AUDIO_LEVELS=<file>`)

Mean final-mix RMS over seconds 20..90 (71 one-second samples): normal run 0.0139, `SNAP_VOL_BGM=0` run 0.0072
(about 48% lower; peak 0.756 -> 0.595). The trace shows the setter posting level 0 for category 0 (BGM) with the game's
own level at 10; SE and voice stayed untouched. Only one run of each, and the two runs are not frame-aligned, so the
ratio is indicative, not exact. Not yet checked: SE and voice at 0, and live changes from the UI.
