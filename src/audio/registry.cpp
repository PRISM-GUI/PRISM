#include <prism/audio/backend.hpp>

#if defined(PRISM_SDL_BACKENDS)
#include "../backends/sdl_registered.hpp"
#endif

#include <algorithm>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

namespace prism::audio {
namespace {

// The live backends, in registration order. A function-local static so that a backend registering
// from another static's constructor cannot run before it.
std::mutex& registry_mutex()
{
    static std::mutex mutex;
    return mutex;
}

// SDL's devices are there from the start when the software backend is built: named here, not
// registered from a static object, which a static link would drop (backends/sdl_registered.hpp).
std::vector<DeviceBackend*> initial_backends()
{
#if defined(PRISM_SDL_BACKENDS)
    return {&sdl_audio_backend()};
#else
    return {};
#endif
}

std::vector<DeviceBackend*>& registry()
{
    static std::vector<DeviceBackend*> backends = initial_backends();
    return backends;
}

bool lists(const std::vector<DeviceInfo>& devices, std::string_view id)
{
    return std::ranges::any_of(devices, [id](const DeviceInfo& device) { return device.id == id; });
}

} // namespace

bool DeviceInfo::accepts(const Format& format) const
{
    if (format.channels <= 0 || format.channels > max_channels) return false;
    return rates.empty() || std::ranges::find(rates, format.sample_rate) != rates.end();
}

void register_backend(DeviceBackend& backend)
{
    const std::lock_guard lock{registry_mutex()};
    if (std::ranges::find(registry(), &backend) == registry().end()) registry().push_back(&backend);
}

void unregister_backend(DeviceBackend& backend)
{
    const std::lock_guard lock{registry_mutex()};
    std::erase(registry(), &backend);
}

std::vector<DeviceInfo> devices()
{
    // The lock is held while a backend answers: it keeps an unregistered backend from being asked.
    const std::lock_guard lock{registry_mutex()};
    std::vector<DeviceInfo> all;
    for (auto* backend : registry()) {
        auto listed = backend->devices();
        std::ranges::move(listed, std::back_inserter(all));
    }
    // The default device first: that is the one a bare Output plays on. Two passes rather than
    // stable_sort, whose temporary buffer libstdc++ 14 frees with the wrong call (ASan's
    // alloc-dealloc-mismatch; fixed in GCC 15 by the PR 105258 rework).
    std::vector<DeviceInfo> ordered;
    ordered.reserve(all.size());
    for (const bool is_default : {true, false})
        for (auto& device : all)
            if (device.is_default == is_default) ordered.push_back(std::move(device));
    return ordered;
}

std::unique_ptr<DeviceStream> open_device(std::string_view id, const Format& format, PullFn pull,
                                          std::string& error)
{
    const std::lock_guard lock{registry_mutex()};
    for (auto* backend : registry()) {
        if (!lists(backend->devices(), id)) continue;
        return backend->open(id, format, std::move(pull), error);
    }
    error = "no such device";
    return nullptr;
}

} // namespace prism::audio
