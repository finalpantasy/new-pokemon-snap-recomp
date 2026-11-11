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

- `snap_audio_set_volume(SNAP_VOL_MASTER, v)` is applied (ramped per block to avoid clicks) and multiplied by the
  per-device volume set through `SetAudioDeviceOutputVolume`.
- BGM, SE and VOICE volumes are stored (`SNAP_VOL_BGM/SE/VOICE`) but **not applied**: the mix is already summed
  inside Wwise before it reaches `nn::audio`. The game's strings show the category events/parameters
  (`Set_volume_bgm_`, `Set_volume_se_`, `Set_volume_voice_`, line 11342, 10779, 13648 of
  `work/v262144-main.strings.txt`), but the actual Wwise bus names live in the soundbank (`Init.bnk`), which this task
  did not parse. Wiring categories means calling `AK::SoundEngine::SetRTPCValue` or `SetBusVolume` from the settings
  menu through the lifted symbols; the API above is ready to carry the values.

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
4. **No category volumes** until a Wwise-level hook exists.
5. **Device-name semantics** (which of the 4 names selects which game behavior) unverified; the host reports
   `AudioTvOutput` and 2 channels by default (`cfg.channelMode` can report 5.1 or follow the device mix format).
6. **Release timing:** buffers are released when consumed by the device thread, not when heard (up to one WASAPI
   period early), which only makes the game's queue accounting slightly optimistic.
7. **Mixed-rate AudioOuts** are rejected (only 48000 is used by the game).
