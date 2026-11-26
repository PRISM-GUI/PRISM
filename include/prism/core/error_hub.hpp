#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <mutex>

namespace prism::core {

using ErrorHandler = std::function<void(std::exception_ptr)>;

namespace error_hub_detail {
// Defined once, in libprism (src/core/shared_state.cpp): an inline variable gets one copy per
// shared object that hides its symbols (a Python extension does), and the copies disagree.
extern std::mutex error_handler_mutex;
extern ErrorHandler error_handler;

// stdio, not iostream: every app links this, and on a board std::cerr drags in iostreams and the
// whole locale machinery (~170 KB, more than a small MCU's flash can spare).
inline void default_error_handler(std::exception_ptr e) {
    try {
        std::rethrow_exception(e);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[prism] unhandled exception in posted callback: %s\n", ex.what());
    } catch (...) {
        std::fputs("[prism] unhandled exception in posted callback: <non-std exception>\n", stderr);
    }
}
} // namespace error_hub_detail

// Any thread. Pass nullptr to restore the default (print-to-stderr) handler.
inline void set_unhandled_error_handler(ErrorHandler h) {
    std::lock_guard<std::mutex> lock(error_hub_detail::error_handler_mutex);
    error_hub_detail::error_handler = std::move(h);
}

inline void report_unhandled_error(std::exception_ptr e) {
    ErrorHandler h;
    {
        std::lock_guard<std::mutex> lock(error_hub_detail::error_handler_mutex);
        h = error_hub_detail::error_handler;
    }
    if (!h) {
        error_hub_detail::default_error_handler(e);
        return;
    }
    try {
        h(e);
    } catch (...) {
        error_hub_detail::default_error_handler(e);
    }
}

} // namespace prism::core
