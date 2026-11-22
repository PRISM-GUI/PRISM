# Damage tracking: redraw only what changed

Status: built for the software renderer (`FrameSink`, `BandSink`). The SDL window still repaints
the whole frame, on purpose (see "Where it applies").

## Why

Most frames change a small part of the screen: a counter, a slider, a timing line. The software
renderer drew all of it every time. On the Korvo-1 panel (800x480) a whole frame is 768 KB of
RGB565, about 13 ms of PSRAM copying at the ~58 MB/s the board measures, before any drawing.
`benchmarks/dirty_area.cpp` measures how much really changes. On the widget demo 0 to 17% of the
pixels do; on the audio scope 85% do, because both plots move every frame.

## How

1. **What changed.** A widget's draw list is shared between snapshots while the widget is
   unchanged (the per-widget cache in `layout_flatten`). `damage_between(before, after)`
   (`render/damage.hpp`) pairs the entries of two snapshots by widget id. A list that is not the
   same object, or whose geometry, visible rect or scale differs, is damage: the rect it drew
   in, and the rect it draws in now (its leading clip, or its geometry grown by `kListMargin`).
   Entries that appeared or went away are damage too. The result is a list of integer rects.
2. **Always at least enough.** Anything it cannot tell apart is full damage: no previous frame, a
   new viewport or background, a changed overlay (popups, focus rings), a changed relative draw
   order, a list that has no clip and no geometry entry. The answer is "redraw this much or
   more", never less.
3. **Redraw into a buffer that is old.** The buffer being drawn into shows an older frame. A
   sink says how old: `FrameSink::buffer_age()` / `BandSink::frame_age()` (the idea of
   EGL's buffer age). 1 is a buffer that keeps the last presented frame, 2 is two buffers that
   flip, 0 (the default) means unknown. With age 2 the runtime redraws this frame's damage united
   with the previous frame's (`DamagePlan`, `unite`). With age 0 every frame is drawn whole, as
   before: a sink that says nothing changes nothing.
4. **Drawing a rect.** `raster::draw_damage` sets the canvas's *scissor* to the rect, clears it to
   the background, and draws the lists that reach it (`draw_band`, which now also skips a list
   that misses the scissor sideways). The pixels equal those of a whole redraw:
   `tests/test_raster_damage.cpp` checks that frame by frame, and `tests/test_frame_app.cpp`
   checks it through the runtime with one buffer and with two that flip.
5. **Bands.** A band target draws and flushes only the bands the damage touches
   (`BandPainter`). A band is drawn whole: the unit of copying is the band's rows.

## Where it applies

| Target | Uses it | Why |
|---|---|---|
| `FrameSink` with `buffer_age() > 0` | yes | the runtime draws into its buffer |
| `BandSink` with `frame_age() > 0` | yes | only changed bands are drawn and copied |
| the SDL window | no | a GPU renderer repaints cheaply, and its back buffer is not kept between frames. The damage is computed the same way, so a later change can clip its repaint. |

## Simplifications

- A changed overlay or draw order is full damage. Upgrade: per-command bounds, a z-order diff.
- More than `kMaxRects` rects collapse to their bounding box. Upgrade: a better merge.
- A list without a clip is bounded by its geometry rect plus `kListMargin`, as `draw_band` has
  always assumed. Upgrade: a measured bounds rect per list.
- A band is drawn whole, not only the damaged rect inside it.

## Left to the boards

The Korvo-1 band display keeps its two PSRAM frame buffers and flips them, so it would say
`frame_age() == 2`; it does not yet. Whether it pays depends on what changes: see the numbers in
`benchmarks/dirty_area.cpp`.
