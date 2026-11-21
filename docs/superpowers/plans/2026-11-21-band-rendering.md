# Band Rendering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The ESP32-S31-Korvo-1 draws the audio scope at the panel's refresh rate: frames drawn in 48-row RGB565 bands in internal SRAM, threads pinned to cores, the plot's point mapping in single precision.

**Architecture:** PRISM's `raster::Canvas` becomes a template on a pixel type (`Argb8888`, `Rgb565`) and can cover a band of rows of a larger frame. The frame runtime gains a band loop behind a `BandSink<P>` interface and a thread-start hook. prism-esp32 replaces its ARGB frame + PPA conversion with a `BandDisplay` that copies each SRAM band into the panel's RGB565 buffer.

**Tech Stack:** C++26, Meson, doctest (PRISM); ESP-IDF master (ad20ded), PPA, esp_lcd RGB panel (prism-esp32).

**Spec:** `doc/design/band-rendering.md` (PRISM 512a98f); background in `doc/design/porting-and-devices.md` ("Drawing: three levels").

## Global Constraints

- Builds and tests run on the Coder workspace `prism`, one build or test invocation per build directory at a time, never backgrounded. PRISM: `ninja -C builddir` and `meson test -C builddir` (GCC 16, reflection); also `builddir-clang` (clang + libc++) and `builddir-arm` (PATH needs `~/opt/arm-gcc-threads/bin:~/opt/arm-gcc/bin`) for code under `src/raster` and `src/platform`. prism-esp32: `. ~/esp/esp-idf/export.sh && scripts/build.sh <app>` in `~/prism-esp32` (its `subprojects/prism` is a symlink to `~/PRSIM`, so PRISM edits rsynced there are picked up).
- No ESP-IDF or board header in PRISM; no `detail` namespace named bare `detail` under a PRISM module.
- Band height: **48 rows**. Panel: **800×480**, RGB565. Two band buffers in internal SRAM.
- RGB565 packing: `r5 = r >> 3`, `g6 = g >> 2`, `b5 = b >> 3`, value `(r5 << 11) | (g6 << 5) | b5` (the same as prism-esp32's `boards/korvo1/rgb565.hpp`, which the panel shows correctly today).
- Existing behaviour unchanged for `FrameSink` users (STM32, QEMU firmwares, tests): ARGB8888, whole frame.
- Every board step is verified with a trace from the board (the trace page, `https://jeandet.github.io/PRISM/trace.html`) and the timing line; the person flashing is the user. Batch board hand-offs at the end of Task 7 unless a task says otherwise.
- Commit with explicit pathspecs; work on `main` in both repositories; never push without the user's consent.

## Review Focus

1. A widget that draws outside its own geometry rect (focus ring, shadow, a popup's card): with draw lists skipped by bounds, a band would lose it. Expected: banded output identical to whole-frame output. Pinned by Task 5's equivalence test on the widget demo and the scope, plus a hand-built snapshot with a command 10 px outside its entry's rect.
2. A primitive spanning a band boundary (a text glyph, a thick line, a rounded rect edge cut by row 48): expected pixel-identical to the whole frame. Pinned by Task 4's band-split test with a boundary at an odd row inside the shapes.
3. Plot data far from zero (UNIX timestamps ~1.7e9 with a 1 s visible range): the single-precision mapping must not jitter. Pinned by Task 1's timestamp test (error ≤ 0.01 px).
4. A translucent colour blended in RGB565 (anti-aliased text edge at alpha 128): expected within 1 unit per channel of the ARGB8888 result after the same packing. Pinned by Task 3's blend test.
5. A band target whose copy is slower than drawing a band: the runtime must not draw into a buffer whose copy is in flight. Pinned by Task 6's slow-sink test.

---

### Task 1: The plot's point mapping in single precision

**Files:**
- Modify: `include/prism/widgets/plot_render.hpp` (`PlotMapping::to_pixel`, lines ~35-44)
- Test: `tests/test_plot.cpp`

**Interfaces:**
- Produces: `Point PlotMapping::to_pixel(double data_x, double data_y) const` — same signature and same results within 0.01 px for ranges whose span is ≥ 1e-6 of their magnitude.

- [ ] **Step 1: Write the guard test** — the rewrite must keep today's results; the speed is checked on the board (Task 7):

```cpp
TEST_CASE("to_pixel: timestamps ~1.7e9 over a 1 s range land within 0.01 px of the exact position")
{
    PlotMapping m{{1.7e9, 1.7e9 + 1.0}, {-1.0, 1.0}, Rect{Point{X{10}, Y{20}}, Size{Width{800}, Height{200}}}};
    for (double f : {0.0, 0.25, 0.5, 0.999}) {
        const Point p = m.to_pixel(1.7e9 + f, 0.5);
        CHECK(std::abs(p.x.raw() - float(10.0 + f * 800.0)) <= 0.01f);
        CHECK(std::abs(p.y.raw() - float(20.0 + 0.25 * 200.0)) <= 0.01f);
    }
}
```

- [ ] **Step 2: Run it** — `meson test -C builddir plot`. Expected: PASS today (the current formula is exact). Then make it fail on purpose once (drop the `double` subtraction, compute `float(data_x) * sx`) to see the test catch the precision loss at 1.7e9, and restore.

- [ ] **Step 3: Rewrite `to_pixel`** — one `double` subtraction per coordinate, the rest in `float`: precompute nothing in the struct (it stays an aggregate compared with `==`); compute `const float sx = float(plot_area.extent.w.raw() / (x_range.max - x_range.min))` and `sy` likewise inside the function, then `px = origin.x + float(data_x - x_range.min) * sx`, `py = origin.y + extent.h - float(data_y - y_range.min) * sy`. NaN in → NaN out, as today (the decimation skips NaN).

- [ ] **Step 4: Run** `meson test -C builddir plot pixels` — Expected: PASS. Then the full suite: `meson test -C builddir` — all pass.

- [ ] **Step 5: Commit**

```bash
git add include/prism/widgets/plot_render.hpp tests/test_plot.cpp
git commit -m "perf(plot): map points with one double subtraction, the rest in float"
```

### Task 2: A thread-start hook, and the ESP32 pins its threads

**Files:**
- Modify: `include/prism/platform/frame_app.hpp`, `src/platform/frame_runtime.cpp`
- Test: `tests/test_frame_app.cpp`
- Modify (prism-esp32): `main/widgets.cpp`, `main/scope.cpp`, `boards/korvo1/app_support.{hpp,cpp}`

**Interfaces:**
- Produces (PRISM):
  ```cpp
  enum class ThreadRole { logic, render };
  struct FrameOptions {
      // Called on the caller's thread right before each runtime thread is created: a port
      // sets its thread-creation attributes there (ESP-IDF: esp_pthread_set_cfg's core).
      std::function<void(ThreadRole)> before_thread_start;
  };
  void run_frames(FrameSink&, TouchSource&, TreeFactory, std::function<void(FrameContext&)> setup,
                  const FrameOptions& options);   // the existing 4-argument overload forwards with {}
  template <class Model> void frame_app(FrameSink&, TouchSource&, Model&,
                                        std::function<void(FrameContext&)> setup, const FrameOptions&);
  ```
- Produces (prism-esp32): `korvo1::FrameOptions korvo1::pinned_threads()` — logic on core 0, render on core 1 (the SIMD core per Espressif's forum, unconfirmed: the task checks it).

- [ ] **Step 1: Failing test** in `tests/test_frame_app.cpp`:

```cpp
TEST_CASE("before_thread_start runs on the caller, for logic then render, before each starts")
{
    Counter model;
    MemorySink sink;
    ScriptedTouch touch;
    std::vector<prism::platform::ThreadRole> roles;
    const auto caller = std::this_thread::get_id();
    bool on_caller = true;
    prism::platform::FrameOptions options{.before_thread_start = [&](prism::platform::ThreadRole r) {
        roles.push_back(r);
        on_caller = on_caller && std::this_thread::get_id() == caller;
    }};
    prism::platform::frame_app(sink, touch, model, [&](FrameContext& ctx) {
        ctx.on_loop([&] { if (sink.presents >= 1) ctx.quit(); });
    }, options);
    CHECK(on_caller);
    CHECK(roles == std::vector{prism::platform::ThreadRole::logic, prism::platform::ThreadRole::render});
}
```

- [ ] **Step 2: Run** `meson test -C builddir frame_app` — Expected: compile error, `FrameOptions` not declared.
- [ ] **Step 3: Implement** in `run_frames`: call `options.before_thread_start(role)` (if set) immediately before each `start_thread`. `RunArgs` does not need it.
- [ ] **Step 4: Run** the suite, `builddir-clang` (frame_app target) and `builddir-arm` — all pass/clean.
- [ ] **Step 5: Commit PRISM** — `feat(platform): FrameOptions::before_thread_start`. Same commit: `doc/design/band-rendering.md` section 3 says a hook, not a core number (ruling: PRISM cannot pin portably; ESP-IDF pins through its own pthread config, Linux through affinity).
- [ ] **Step 6: Board** — `pinned_threads()` returns options whose hook calls `esp_pthread_set_cfg` with `esp_pthread_get_default_config()` and `pin_to_core` 0 (logic) or 1 (render). The runtime's own stack sizes still apply (pthread attributes override the config's). `frame_app` never returns on this board, so the config is not restored. Use it in `widgets.cpp` and `scope.cpp`. Add to the self-test (`main/selftest.cpp`) one check: a `std::thread` created with the render pin runs on core 1 (`esp_cpu_get_core_id()`), and executes one Espressif vector instruction without trapping (inline asm `esp.zero.q q0`); print `SIMD on core 1 PASS`, or FAIL with the hint "the SIMD core is core 0 on this chip: swap the pins in pinned_threads()".
- [ ] **Step 7: Build** `scripts/build.sh all` — three images build.
- [ ] **Step 8: Commit prism-esp32** — `perf(board): render on the SIMD core, logic on the other`.

### Task 3: Pixel formats as types; the canvas on a pixel type

**Files:**
- Create: `include/prism/raster/pixel.hpp`
- Modify: `include/prism/raster/canvas.hpp`, `src/raster/canvas.cpp`, `include/prism/raster/draw_snapshot.hpp`, `src/raster/draw_snapshot.cpp`, `src/platform/frame_runtime.cpp`, `benchmarks/scope_frame.cpp`, `tests/embedded/prism_qemu/main.cpp`
- Test: `tests/test_raster_canvas.cpp`, `tests/test_raster_pixel.cpp` (new, add to `headless_tests`)

**Interfaces:**
- Produces:
  ```cpp
  namespace prism::raster {
  struct Argb8888 { using storage = std::uint32_t;
      static constexpr storage pack(Color c);                 // == argb(c)
      static constexpr storage over(storage dst, Color src); }; // today's src-over
  struct Rgb565  { using storage = std::uint16_t;
      static constexpr storage pack(Color c);                 // the Global Constraints packing
      static constexpr storage over(storage dst, Color src); }; // unpack (replicate high bits), blend as Argb8888 does, repack
  template <class P> class BasicCanvas;   // today's Canvas, pixels as std::span<typename P::storage>
  using Canvas = BasicCanvas<Argb8888>;   // every existing user keeps compiling
  using Canvas565 = BasicCanvas<Rgb565>;
  template <class P> void draw(BasicCanvas<P>&, const render::SceneSnapshot&, const Fonts&);
  }
  ```
  Definitions stay in the `.cpp` files with explicit instantiations for both formats (`template class BasicCanvas<Argb8888>;` …), so headers do not grow.

- [ ] **Step 1: Failing tests** in `tests/test_raster_pixel.cpp`:
  - `Rgb565::pack(Color::rgba(255, 0, 0)) == 0xF800`, `(0,255,0) == 0x07E0`, `(0,0,255) == 0x001F`, `(255,255,255) == 0xFFFF`.
  - Blend (Review Focus 4): for `dst = Rgb565::pack(white)`, `src = rgba(0, 0, 0, 128)`: unpack `Rgb565::over(dst, src)` and the packing of `Argb8888::over(argb(white), src)`; each channel differs by at most 1 (in 5/6-bit units).
  - `Argb8888::over` equals today's canvas result for the "50% alpha over white" case already in `test_raster_canvas.cpp`.
- [ ] **Step 2: Run** `meson test -C builddir raster_pixel` — Expected: compile error (no `pixel.hpp`).
- [ ] **Step 3: Implement** `pixel.hpp`, then turn `Canvas` into `BasicCanvas<P>`: every pixel write goes through `P::pack`/`P::over`; `clear` packs once. `fonts.cpp` is untouched (the canvas calls into fonts for coverage, not the reverse — check, and template the call site if not).
- [ ] **Step 4: Run** the full suite (all existing raster tests unchanged and passing), `builddir-clang`, `builddir-arm`, and `benchmarks/bench_scope_frame` in `builddir-release` (draw time within 10% of 1.12 ms).
- [ ] **Step 5: Commit** — `refactor(raster): the canvas on a pixel type; RGB565`.

### Task 4: A canvas over a band of a larger frame

**Files:**
- Modify: `include/prism/raster/canvas.hpp`, `src/raster/canvas.cpp`
- Test: `tests/test_raster_canvas.cpp`

**Interfaces:**
- Produces: `BasicCanvas(std::span<storage> pixels, int width, int height, int stride, int first_row)` — the buffer holds rows `[first_row, first_row + height)` of a frame `width` wide; every operation takes frame coordinates; the clip starts as those rows. `void retarget(std::span<storage> pixels, int first_row)` overload. The 4-argument constructor means `first_row = 0` (unchanged).

- [ ] **Step 1: Failing test** (Review Focus 2) — `"a frame drawn in bands equals the frame drawn whole"`: a 120×100 ARGB frame drawn whole with: a text string crossing row 47-49, a thick line (4 px) from (5,10) to (110,90), a rounded rect (10,40)-(110,60) radius 8, a filled polygon, a circle at (60,49) r 12; then the same calls on bands of 48 rows (rows 0-47, 48-95, 96-99), each band its own buffer, copied into a second frame. Both frames equal, every pixel. Repeat for `Canvas565`.
- [ ] **Step 2: Run** `meson test -C builddir raster_canvas` — Expected: compile error (5-argument constructor).
- [ ] **Step 3: Implement** — one private `row(y)` returning the storage row for frame row `y` (`pixels_[(y - first_row_) * stride_]`); every pixel access uses it; `clip_of` and the default clip use `[first_row, first_row + height)`. Spans and polygons iterate only the clipped rows, so a band does not walk the whole shape.
- [ ] **Step 4: Run** the suite, clang, arm — all pass.
- [ ] **Step 5: Commit** — `feat(raster): a canvas over a band of rows`.

### Task 5: Drawing a snapshot band by band

**Files:**
- Modify: `include/prism/raster/draw_snapshot.hpp`, `src/raster/draw_snapshot.cpp`
- Test: `tests/test_raster_snapshot.cpp`

**Interfaces:**
- Produces: `template <class P> void draw_band(BasicCanvas<P>& band, const render::SceneSnapshot& snap, const Fonts& fonts);` — draws the snapshot's draw lists that can touch the band's rows (skipping those whose `snap.geometry[i]` rect, grown by `kBandMargin = 16` px on each side, misses them) and always the overlay. `draw` (whole frame) is `draw_band` with a canvas over all rows.

- [ ] **Step 1: Failing tests** (Review Focus 1):
  - `"the widget demo drawn in 48-row bands equals the whole frame"`: `prism_examples::WidgetsModel` at 800×480 (`build_snapshot` with `raster::Fonts` as measurer), drawn whole and in 10 bands, ARGB and RGB565; frames identical.
  - Same for the audio scope (`ScopeModel` + `ScopeState` over a source that hands out 533 samples once, as `benchmarks/scope_frame.cpp`'s `ToneSource`).
  - A hand-built snapshot: one entry with geometry (10,10,20,20) whose draw list fills (10,25)-(30,45) — 5 px outside its rect: banded equals whole (margin covers it). Document in the header that a widget may draw at most `kBandMargin` px outside its rect.
- [ ] **Step 2: Run** `meson test -C builddir raster_snapshot` — Expected: compile error (`draw_band`).
- [ ] **Step 3: Implement** `draw_band`; `draw` calls it. Keep the `"draw list"` profile mark per drawn list.
- [ ] **Step 4: Run** the suite, clang, arm; `bench_scope_frame` gains a "banded" line (10 bands of 48) — print it; expect under 2× the whole-frame time.
- [ ] **Step 5: Commit** — `feat(raster): draw a snapshot band by band`.

### Task 6: The runtime's band loop

**Files:**
- Modify: `include/prism/platform/frame_target.hpp`, `include/prism/platform/frame_app.hpp`, `src/platform/frame_runtime.cpp`
- Test: `tests/test_frame_app.cpp`

**Interfaces:**
- Produces:
  ```cpp
  template <class P> struct BandSink {
      virtual int band_height() const = 0;                         // rows per band
      virtual std::span<typename P::storage> band(int index) = 0;  // index 0 or 1; width * band_height
      // Starts copying `band(index)` (rows [first_row, first_row + rows)) to the frame and
      // returns. band(index) is not handed out again until that copy is done: the sink waits
      // in band() if it must.
      virtual void flush(int index, int first_row, int rows) = 0;
      virtual void present() = 0;                                  // every band flushed: show it
      virtual core::Size size() const = 0;
      virtual ~BandSink() = default;
  };
  void run_frames(BandSink<raster::Rgb565>&, TouchSource&, TreeFactory, std::function<void(FrameContext&)>, const FrameOptions& = {});
  void run_frames(BandSink<raster::Argb8888>&, …same…);
  template <class P, class Model> void frame_app(BandSink<P>&, TouchSource&, Model&, std::function<void(FrameContext&)> = {}, const FrameOptions& = {});
  ```
  The render loop per frame: for each band `k` (index `k % 2`): `band(index)`, a canvas over it at `first_row = k * band_height`, clear to the window background, `draw_band`, `flush`. Then `present()`. The timing line's "draw" is all bands; "present" is `present()`. The last band may be shorter (480 = 10 × 48 exactly here; test 100 rows).

- [ ] **Step 1: Failing tests** in `tests/test_frame_app.cpp`:
  - `MemoryBandSink<P>`: two band buffers, `flush` copies the band into a full frame vector. `"a band sink's frame equals a frame sink's frame"`: the `Counter` model at 120×100, band height 48, run until one present, compare with `MemorySink` pixels (ARGB). Same model with `Rgb565`: compare against packing the ARGB frame with `Rgb565::pack` of each unpacked pixel (exact for opaque pixels; the counter has none translucent — check `Counter` draws opaque only, else allow 1 unit).
  - Review Focus 5, `"the runtime never draws into a band whose copy is in flight"`: a sink whose `flush` marks the buffer busy and a helper thread clears it after 5 ms; `band(index)` waits on it; the test asserts each `band()` return found the buffer not busy (the sink records violations; expect 0) over ≥ 20 bands.
- [ ] **Step 2: Run** `meson test -C builddir frame_app` — Expected: compile error (`BandSink`).
- [ ] **Step 3: Implement** — the existing render loop body becomes a function templated on how a frame is drawn; the `FrameSink` path is unchanged. Instantiate for both pixel types in `frame_runtime.cpp`.
- [ ] **Step 4: Run** suite, clang, arm (the QEMU embedded tests in `builddir-arm` still pass under `meson test`).
- [ ] **Step 5: Commit** — `feat(platform): BandSink and the runtime's band loop`.

### Task 7: The Korvo-1 band display

**Files (prism-esp32):**
- Create: `boards/korvo1/band_display.{hpp,cpp}`
- Delete: `boards/korvo1/display.{hpp,cpp}`, `boards/korvo1/rgb565.hpp`, `host/test_rgb565.cpp` (and its `host/meson.build` entry) — PRISM's `Rgb565` replaces them
- Modify: `boards/korvo1/CMakeLists.txt`, `boards/korvo1/Kconfig` (`PRISM_DISPLAY_CPU_CONVERT` → `PRISM_DISPLAY_CPU_COPY`), `sdkconfig.defaults`, `main/widgets.cpp`, `main/scope.cpp`, `README.md`

**Interfaces:**
- Consumes: `prism::platform::BandSink<prism::raster::Rgb565>`, `frame_app(BandSink<P>&, …, FrameOptions)`, `korvo1::pinned_threads()`.
- Produces: `class korvo1::BandDisplay final : public prism::platform::BandSink<prism::raster::Rgb565>` with `static std::expected<BandDisplay, std::string> open()` (same error texts and hints as today's `Display::open`, minus the ARGB frame).

- [ ] **Step 1: Implement** `BandDisplay`:
  - Two band buffers of 800×48 `uint16_t` from `heap_caps_aligned_alloc(CONFIG_CACHE_L1_DCACHE_LINE_SIZE, …, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)`; an error naming the 150 KB it needs if they fail.
  - The panel as today: two RGB565 frame buffers owned by esp_lcd, `on_frame_buf_complete` giving `flipped_`.
  - `flush(index, first_row, rows)`: on the frame's first band (`first_row == 0`), wait for the panel's switch from the last `present()` (as `show()` does today, `kFlipTimeout`), so the back buffer is free. Then a PPA SRM RGB565→RGB565 copy of the band into the back buffer at `block_offset_y = first_row`, **non-blocking**, with `on_trans_done` giving the band's own binary semaphore. `band(index)` takes that semaphore (given once at open) before returning the buffer. With `CONFIG_PRISM_DISPLAY_CPU_COPY`, `flush` copies with `std::memcpy` per row and gives the semaphore itself.
  - `present()`: wait for both bands' copies, drain `flipped_`, `esp_lcd_panel_draw_bitmap` the back buffer, swap.
  - Profile marks: `"wait for panel"`, `"wait for band"`.
- [ ] **Step 2:** `widgets.cpp` and `scope.cpp` open `BandDisplay` and call `frame_app(display, input, model, setup, korvo1::pinned_threads())`. The README's "Speed" paragraph says bands in SRAM, RGB565, no conversion.
- [ ] **Step 3: Build** `scripts/build.sh all` and the host tests (`host/`) — all build, host tests pass.
- [ ] **Step 4: Commit** — `perf(display): draw in 48-row RGB565 bands in SRAM`.
- [ ] **Step 5: Board hand-off** (the user): flash selftest (expect `SIMD on core 1 PASS`, all PASS), widgets and scope; for each, three timing lines and a trace saved from the trace page. Expected: widgets at the panel's refresh (~35 fps, frame gap ~28 ms); scope draw well under 28 ms, record (logic "plot series") well under the 5.4 ms measured before Task 1; no tearing, colours as before.
- [ ] **Step 6:** Update `doc/design/band-rendering.md` status (implemented, with the measured numbers) and the memory notes; commit PRISM — `doc(design): band rendering, measured`.
