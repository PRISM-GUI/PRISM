#pragma once

// What a framebuffer target gives the frame runtime (platform/frame_app.hpp): pixels to draw
// into and a touch panel to poll. A port implements a sink and a touch source (the STM32 board:
// the LTDC back buffer and its touch controller).

#include <prism/core/types.hpp>
#include <prism/raster/pixel.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace prism::platform {

// What the render thread draws into and presents.
struct FrameSink {
    virtual std::span<std::uint32_t> back() = 0; // ARGB8888 pixels, stride = width
    // Fills back() with `argb` (0xAARRGGBB) before a frame is drawn, and returns true; false (the
    // default) leaves the clear to the CPU. A board with a 2D engine clears faster than the CPU.
    virtual bool clear(std::uint32_t argb) { (void)argb; return false; }
    virtual void present() = 0;                  // flip to what was drawn into back()
    virtual core::Size size() const = 0;         // in pixels: back() holds width x height
    virtual ~FrameSink() = default;
};

// A target with no whole frame in RAM (a panel with its own frame memory, fed over SPI): the
// runtime draws each frame as bands of rows into one of two band buffers, in the panel's pixel
// format P (raster/pixel.hpp), and hands each band over as it is done
// (doc/design/band-rendering.md).
template <class P>
struct BandSink {
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

// The touch panel, polled: the finger's position in window/panel pixels, or nullopt
// when no finger is down.
struct TouchSource {
    virtual std::optional<core::Point> read() = 0;
    virtual ~TouchSource() = default;
};

} // namespace prism::platform
