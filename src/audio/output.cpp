#include <prism/audio/output.hpp>

#include <prism/audio/backend.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace prism::audio {

namespace {
using clock = std::chrono::steady_clock;

// Sleeps in short slices so a stop request is noticed at once; true when one came.
bool sleep_until(const std::stop_token& stop, clock::time_point deadline)
{
    constexpr auto slice = std::chrono::milliseconds{5};
    while (clock::now() < deadline) {
        if (stop.stop_requested()) return true;
        std::this_thread::sleep_for(slice);
    }
    return stop.stop_requested();
}

float budget_ms(const Format& f)
{
    return f.sample_rate <= 0 ? 0.f
                              : 1000.f * static_cast<float>(f.block_frames) / static_cast<float>(f.sample_rate);
}

std::size_t block_samples(const Format& f)
{
    return static_cast<std::size_t>(kMaxBlockFrames) * static_cast<std::size_t>(std::max(1, f.channels));
}

std::size_t bytes_per_sample(SampleType type)
{
    return type == SampleType::s16 ? sizeof(std::int16_t) : sizeof(float);
}

// Written by the one thread that renders, read by anyone: the atomics are the published load.
class LoadMeter {
public:
    explicit LoadMeter(int sample_rate) : sample_rate_(sample_rate) {}

    // A one-second window of audio, not of wall time: no extra clock read per block, and the
    // numbers still mean "what a second of audio cost".
    void record(float milliseconds, int frames)
    {
        window_max_ms_ = std::max(window_max_ms_, milliseconds);
        window_sum_ms_ += milliseconds;
        window_calls_ += 1;
        window_frames_ += frames;
        max_ms_.store(window_max_ms_, std::memory_order_relaxed);
        mean_ms_.store(window_sum_ms_ / static_cast<float>(window_calls_), std::memory_order_relaxed);
        calls_.store(window_calls_, std::memory_order_relaxed);
        block_frames_.store(frames, std::memory_order_relaxed);
        if (window_frames_ >= sample_rate_) {
            window_max_ms_ = window_sum_ms_ = 0.f;
            window_calls_ = 0;
            window_frames_ = 0;
        }
    }

    // The budget is the block the device last asked for: a device pulls its own size (the Korvo's
    // speaker 1024 frames), not the Format's block_frames, which only the timer uses.
    Load read(float budget) const
    {
        if (const auto frames = block_frames_.load(std::memory_order_relaxed); frames > 0)
            budget = 1000.f * static_cast<float>(frames) / static_cast<float>(sample_rate_);
        return {.max_ms = max_ms_.load(std::memory_order_relaxed),
                .mean_ms = mean_ms_.load(std::memory_order_relaxed),
                .calls = calls_.load(std::memory_order_relaxed),
                .budget_ms = budget};
    }

private:
    int sample_rate_;
    std::atomic<float> max_ms_{0.f};
    std::atomic<float> mean_ms_{0.f};
    std::atomic<std::uint32_t> calls_{0};
    std::atomic<int> block_frames_{0};
    float window_max_ms_ = 0.f;
    float window_sum_ms_ = 0.f;
    int window_frames_ = 0;
    std::uint32_t window_calls_ = 0;
};
} // namespace

struct Output::Impl {
    Impl(Format f, std::variant<RenderFn, SampleRenderFn<std::int16_t>> r)
        : format(f), render(std::move(r)), meter(f.sample_rate), scratch(block_samples(f)),
          scratch_s16(block_samples(f))
    {
    }

    // A stream must not die under a pull, so it goes first; the timer goes after.
    ~Impl()
    {
        stream.reset();
        timer = std::jthread{};
    }

    int channels() const { return format.channels > 0 ? format.channels : 2; }

    // The default device when there is one: an Output with no id takes what the platform offers
    // and falls back to the timer, which is what the desktop did before devices existed.
    void start()
    {
        const auto all = devices();
        const auto found = std::ranges::find_if(
            all, [this](const DeviceInfo& info) { return info.is_default && info.accepts(format); });
        if (found != all.end() && open(*found)) return;
        error = "no audio device";
        start_timer();
    }

    // A named device that is missing or refuses the format is lost, never a quiet fallback to the
    // timer: the caller asked for that device by name.
    void start(std::string_view id)
    {
        const auto all = devices();
        const auto found = std::ranges::find_if(all, [id](const DeviceInfo& info) { return info.id == id; });
        if (found == all.end()) {
            error = "no such device";
            state_.store(State::lost);
            return;
        }
        if (!found->accepts(format)) {
            error = "the device cannot play this format";
            state_.store(State::lost);
            return;
        }
        if (!open(*found)) state_.store(State::lost);
    }

    bool open(const DeviceInfo& info)
    {
        native = info.native;
        stream = open_device(
            info.id, format, [this](std::span<std::byte> block, int frames) { pull_block(block, frames); }, error);
        if (!stream) return false;
        state_.store(State::playing);
        return true;
    }

    State state() const
    {
        if (stream && stream->lost()) return State::lost;
        return state_.load();
    }

    // The device asks for whatever block it needs; the render function only ever sees its own
    // block size, and the meter counts the chunks it actually costs.
    // Never touches `stream`: the device may call this before open() has stored it. A lost device
    // stops pulling (a DeviceStream's contract); state() asks the stream itself.
    void pull_block(std::span<std::byte> block, int frames)
    {
        const auto stride = static_cast<std::size_t>(channels()) * bytes_per_sample(native);
        auto done = 0;
        while (done < frames) {
            const auto chunk = std::min(frames - done, kMaxBlockFrames);
            render_chunk(block.subspan(static_cast<std::size_t>(done) * stride,
                                       static_cast<std::size_t>(chunk) * stride),
                         chunk);
            done += chunk;
        }
    }

    void render_chunk(std::span<std::byte> out, int frames)
    {
        const auto began = clock::now();
        const auto samples = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels());
        const auto gain = gain_.load();
        auto as_s16 = [&out](std::size_t count) {
            return std::span<std::int16_t>{reinterpret_cast<std::int16_t*>(out.data()), count};
        };
        auto as_f32 = [&out](std::size_t count) {
            return std::span<float>{reinterpret_cast<float*>(out.data()), count};
        };
        if (native == SampleType::s16) {
            if (const auto* floats = std::get_if<RenderFn>(&render)) {
                (*floats)(std::span<float>{scratch.data(), samples});
                convert(std::span<const float>{scratch.data(), samples}, as_s16(samples), gain);
            } else {
                const auto block = as_s16(samples);
                std::get<SampleRenderFn<std::int16_t>>(render)(block);
                if (gain != 1.f) apply_gain(block, gain);
            }
        } else {
            if (const auto* floats = std::get_if<RenderFn>(&render)) {
                const auto block = as_f32(samples);
                (*floats)(block);
                if (gain != 1.f) apply_gain(block, gain);
            } else {
                std::get<SampleRenderFn<std::int16_t>>(render)(std::span<std::int16_t>{scratch_s16.data(), samples});
                convert(std::span<const std::int16_t>{scratch_s16.data(), samples}, as_f32(samples), gain);
            }
        }
        meter.record(std::chrono::duration<float, std::milli>(clock::now() - began).count(), frames);
    }

    void start_timer()
    {
        try {
            timer = std::jthread([this](std::stop_token stop) { run_timer(stop); });
        } catch (const std::exception&) {
            // No thread left to render on: the app still runs, it just renders nothing.
        }
    }

    void run_timer(const std::stop_token& stop)
    {
        const auto block = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<float, std::milli>(budget_ms(format)));
        auto deadline = clock::now();
        while (!stop.stop_requested()) {
            render_block(std::min(format.block_frames, kMaxBlockFrames));
            deadline = std::max(deadline + block, clock::now());   // a slow block: no catching up
            if (sleep_until(stop, deadline)) return;
        }
    }

    // The timer has no device to convert for, so it renders into its own scratch buffer.
    void render_block(int frames)
    {
        const auto began = clock::now();
        const auto samples = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels());
        if (const auto* floats = std::get_if<RenderFn>(&render)) {
            (*floats)(std::span<float>{scratch.data(), samples});
        } else {
            std::get<SampleRenderFn<std::int16_t>>(render)(std::span<std::int16_t>{scratch_s16.data(), samples});
        }
        meter.record(std::chrono::duration<float, std::milli>(clock::now() - began).count(), frames);
    }

    Format format;
    std::variant<RenderFn, SampleRenderFn<std::int16_t>> render;
    SampleType native = SampleType::f32;
    LoadMeter meter;
    std::vector<float> scratch;
    std::vector<std::int16_t> scratch_s16;
    std::string error;
    std::atomic<State> state_{State::timer};
    std::atomic<float> gain_{1.f};
    std::unique_ptr<DeviceStream> stream;
    std::jthread timer;
};

Output::Output(Format format, RenderFn render) : impl_(std::make_unique<Impl>(format, std::move(render)))
{
    impl_->start();
}

Output::Output(Format format, SampleRenderFn<std::int16_t> render)
    : impl_(std::make_unique<Impl>(format, std::move(render)))
{
    impl_->start();
}

Output::Output(std::string_view id, Format format, RenderFn render)
    : impl_(std::make_unique<Impl>(format, std::move(render)))
{
    impl_->start(id);
}

Output::Output(std::string_view id, Format format, SampleRenderFn<std::int16_t> render)
    : impl_(std::make_unique<Impl>(format, std::move(render)))
{
    impl_->start(id);
}

Output::~Output() = default;

State Output::state() const { return impl_->state(); }

bool Output::playing() const { return state() == State::playing; }

const std::string& Output::error() const { return impl_->error; }

Format Output::format() const { return impl_->format; }

Load Output::load() const { return impl_->meter.read(budget_ms(impl_->format)); }

void Output::set_volume(float volume)
{
    const auto wanted = std::clamp(volume, 0.f, 1.f);
    // The device takes the volume when it can; otherwise the samples are scaled on their way out.
    impl_->gain_.store(impl_->stream && impl_->stream->set_volume(wanted) ? 1.f : wanted);
}

} // namespace prism::audio
