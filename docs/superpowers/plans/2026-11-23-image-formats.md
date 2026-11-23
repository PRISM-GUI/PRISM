# Image formats Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `render::Image` carries a pixel format. It can be scaled and converted. Decoders are
registered backends. `paint_image<P>` freezes a canvas into an image. The player drops its own
shrink and JPEG code.

**Architecture:** A `PixelFormat` tag on the one Image class. One general area-average routine,
`fit_pixels`, behind `scaled`, `converted` and every decoder. A decoder registry shaped like the
audio device registry. A hardware JPEG decoder in prism-esp32.

**Tech Stack:** C++26, meson, doctest, SDL3_image, ESP-IDF 6 `driver/jpeg_decode.h`.

**Spec:** /home/jeandet/Documents/prog/PRSIM/docs/superpowers/specs/2026-11-23-image-formats-design.md

## Global Constraints

- `enum class PixelFormat : std::uint8_t { rgba8888, rgb565 };`
  - `rgba8888` means bytes R, G, B, A, straight alpha.
  - `rgb565` is one native-endian `uint16_t` per pixel, packed like `raster::Rgb565::pack`.
- Existing callers of `Image::from_rgba`, `decode_image(bytes)`, `decode_image(bytes, svg_size)`
  and `load_image` compile and behave unchanged.
- `scaled` never enlarges. `scaled` and `converted` return the same `shared_ptr` when nothing
  changes.
- Float, not double, on paths the ESP32 runs (`fit_pixels`, the canvas, the decoders).
- The registry tries the most recently registered decoder first. With none registered, the error
  string is exactly `"no image decoder"`.
- The decoder registry sources go where the audio sources go: in the `prism-software-backend`
  library when SDL is enabled, in libprism otherwise (`src/meson.build`, like
  `prism_audio_sources`).
- Style: PRISM AGENTS.md. Never name a namespace `detail`. Mark deliberate cuts with `simplify:`.

## Review Focus

1. A band boundary that cuts an `rgb565` image mid-picture must copy exactly the band's rows.
   Covered by a Task 3 test.
2. An image smaller than the box must come back at its own size, as the same pointer. Covered
   in Task 2.
3. A stride larger than a row, as with the ESP32 engine's MCU padding, must have its padding
   ignored. Covered in Task 2.
4. A decoder that throws or returns null without an error string must not crash the registry,
   which reports a generic reason. Covered in Task 4.
5. `Argb8888::over` on an opaque destination must stay bit-identical to today, so framebuffer
   pixel tests don't move. Covered in Task 1.

---

### Task 1: Exact straight-alpha `over` for Argb8888, and the byte-order comment

**Files:**
- Modify: `include/prism/raster/pixel.hpp`
- Test: `tests/test_raster_pixel.cpp`. Create it if absent, and register it in `headless_tests`.

**Interfaces:**
- Produces: `Argb8888::over(dst, src)`, exact for any destination alpha.

- [ ] **Step 1: Write the failing tests.**
  - `over_onto_opaque_is_unchanged`: for a table of (dst, src) pairs with dst alpha 255, the new
    `over` equals the old formula. Copy the old body into the test as `reference_over`.
  - `half_red_onto_transparent`: `over(0x00000000, rgba(255, 0, 0, 128)) == 0x80FF0000`.
  - `half_red_onto_half_blue`: the out alpha is `128 + 128*127/255`, rounded, and the colour
    channels equal `(s*sa + d*da*(255-sa)/255) / out_a`, rounded. Compute these in the test.
- [ ] **Step 2: Run them.** Expect the opaque test to pass and the other two to FAIL.
- [ ] **Step 3: Implement.**
  - Keep the current path when `dst >> 24 == 255`.
  - Otherwise, use straight-alpha src-over with integer math and rounding.
  - Fix the comment: on little-endian targets the bytes in memory are B, G, R, A.
- [ ] **Step 4: Run the full suite.** Expect everything to pass, including `test_pixels`.
- [ ] **Step 5: Commit.** `fix(raster): exact src-over onto a translucent Argb8888 pixel`

### Task 2: Image format, `fit_pixels`, `scaled`, `converted`

**Files:**
- Modify: `include/prism/render/draw_list.hpp` (Image)
- Create: `include/prism/render/image_ops.hpp`, `src/render/image_ops.cpp` (in libprism)
- Test: `tests/test_image_ops.cpp` (headless)

**Interfaces:**
- Produces:
  - `enum class PixelFormat`
  - `Image::format()`
  - `static Image::from_rgb565(int w, int h, std::vector<std::uint16_t>)`, which returns
    `std::expected<std::shared_ptr<const Image>, std::string>`
  - `enum class SourceFormat { rgb888, rgba8888, rgb565 }`
  - `struct PixelView { std::span<const std::byte> bytes; int width, height, stride; SourceFormat format; }`,
    with `stride` in bytes
  - `std::shared_ptr<const Image> fit_pixels(PixelView, core::Size box, PixelFormat out)`
  - `PixelView view_of(const Image&)`
  - `std::shared_ptr<const Image> scaled(const std::shared_ptr<const Image>&, core::Size box)`
  - `std::shared_ptr<const Image> converted(const std::shared_ptr<const Image>&, PixelFormat)`

  `scaled` and `converted` are free functions, so they can hand back the same pointer.

- [ ] **Step 1: Write the failing tests.**
  - `average_4x4_to_2x2`: each output pixel is the rounded mean of its 2×2 block.
  - `aspect_kept`: a 400×200 view into a 100×100 box gives 100×50.
  - `never_enlarges`: 10×10 into 100×100 gives 10×10.
  - `stride_padding_ignored`: an rgb888 view with stride `16*3` and width 10. Padding bytes are
    0xFF, and the output has no 0xFF bleed.
  - `transparent_does_not_bleed`: one opaque red pixel averaged with three fully transparent
    green ones gives a red with alpha 64.
  - `rgb888_to_rgb565_packs`: the result equals `raster::Rgb565::pack` of the same colour.
  - `bad_view_is_null`: a stride below `width*bpp`, and too few bytes.
  - `scaled_same_pointer_when_fits`, `converted_same_pointer_same_format`.
  - `round_trip_565`: `rgba8888` → `rgb565` → `rgba8888` stays within ±4 on r and b and ±2 on g.
  - `from_rgb565_rejects_wrong_size`.
- [ ] **Step 2: Run them.** Expect FAIL.
- [ ] **Step 3: Implement.**
  - Area averaging uses integer per-channel sums over each output pixel's source rectangle.
    Rectangle edges are `i*src/dst`. Port the core of the player's
    `prism-mp3-player/core/shrink.cpp`.
  - Alpha is premultiplied during the sum and divided out at the end.
  - Output `rgb565` packs like `Rgb565::pack`.
- [ ] **Step 4: Run the full suite.** Expect PASS.
- [ ] **Step 5: Commit.** `feat(render): Image carries a pixel format; fit_pixels, scaled, converted`

### Task 3: Readers of `rgb565` images

**Files:**
- Modify:
  - `src/raster/canvas.cpp` (`BasicCanvas<P>::image`)
  - `include/prism/render/draw_list.hpp` (`as_symbolic`)
  - `src/backends/sdl_window.cpp` (texture upload near line 540)
  - `src/backends/wgpu_renderer.cpp` (upload near line 343)
  - `include/prism/render/svg_export.hpp` (PNG embed near line 91)
- Test: `tests/test_raster_canvas.cpp` (or the existing canvas test file), `tests/test_pixels.cpp`

**Interfaces:**
- Consumes: from Task 2, `PixelFormat`, `converted`, `view_of`.

- [ ] **Step 1: Write the failing tests.**
  - `rgb565_image_1to1_is_row_copy`: an `Rgb565` canvas, with an image drawn at its own pixel
    size. The canvas rows equal the image rows.
  - `rgb565_image_cut_by_band`: as above, but on a band canvas with `first_row` in the middle of
    the image. The band rows equal the matching image rows.
  - `rgb565_image_scaled_blends`: drawn at 2×, each source pixel covers 2×2.
  - `rgb565_image_tinted`: a tint of half grey halves the channels, within rounding.
  - `symbolic_of_rgb565`: the result is `rgba8888`, white, alpha 255.
  - Pixel test `rgb565_matches_rgba_conversion`: through the SDL backend, an `rgb565` image and
    its `converted(rgba8888)` render the same pixels.
- [ ] **Step 2: Run them.** Expect FAIL.
- [ ] **Step 3: Implement.**
  - **Canvas:** read the source pixel by format. The fast path (`rgb565` into `Rgb565`, 1:1 in
    pixels, alpha 255, white tint, no ui scale) does a `std::copy` of each clipped row.
  - **SDL:** use `SDL_PIXELFORMAT_RGB565` with pitch `w*2` for `rgb565`.
  - **WebGPU:** upload `converted(img, rgba8888)` for `rgb565`.
  - **SVG export:** use `converted(..., rgba8888)`.
  - **`as_symbolic`:** convert first.
- [ ] **Step 4: Run the full suite, and once in `builddir-nosdl`.** Expect PASS.
- [ ] **Step 5: Commit.** `feat(render): canvas, SDL, WebGPU and SVG read rgb565 images`

### Task 4: Decoder registry

**Files:**
- Modify: `include/prism/render/image_decoder.hpp`, `src/backends/sdl_image_decoder.cpp`,
  `src/meson.build`
- Create: `src/render/image_decoders.cpp` (the registry, `decode_image`, `load_image`)
- Test: `tests/test_image_decoder_registry.cpp`. Link it like `audio_device` in
  `tests/meson.build`, with `prism_software_backend_dep` when SDL is on.

**Interfaces:**
- Produces:
  - `struct DecodeOptions { std::optional<core::Size> fit; PixelFormat format = PixelFormat::rgba8888; };`
  - `class ImageDecoder { virtual std::shared_ptr<const Image> decode(std::span<const std::byte>, const DecodeOptions&, std::string& error) = 0; }`
  - `register_decoder(ImageDecoder&)`, `unregister_decoder(ImageDecoder&)`
  - `decode_image(bytes, const DecodeOptions& = {})`
  - `decode_image(bytes, std::optional<core::Size> svg_size)`, which forwards
  - `load_image(path, const DecodeOptions& = {})`
  - `load_image(path, std::optional<core::Size>)`, which forwards

- [ ] **Step 1: Write the failing tests.** Use fake decoders.
  - `last_registered_tried_first`.
  - `failure_falls_through`: the first fails, the second succeeds.
  - `all_fail_reports_last_error`.
  - `none_registered`: the error is `"no image decoder"`. This needs the SDL decoder
    unregistered, or the test built without it. Pick whichever the build allows, and note it.
  - `unregister_removes`.
  - `null_without_error_is_generic_failure`.
  - `exception_is_failure`.
  - `options_reach_decoder`: fit and format are passed through.
  - With SDL: `sdl_decodes_png_to_rgb565`. A small PNG made with `encode_png`, decoded with
    `{.format = rgb565}`, gives an `rgb565` image of the right size. Also
    `svg_size_overload_still_renders_svg`.
- [ ] **Step 2: Run them.** Expect FAIL.
- [ ] **Step 3: Implement.**
  - The registry is a function-local static vector under a mutex, as `src/audio/registry.cpp`
    does.
  - The SDL decoder becomes a class registered by a static object, as `sdl_audio.cpp` does. It
    applies `fit` and `format` through `fit_pixels` for bitmaps. For an SVG, `fit` is the render
    box, then `format` is applied.
  - Move the sources in `src/meson.build` as the Global Constraints say.
- [ ] **Step 4: Run the full suite, plus `builddir-nosdl` and `builddir-tsan`.** Expect PASS.
- [ ] **Step 5: Commit.** `feat(render): image decoders are registered backends`

### Task 5: `paint_image<P>`

**Files:**
- Create: `include/prism/raster/paint_image.hpp`. Explicit instantiations for both formats go in
  `src/raster/paint_image.cpp`.
- Test: `tests/test_paint_image.cpp`

**Interfaces:**
- Produces:
  `template <class P> std::shared_ptr<const render::Image> paint_image(int width, int height, const std::function<void(BasicCanvas<P>&)>& paint);`
  It returns null when the size is not positive.

- [ ] **Step 1: Write the failing tests.**
  - `rgb565_image_of_drawn_rect`: the format is `rgb565`, and the pixels equal `Rgb565::pack`
    of the fill.
  - `argb_image_is_rgba_bytes`: an opaque (10, 20, 30) fill reads back as bytes 10, 20, 30, 255.
  - `argb_starts_transparent`: an untouched pixel has alpha 0.
  - `half_red_on_transparent_reads_back`: the bytes are (255, 0, 0, 128). This needs Task 1.
  - `bad_size_is_null`.
- [ ] **Step 2: Run them.** Expect FAIL.
- [ ] **Step 3: Implement.**
  - The `Rgb565` pixel vector moves into `from_rgb565`.
  - `Argb8888` is swizzled into an RGBA byte vector, then `from_rgba`.
- [ ] **Step 4: Run the full suite.** Expect PASS.
- [ ] **Step 5: Commit.** `feat(raster): paint_image freezes a canvas into an Image`

### Task 6 (prism-esp32): hardware JPEG decoder

**Files:**
- Create: `boards/korvo1/jpeg_decoder.hpp`, `boards/korvo1/jpeg_decoder.cpp`
- Modify: the board component CMake that lists `speaker_device.cpp`, and `main/tone.cpp`, which
  gets a decode check.

**Interfaces:**
- Produces: `void prism::esp32::register_jpeg_decoder();`. It is idempotent, with a static
  backend like `register_speaker`.
- Consumes: from Tasks 2 and 4, `ImageDecoder`, `fit_pixels`, `PixelView{..., SourceFormat::rgb888}`.

- [ ] **Step 1: Implement.** This is a board-only task. There are no host tests.
  - Move `prism-mp3-player/boards/korvo1/main/jpeg_cover.cpp`.
  - Return null with `"not a JPEG"` unless the bytes start with `FF D8`.
  - A mutex serialises engine use.
  - Keep the 1600×1600 cap with its `simplify:` comment.
  - `fit` defaults to the full size. `format` comes from the options.
- [ ] **Step 2: Add the tone app check.** Decode a small baseline JPEG embedded as a byte array,
  generated once on the host, at 64×64 `rgb565`. Print `jpeg: 64x64 rgb565 px0=0x....`.
- [ ] **Step 3: Build.** Run `scripts/build.sh tone` on Coder. Expect a clean build.
- [ ] **Step 4: Commit.** `feat(korvo1): the JPEG engine as a PRISM image decoder`

### Task 7 (prism-mp3-player): use PRISM's images

**Files:**
- Delete: `core/shrink.hpp`, `core/shrink.cpp`, `boards/korvo1/main/jpeg_cover.hpp`,
  `boards/korvo1/main/jpeg_cover.cpp`, and the shrink test
- Modify: `desktop/desktop_app.cpp` (`decode_cover`), `boards/korvo1/main/player_main.cpp`,
  `boards/korvo1/main/CMakeLists.txt`, `boards/korvo1/components/player_core/CMakeLists.txt`,
  `meson.build` / `tests/meson.build`

**Interfaces:**
- Consumes: `decode_image(bytes, {.fit, .format})` from Task 4, and
  `register_jpeg_decoder()` from Task 6.

- [ ] **Step 1: Change the default decoder.**
  - The default `CoverDecoder` is
    `decode_image(bytes, {.fit = Size{side, side}, .format = kCoverFormat}).value_or(nullptr)`.
  - `kCoverFormat` is `rgb565` on the board and `rgba8888` on the desktop.
  - The board's `player_main` calls `register_jpeg_decoder()` at boot.
- [ ] **Step 2: Delete the files listed above.** Move any shrink test cases that Task 2 doesn't
  already cover into PRISM.
- [ ] **Step 3: Run the host suite on Coder, and build the board with `scripts/build-korvo.sh`.**
  Expect all suites to pass and the firmware to build.
- [ ] **Step 4: Commit.** `refactor: covers through PRISM's image decoders`
