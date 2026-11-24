# Modern Look Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** PRISM's default look becomes modern (rounded boxes, pill controls, gradient accents,
spacing) in a light and a dark palette; today's look stays as the classic themes.

**Architecture:** The theme grows shape values (corner radius, pill controls, spacing, two
gradients); one new draw command, `GradientRect`, is implemented by every renderer; widgets read
the theme through a few shared helpers (`fill_box`, `outline_box`, `draw_field_box`, and new
`fill_gradient`) instead of fixed shapes.

**Tech Stack:** C++26, doctest, Meson; SDL3 renderer, WebGPU, `raster::BasicCanvas<P>`, SVG export.

**Spec:** /home/jeandet/Documents/prog/PRSIM/docs/superpowers/specs/2026-11-24-modern-look-design.md

## Global Constraints

- The classic themes draw exactly as before: `corner_radius = 0`, `pill_controls = false`,
  `spacing = 0`, solid fills (gradient `from == to`). Existing pixel tests stay unchanged.
- `Gradient{Color from, Color to, Axis axis}`, `Axis::horizontal` by default; equal colours draw a
  solid fill. `DrawList::gradient_rect(Rect r, const Gradient& g, float radius = 0.f)`.
- An explicit `vb.gap()` always wins: a stack with any explicit gap gets no theme spacing.
- Every check is also a look: render `examples/showcase` modern light and dark to SVG and look at
  them in a browser before a task is done.
- Builds and tests on the Coder workspace, one at a time; style per AGENTS.md.

## Review Focus

1. A gradient on a zero-width or zero-height rect, or a radius larger than half the rect, must
   draw nothing wrong (no NaN, no overdraw). Pinned in Task 3.
2. A gradient on an RGB565 board canvas must land on the right colours at both ends (pack once).
   Pinned in Task 3.
3. Theme spacing in a stack nested inside a stack with explicit gaps must apply only to the stack
   that has none. Pinned in Task 4.
4. A widget drawn in a modern theme must keep the size it has in the classic theme (a leaf is
   measured from its drawing; the checkbox spike bug). Pinned in Tasks 5 and 6 for each widget.
5. Switching between classic and modern at runtime must relayout and redraw every widget. Pinned
   in Task 7.

---

### Task 1: The spike as a base, fresh screenshots, guard tests

**Files:** the spike's uncommitted changes (include/prism/ui/context.hpp, include/prism/ui/delegate.hpp,
src/ui/delegate.cpp, src/delegates/dropdown_delegates.cpp, src/delegates/text_delegates.cpp,
examples/showcase/showcase_modern.cpp, examples/showcase/meson.build, tests/test_theme.cpp,
tests/test_model_app.cpp)

- [ ] **Step 1:** Make every showcase `custom_target` depend on PRISM's libraries (so a change in libprism re-makes the SVGs). Verify: touch src/ui/delegate.cpp, rebuild, the SVG's mtime changes.
- [ ] **Step 2:** Full suite. Expected: all pass.
- [ ] **Step 3:** Commit `feat(ui): corner radius in the theme, modern light and dark palettes (spike)`.

### Task 2: Theme values

**Files:** include/prism/ui/context.hpp, tests/test_theme.cpp

**Interfaces:** Produces `enum class Axis { horizontal, vertical }`, `struct Gradient { Color from, to; Axis axis = Axis::horizontal; bool operator==(const Gradient&) const = default; [[nodiscard]] bool solid() const; }`, Theme fields `bool pill_controls`, `float spacing`, `Gradient primary_fill`, `Gradient accent_fill`; functions `classic_dark_theme()`, `classic_light_theme()` (today's values).

- [ ] **Step 1: Failing tests:** classic themes have radius 0, no pill, spacing 0, solid fills equal to `primary` / `accent`; modern themes have radius 8, pill, spacing 10, non-solid fills (teal→blue for primary, lavender→violet for accent); `Gradient{c, c}.solid()`.
- [ ] **Step 2: Run red. Step 3: Implement. Step 4: Run green + full suite. Step 5: Commit** `feat(ui): theme shape values: pills, spacing, gradient fills`.

### Task 3: The gradient draw command, in every renderer

**Files:** include/prism/render/draw_list.hpp (command, API, bounding box), include/prism/render/svg_export.hpp,
include/prism/render/tessellate.hpp + shape_mesh.hpp (per-vertex colour), src/backends/sdl_window.cpp,
src/backends/wgpu_renderer.cpp, src/raster/canvas.cpp + include/prism/raster/canvas.hpp,
src/raster/draw_snapshot.cpp, the damage/snapshot code that switches on command types.
Tests: tests/test_draw_list.cpp (or the existing draw-list test), tests/test_raster_canvas.cpp,
tests/test_pixels.cpp (SDL), tests/test_svg_export.cpp.

- [ ] **Step 1: Failing tests:** `GradientRect` bounding box equals its rect; SVG has a `<linearGradient>` with both stops; the software canvas (Argb8888 and Rgb565) has the `from` colour at the first pixel, `to` at the last and the midpoint colour in the middle, for horizontal and vertical; rounded corners stay clear; zero-size rect draws nothing; radius > half is clamped; SDL pixel test: same three samples through the real renderer.
- [ ] **Step 2: Run red. Step 3: Implement** (GPU paths: interpolate the colour per vertex along the axis, subdividing a rect into strips; canvas: per-row or per-run lerp, pack once per row). **Step 4: Green + full suite, plus builddir-nosdl. Step 5: Commit** `feat(render): gradient_rect in every renderer`.

### Task 4: Theme spacing in stacks

**Files:** the layout code that builds vstack/hstack nodes (include/prism/app/widget_tree_layout.hpp,
include/prism/ui/layout.hpp), tests/test_layout*.cpp

- [ ] **Step 1: Failing tests:** in a theme with spacing 10, three children of a vstack are 10 apart; a vstack with one `vb.gap(4)` has only that gap; a nested stack without gaps inside one with gaps gets the spacing; spacing 0 changes nothing.
- [ ] **Step 2–5:** red, implement, green + full suite, commit `feat(layout): stacks space their children by the theme's spacing`.

### Task 5: Widget pass A: button, slider, progress bar, checkbox, radio, switch

**Files:** src/ui/delegate.cpp, include/prism/ui/delegate.hpp, src/delegates/form_delegates.cpp,
include/prism/delegates/form_delegates.hpp, the showcase, tests/test_theme.cpp

- [ ] **Step 1: Failing tests** (draw-list level, modern theme): a primary button draws a `GradientRect` with `primary_fill`; a checked checkbox and an on toggle draw one with `accent_fill`; a slider's filled track is a gradient pill and its thumb ring lies inside the widget's box; every widget's measured size equals its classic size (Review Focus 4).
- [ ] **Step 2–4:** red, implement (add `delegate_detail::fill_gradient(dl, rect, gradient, radius)`, solid gradients draw as fills), green + full suite; render the showcase light/dark and look.
- [ ] **Step 5: Commit** `feat(ui): modern buttons, sliders, progress, checkboxes and radios`.

### Task 6: Widget pass B: tabs and segmented menus, cards and popups, tables and lists

**Files:** src/delegates/tabs_delegates.cpp, src/delegates/menu_delegates.cpp, include/prism/ui/frame.hpp,
include/prism/app/popup_layout.hpp, include/prism/app/table_paint.hpp, list delegates, tooltip drawing; tests

- [ ] **Step 1: Failing tests:** a selected tab/segment draws `primary_fill` as a pill; group boxes, popups, menus and tooltips use `corner_radius`; a selected list row is rounded in the accent; sizes equal the classic ones.
- [ ] **Step 2–4:** red, implement, green + full suite; render the widget gallery light/dark and look.
- [ ] **Step 5: Commit** `feat(ui): modern tabs, cards, popups, menus and lists`.

### Task 7: The default switches to modern

**Files:** include/prism/ui/context.hpp, every test that checks theme values or pixels (pin
`classic_*`), python bindings' theme names if any, doc/app/themes.md (screenshots of both
palettes from the showcase), examples/widget_gallery.

- [ ] **Step 1: Failing test:** `default_theme() == modern_dark_theme()`, `light_theme() == modern_light_theme()`; switching classic → modern → classic at runtime relayouts and redraws every widget (Review Focus 5).
- [ ] **Step 2–4:** red, implement, green + full suite + web build of the gallery looked at in a browser.
- [ ] **Step 5: Commit** `feat(ui): the modern look is the default; classic themes stay`.
