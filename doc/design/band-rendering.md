# Drawing in bands, in the panel's pixel format

**Status:** Proposal — for review before code.

## Why

The ESP32-S31-Korvo-1 draws the widget demo at the panel's refresh rate (35.8 fps), but the audio
scope at 15 fps. A trace of the scope (2026-11-21, `profiling.md`'s marks) says where its 68 ms go:

| Thread | Cost per frame | Where |
|---|---|---|
| render | ~28 ms | the two plots' own rectangles (background, frame) filled by the CPU in PSRAM |
| render | ~13 ms | 68 grid lines, 0.1–0.3 ms each: one pixel per row, so one PSRAM cache miss per pixel |
| render | ~6 ms | the two signal polylines |
| render | ~5 ms | the clear (PPA fill, since `FrameSink::clear`) |
| present | 21 ms | ARGB8888 → RGB565 by the PPA, in parallel with drawing |
| logic | 2 × 10 ms | recording each plot: 5.4 ms its points, 2.8 ms mapping and ticks, 1.9 ms labels |

On the host, the same frame draws in 1.1 ms and records in well under 1 ms
(`benchmarks/scope_frame.cpp`). The board is not 60× slower at arithmetic. It is slower at:

1. **Memory latency.** The frame (1.5 MB ARGB8888) lives in PSRAM. Rows are cheap (one cache line
   holds 16 pixels); columns are not. Every vertical line, steep polyline segment and glyph edge
   pays a PSRAM round trip per row.
2. **Doubles.** The chip's FPU is single precision (`-mabi=ilp32f`). The plot maps every point
   through `double` arithmetic, which is a library call per operation.

## Goals

- The scope at the panel's refresh rate on the ESP32; no regression on the STM32 or the desktop.
- The rasterizer draws into fast memory, in the panel's format, and never touches PSRAM per pixel.
- Each core has a role, and the core with the 128-bit SIMD unit runs the pixel loops.

## Non-goals

- Redrawing only what changed (porting-and-devices.md keeps it for later; this note makes it
  easier, not done).
- SIMD code itself: this note only decides where it will run. It comes with measurements, after
  the bands.
- Changing `Field`, `DrawList` or the snapshot.

## Design

### 1. Bands in internal RAM (porting-and-devices.md, level 0)

The render thread draws the frame in horizontal bands. Each band is a buffer in internal SRAM, as
wide as the panel and `band_height` rows tall. For each band, the rasterizer replays the snapshot
and skips draw lists whose bounds miss the band. Then it hands the band to the target. The
target copies it to its frame buffer by DMA (the ESP32's PPA or 2D-DMA, the STM32's DMA2D) and
returns. Two band buffers alternate, so the copy of one overlaps the drawing of the next.

```cpp
template <class T>
concept BandTarget = requires(T& t, std::span<typename T::pixel> band, int y0, int rows) {
    typename T::pixel;
    { t.band_height() } -> std::convertible_to<int>;
    { t.band(0) } -> std::same_as<std::span<typename T::pixel>>;   // buffer 0 or 1, in SRAM
    { t.flush(band, y0, rows) } -> std::same_as<void>;  // start the copy; returns at once
    { t.present() } -> std::same_as<void>;              // all bands flushed: show the frame
};
```

`flush` must not return the same buffer for drawing until its copy is done. The target owns that
wait, as `present()` owns the panel's.

*Our call:* 48 rows. At 800 px and RGB565, a band is 75 KB and two are 150 KB, out of ~400 KB of
free internal RAM on the ESP32 board. Ten bands per frame.

The clear becomes part of each band: the CPU fills 75 KB of SRAM, which takes microseconds.
`FrameSink::clear` and the PPA fill are no longer needed on this path.

### 2. The pixel format is a type (porting-and-devices.md, level 1)

The rasterizer becomes a template on the pixel type: `argb8888`, `rgb565`. A canvas of
`rgb565` blends in RGB565 (unpack, blend, repack); text and anti-aliased edges lose a little
precision, which is not visible at these densities. The ESP32 draws RGB565 into its bands, and
the band copy is a plain copy into the panel's buffer: no ARGB frame, no conversion, no present
task. The STM32's LTDC also reads RGB565, so it can halve its frame too; it stays on ARGB8888
until measured.

### 3. Core roles

ESP-IDF pins a thread only when it is created. *Our call:* a runtime option names the core of each
thread the runtime creates (`frame_app` options: `logic_core`, `render_core`, default: no pin).
The ESP32 port pins the render thread to the SIMD core and the logic thread to the other. On
POSIX and the STM32, the option does nothing.

Both cores drawing (bands split between two render workers) is a later step. In the scope, the
logic thread uses most of the second core today. Making the plot cheaper (section 4) comes first.

### 4. The plot in single precision where it is safe

A plot's data stays `double` (timestamps need it). The mapping of a point to a pixel subtracts the
range's start in `double`, once per point, and does the rest in `float`. That is one library call
per point instead of about six. The tick and label code stays `double`: it runs a few dozen times
per plot, not hundreds.

## Order

1. The plot's mapping in single precision (PRISM, host tests unchanged, board trace).
2. Core roles: the runtime option, then the ESP32 pins.
3. The rasterizer on a pixel type, ARGB8888 only; no behaviour change.
4. RGB565 canvas; host tests render both formats.
5. Bands: the runtime's band loop, then the ESP32 band target (PPA or 2D-DMA copy).
6. Measure. Then decide SIMD and two drawing cores.

Each step is measured on the board with the trace page.

## Risks

- **RGB565 blending precision.** Gradients and anti-aliased text may band. Host pixel tests on
  both formats compare against the ARGB8888 output within a tolerance.
- **Replaying the snapshot ten times.** Cheap when draw lists are skipped by bounds; a draw list
  that spans the whole frame (the window background, a full-screen plot) is walked ten times.
  Measure; if needed, clip commands to the band before replaying.
- **Two DMA engines and the panel on one PSRAM.** The panel's scan-out reads PSRAM continuously.
  Band copies are sequential writes, the cheapest kind.

## Left to the builder

- The exact copy engine on the ESP32 (PPA SRM in RGB565 → RGB565, or 2D-DMA).
- How the runtime finds a target's draw-list bounds cheaply (they exist in the snapshot's
  geometry).
