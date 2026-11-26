#pragma once

// Sound in, the mirror of Output: an Input opens a capture device (a microphone on the desktop, an
// ADC or a digital microphone array on a board) and hands the samples over as floats, whatever the
// device's native format. A port registers a CaptureBackend for its hardware
// (audio/backend.hpp); an app builds an Input and reads what arrived.
//
// The device thread only writes into a lock-free ring: read() is called from the app's logic
// thread and never blocks the device, and on_samples() wakes that thread (a post) when a block
// arrived, so a silent device costs nothing per loop.

#include <prism/audio/backend.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace prism::audio {

class Input {
public:
    // The first default capture device, or `device_id` when named. on_samples runs once per block,
    // on the device's thread, so it must not block: an app posts its tick from it.
    explicit Input(Format format = {.sample_rate = 16000, .channels = 1, .block_frames = 256},
                   std::string_view device_id = {}, std::function<void()> on_samples = {});
    ~Input();

    Input(const Input &) = delete;
    Input &operator=(const Input &) = delete;

    [[nodiscard]] bool ok() const;  // a device opened
    [[nodiscard]] std::string_view name() const;
    [[nodiscard]] float sample_rate() const;

    // Whatever is buffered, up to out.size(); 0 when nothing arrived since the last read.
    std::size_t read(std::span<float> out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace prism::audio
