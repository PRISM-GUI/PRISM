#pragma once

// The audio scope: a SampleSource (the machine's sound in through prism::audio::Input, or a
// generated sweep when it has no capture device) shown as a waveform plot (the last 20 ms) and a
// spectrum plot (the mean removed, a logarithmic frequency axis up to half the sample rate), with
// freeze, gain, dB/linear and fit controls. The title reports the samples received each second and
// their peak level, so a silent or stalled source shows. Everything goes through PRISM
// (plot::PlotModel on vb.canvas), so what is slow on the board is PRISM's cost.
//
// A model (ScopeModel), the state beside it (ScopeState), and the app around them (Scope). Scope
// owns its source and posts one tick per block onto the logic thread, coalesced, so a burst costs
// one repaint and no frame hook is needed: an app is app::run + attach (scope_main.cpp).

#include <embedded/sample_source.hpp>
#include <embedded/spectrum.hpp>
#include <embedded/synthetic_source.hpp>

#include <prism/app/widget_tree.hpp>
#include <prism/audio/input.hpp>
#include <prism/core/field.hpp>
#include <prism/dev/spsc_ring.hpp>
#include <prism/widgets/plot.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace prism::app {
class AppContext;
} // namespace prism::app

namespace prism_examples {

struct ScopeModel {
    prism::core::Field<prism::ui::Label<>> title{{.value = "PRISM scope", .heading = true}};
    prism::core::Field<prism::ui::Checkbox> frozen{{.checked = false, .label = "freeze"}};
    prism::core::Field<prism::ui::Checkbox> db_scale{{.checked = true, .label = "dB"}};
    prism::core::Field<prism::ui::Slider<>> gain{
        {.value = 1.0, .min = 0.5, .max = 8.0, .step = 0.5, .label = "gain"}};
    prism::core::Field<prism::ui::Button> fit{{.text = "fit"}}; // both plots to the signal now, zoom and pan reset
    prism::plot::PlotModel waveform, spectrum;
    prism::core::Field<prism::ui::Label<>> stats{{.value = "measuring…", .wrap = true}};
    // Not in this app's view: the timing line is the port's (its console), so the scope's own level
    // readout is the title. scope_show_stats() fills this label for a caller that has a line.

    void view(prism::app::WidgetTree::ViewBuilder &vb);
};

namespace scope_detail {

// The source the scope reads when the machine records: audio::Input, under its own name.
class InputSource final : public SampleSource {
public:
    explicit InputSource(prism::audio::Input &input) : input_(input), name_(input.name()) {}

    std::size_t read(std::span<float> out) override { return input_.read(out); }
    float sample_rate() const override { return input_.sample_rate(); }
    const char *name() const override { return name_.c_str(); }

private:
    prism::audio::Input &input_;
    std::string name_;
};

// The source it reads otherwise: the generated sweep, pushed block by block by the thread Scope
// runs for it, so both sources reach the scope the same way.
class SweepSource final : public SampleSource {
public:
    explicit SweepSource(float rate) : rate_(rate) {}

    std::size_t read(std::span<float> out) override { return ring_.pop(out); }
    float sample_rate() const override { return rate_; }
    const char *name() const override { return "synthetic"; }
    void push(std::span<const float> samples) { ring_.push(samples); }

private:
    float rate_;
    prism::dev::SpscRing<float, 4096> ring_;
};

} // namespace scope_detail

// What the scope holds besides its model: the sample window, the FFT, the plotted buffers and
// the title's level readout. The model's plots draw from this state's buffers (scope_bind), so
// it must outlive the model's use.
class ScopeState {
public:
    explicit ScopeState(SampleSource &source) : source_(source) {}

    std::span<const float> waveform() const { return waveform_; }  // what the waveform plot shows
    std::span<const float> spectrum() const { return spectrum_; }  // what the spectrum plot shows

    static constexpr std::size_t kFftSize = 1024;
    static constexpr std::size_t kWaveformSamples = 320; // 20 ms at 16 kHz

private:
    friend class Scope;
    friend void scope_bind(ScopeModel &, ScopeState &);
    friend void scope_tick(ScopeModel &, ScopeState &);
    friend void scope_show_stats(ScopeModel &, ScopeState &, std::string_view);

    bool drain_source(bool frozen); // true when new samples entered the window
    void update_plots(ScopeModel &model);
    void fit_ranges(ScopeModel &model);
    prism::plot::AxisRange spectrum_x_range() const; // the log10 of the first bin .. Nyquist
    void show_level(ScopeModel &model);

    SampleSource &source_;
    Spectrum fft_{kFftSize};
    std::vector<float> window_ = std::vector<float>(kFftSize, 0.f); // the last kFftSize samples
    std::vector<float> scratch_ = std::vector<float>(kFftSize);
    std::vector<float> gained_ = std::vector<float>(kFftSize);
    std::vector<float> centered_ = std::vector<float>(kFftSize); // gained_ minus its mean: what the FFT sees
    std::vector<float> waveform_ = std::vector<float>(kWaveformSamples, 0.f);
    std::vector<float> spectrum_ = std::vector<float>(kFftSize / 2, Spectrum::kFloorDb);
    double shown_gain_ = 0.0; // the settings the plots were last computed with
    bool shown_db_ = false;
    std::uint64_t fit_clicks_ = 0;
    std::size_t samples_since_report_ = 0; // for the title's level readout
    float peak_since_report_ = 0.f;
    double sum_since_report_ = 0.0;
    std::array<char, 96> title_text_{};
};

// The scope as an app: its model, the state beside it, and the source it reads (the machine's
// microphone through audio::Input, the sweep when that did not open). attach() binds the plots and
// starts feeding them; nothing else is needed, and nothing about a board is here.
class Scope {
public:
    Scope();
    ~Scope();

    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

    // Binds the plots and starts feeding them: the input's callback posts a tick per block, and
    // the sweep gets a thread pushing a block every block period into the same path.
    void attach(prism::app::AppContext &ctx);

private:
    void samples_arrived(); // from whichever thread produced the samples
    void tick();            // on the logic thread
    void run_sweep(std::stop_token stop);

    // Before the state below, which reads from the source picked here.
    std::atomic<bool> started_{false};
    std::atomic<bool> pending_{false};
    std::function<void(std::function<void()>)> poster_;
    prism::audio::Input input_;
    SyntheticSource sweep_{16000.f};
    scope_detail::InputSource input_source_;
    scope_detail::SweepSource sweep_source_;
    std::jthread sweep_thread_;
    std::chrono::steady_clock::time_point last_report_{};

public:
    ScopeModel model;
    ScopeState state;
};

// Points the model's plots at the state's buffers and titles it after the source. Once, before
// the first tick.
void scope_bind(ScopeModel &model, ScopeState &state);
// Each logic loop: drains the source, recomputes the plots when the samples or settings changed,
// fits the ranges when "fit" was clicked.
void scope_tick(ScopeModel &model, ScopeState &state);
// The level readout into the title, and `line` (an outside caller's timing line) into the stats
// label. Scope reports its own level once a second from tick(); this is for a caller that has a
// timing line of its own to show.
void scope_show_stats(ScopeModel &model, ScopeState &state, std::string_view line);

} // namespace prism_examples
