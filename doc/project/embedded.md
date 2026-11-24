# Embedded boards (STM32, ESP32)

PRISM runs on a microcontroller with a screen: the STM32 port and the ESP32-S3 Korvo-1
port are the two that ship. This page lists what changes on a board, and the three rules
that keep a firmware from failing in ways that are hard to see.

## Build options that matter on a board

These are in `meson_options.txt`. They exist because flash and RAM are small.

| Option | Default | What you save | What you lose |
| --- | --- | --- | --- |
| `-Dfmt_locale=false` | on | about 184 KiB on the Cortex-M builds: `{fmt}`'s `{:L}` path and libstdc++'s locale facets | `{:L}` formatting; `{fmt}` ignores the flag |
| `-Dline_breaking=simple` | `unicode` | about 100 KiB: libunibreak's UAX #14 tables | the finer Unicode line-break rules; spaces, hyphens and CJK ideographs still break |
| `-Dembedded_bold_face=false` | on | about 20 KiB: the bold embedded face | bold text is drawn with the regular face |

Turn on the ones that pay only if your text does not need them. A firmware with a
localized date format needs `fmt_locale`. A firmware of Latin text does not.

The three options are ordinary Meson options, so a board port sets them in its own
cross file or in the `meson setup` line of its component.

## Nothing may allocate before `main`

Static constructors run before `main`, on a heap that the SDK has not set up and a PSRAM
that is not yet mapped. A `std::vector`, a `std::string` or a `std::function` built in a
namespace-scope constructor therefore allocates into a heap that does not exist.

The rule:

- A static constructor may not allocate.
- It may not rely on a registration surviving: a static registrar is dropped by a static
  link. Boards register their devices and decoders by name from `main` / `app_main`.
- It may not touch hardware, a bus or a clock that the SDK has not configured.

`tests/test_no_static_allocation.cpp` holds PRISM to that rule: it replaces the global
`operator new` before `main`, records every allocation made by static constructors, and
fails the test if there is one. A board port that adds a static object runs the same
test, which is how the cost of a global stays visible.

## A backend registration needs a linked object file

`register_backend()` and `register_decoder()` do nothing if the object file that calls
them is dropped. A linker drops members of a static library that nothing references, and
a registrar mentions nothing but itself.

On a board, call the registration from `app_main`:

```cpp
void app_main() {
    prism::esp32::register_speaker();
    prism::esp32::register_jpeg_decoder();
    // ...
}
```

On the desktop, the SDL backend's sources live in `prism-software-backend`, the library
a program links when it makes an `Output` or decodes an image, and `sdl_registered.hpp`
holds the registrar as a single object the library owns. `prism-mp3-player` links that
library for its audio, so the registration cannot be dropped.

## Boot order on a board

![A board's boot order](../diagrams/board-boot.svg)

*What must be up before static constructors, then `main`, then the tasks.*

Read it top to bottom. The heap, the PSRAM and the drivers come before anything
allocates. Static constructors allocate nothing and touch no hardware. `app_main` (or
`main`) registers the board's own devices and decoders, then starts the tasks and
`frame_app`.

## Flash and RAM

A board build links the same `libprism` the desktop does, minus the optional parts the
options above switch off. What still costs flash:

- the embedded fonts, in `raster/fonts.cpp`, which carry only the faces you compiled in;
- the image formats your decoders support, since a decoder pulls in its library;
- the profiling tools, which a board build leaves off unless asked for.

Measure before guessing: `idf.py size-components` on ESP-IDF, `arm-none-eabi-size` on an
STM32 build.

## The two ports

- **ESP32-S3 Korvo-1** (`prism-esp32`): the panel and touch are PRISM's `BandSink` and
  `TouchSource`, the speaker is a PRISM audio device, and Bluetooth is the `prism_bt`
  component. The board's README has the build, the flash and the controls.
- **STM32**: the same split, with the panel driver and the audio codec in the board
  component. The board's README has the cross file and the flash command.

Both ports keep their device code out of PRISM itself: a board registers what it has, and
the rest of the framework reads the registries.
