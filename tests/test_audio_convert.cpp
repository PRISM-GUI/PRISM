#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/audio/output.hpp>

#include <array>
#include <cstdint>

namespace {

namespace audio = prism::audio;

constexpr float kTolerance = 1e-6f;

} // namespace

TEST_CASE("float to s16 rounds to nearest")
{
    const std::array<float, 4> in{0.f, 0.5f, -0.5f, 1.f / 32767.f};
    std::array<std::int16_t, 4> out{};
    audio::convert(in, out);
    CHECK(out[0] == 0);
    CHECK(out[1] == 16384);
    CHECK(out[2] == -16384);
    CHECK(out[3] == 1);
}

TEST_CASE("float to s16 saturates")
{
    const std::array<float, 4> in{1.f, -1.f, 1.5f, -2.f};
    std::array<std::int16_t, 4> out{};
    audio::convert(in, out);
    CHECK(out[0] == 32767);
    CHECK(out[1] == -32767);
    CHECK(out[2] == 32767);
    CHECK(out[3] == -32768);
}

TEST_CASE("s16 to float")
{
    const std::array<std::int16_t, 3> in{0, 32767, -32768};
    std::array<float, 3> out{};
    audio::convert(in, out);
    CHECK(out[0] == 0.f);
    CHECK(out[1] == doctest::Approx(1.f).epsilon(kTolerance));
    CHECK(out[2] == doctest::Approx(-1.0000305f).epsilon(kTolerance));
}

TEST_CASE("gain applies before saturation")
{
    const std::array<float, 1> in{0.6f};
    std::array<std::int16_t, 1> out{};
    audio::convert(in, out, 2.f);
    CHECK(out[0] == 32767);

    std::array<std::int16_t, 1> loud{20000};
    audio::apply_gain(loud, 2.f);
    CHECK(loud[0] == 32767);

    std::array<std::int16_t, 1> quiet{-20000};
    audio::apply_gain(quiet, 2.f);
    CHECK(quiet[0] == -32768);
}

TEST_CASE("output sizes")
{
    const std::array<float, 4> in{0.25f, 0.5f, -0.25f, -0.5f};
    std::array<std::int16_t, 6> longer{7, 7, 7, 7, 7, 7};
    audio::convert(in, longer);
    CHECK(longer[0] == 8192);
    CHECK(longer[1] == 16384);
    CHECK(longer[2] == -8192);
    CHECK(longer[3] == -16384);
    CHECK(longer[4] == 7);   // past the input: untouched
    CHECK(longer[5] == 7);

    const std::array<float, 4> wide{1.f, 0.5f, 0.25f, 0.f};
    std::array<std::int16_t, 2> shorter{};
    audio::convert(wide, shorter);
    CHECK(shorter[0] == 32767);
    CHECK(shorter[1] == 16384);
}
