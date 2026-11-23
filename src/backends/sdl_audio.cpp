#include <prism/audio/backend.hpp>

#include "sdl_registered.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace prism::audio {
namespace {

// The device a player gets when it asks for none: SDL's own choice of playback device.
constexpr const char* kDefaultId = "sdl:default";
constexpr std::string_view kIdPrefix = "sdl:";

std::string device_id(SDL_AudioDeviceID device) { return std::string{kIdPrefix} + std::to_string(device); }

// "sdl:123" back to SDL's id; false for anything else, including "sdl:default".
bool parse_id(std::string_view id, SDL_AudioDeviceID& device)
{
    if (!id.starts_with(kIdPrefix)) return false;
    const auto digits = id.substr(kIdPrefix.size());
    if (digits.empty() || digits == "default") return false;
    unsigned int value = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) return false;
    device = static_cast<SDL_AudioDeviceID>(value);
    return true;
}

// SDL's audio thread asks for a block and the Output fills it. The bytes go straight to the stream:
// SDL owns the queueing and the conversion to the device's own format.
class SdlStream final : public DeviceStream {
public:
    SdlStream(const Format& format, PullFn pull)
        : format(format), pull(std::move(pull)),
          block(static_cast<std::size_t>(kMaxBlockFrames)
                * static_cast<std::size_t>(format.channels > 0 ? format.channels : 2))
    {
    }

    // SDL_DestroyAudioStream waits for the audio thread, so no pull can still be inside once this
    // returns: that is the handshake every DeviceStream owes its Output. The stream holds one
    // reference on SDL's audio subsystem, taken by open() before it was built.
    ~SdlStream() override
    {
        if (stream != nullptr) SDL_DestroyAudioStream(stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    [[nodiscard]] bool lost() const override { return gone.load(std::memory_order_relaxed); }

    // SDL calls this with the stream as userdata; public so the backend can hand it over.
    static void SDLCALL callback(void* userdata, SDL_AudioStream* stream, int additional, int total)
    {
        (void)total;
        static_cast<SdlStream*>(userdata)->fill(stream, additional);
    }

    // Owned from here on: the destructor is the only place that closes it.
    void attach(SDL_AudioStream* opened) { stream = opened; }

private:
    int channels() const { return format.channels > 0 ? format.channels : 2; }

    // A lost stream stops pulling by itself: SDL keeps asking, the block stays silence.
    // simplify: a device that goes away is noticed on the first put that fails; SDL's removal event
    // is bound to a window loop, and this backend has none.
    void fill(SDL_AudioStream* stream, int additional)
    {
        if (gone.load(std::memory_order_relaxed)) return;
        const auto frame_bytes = static_cast<int>(sizeof(float)) * channels();
        auto frames = additional / frame_bytes;
        while (frames > 0) {
            const auto chunk = std::min(frames, static_cast<int>(kMaxBlockFrames));
            const auto samples = static_cast<std::size_t>(chunk) * static_cast<std::size_t>(channels());
            pull(std::as_writable_bytes(std::span<float>{block.data(), samples}), chunk);
            if (!SDL_PutAudioStreamData(stream, block.data(), static_cast<int>(samples * sizeof(float)))) {
                gone.store(true, std::memory_order_relaxed);
                return;
            }
            frames -= chunk;
        }
    }

    Format format;
    PullFn pull;
    std::vector<float> block;
    std::atomic<bool> gone{false};
    SDL_AudioStream* stream = nullptr;
};

// One device aside from SDL's default: a stream opened by id, so a player can name what it wants.
class SdlBackend final : public DeviceBackend {
public:
    [[nodiscard]] std::vector<DeviceInfo> devices() override
    {
        std::vector<DeviceInfo> out;
        out.push_back({.id = kDefaultId,
                       .name = "Default",
                       .rates = {},
                       .max_channels = 2,
                       .native = SampleType::f32,
                       .is_default = true});
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return out;
        int count = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
        if (ids == nullptr) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return out;
        }
        for (int i = 0; i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            out.push_back({.id = device_id(ids[i]),
                           .name = name != nullptr ? name : "SDL device",
                           .rates = {},
                           .max_channels = 2,
                           .native = SampleType::f32,
                           .is_default = false});
        }
        SDL_free(ids);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return out;
    }

    [[nodiscard]] std::unique_ptr<DeviceStream> open(std::string_view id, const Format& format, PullFn pull,
                                                     std::string& error) override
    {
        SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
        if (id != kDefaultId && !parse_id(id, device)) {
            error = "no such device";
            return nullptr;
        }
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            error = SDL_GetError();
            return nullptr;
        }
        auto opened = std::make_unique<SdlStream>(format, std::move(pull));
        const SDL_AudioSpec spec{.format = SDL_AUDIO_F32,
                                 .channels = format.channels > 0 ? format.channels : 2,
                                 .freq = format.sample_rate};
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(device, &spec, &SdlStream::callback, opened.get());
        if (stream == nullptr) {
            error = SDL_GetError();
            return nullptr;
        }
        opened->attach(stream);
        // The stream starts paused; a pull can arrive from here on, before this function returns.
        if (!SDL_ResumeAudioStreamDevice(stream)) {
            error = SDL_GetError();
            return nullptr;   // `opened` destroys the stream
        }
        return opened;
    }
};

} // namespace

// One backend for the process, made on first use: audio/registry.cpp starts with it.
DeviceBackend& sdl_audio_backend()
{
    static SdlBackend backend;
    return backend;
}

} // namespace prism::audio
