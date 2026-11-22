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
    // How many presented frames old what back() holds is: 1 when it keeps the frame presented
    // last (a single buffer, or a copy), 2 when two buffers flip. The runtime then redraws only
    // what changed since then (render/damage.hpp). 0, the default, means its contents are
    // unknown: every frame is drawn whole.
    virtual int buffer_age() const { return 0; }
    virtual ~FrameSink() = default;
};

// A target that takes its frame in bands: a panel with its own frame memory fed over SPI, or a
// board that draws bands in fast RAM and copies them to a frame buffer. The runtime draws each
// frame as bands of rows into one of two band buffers, in the target's pixel format P
// (raster/pixel.hpp), and hands each band over as it is done (doc/design/band-rendering.md).
template <class P>
struct BandSink {
    virtual int band_height() const = 0;                         // rows per band
    virtual std::span<typename P::storage> band(int index) = 0;  // index 0 or 1; width * band_height
    // Starts copying `band(index)` (rows [first_row, first_row + rows)) to the frame and
    // returns. band(index) is not handed out again until that copy is done: the sink waits
    // in band() if it must. band(i) asked again before flush(i) returns the same buffer without
    // waiting (the runtime asks for band 0 to build its canvas, then again for the first band).
    virtual void flush(int index, int first_row, int rows) = 0;
    virtual void present() = 0;                                  // every band flushed: show it
    virtual core::Size size() const = 0;
    // As FrameSink::buffer_age, for the frame memory the bands are copied into: 1 or 2 when it
    // keeps what earlier frames drew there, so only the bands that changed are drawn and flushed.
    // 0, the default: every band of every frame.
    virtual int frame_age() const { return 0; }
    virtual ~BandSink() = default;
};

// The touch panel, polled: the finger's position in window/panel pixels, or nullopt
// when no finger is down.
struct TouchSource {
    virtual std::optional<core::Point> read() = 0;
    virtual ~TouchSource() = default;
};

} // namespace prism::platform
