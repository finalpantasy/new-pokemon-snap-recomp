# Proposed shared changes (runtime/hle)

`runtime/hle` did not edit any shared header. Each item below is what the HLE layer needs from the loader, `runtime/os`
or `runtime/nvn`, with the smallest change that works.

## 1. Bind the overrides in the loader (needed)

`hle::bind_all(BindFn bind, void* user)` calls `bind(symbol, fn, user)` for every override (about 220 symbols) and for
the generated "not implemented" stubs. Wire it where the loader applies overrides (TotK name: `hostfn_add_override`):

```cpp
hle::init_trace_from_env();
hle::load_config("");                       // before the first fs call: fs::instance() reads the config once
hle::bind_all([](const char* sym, GuestFn fn, void* u) { hostfn_add_override(*(Module*)u, sym, fn); }, &module);
```

Both `main` and `sdk` get the same binder: 725 of the 731 imports resolve into `sdk`, so an override must replace the
symbol in every module that defines or imports it. The `sdk`-internal overrides (the 14 rows in `hle_coverage.tsv` with
`boot = n/a`, for example `nn::socket::Write`, `nn::socket::Read`, the `nn::err` constructor) only match inside `sdk`.

## 2. Two os hooks (needed for vsync and account async)

`hle::os_hooks()` has two function pointers that `runtime/os` should fill once its event type exists:

```cpp
hle::os_hooks().init_system_event   = [](void* ev, bool manual_clear) { /* nn::os::InitializeSystemEvent-like, in place */ };
hle::os_hooks().signal_system_event = [](void* ev) { /* nn::os::SignalSystemEvent-like, callable from any host thread */ };
```

Users: `nn::vi::GetDisplayVsyncEvent` starts a 60 Hz thread that signals the event, and
`nn::account::AsyncContext::GetSystemEvent` signals an already-done event. Without the hooks both log one line and return
success; the game then waits on an event that never fires. Call `hle::vi_stop_vsync()` at shutdown.

## 3. nn::vi native window token for NVN (needed by runtime/nvn)

`nn::vi::OpenDefaultDisplay`, `CreateLayer` and `GetNativeWindow` return opaque tokens. `hle::vi_native_window()` is the
`nn::vi::NativeWindowHandle` the game then hands to the NVN window builder. `runtime/nvn` should accept exactly that
pointer (or ignore the argument) and create the host swapchain itself. HLE does not touch NVN.

## 4. Mouse position from the NVN window (optional)

`input::RawInput::mouse_x/mouse_y` are 0..1 over the game's client area. The Win32 backend uses the foreground window of
the process. When `runtime/nvn` owns an SDL/GLFW window it can call a future `input::set_window_provider()`; until then the
SDL3 backend reads the focused SDL window and the Win32 backend needs nothing.

## 5. Symbols registered here that another layer might also register

- `nv::SetGraphicsAllocator` and `nv::InitializeGraphics` are no-ops here (service `nv`). `runtime/nvn` should not
  register them again; if it needs them, delete the two `HLE_NOOP("nv", ...)` lines in `reg_sys.inc` and rerun `gen_hle_stubs.py`.
- `nn::diag::detail::AbortImpl` and `nndetailDiagAbortFormatImpl` log the expression, function, file and line and then
  call `hle::fatal()`. If `runtime/os` wants a guest crash dump it can set `hle::fatal_hook`.
- No `nn::os` or `nn::audio` symbol is overridden here. Those 102 imports are `todo` with owner `runtime/os` and
  `runtime/audio` in `hle_coverage.tsv`.

## 6. Link the generated backstop (optional)

`generated/hle_unimplemented.cpp` (from `gen_hle_stubs.py`) registers a logging abort stub for each `nn::` import this layer
owns but has no override for. It is empty today (every owned import has an override). Run `gen_hle_stubs.py --all` to also
stub the os, audio and nvn imports while those layers are unfinished; `bind_all` never lets a stub replace a real override
from this layer, but the loader must bind other layers' overrides after `bind_all`.

## 7. Build flags

- C++20, add `runtime/` to the include path. Windows needs `-luser32` (message box, `GetAsyncKeyState`).
- Default input backend: Win32 (XInput loaded at run time, `GetAsyncKeyState`, cursor). For SDL3 gamepads (PS4/PS5,
  Switch Pro, generic) add `-DHLE_WITH_SDL3`, `-Itools/dl_sdl3/x/SDL3-3.4.20/include` and link SDL3. Both compile clean.
- Define `_CRT_SECURE_NO_WARNINGS` with clang-cl/clang on MSVC headers or the `getenv`/`sscanf` deprecation warnings appear.

## 8. Config must be loaded before the first fs call

`hle::fs::instance()` snapshots `rom_layers`, `romfs_root`, `save_root` and `save_backups` on first use. Call
`hle::load_config()` first, or call `set_rom_layers()` / `set_save_root()` afterwards.
