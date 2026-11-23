#pragma once

#if !defined(PRISM_SIMPLE_SEGMENTATION)
#include <graphemebreak.h>
#include <linebreak.h>
#endif

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// Unicode text boundaries: user-perceived characters (UAX #29 graphemes) for the caret, and where
// a line may break (UAX #14) for word wrap. One interface, implemented with libunibreak (chosen
// at build time, doc/review-2026-11-02.md §4), or — with the meson option line_breaking=simple —
// with the small rule set below, which needs no tables (and no libunibreak at all).
// UTF-8 in, byte offsets out.
namespace prism::ui {

namespace segmentation_detail {

#if defined(PRISM_SIMPLE_SEGMENTATION)
// ---- the table-free segmentation (meson option line_breaking=simple) ----
//
// A character is one UTF-8 code point: combining marks and emoji sequences are not joined, so the
// caret steps once per code point. A line may start after a space or a hyphen, before and after a
// CJK ideograph, at a newline and at the end — the rules of UAX #14 that the tables add to these
// (numbers, punctuation, kinsoku, …) are not applied. Sized for a small board, not for a word
// processor: ~100 KiB of tables less than the Unicode path.
inline constexpr char kBreak = 1;
inline constexpr char kNoBreak = 0;

inline bool utf8_continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U; }

inline void init() {}

// One mark per byte, at the last byte of each character (the same shape libunibreak's marks have).
inline std::vector<char> grapheme_marks(std::string_view s) {
    std::vector<char> marks(s.size(), kNoBreak);
    for (std::size_t i = 0; i + 1 < s.size(); ++i)
        if (!utf8_continuation(s[i + 1])) marks[i] = kBreak;
    if (!s.empty()) marks[s.size() - 1] = kBreak;
    return marks;
}

inline std::uint32_t codepoint_at(std::string_view s, std::size_t at, std::size_t *length) {
    const auto b0 = static_cast<unsigned char>(s[at]);
    if (b0 < 0x80U) {
        *length = 1;
        return b0;
    }
    const auto tail = [s, at](std::size_t n) {
        std::uint32_t cp = 0;
        for (std::size_t k = 1; k <= n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[at + k]) & 0x3FU);
        return cp;
    };
    if ((b0 & 0xE0U) == 0xC0U && at + 1 < s.size()) {
        *length = 2;
        return ((b0 & 0x1FU) << 6) | tail(1);
    }
    if ((b0 & 0xF0U) == 0xE0U && at + 2 < s.size()) {
        *length = 3;
        return ((b0 & 0x0FU) << 12) | tail(2);
    }
    if ((b0 & 0xF8U) == 0xF0U && at + 3 < s.size()) {
        *length = 4;
        return ((b0 & 0x07U) << 18) | tail(3);
    }
    *length = 1;
    return b0;
}

inline bool is_ideograph(std::uint32_t cp) {
    return (cp >= 0x2E80U && cp <= 0x9FFFU) || (cp >= 0xF900U && cp <= 0xFAFFU) || (cp >= 0x20000U && cp <= 0x3FFFFU);
}
#else
// ---- libunibreak (the default) ----
inline constexpr char kBreak = GRAPHEMEBREAK_BREAK;

inline void init() {
    static const bool done = (init_graphemebreak(), init_linebreak(), true);   // thread-safe once
    (void)done;
}

// libunibreak's marks: one per byte, about the position after that byte.
inline std::vector<char> grapheme_marks(std::string_view s) {
    init();
    std::vector<char> marks(s.size());
    if (!s.empty())
        set_graphemebreaks_utf8(reinterpret_cast<const utf8_t*>(s.data()), s.size(), nullptr, marks.data());
    return marks;
}
#endif
} // namespace segmentation_detail

namespace segmentation_detail {
// A character never spans a newline (UAX #29 always breaks after one), so stepping looks only
// at the line around the caret, not the whole text.
// simplify: a single very long line (a 1 MB minified file) is still read whole per step; a
// window back to a safe break (not inside a run of regional indicators) would bound it.
inline std::size_t line_start(std::string_view s, std::size_t at) {
    auto nl = at == 0 ? std::string_view::npos : s.rfind('\n', at - 1);
    return nl == std::string_view::npos ? 0 : nl + 1;
}
} // namespace segmentation_detail

// The offset after the character that starts at `at` (s.size() at the end).
inline std::size_t next_grapheme(std::string_view s, std::size_t at) {
    if (at >= s.size()) return s.size();
    auto from = segmentation_detail::line_start(s, at);
    auto nl = s.find('\n', at);
    auto to = nl == std::string_view::npos ? s.size() : nl + 1;
    auto marks = segmentation_detail::grapheme_marks(s.substr(from, to - from));
    for (std::size_t i = at - from; i < marks.size(); ++i)
        if (marks[i] == segmentation_detail::kBreak) return from + i + 1;
    return to;
}

// The offset where the character ending at `at` starts (0 at the start).
inline std::size_t prev_grapheme(std::string_view s, std::size_t at) {
    if (at == 0 || s.empty()) return 0;
    if (at > s.size()) at = s.size();
    auto from = segmentation_detail::line_start(s, at - 1);   // the line of the byte before `at`
    auto marks = segmentation_detail::grapheme_marks(s.substr(from, at - from));
    for (std::size_t i = at - from - 1; i-- > 0;)
        if (marks[i] == segmentation_detail::kBreak) return from + i + 1;
    return from;
}

// For each byte offset 0..s.size(): whether a line may start there -- after a space or a hyphen,
// between ideographs, at a newline, at the end; never inside a word or a character.
inline std::vector<bool> line_break_opportunities(std::string_view s) {
#if defined(PRISM_SIMPLE_SEGMENTATION)
    std::vector<bool> ok(s.size() + 1, false);
    for (std::size_t i = 0; i < s.size();) {
        std::size_t length = 1;
        const std::uint32_t cp = segmentation_detail::codepoint_at(s, i, &length);
        const std::size_t next = i + length;
        if (cp == '\n' || cp == ' ' || cp == '-')
            ok[next] = true;
        else if (segmentation_detail::is_ideograph(cp))
            ok[i] = ok[next] = true;
        i = next;
    }
    if (!s.empty()) ok[s.size()] = true;
    return ok;
#else
    segmentation_detail::init();
    std::vector<char> marks(s.size());
    if (!s.empty())
        set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(s.data()), s.size(), nullptr, marks.data());
    std::vector<bool> ok(s.size() + 1, false);
    for (std::size_t i = 0; i < s.size(); ++i)
        ok[i + 1] = marks[i] == LINEBREAK_ALLOWBREAK || marks[i] == LINEBREAK_MUSTBREAK;
    if (!s.empty()) ok[s.size()] = true;
    return ok;
#endif
}

} // namespace prism::ui
