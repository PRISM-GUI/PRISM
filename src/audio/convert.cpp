#include <prism/audio/output.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace prism::audio {
namespace {

constexpr float kFullScale = 32767.f;
constexpr long kMinS16 = -32768;
constexpr long kMaxS16 = 32767;

// Rounds to nearest and clamps (not wraps): a gain over 1 or a sample past full scale has to stop
// at the ends of the range, or a loud passage turns into noise. Float, not double: the ESP32s have
// a single-precision FPU only, and this runs per sample on the audio thread.
std::int16_t clamp_to_s16(float value) {
    return static_cast<std::int16_t>(std::clamp(std::lround(value), kMinS16, kMaxS16));
}

} // namespace

void convert(std::span<const float> in, std::span<std::int16_t> out, float gain)
{
    const auto count = std::min(in.size(), out.size());
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = clamp_to_s16(in[i] * gain * kFullScale);
    }
}

void convert(std::span<const std::int16_t> in, std::span<float> out, float gain)
{
    const auto count = std::min(in.size(), out.size());
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<float>(in[i]) / kFullScale * gain;
    }
}

void apply_gain(std::span<float> samples, float gain)
{
    for (auto& sample : samples) sample *= gain;
}

void apply_gain(std::span<std::int16_t> samples, float gain)
{
    for (auto& sample : samples) sample = clamp_to_s16(static_cast<float>(sample) * gain);
}

} // namespace prism::audio
