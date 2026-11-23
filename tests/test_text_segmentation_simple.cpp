// The table-free segmentation (meson option line_breaking=simple, ui/text_segmentation.hpp). Built
// on its own, without libprism: libprism compiles the Unicode variant of the same inline
// functions, and one program must not hold both.
#define PRISM_SIMPLE_SEGMENTATION
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <prism/ui/text_segmentation.hpp>

#include <string>

using namespace prism::ui;

TEST_CASE("simple: one step per code point, whatever its length") {
    const std::string s = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80z";   // a é € 😀 z
    CHECK(next_grapheme(s, 0) == 1);
    CHECK(next_grapheme(s, 1) == 3);
    CHECK(next_grapheme(s, 3) == 6);
    CHECK(next_grapheme(s, 6) == 10);
    CHECK(prev_grapheme(s, 10) == 6);
    CHECK(prev_grapheme(s, 3) == 1);
}

TEST_CASE("simple: a line may start after a space or a hyphen, never inside a word") {
    const std::string s = "ab cd-ef";
    const auto ok = line_break_opportunities(s);
    REQUIRE(ok.size() == s.size() + 1);
    CHECK_FALSE(ok[1]);   // inside "ab"
    CHECK(ok[3]);         // after the space
    CHECK_FALSE(ok[4]);
    CHECK(ok[6]);         // after the hyphen
    CHECK(ok[8]);         // the end
}

TEST_CASE("simple: around each CJK ideograph, and after a newline") {
    const std::string s = "\xe4\xb8\xad\xe6\x96\x87\nx";   // 中文\nx
    const auto ok = line_break_opportunities(s);
    CHECK(ok[0]);
    CHECK(ok[3]);   // between the two ideographs
    CHECK(ok[6]);
    CHECK(ok[7]);   // after the newline
    CHECK_FALSE(ok[1]);   // never inside a character
}
