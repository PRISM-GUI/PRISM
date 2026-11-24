#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

// Only the umbrella header: doc/concepts/model.md and doc/concepts/threads.md show
// core::set_unhandled_error_handler with `#include <prism/prism.hpp>` and nothing else, so that
// one include has to give it. No other PRISM header, no std header.
#include <prism/prism.hpp>

TEST_CASE("the umbrella header provides core::set_unhandled_error_handler") {
    int seen = 0;
    prism::core::set_unhandled_error_handler([&seen](auto&&) { ++seen; });
    prism::core::report_unhandled_error(nullptr);
    prism::core::set_unhandled_error_handler(nullptr);
    CHECK(seen == 1);
}
