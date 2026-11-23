#pragma once

#include <prism/audio/backend.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// A device a test drives by hand: it registers itself as backend "manual" with one device whose id
// is "manual", and pull_*() runs the attached Output's render function on the calling thread, so a
// test decides when a block is played instead of waiting on a device's own thread.
namespace prism::audio {

class ManualDevice {
public:
    // `native` is the sample type of the block this device hands over; `rates` empty means any
    // rate.
    explicit ManualDevice(SampleType native = SampleType::s16, std::vector<int> rates = {});
    ~ManualDevice();

    ManualDevice(const ManualDevice&) = delete;
    ManualDevice& operator=(const ManualDevice&) = delete;

    // Renders `frames` frames; the result is valid until the next pull. Silence when no Output is
    // attached, when the device is lost, or when the Output renders the other sample type.
    std::span<const std::int16_t> pull_s16(int frames);
    std::span<const float> pull_f32(int frames);

    // The open stream reports lost from now on: render stops and pulls return silence.
    void lose();

    [[nodiscard]] bool open() const;             // an Output is attached
    [[nodiscard]] float device_volume() const;   // what an Output's set_volume reached

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace prism::audio
