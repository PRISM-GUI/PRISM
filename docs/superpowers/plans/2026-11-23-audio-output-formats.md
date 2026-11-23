# Audio output: sample formats, devices and state — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `prism::audio::Output` plays `int16_t` or `float`, on a chosen device, with a state and a volume; prism-esp32 provides the Korvo's speaker and Bluetooth headset as devices; prism-mp3-player drops its own output interface.

**Architecture:** `Output` (PRISM, src/audio/) owns conversion, volume, state, load and the timer fallback, over a small `DeviceBackend` interface each platform registers (SDL on the desktop, the board in prism-esp32, `ManualDevice` in tests). Board devices open their hardware once and outlive Outputs; an Output attaches its render function through a `RenderGate`.

**Tech Stack:** C++26, Meson (PRISM, player host), ESP-IDF 6.x CMake (board), SDL3 audio, doctest, ThreadSanitizer.

**Spec:** /home/jeandet/Documents/prog/PRSIM/docs/superpowers/specs/2026-11-23-audio-output-formats-design.md

## Global Constraints

- Repos: PRISM `~/Documents/prog/PRSIM` (tasks 1–4), prism-esp32 `~/Documents/prog/prism-esp32` (5–6), prism-mp3-player `~/Documents/prog/prism-mp3-player` (7–8). Work on main; commit with explicit pathspecs; never push.
- Builds and tests only on the Coder workspace `prism` (`. ~/.config/lpp-coder/env`), one build or test per build directory, foreground. Commands: `.superpowers/sdd/2026-11-22-korvo-player/env.md` in prism-mp3-player; PRISM: rsync to `~/PRSIM`, `ninja -C builddir && meson test -C builddir`; TSan dir `builddir-tsan` where it exists.
- No allocation, lock or blocking call on a rendering thread, for either sample type.
- `Output(Format, RenderFn)` with a float render function keeps compiling and behaving as today; `RenderFn` stays `std::function<void(std::span<float>)>`.
- `Output` is a non-template class; non-template code in `src/`; core headers SDL-free; never a namespace named `detail`.
- PRISM must still build with `-Dsdl=disabled` (bare metal, ESP-IDF): everything in tasks 1, 2, 4 builds there.
- prism-esp32: never stage ` D subprojects/prism` or `?? host/subprojects/.wraplock`.
- Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Style: PRISM AGENTS.md; `simplify:` comment on each deliberate cut (the spec names three: no state callback, devices never close, no headset "connecting" state).

## Review Focus

1. A device asks for more frames than the conversion buffer holds (an SDL callback of 4096 frames) → Output renders in chunks, never overruns. Test in Task 2.
2. The headset disconnects mid-track → state `lost`, render stops at once, the player pauses with its message. Test in Task 7 (`ManualDevice::lose()`).
3. Volume set before any Output exists, then a track change → the new Output plays at that volume. Test in Task 7.
4. An `int16_t` render on a float device and back with gain > 1 → saturates, never wraps. Test in Task 1.
5. A speaker track at 44.1 kHz followed by one at 48 kHz → the open device changes rate, no restart of the writer. Board check in Task 5.

## Ruling (plan vs spec)

Ruling: `RenderGate` lives in PRISM (`include/prism/audio/render_gate.hpp`), not prism-esp32 — it is platform-free and PRISM's TSan build tests it; prism-esp32's host project is C++20 and has no TSan dir — cost if wrong: one header moves later.

---

### Task 1: Sample types and conversion (PRISM)

**Files:**
- Modify: `include/prism/audio/output.hpp` (types only)
- Create: `src/audio/convert.cpp`; add `'audio/convert.cpp'` to `prism_lib_sources` in `src/meson.build`
- Test: `tests/test_audio_convert.cpp`, registered in `tests/meson.build`'s `headless_tests`

**Interfaces:**
- Produces: `concept Sample` (`int16_t` or `float`); `enum class SampleType { s16, f32 }`; `template <Sample T> using SampleRenderFn = std::function<void(std::span<T>)>`; `using RenderFn = SampleRenderFn<float>`; `void convert(std::span<const float>, std::span<std::int16_t>, float gain = 1.f)`; `void convert(std::span<const std::int16_t>, std::span<float>, float gain = 1.f)`; `void apply_gain(std::span<float>, float)`, `void apply_gain(std::span<std::int16_t>, float)` (saturating).

- [ ] **Step 1: Write the failing tests**
  - "float to s16 rounds to nearest": `{0.f, 0.5f, -0.5f, 1.f/32767}` → `{0, 16384, -16384, 1}` (scale 32767 for positive and negative full scale: `1.f → 32767`, `-1.f → -32767`).
  - "float to s16 saturates": `{1.5f, -2.f}` → `{32767, -32768}`.
  - "s16 to float": `{0, 32767, -32768}` → `{0.f, 1.f (within 1e-6), -1.0000305f (within 1e-6)}` (divide by 32767).
  - "gain applies before saturation": `convert({0.6f}, out, 2.f)` → `32767`; `apply_gain(s16{20000}, 2.f)` → `32767`; `apply_gain(s16{-20000}, 2.f)` → `-32768`.
  - "output sizes": convert writes `min(in.size(), out.size())` samples.
- [ ] **Step 2: Run; expect compile failure (no `convert`).** `meson test -C builddir audio_convert`
- [ ] **Step 3: Implement** the types in output.hpp and the four functions in src/audio/convert.cpp (`std::lround`, clamp to [-32768, 32767]).
- [ ] **Step 4: Run; expect PASS.** Then the full PRISM suite: all pass.
- [ ] **Step 5: Commit** `feat(audio): sample types and conversion`.

### Task 2: Output over a device backend, ManualDevice (PRISM)

**Files:**
- Create: `include/prism/audio/backend.hpp`, `include/prism/audio/manual_device.hpp`, `src/audio/output.cpp`, `src/audio/registry.cpp`, `src/audio/manual_device.cpp` (all in `prism_lib_sources`)
- Modify: `include/prism/audio/output.hpp` (Output API), `src/backends/sdl_audio.cpp` (its `Output::Impl`, LoadMeter and timer move to `src/audio/output.cpp`; until Task 3 SDL registers nothing, so desktop Outputs run on the timer — Task 3 restores the device)
- Test: `tests/test_audio_device.cpp` (headless)

**Interfaces:**
- Consumes: Task 1's types and `convert`/`apply_gain`.
- Produces:
  ```cpp
  struct DeviceInfo { std::string id, name; std::vector<int> rates; int max_channels = 2;
                      SampleType native = SampleType::f32; bool is_default = false;
                      bool accepts(const Format&) const; };   // rates empty = any rate
  enum class State { playing, timer, lost };
  using PullFn = std::function<void(std::span<std::byte> native_block, int frames)>;
  class DeviceStream { public: virtual ~DeviceStream() = default;   // must not return while pull runs
                       virtual bool lost() const = 0;
                       virtual bool set_volume(float) { return false; } };  // true: the device applies it
  struct DeviceBackend { virtual ~DeviceBackend() = default;
                         virtual std::vector<DeviceInfo> devices() = 0;
                         virtual std::unique_ptr<DeviceStream> open(std::string_view id, const Format&,
                                                                    PullFn, std::string& error) = 0; };
  void register_backend(DeviceBackend&);  void unregister_backend(DeviceBackend&);
  std::vector<DeviceInfo> devices();
  class Output { Output(Format, RenderFn); Output(Format, SampleRenderFn<std::int16_t>);
                 Output(std::string_view id, Format, RenderFn); Output(std::string_view id, Format, SampleRenderFn<std::int16_t>);
                 State state() const; bool playing() const; const std::string& error() const;
                 Format format() const; Load load() const; void set_volume(float); };
  class ManualDevice {   // registers itself as a backend with one device, id "manual"
  public: explicit ManualDevice(SampleType native = SampleType::s16, std::vector<int> rates = {});
          ~ManualDevice();
          std::span<const std::int16_t> pull_s16(int frames);   // runs render; valid until the next pull
          std::span<const float> pull_f32(int frames);
          void lose();                    // the open stream reports lost; pulls return silence
          bool open() const;              // an Output is attached
          float device_volume() const; }; // what set_volume reached (ManualDevice applies volume itself)
  ```
  The native block is `frames × channels` samples of the device's `native` type; Output reinterprets it (`std::int16_t` or `float`) and fills it from the app's render function, converting through a scratch buffer of `kMaxBlockFrames × channels` when types differ, in chunks of at most `kMaxBlockFrames`.
- Rules (from the spec): default-device constructors fall back to the timer (`State::timer`) when no backend lists a default device or its open fails; a named device that is missing, refuses the format or fails to open → `State::lost` and render never runs. `lost` is sticky. Volume: `DeviceStream::set_volume` first; if it returns false, Output applies the gain. ManualDevice's stream returns true and records the value.

- [ ] **Step 1: Write the failing tests** (`tests/test_audio_device.cpp`):
  - "devices lists a ManualDevice while it lives": with one alive, `devices()` holds id "manual"; after it dies, it does not.
  - "an s16 render on an s16 device passes straight through": render writes `{1, 2, 3, 4}`; `pull_s16(2)` returns `{1, 2, 3, 4}`.
  - "an f32 render on an s16 device is converted": render writes `0.5f`; pull returns `16384`s.
  - "a pull larger than kMaxBlockFrames renders in chunks": float render on an s16 device; `pull_s16(4096)` returns 8192 samples, all converted, render called ≥ 2 times with blocks ≤ `kMaxBlockFrames` frames.
  - "lose stops render and the state says lost": after `lose()`, `state() == State::lost`, further pulls do not call render.
  - "a named device that does not exist is lost": `Output("nope", …).state() == State::lost`, render never called.
  - "a named device that refuses the rate is lost": ManualDevice rates `{44100}`, Output at 48000 → lost.
  - "the default constructor uses the timer when nothing is listed": no ManualDevice → `state() == State::timer`; the existing timer test semantics hold (render called).
  - "volume reaches the device": `set_volume(0.25f)` → `device_volume() == 0.25f`.
  - "render is never called after the Output is destroyed": pull after destruction calls nothing (counter unchanged).
  - "nothing allocates on the rendering thread between blocks, s16 and f32": reuse `check_no_allocation_between_blocks`'s counting approach from tests/test_audio_output.cpp with ManualDevice pulls.
- [ ] **Step 2: Run; expect compile failure.**
- [ ] **Step 3: Implement** registry (a mutex-guarded vector of backends; `devices()` concatenates), `Output` (variant of the two render functions, the scratch buffer allocated at construction, chunking, gain, state, timer path and `LoadMeter` moved verbatim from sdl_audio.cpp), `ManualDevice`. Reduce sdl_audio.cpp to nothing but what Task 3 rebuilds (it may register no backend yet).
- [ ] **Step 4: Run the new tests, then the full PRISM suite.** Expected: all pass except possibly `audio_output_device` (the SDL device path) — it must pass again after Task 3; note it in the commit message if red.
- [ ] **Step 5: Build `-Dsdl=disabled`** (a cross-less host config is enough: `meson setup builddir-nosdl -Dsdl=disabled && ninja -C builddir-nosdl`). Expected: builds.
- [ ] **Step 6: Commit** `feat(audio): Output over device backends; ManualDevice`.

### Task 3: The SDL backend as a DeviceBackend (PRISM)

**Files:**
- Modify: `src/backends/sdl_audio.cpp`
- Test: `tests/test_audio_output.cpp` (unchanged cases must pass; add the new ones below)

**Interfaces:**
- Consumes: Task 2's `DeviceBackend`, `register_backend`.
- Produces: an SDL backend registered by a static object in sdl_audio.cpp (the software-backend library is shared, so its initializer runs at load). Device ids `"sdl:<SDL_AudioDeviceID>"`; the default device listed with `is_default = true` and id `"sdl:default"` (opens `SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK`); `native = f32`; `rates` empty. `lost()` true once the stream's device is gone (`SDL_EVENT_AUDIO_DEVICE_REMOVED` is window-loop bound; check `SDL_AudioStreamDevicePaused`/a failed put instead and pick what the dummy driver lets a test exercise).

- [ ] **Step 1: Write the failing tests** (device run, `SDL_AUDIO_DRIVER=dummy`): "device: devices lists SDL's default device" (`is_default`, id `sdl:default`); "device: an s16 render plays on SDL's device" (`state() == playing`, render called).
- [ ] **Step 2: Run `meson test -C builddir audio_output_device audio_output_timer`; expect the new cases to fail.**
- [ ] **Step 3: Implement** the backend: open = `SDL_OpenAudioDeviceStream` with `SDL_AUDIO_F32`, its callback calls `PullFn` with the float block.
- [ ] **Step 4: Run; all audio tests pass, then the full suite and the synth rack builds unchanged.**
- [ ] **Step 5: Commit** `feat(audio): SDL's devices through the backend seam`.

### Task 4: RenderGate (PRISM)

**Files:**
- Create: `include/prism/audio/render_gate.hpp` (header-only)
- Test: `tests/test_audio_render_gate.cpp` (headless; also run in the TSan build dir if one exists)

**Interfaces:**
- Produces: `class RenderGate { void attach(PullFn*) ; void detach();  // after it returns, no call is inside
  bool call(std::span<std::byte> block, int frames);  // false: nothing attached (caller writes silence)
  bool attached() const; };` Every atomic seq_cst (Dekker pair: store pointer / load inside vs store inside / load pointer); a comment says why acquire/release is not enough.
- Port of prism-mp3-player's `core/ring_tap.hpp` + `tests/test_ring_tap.cpp` (read both), generalized from a ring to a `PullFn`.

- [ ] **Step 1: Port the four tests** (silence with nothing attached → `call` false; attached → the function runs; detach while a caller loops, 200 rounds, the function's owner destroyed right after detach, with the "wait for the first call" start; `attached()`).
- [ ] **Step 2: Run; expect failure (stub).**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run 30 times in a loop, then under TSan.** Expected: 30/30, TSan clean.
- [ ] **Step 5: Commit** `feat(audio): RenderGate, the attach/detach handshake for long-lived devices`.

### Task 5: The Korvo speaker device (prism-esp32)

**Files:**
- Create: `boards/korvo1/speaker_device.hpp/.cpp`; add to `boards/korvo1/CMakeLists.txt` SRCS
- Create: `main/tone.cpp` (a smoke app: lists devices, plays a 440 Hz tone 2 s on each, at 44.1 then 48 kHz, logs `state()`), plus `sdkconfig.tone` like the other apps
- Port from: prism-mp3-player `boards/korvo1/main/speaker_output.cpp` (writer thread, codec, `spawn_with` thread config from `thread_config.hpp` — move that header to `boards/korvo1/` too)
- Test: board only

**Interfaces:**
- Consumes: Tasks 2 and 4.
- Produces: `void prism::esp32::register_speaker();` — one `DeviceBackend` with device id `"speaker"`, name `"Speaker"`, `native = s16`, rates `{44100, 48000}`, hardware volume (`esp_codec_dev_set_out_vol`). Codec and writer open on the first `open()` and stay open (simplify comment); a later `open()` at another rate reconfigures the codec only when the rate differs; between Outputs the writer writes silence. The writer calls `RenderGate::call`, silence when it returns false.

- [ ] **Step 1: Implement**, moving code from the player's speaker_output.cpp.
- [ ] **Step 2: Build the tone app on Coder** (`scripts/build.sh tone`). Expected: builds, prints the bin size.
- [ ] **Step 3: Board check (the user):** tone plays at 44.1 then 48 kHz with no click at the switch beyond the rate change itself; log shows `playing` both times.
- [ ] **Step 4: Commit** `feat(korvo1): the speaker as a PRISM audio device; tone smoke app`.

### Task 6: Bluetooth link and headset device (prism-esp32)

**Files:**
- Create: `components/prism_bt/` (CMakeLists.txt, `bluetooth.hpp` public API, and the moved `bt_link.*`, `bt_callbacks.cpp`, `bt_bringup.cpp`, `bt_shared.hpp`, `headset_device.cpp`); Kconfig for the headset name (port `CONFIG_PLAYER_BT_NAME` as `CONFIG_PRISM_BT_HEADSET_NAME`)
- Modify: `main/tone.cpp` (plays on the headset too when built with Bluetooth), `sdkconfig.tone` (Bluetooth on; copy the player's BT sdkconfig lines)
- Port from: prism-mp3-player `boards/korvo1/main/bt_*` and `bt_output.cpp`, moves only
- Test: board only

**Interfaces:**
- Consumes: Tasks 2 and 4.
- Produces: in namespace `prism::esp32::bluetooth`: `void start();` (brings the stack up, reconnects the bonded headset or scans for the configured name; registers the headset backend), `void on_remote(std::function<void(Remote)>)` with `enum class Remote { play_pause, next, previous }` (map from the player's `Button` use), `std::string state_text()`, `std::uint32_t state_revision()`. Headset device: id `"headset"`, name = the headset's name once known, `native = s16`, rates `{44100}`, gain volume (AVRCP absolute volume = the noted upgrade, keep the existing `simplify:` comment), listed only after `start()`. `open()` while not connected → null with error `"headset not connected"` (→ Output `lost`); disconnect → the stream's `lost()` true. The A2DP data callback calls `RenderGate::call`, silence otherwise. The link logic itself is moved unchanged.

- [ ] **Step 1: Move and adapt.**
- [ ] **Step 2: Build the tone app with Bluetooth.** Expected: builds.
- [ ] **Step 3: Board check (the user):** headset connects, tone plays on it; switching the headset off makes `state()` `lost`; on again, a new Output plays; a headset button press is logged.
- [ ] **Step 4: Commit** `feat(bt): the A2DP headset as a PRISM audio device; the link moved from the player`.

### Task 7: The player on audio::Output (prism-mp3-player, host)

**Files:**
- Modify: `core/player.hpp/.cpp`, `core/audio.hpp` (delete `PcmOutput`; keep `PcmFormat`, `Seconds`, sample helpers), `desktop/desktop_app.*`, `tests/test_player.cpp`, `tests/test_player_threads.cpp`, `tests/test_ui.cpp`, `tests/test_desktop_smoke.cpp`
- Delete: `desktop/sdl_pcm_output.*`, `core/ring_tap.hpp`, `tests/test_ring_tap.cpp`, `tests/fake_output.hpp` (and their build entries)
- Bump: `subprojects/prism` to the PRISM commit after Task 4 (the player's host build uses PRISM there — check `subprojects/prism.wrap`/symlink in env.md)

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces: `Player(Storage&, std::string device_id = {})` (empty: the default device). Per track start: `audio::Output(device_id, {rate, 2}, SampleRenderFn<int16_t>)` (or the default-device constructor when the id is empty), render = pop `ring_` into the block; destroying it = stop. `accepts` = the device's `DeviceInfo::accepts` (default device: accept any); "connected" = no Output or `state() != lost`; volume kept by the Player and passed to each new Output; `underruns` = the ring's. `poll()` pauses with today's message when its Output is `lost`.

- [ ] **Step 1: Port the tests to ManualDevice** (each FakeOutput use becomes a `ManualDevice` + `pull_s16`; disconnect = `lose()`; assertions keep their values). Add: "volume set before the first track reaches the first Output" (`set_volume(0.3f)`, play → `device_volume() == 0.3f`); "a headset lost mid-track pauses with the disconnected message" (`lose()`, poll → Paused, message as today).
- [ ] **Step 2: Run; expect compile failures (Player still takes PcmOutput&).**
- [ ] **Step 3: Implement**; delete the old files.
- [ ] **Step 4: Full suite + TSan (player, player_threads, ui).** Expected: all pass; counts as before minus test_ring_tap's 4 plus the 2 new.
- [ ] **Step 5: Run the desktop app's smoke test.** Expected: pass.
- [ ] **Step 6: Commit** `refactor(player): play through prism::audio::Output; PcmOutput and its outputs removed`.

### Task 8: The player's board build on prism-esp32's devices (prism-mp3-player)

**Files:**
- Modify: `boards/korvo1/main/player_main.cpp` (boot setting → `register_speaker()` or `bluetooth::start()`; Player gets `"speaker"` or `"headset"`; the Settings Bluetooth text from `bluetooth::state_text/state_revision`; remote buttons from `bluetooth::on_remote`), `boards/korvo1/main/CMakeLists.txt`, `boards/korvo1/sdkconfig*` if the Kconfig name moved
- Delete: `boards/korvo1/main/{speaker_output,bt_output,bt_link,bt_callbacks,bt_bringup,bt_shared,thread_config}.*` (thread_config moved in Task 5; the player's other threads include it from prism-esp32 now)
- Bump: `subprojects/prism-esp32` to the commit after Task 6

- [ ] **Step 1: Implement.**
- [ ] **Step 2: Build the firmware on Coder** (`scripts/build-korvo.sh`); fetch `build/player.bin` to `build-local/`. Expected: builds.
- [ ] **Step 3: Host suite once more** (nothing host-side should change). Expected: all pass.
- [ ] **Step 4: Board check (the user):** speaker plays tracks at 44.1 and 48 kHz, volume works; headset connects, plays, pauses, its buttons work, power-off pauses with the message, power-on reconnects and plays.
- [ ] **Step 5: Commit** `refactor(korvo1): the speaker and headset come from prism-esp32`.
