# Proposed shared changes (audio)

None of these edit files outside `runtime\audio` and `tests\audio`; the owner of each area applies them.

1. **Override registration (lifter/runtime owner).** `snap_audio_overrides(&n)` returns 16 `{mangled name, host fn}`
   pairs for the `nn::audio` imports. The host functions use the plain x86-64 C ABI with host pointers. `nn::Result`
   is returned as `uint32_t` in the low 32 bits (0 = success); the AArch64 wrapper must pass x0..x3 as the first four
   integer args and `s0` (SetAudioDeviceOutputVolume, second arg after the pointer) as the float arg. Add the table to
   the same GOT-by-name mechanism as the NVN table.
2. **`nn::os::SystemEvent` signal (runtime\os).** `AcquireAudioDeviceNotificationForOutput(SystemEvent*)` stores the
   guest pointer; on a default-device change the device thread calls the hook installed with
   `snap_audio_set_os_hooks(fn)`. runtime\os should install `fn = signal SystemEvent`. The hook runs on the audio device
   thread, so the signal function must be thread-safe and non-blocking. The game (0x123480) polls the event with
   `TryWaitSystemEvent`; it never blocks on it.
3. **Build.** Add `runtime/audio/*.cpp` to the runtime target; link `ole32 avrt` (Windows). No third-party code.
4. **Thread priority.** The device thread self-elevates through MMCSS ("Pro Audio"). The guest sink thread (Wwise) is
   created through `nn::os::CreateThread`; map the Wwise priority to something at or above normal. The test shows
   BelowNormal sink threads underrun on a loaded machine.
5. **Wwise-level volume (later).** Category volumes need `AK::SoundEngine` calls (RTPC/bus) from the settings menu;
   those functions are lifted guest code, so the menu must call them through the lifted symbol. See `docs\audio.md`.
