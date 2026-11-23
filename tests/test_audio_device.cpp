// audio::Output over a device backend (include/prism/audio/backend.hpp), driven by
// audio::ManualDevice: what a render function writes, conversions between sample types, chunking
// of a pull larger than kMaxBlockFrames, loss, device volume, and no allocation while pulling.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/audio/manual_device.hpp>
#include <prism/audio/output.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

using namespace prism;
using namespace std::chrono_literals;

namespace {
// Counts allocations made on the thread that pulls, once it has pulled once.
thread_local bool counting = false;
std::atomic<int> pull_allocations{0};
}

// new is malloc and delete is free on purpose, as in tests/test_audio_output.cpp.
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void* operator new(std::size_t n) {
    if (counting) pull_allocations.fetch_add(1);
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

const audio::Format kFormat{.sample_rate = 48000, .channels = 2};

bool lists(const std::vector<audio::DeviceInfo>& devices, std::string_view id)
{
    return std::ranges::any_of(devices, [id](const audio::DeviceInfo& device) { return device.id == id; });
}

bool all_are(std::span<const std::int16_t> block, std::int16_t value)
{
    return std::ranges::all_of(block, [value](std::int16_t sample) { return sample == value; });
}

} // namespace

TEST_CASE("devices lists a ManualDevice while it lives")
{
    CHECK_FALSE(lists(audio::devices(), "manual"));
    {
        audio::ManualDevice device{};
        CHECK(lists(audio::devices(), "manual"));
        CHECK_FALSE(device.open());
    }
    CHECK_FALSE(lists(audio::devices(), "manual"));
}

TEST_CASE("an s16 render on an s16 device passes straight through")
{
    audio::ManualDevice device{audio::SampleType::s16};
    audio::Output out{"manual", kFormat, [](std::span<std::int16_t> block) {
                          const std::int16_t values[]{1, 2, 3, 4};
                          std::ranges::copy(values, block.begin());
                      }};
    REQUIRE(out.state() == audio::State::playing);
    const auto block = device.pull_s16(2);
    REQUIRE(block.size() == 4);
    CHECK(block[0] == 1);
    CHECK(block[1] == 2);
    CHECK(block[2] == 3);
    CHECK(block[3] == 4);
}

TEST_CASE("an f32 render on an s16 device is converted")
{
    audio::ManualDevice device{audio::SampleType::s16};
    audio::Output out{"manual", kFormat, [](std::span<float> block) { std::ranges::fill(block, 0.5f); }};
    const auto block = device.pull_s16(2);
    REQUIRE(block.size() == 4);
    CHECK(all_are(block, 16384));
}

TEST_CASE("a pull larger than kMaxBlockFrames renders in chunks")
{
    audio::ManualDevice device{audio::SampleType::s16};
    std::atomic<int> calls{0};
    std::atomic<std::size_t> biggest{0};
    audio::Output out{"manual", kFormat, [&calls, &biggest](std::span<float> block) {
                          biggest = std::max(biggest.load(), block.size());
                          std::ranges::fill(block, 0.5f);
                          calls.fetch_add(1);
                      }};
    const auto block = device.pull_s16(4096);   // twice kMaxBlockFrames
    CHECK(block.size() == 8192);
    CHECK(calls.load() >= 2);
    CHECK(biggest.load() <= static_cast<std::size_t>(audio::kMaxBlockFrames * kFormat.channels));
    CHECK(all_are(block, 16384));
}

TEST_CASE("lose stops render and the state says lost")
{
    audio::ManualDevice device{};
    std::atomic<int> calls{0};
    audio::Output out{"manual", kFormat, [&calls](std::span<std::int16_t> block) {
                          std::ranges::fill(block, std::int16_t{7});
                          calls.fetch_add(1);
                      }};
    device.pull_s16(16);
    REQUIRE(calls.load() == 1);

    device.lose();
    CHECK(out.state() == audio::State::lost);
    const auto block = device.pull_s16(16);
    CHECK(calls.load() == 1);   // render never runs again
    CHECK(all_are(block, 0));
}

TEST_CASE("a named device that does not exist is lost")
{
    std::atomic<int> calls{0};
    audio::Output out{"nope", kFormat, [&calls](std::span<std::int16_t>) { calls.fetch_add(1); }};
    CHECK(out.state() == audio::State::lost);
    CHECK_FALSE(out.error().empty());
    CHECK(calls.load() == 0);
}

TEST_CASE("a named device that refuses the rate is lost")
{
    audio::ManualDevice device{audio::SampleType::s16, {44100}};
    std::atomic<int> calls{0};
    audio::Output out{"manual", kFormat, [&calls](std::span<std::int16_t>) { calls.fetch_add(1); }};
    CHECK(out.state() == audio::State::lost);
    CHECK(calls.load() == 0);
    CHECK_FALSE(device.open());
}

TEST_CASE("the default constructor uses the timer when nothing is listed")
{
    std::atomic<int> calls{0};
    audio::Output out{kFormat, [&calls](std::span<float> block) {
                          std::ranges::fill(block, 0.f);
                          calls.fetch_add(1);
                      }};
    CHECK(out.state() == audio::State::timer);
    for (auto waited = 0ms; calls.load() < 2 && waited < 1000ms; waited += 5ms)
        std::this_thread::sleep_for(5ms);
    CHECK(calls.load() >= 2);
}

TEST_CASE("volume reaches the device")
{
    audio::ManualDevice device{};
    audio::Output out{"manual", kFormat, [](std::span<std::int16_t> block) { std::ranges::fill(block, 0); }};
    out.set_volume(0.25f);
    CHECK(device.device_volume() == doctest::Approx(0.25f));
}

TEST_CASE("render is never called after the Output is destroyed")
{
    audio::ManualDevice device{};
    std::atomic<int> calls{0};
    {
        audio::Output out{"manual", kFormat, [&calls](std::span<std::int16_t> block) {
                              std::ranges::fill(block, std::int16_t{0});
                              calls.fetch_add(1);
                          }};
        device.pull_s16(8);
    }
    CHECK(calls.load() == 1);
    const auto block = device.pull_s16(8);
    CHECK(calls.load() == 1);
    CHECK(all_are(block, 0));
}

TEST_CASE("nothing allocates on the rendering thread between blocks, s16 and f32")
{
    {
        audio::ManualDevice device{audio::SampleType::s16};
        std::atomic<int> calls{0};
        audio::Output out{"manual", kFormat, [&calls](std::span<std::int16_t> block) {
                              counting = true;
                              std::ranges::fill(block, std::int16_t{1});
                              calls.fetch_add(1);
                          }};
        REQUIRE_FALSE(device.pull_s16(256).empty());
        pull_allocations = 0;
        for (int i = 0; i < 20; ++i) device.pull_s16(256);
        CHECK(calls.load() >= 20);
        CHECK(pull_allocations.load() == 0);
        counting = false;
    }
    {
        audio::ManualDevice device{audio::SampleType::f32};
        std::atomic<int> calls{0};
        audio::Output out{"manual", kFormat, [&calls](std::span<float> block) {
                              counting = true;
                              std::ranges::fill(block, 0.f);
                              calls.fetch_add(1);
                          }};
        REQUIRE_FALSE(device.pull_f32(256).empty());
        pull_allocations = 0;
        for (int i = 0; i < 20; ++i) device.pull_f32(256);
        CHECK(calls.load() >= 20);
        CHECK(pull_allocations.load() == 0);
        counting = false;
    }
}
