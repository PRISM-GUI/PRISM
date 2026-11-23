# Images with a pixel format, decoders as backends, painting into an image

Date: 2026-11-23. Status: draft, for approval.

## Why

The MP3 player shows album covers on two platforms:

1. On the desktop it calls PRISM's `decode_image`, which goes through SDL_image.
2. On the Korvo board it has its own code. That code runs the chip's JPEG engine, shrinks the
   picture with its own `shrink_rgb888`, and builds a `render::Image`.

Every board app that shows a picture would have to write the same thing again. There is also a
cost on the board. `render::Image` is always RGBA8, at 4 bytes per pixel. The board's screen is
RGB565, so every frame converts the cover pixel by pixel while it blends it.

This is the image half of the work already agreed in conversation. The audio half, moving the
output devices into PRISM, is done. The pattern is the same:

1. PRISM gets the abstraction.
2. The platform registers what it can do.
3. The app drops its private copy.

## Goals

- `render::Image` carries a pixel format, `rgba8888` or `rgb565`, like the raster canvas's pixel
  types. A board can keep a cover in the format of its screen, at half the memory, and draw it
  with a row copy.
- An image API that feels like QImage:
  - `scaled(box)`, an area-average shrink that keeps the aspect ratio. It is `shrink_rgb888`
    made general.
  - `converted(format)`.
  - `decode_image(bytes, {fit, format})`, which returns an image already at the size and format
    the caller needs.
- Image decoders are backends that register themselves, like audio devices:
  - SDL_image on the desktop.
  - The ESP32-S31 hardware JPEG engine in prism-esp32.
  - An app can register its own.
- `paint_image<P>(width, height, fn)`. You draw on a normal `BasicCanvas<P>` and get back a frozen
  `shared_ptr<const Image>`. It is the canvas image renderer.
- The player deletes `core/shrink.*` and `boards/korvo1/main/jpeg_cover.*`.

## Non-goals

- Pixel formats beyond these two (no ARGB4444, no palettes, no YUV). Add one when a screen
  needs it.
- Enlarging. `scaled` never makes an image bigger. Canvases and GPUs already scale at draw time.
- A software JPEG or PNG decoder for the board. The board still shows the placeholder for PNG
  and progressive JPEG.
- Anti-aliased shapes in `paint_image`. It inherits the canvas's aliased shapes; only text is
  anti-aliased. The canvas header already marks this with `simplify:`.
- Editing an image in place. An Image stays immutable: drawing makes a new one.

## Anti-goals

- It is a failure if desktop code that never mentions a format has to change: `from_rgba`,
  `decode_image(bytes)`, `load_image`, icons, symbolic icons.
- It is a failure if the board decodes a cover and then also keeps a full-size RGBA8 copy.

## Approaches considered

1. **A format tag on the one Image class** (chosen). Image stays a single type-erased class
   carried in draw lists. A `PixelFormat` field tells readers how to read the bytes. The three
   readers (raster canvas, SDL texture upload, WebGPU upload) switch on it once per image, not
   once per pixel.
2. **`Image<P>` templated on the pixel type.** It catches mistakes at compile time. But draw
   lists, `ImageCmd` and the GPU caches would all need a variant or a type-erased wrapper, which
   rebuilds option 1 with more code.
3. **Convert at the edge.** Image stays RGBA8 and the board converts to RGB565 at draw time, as
   it does today. This is no work, but it keeps the per-frame cost and twice the memory that
   this design exists to remove.

## Design

### Pixel format on Image (`render/draw_list.hpp`)

```cpp
enum class PixelFormat : std::uint8_t { rgba8888, rgb565 };
```

- `rgba8888` is the current layout: bytes R, G, B, A, straight alpha.
- `rgb565` is one `uint16_t` per pixel, native endian, laid out like `raster::Rgb565::storage`.
  It has no alpha, so it is opaque.

Changes to Image:

- `Image::from_rgba(w, h, bytes)` is unchanged.
- New: `Image::from_rgb565(w, h, std::vector<std::uint16_t>)`.
- `format()` is added.
- `pixels()` keeps returning the raw bytes. Rows are packed with no stride.
- `as_symbolic()` on an `rgb565` image converts to `rgba8888` first, because a symbolic icon
  needs alpha.

### Pixel views and the shrink (`render/image_ops.hpp`, new)

```cpp
enum class SourceFormat : std::uint8_t { rgb888, rgba8888, rgb565 };
struct PixelView { std::span<const std::byte> bytes; int width, height, stride; SourceFormat format; };

std::shared_ptr<const Image> fit_pixels(PixelView src, core::Size box, PixelFormat out);
```

- `fit_pixels` area-averages the source down to the largest size that fits `box` with the aspect
  ratio kept. It never enlarges. It writes the result in `out`.
- It returns null on a bad view: a stride smaller than a row, or too few bytes.
- `rgb888` is a source format only. It covers decoders that output 3 bytes per pixel, such as
  the ESP32 engine. The `stride` field exists for the engine's 16-pixel MCU padding.
- The averaging is `shrink_rgb888`'s, moved and made general: per-channel sums, then rounding.
- Alpha is premultiplied while averaging, so transparent pixels don't bleed their colour.

Members built on it:

- `image.scaled(box)` is `fit_pixels(view_of(image), box, image.format())`.
- `image.converted(format)` is `fit_pixels` with a box equal to the image's own size.
- Both return the same image, without a copy, when nothing changes.

### Decoders as backends (`render/image_decoder.hpp`)

```cpp
struct DecodeOptions { std::optional<core::Size> fit; PixelFormat format = PixelFormat::rgba8888; };

class ImageDecoder {
public:
    virtual ~ImageDecoder() = default;
    // Null and `error` set when this decoder cannot read these bytes; the next one is tried.
    virtual std::shared_ptr<const Image> decode(std::span<const std::byte> bytes, const DecodeOptions& options,
                                                std::string& error) = 0;
};
void register_decoder(ImageDecoder& decoder);
void unregister_decoder(ImageDecoder& decoder);

std::expected<std::shared_ptr<const Image>, std::string>
decode_image(std::span<const std::byte> bytes, const DecodeOptions& options = {});
```

How it behaves:

- `decode_image` tries decoders in reverse registration order, so the most recently registered
  is tried first. A board's hardware decoder registered at boot therefore beats a generic one.
  The first image returned wins.
- If every decoder fails, the error names the last decoder's reason. If none is registered, the
  error is "no image decoder".
- The registry guards its list with a mutex. Decoding can run on any thread. A decoder that is
  not thread-safe serialises itself; the ESP32 engine does this.
- `fit` means two things:
  - For an SVG, it is the box to render into. This is today's `svg_size`.
  - For a bitmap, it is the shrink target. A decoder may meet it any way it likes; the default
    is `fit_pixels`.
- `format` is honoured by every decoder, through `fit_pixels` or natively.
- `load_image(path, options)` reads the file and calls `decode_image`.
- `svg_size` overloads: today's callers pass `std::optional<core::Size>` positionally (zero
  found outside the SDL decoder). Keep one overload taking `std::optional<core::Size>` that
  forwards as `{.fit = size}`, so nothing breaks.
- The SDL_image decoder becomes an `ImageDecoder` registered by a static object, as
  `sdl_audio.cpp` does.
- The registry source goes in the same place as the audio registry: in the software backend
  library when SDL is enabled, and in libprism otherwise. This avoids the `--as-needed` trap we
  hit with audio, where the linker dropped the library holding the static registrar.

### Painting into an image (`raster/paint_image.hpp`, new)

```cpp
template <class P>   // raster::Argb8888 or raster::Rgb565
std::shared_ptr<const render::Image> paint_image(int width, int height,
                                                 const std::function<void(BasicCanvas<P>&)>& paint);
```

What it does:

- It allocates the pixels and makes a canvas on them.
  - `Argb8888` starts fully transparent.
  - `Rgb565` starts black.
- It calls `paint`, then freezes the result.
  - `Rgb565` hands its pixels straight to `from_rgb565`, with no copy.
  - `Argb8888` swizzles once into `rgba8888`.
- The canvas has no `Fonts` by default. A caller who wants text passes them in its lambda, as
  canvas text calls already do.
- It runs on the calling thread. It is not meant for every frame. Each call makes a new image id,
  so the GPU caches upload it again. The caches already evict images unused for two frames, so
  this wastes work but leaks nothing.

Two fixes in `raster/pixel.hpp` that this needs:

1. `Argb8888::over` treats the destination as opaque when it blends colours. That is exact on a
   framebuffer, which is always opaque. It is wrong in a transparent `paint_image` buffer: a 50%
   red drawn on transparent comes out as 50% dark red. The fix adds the proper straight-alpha
   path for `dst alpha < 255`, dividing by the output alpha. The opaque-destination path stays
   as it is, so framebuffer drawing costs no more.
2. The comment on `Argb8888` says the bytes in memory are R, G, B, A. On little-endian targets
   they are B, G, R, A. The swizzle above depends on this, so the comment gets corrected.

### Readers

- **Raster canvas `image()`:** it reads either format. It gets one fast path: an `rgb565` image
  drawn into an `Rgb565` canvas at 1:1, with alpha 255 and a white tint, becomes a row copy,
  clipped to the band. Everything else goes through the existing nearest-neighbour blend loop.
- **SDL texture upload:** it makes an `SDL_PIXELFORMAT_RGB565` texture for `rgb565` and an
  `SDL_PIXELFORMAT_ABGR8888` texture for `rgba8888`, which matches what it does today.
- **WebGPU upload:** it converts `rgb565` to `rgba8888` once at upload. The cache is keyed by
  image id, so the conversion happens once per image.
- **SVG export:** it calls `converted(rgba8888)` before encoding the PNG.

### prism-esp32: the JPEG engine as a decoder

- `boards/korvo1/jpeg_decoder.cpp` provides `register_jpeg_decoder()`. It is the player's
  `jpeg_cover.cpp` moved over.
- It only accepts JPEG bytes, starting with `FF D8`. For anything else it reports "not a JPEG",
  so the next decoder gets a try.
- The engine writes RGB888 with MCU padding. The decoder wraps that output in a `PixelView` and
  calls `fit_pixels(view, options.fit or the full size, options.format)`. The full-size RGB888
  buffer is freed straight away.
- The 1600×1600 cap stays, with its existing `simplify:` comment.
- One engine handle is created lazily, and a mutex serialises calls, because the engine is not
  re-entrant.
- The tone app, or a small `image` test app, decodes a JPEG embedded in flash and logs the size
  and a few pixels. That gives a board check without the player.

### prism-mp3-player

The cover loader keeps its `CoverDecoder` seam, which its tests use. The default decoder becomes:

```cpp
prism::render::decode_image(bytes, {.fit = Size{side, side}, .format = kCoverFormat})
```

- `kCoverFormat` is `rgb565` on the board and `rgba8888` on the desktop.
- The board's `player_main` calls `prism::esp32::register_jpeg_decoder()`.
- Deleted: `core/shrink.*`, `boards/korvo1/main/jpeg_cover.*`, and their test. The shrink test
  cases move to PRISM's `fit_pixels` tests.
- The board's cover draw becomes a row copy per band.

## Tests (PRISM, doctest, headless)

- `fit_pixels`:
  - It averages a 4×4 into a 2×2 to exact values.
  - It keeps the aspect ratio: 400×200 into a 100 box gives 100×50.
  - It never enlarges.
  - The stride skips padding.
  - Transparent pixels don't bleed colour.
  - `rgb888` → `rgb565` packs like `Rgb565::pack`.
  - A bad view returns null.
- `scaled` and `converted` return the same pointer when nothing changes. A round trip of
  `rgba8888` → `rgb565` → `rgba8888` stays within the 565 rounding error.
- Registry:
  - The last registered decoder is tried first.
  - A failure falls through to the next decoder.
  - With no decoders, the error is "no image decoder".
  - `unregister` removes a decoder.
  - The `svg_size` overload still works.
- `paint_image`:
  - `<Rgb565>` gives an `rgb565` image of the drawn pixels.
  - `<Argb8888>` gives `rgba8888` bytes in R, G, B, A order.
  - 50% red drawn on transparent reads back as (255, 0, 0, 128).
- `Argb8888::over`:
  - On an opaque destination, the result is unchanged from today.
  - On a translucent destination, it is the exact straight-alpha result.
- Canvas:
  - An `rgb565` image at 1:1 in an `Rgb565` band is identical to the source rows, including a
    band that cuts the image.
  - A scaled or tinted `rgb565` image goes through the blend path.
- Pixel test: an `rgb565` image renders through the SDL backend the same as its `rgba8888`
  conversion.

## Left to the builder

- Names of private helpers.
- The internal shape of the averaging loop.
- Whether `view_of(image)` is public.
- Where exactly the `paint_image` swizzle lives.
