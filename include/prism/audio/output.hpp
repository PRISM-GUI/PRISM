#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// Sound out: a playback device that calls a render function on its own thread. The render
// function fills one block of interleaved float samples; it runs on a real-time thread, so it
// must not block, allocate or touch a Field (hand it values through a core::Latest).
//
// When no device opens (no sound card, a headless machine, a browser that has not started audio
// yet) a timer thread calls the same function at the block rate instead: exactly one thread
// renders either way, so whatever reads the samples stays live. state() and error() say which.
// Devices come from the platform backends (prism/audio/backend.hpp): SDL on the desktop, the
// board's codec and Bluetooth stack in prism-esp32. None registered: the timer.
namespace prism::audio {

inline constexpr int kMaxBlockFrames = 2048;   // the largest block one call renders

struct Format {
    int sample_rate = 48000;
    int channels = 2;
    int block_frames = 512;   // what the timer renders per call; a device may ask for other sizes
};

// What the render function costs, over the last second of audio.
struct Load {
    float max_ms = 0.f;
    float mean_ms = 0.f;
    std::uint32_t calls = 0;
    float budget_ms = 0.f;   // how long one block takes to play
};

// The two sample types a device block holds: float, or 16-bit signed scaled to [-1, 1].
enum class SampleType { s16, f32 };

template <class T>
concept Sample = std::same_as<T, float> || std::same_as<T, std::int16_t>;

// `interleaved` holds frames * channels samples, frames <= kMaxBlockFrames.
template <Sample T>
using SampleRenderFn = std::function<void(std::span<T> interleaved)>;

using RenderFn = SampleRenderFn<float>;

// Copies min(in.size(), out.size()) samples, scaling by `gain` first. Full scale is 1.f and the
// s16 side clamps, so a gain over 1 saturates instead of wrapping.
void convert(std::span<const float> in, std::span<std::int16_t> out, float gain = 1.f);
void convert(std::span<const std::int16_t> in, std::span<float> out, float gain = 1.f);

// Scales samples in place; the s16 overload saturates at the ends of its range.
void apply_gain(std::span<float> samples, float gain);
void apply_gain(std::span<std::int16_t> samples, float gain);

// What an Output is doing: rendering on its device, rendering on the timer instead because no
// device opened, or lost because its device went away or never opened. Lost is sticky.
enum class State { playing, timer, lost };

class Output {
public:
    // Starts rendering at once, on a device or, failing that, on the timer.
    Output(Format format, RenderFn render);
    // The same, for a render function that fills s16 blocks (a decoded track): converted to the
    // device's own sample type.
    Output(Format format, SampleRenderFn<std::int16_t> render);
    // Plays on the device `id`: one that is missing, refuses `format` or fails to open leaves the
    // Output lost instead of falling back to the timer.
    Output(std::string_view id, Format format, RenderFn render);
    Output(std::string_view id, Format format, SampleRenderFn<std::int16_t> render);
    ~Output();   // stops the thread that renders; render is never called after it returns

    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    [[nodiscard]] State state() const;
    [[nodiscard]] bool playing() const;              // state() == State::playing
    [[nodiscard]] const std::string& error() const;  // why no device opened, or why it was lost
    [[nodiscard]] Format format() const;
    [[nodiscard]] Load load() const;

    // 0..1. The device applies it when it can (hardware volume); otherwise Output scales the
    // samples on their way out.
    void set_volume(float volume);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace prism::audio
