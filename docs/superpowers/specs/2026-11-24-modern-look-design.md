# A modern look for PRISM: rounded shapes, gradient accents, light and dark

Date: 2026-11-24. Status: draft, for approval.

## Why

The user wants PRISM to look like a current UI: the reference they gave is a dark "material"
style kit (navy base, teal-blue and lavender-violet accents, gradients on whatever is "on", pill
switches and segmented menus, rounded cards with thin light borders, big round slider thumbs,
generous spacing), and the same in light is as good. Today every box is a square, there is no
gradient anywhere, and stacks have no spacing: the widgets look dated next to anything modern.

A spike (2026-11-24, uncommitted) proved the direction: a corner radius in the theme, two new
palettes, and six widgets drawn through two shape helpers already read as modern in both light
and dark, with the classic look pixel-identical (the pixel tests pass unchanged).

## Goals

- One new look with a light and a dark palette sharing the same shapes and states.
- It becomes PRISM's default; today's look stays available as the classic themes.
- The things that make the reference read as modern: rounded boxes, pill controls, gradient
  accents on "on" states, clear state styling (outlined / dim / filled / bright outline), spacing.
- Works on every backend: SDL, WebGPU, the software canvas (boards), SVG export.
- Nothing changes for an app that picks the classic theme.

## Non-goals

- Soft glows and drop shadows (blur is expensive, worst of all on a microcontroller): later.
- Anti-aliased shapes in the software canvas: later (its own item; boards keep jagged corners).
- Animations of state changes.
- New widgets (rating stars, knobs): the look applies to the existing catalog.

## Approaches considered

1. **Theme values plus a widget pass, with a gradient draw command (chosen).** The theme grows a
   few shape values; widgets read them; one new draw command gives every renderer gradients.
2. **Theme values only, solid colours.** The spike's look: clearly better, but without gradients
   it misses most of what makes the reference read as modern.
3. **Images for widget backgrounds (9-slice skins).** Any look, but every state and size is an
   asset, it is heavy on a board, and it cannot follow a palette or a UI scale.

## Design

### 1. The theme gains shape and spacing values

```cpp
float corner_radius = 0.f;    // boxes: fields, buttons, cards, popups (spike: done)
bool  pill_controls = false;  // switches, segmented menus, sliders: fully rounded ends
float spacing       = 0.f;    // between the children of a vstack/hstack that set none
Gradient primary_fill{};      // buttons marked primary, a progress bar, a filled slider track
Gradient accent_fill{};       // a checked box, a switch that is on, a selected segment
```

`Gradient` is `{Color from, Color to, Axis axis}` with `Axis::horizontal` by default; a gradient
whose two colours are equal draws as a solid fill, which is what the classic themes hold. The
classic themes keep `corner_radius = 0`, `pill_controls = false`, `spacing = 0` and solid
fills, so they draw exactly as today.

`spacing` is applied by the layout: a stack inserts it between children, unless the view set its
own gaps with `vb.gap()` (an explicit gap always wins; a stack with any explicit gap gets none
added). The spike's hand-written gaps then go away.

### 2. A gradient draw command

```cpp
struct GradientRect { Rect rect; Color from, to; Axis axis; float radius; };
void DrawList::gradient_rect(Rect r, const Gradient& g, float radius = 0.f);
```

Two stops, horizontal or vertical, optionally rounded: what the reference uses. Each renderer:

- **SDL and WebGPU** already draw rounded shapes as triangle meshes (render/shape_mesh.hpp):
  each vertex gets the colour interpolated at its position along the axis, and the GPU blends
  between vertices. A rectangle is subdivided along the axis so long gradients stay smooth.
- **The software canvas** (`raster::BasicCanvas<P>`, boards): each span's colour is interpolated
  per row (vertical) or per pixel run (horizontal). Cheap on a board: one lerp per row or a
  running add per pixel; RGB565 targets dither nothing (simplify: banding on long 565 gradients).
- **SVG export**: a `<linearGradient>` in `<defs>`, referenced by the rect.
- `bounding_box()`, damage tracking and the snapshot code treat it like a `RoundedRect`.

### 3. The widget pass

Each widget reads the theme instead of fixed shapes, in the order people notice:

1. Button: rounded; primary uses `primary_fill`; a checked toggle uses `accent_fill`; normal is
   outlined; disabled is the dim outline. (Spike: rounded, solid.)
2. Text field, text area, dropdown, combo box, their popups: rounded field box with a light
   border, the border brighter on hover and in the accent when focused. (Spike: done.)
3. Slider and progress bar: pill track, filled part in the gradient, round thumb ringed in white,
   kept inside the widget's box (the spike clips the ring by one pixel). (Spike: solid.)
4. Checkbox and radio: rounded box / circle filled with `accent_fill` when on; no box around the
   label. Switch: a pill whose track is `accent_fill` when on and a white round knob.
5. Tabs and segmented menus: pill segments, the selected one filled with `primary_fill`.
6. Cards, group boxes, popups, menus, tooltips: `corner_radius`, thin light border.
7. Tables, lists, trees: selected rows in the accent, rounded selection inside lists.

The checkbox's transparent sizing rect (spike) stays: a leaf is measured from what it draws.

### 4. Themes and the default

`ui::modern_dark_theme()` and `ui::modern_light_theme()` (spike palettes, tuned during the pass),
`ui::classic_dark_theme()` and `ui::classic_light_theme()` (today's values), and
`default_theme()`, `dark_theme()`, `light_theme()` returning the modern ones at the end of the
project. Existing tests that check theme values or pixels pin the classic themes explicitly, so
the switch of default changes no test's meaning. `follow_system_theme()` picks modern light or
dark.

### 5. Prerequisite: the theme-switch layout bug

A theme set from an app's setup (`AppContext::set_theme`) left layout sizes from the previous
theme (found by the spike: a checkbox row measured 16 px instead of 30). It is fixed first, with
a test through the app path, since every app sets its theme that way.

## Checking it

- Each step renders the modern showcase and the widget gallery to SVG, light and dark, and the
  screenshots are looked at in a browser before the step is done.
- Pixel tests (tests/test_pixels.cpp) for the gradient command on each renderer: the colour at
  the start, the middle and the end of a horizontal and a vertical gradient, rounded corners
  left clear.
- The classic themes' pixel tests stay unchanged throughout.
- The F746 and ESP32 builds keep their size within a few KB (the gradient code in the canvas is
  small); measured with `prism size`.

## Left to the builder

- Exact palette values and the spacing value, tuned on the screenshots.
- How many subdivisions a GPU gradient rectangle needs.
- The order of the widget pass inside each numbered step.
