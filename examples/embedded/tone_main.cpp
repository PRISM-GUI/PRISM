// The tone generator as an app: pick an output device, set a frequency, play or stop. A plain
// main() like any other PRISM app; the port supplies the devices, and a machine with none plays on
// the Output's timer.

#include <prism/app/run.hpp>
#include <prism/app/widget_tree.hpp>
#include <prism/audio/backend.hpp>
#include <prism/core/connection.hpp>
#include <prism/core/field.hpp>
#include <prism/core/latest.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr prism::audio::Format kFormat{.sample_rate = 48000, .channels = 2, .block_frames = 512};
constexpr double kTwoPi = 2.0 * std::numbers::pi;

struct ToneModel {
    prism::core::Field<prism::ui::Choice> device{{.placeholder = "system default"}};
    prism::core::Field<prism::ui::Slider<double>> frequency{
        {.value = 440.0, .min = 110.0, .max = 880.0, .step = 1.0, .label = "frequency", .unit = "Hz"}};
    prism::core::Field<bool> playing{true};

    void view(prism::app::WidgetTree::ViewBuilder &vb)
    {
        vb.vstack([&] {
            vb.widget(device);
            vb.gap(12.f);
            vb.widget(frequency);
            vb.gap(12.f);
            vb.widget(playing);
        });
    }
};

// The app around the model: an Output rendering a sine at whatever the slider shows. The controls
// publish into Latest, which the audio thread reads without a lock; switching device reopens the
// Output, which stops the old one's thread first.
class Tone {
public:
    Tone();

    ToneModel model;

private:
    void open_output(std::string_view id);
    void render(std::span<float> out);

    std::vector<std::string> ids_;  // the devices the choice lists, by index
    prism::core::Latest<float> frequency_{440.f};
    prism::core::Latest<bool> playing_{true};
    double phase_ = 0.0;
    std::unique_ptr<prism::audio::Output> output_;
    prism::core::Connection on_frequency = model.frequency.on_change().connect(
        [this](const prism::ui::Slider<double> &slider) { frequency_.publish(float(slider.value)); });
    prism::core::Connection on_playing = model.playing.on_change().connect(
        [this](bool playing) { playing_.publish(playing); });
    prism::core::Connection on_device = model.device.on_change().connect(
        [this](const prism::ui::Choice &choice) {
            if (choice.selected) open_output(ids_[*choice.selected]);
        });
};

Tone::Tone()
{
    std::vector<std::string> names;
    for (const auto &device : prism::audio::devices()) {
        ids_.push_back(device.id);
        names.push_back(device.name);
    }
    model.device.mutate([&names](prism::ui::Choice &choice) {
        choice.options = std::move(names);
        if (!choice.options.empty()) choice.selected = 0;  // opens in the observer above
    });
    if (!output_) open_output({});  // no devices listed: the Output plays on its timer
}

void Tone::open_output(std::string_view id)
{
    output_.reset();  // stops the old device's thread before the new one starts
    if (id.empty())
        output_ = std::make_unique<prism::audio::Output>(
            kFormat, [this](std::span<float> out) { render(out); });
    else
        output_ = std::make_unique<prism::audio::Output>(
            id, kFormat, [this](std::span<float> out) { render(out); });
}

void Tone::render(std::span<float> out)
{
    const double step = kTwoPi * double(frequency_.read()) / double(kFormat.sample_rate);
    const bool playing = playing_.read();
    const auto channels = std::size_t(std::max(kFormat.channels, 1));
    constexpr float kAmplitude = 0.2f;  // a test tone, not a signal generator
    for (std::size_t i = 0; i + channels <= out.size(); i += channels) {
        const auto sample = kAmplitude * float(std::sin(phase_));
        for (std::size_t c = 0; c < channels; ++c) out[i + c] = sample;
        if (!playing) continue;
        phase_ += step;
        if (phase_ >= kTwoPi) phase_ -= kTwoPi;
    }
}

} // namespace

int main()
{
    Tone tone;
    return prism::app::run(tone.model, {.title = "Tone"});
}
