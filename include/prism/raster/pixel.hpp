#pragma once

// Pixel formats the rasterizer draws into. A format is a type: `storage` is one pixel in memory,
// `pack` turns a colour into it, `over` blends a straight-alpha colour onto it (src-over).
// BasicCanvas<P> takes one of these (doc/design/band-rendering.md, section 2).

#include <prism/core/types.hpp>

#include <cstdint>

namespace prism::raster {

using prism::core::Color;

// A colour as an ARGB8888 framebuffer stores it: 0xAARRGGBB.
constexpr std::uint32_t argb(Color c)
{
    return (std::uint32_t(c.a) << 24) | (std::uint32_t(c.r) << 16) | (std::uint32_t(c.g) << 8) | c.b;
}

namespace pixel_detail {

struct Rgb8 { std::uint32_t r, g, b; };

// src-over of one 8-bit channel, rounded.
constexpr std::uint32_t blend(std::uint32_t s, std::uint32_t d, std::uint32_t a) {
    return (s * a + d * (255u - a) + 127u) / 255u;
}

constexpr Rgb8 blend(Color src, Rgb8 dst) {
    return {blend(src.r, dst.r, src.a), blend(src.g, dst.g, src.a), blend(src.b, dst.b, src.a)};
}

} // namespace pixel_detail

// Each pixel is 0xAARRGGBB (on the little-endian targets built here the bytes in memory are
// R, G, B, A).
struct Argb8888 {
    using storage = std::uint32_t;

    static constexpr storage pack(Color c) { return argb(c); }

    // Straight alpha. The framebuffer's alpha stays 255 (the caller clears it opaque), so the
    // plain channel lerp is exact; a translucent destination would need the /out_a step.
    static constexpr storage over(storage dst, Color src) {
        if (src.a == 0) return dst;
        if (src.a == 255) return pack(src);
        const auto c = pixel_detail::blend(src, {(dst >> 16) & 0xFFu, (dst >> 8) & 0xFFu, dst & 0xFFu});
        const std::uint32_t oa = src.a + ((dst >> 24) * (255u - src.a) + 127u) / 255u;
        return (oa << 24) | (c.r << 16) | (c.g << 8) | c.b;
    }
};

// 16 bits, no alpha: rrrrrggg gggbbbbb. Blending unpacks to 8 bits (high bits replicated into
// the low ones, so 31 -> 255), blends as Argb8888 does, and packs again.
struct Rgb565 {
    using storage = std::uint16_t;

    static constexpr storage pack(Color c) {
        return static_cast<storage>(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
    }

    static constexpr storage over(storage dst, Color src) {
        if (src.a == 0) return dst;
        if (src.a == 255) return pack(src);
        const auto c = pixel_detail::blend(src, unpack(dst));
        return pack(Color::rgba(static_cast<std::uint8_t>(c.r), static_cast<std::uint8_t>(c.g),
                                static_cast<std::uint8_t>(c.b)));
    }

private:
    static constexpr pixel_detail::Rgb8 unpack(storage p) {
        const std::uint32_t r5 = p >> 11, g6 = (p >> 5) & 0x3Fu, b5 = p & 0x1Fu;
        return {(r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)};
    }
};

} // namespace prism::raster
