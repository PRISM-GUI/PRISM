#pragma once

#include <cstddef>
#include <string>
#include <type_traits>

#if __cpp_impl_reflection
#include <meta>
#else
// Included here, at file scope, so magic_enum's compile-time
// __PRETTY_FUNCTION__ parsing sees its own unqualified ::magic_enum::
// names. Including it from inside a namespace (as before) nests the
// library as prism::ui::magic_enum::..., which shifts GCC's
// __PRETTY_FUNCTION__ output and silently breaks magic_enum's
// fixed-offset name extraction -- every candidate enum value in the
// scan range then reads as "valid" with a garbled name.
#include <magic_enum/magic_enum.hpp>
#endif

namespace prism::core {

// The enumerator names of a scoped enum, enumerator by enumerator: the same names whether or not
// the build has reflection, so a model value written by name reads back on either.

// Concept: scoped enum type
template <typename T>
concept ScopedEnum = std::is_scoped_enum_v<T>;

// Enum introspection helpers for scoped enums
#if __cpp_impl_reflection

template <ScopedEnum T>
consteval size_t enum_count() {
    return std::meta::enumerators_of(^^T).size();
}

template <ScopedEnum T>
std::string enum_label(size_t index) {
    static constexpr auto enums = std::define_static_array(
        std::meta::enumerators_of(^^T));
    std::string result;
    size_t i = 0;
    template for (constexpr auto e : enums) {
        if (i == index) result = std::string(std::meta::identifier_of(e));
        ++i;
    }
    return result;
}

template <ScopedEnum T>
constexpr size_t enum_index(T value) {
    static constexpr auto enums = std::define_static_array(
        std::meta::enumerators_of(^^T));
    size_t result = 0, i = 0;
    template for (constexpr auto e : enums) {
        if ([:e:] == value) result = i;
        ++i;
    }
    return result;
}

template <ScopedEnum T>
T enum_from_index(size_t index) {
    static constexpr auto enums = std::define_static_array(
        std::meta::enumerators_of(^^T));
    T result{};
    size_t i = 0;
    template for (constexpr auto e : enums) {
        if (i == index) result = [:e:];
        ++i;
    }
    return result;
}

#else

template <ScopedEnum T>
constexpr size_t enum_count() {
    return magic_enum::enum_count<T>();
}

template <ScopedEnum T>
std::string enum_label(size_t index) {
    auto values = magic_enum::enum_values<T>();
    return std::string(magic_enum::enum_name(values[index]));
}

template <ScopedEnum T>
constexpr size_t enum_index(T value) {
    return magic_enum::enum_index(value).value();
}

template <ScopedEnum T>
T enum_from_index(size_t index) {
    return magic_enum::enum_values<T>()[index];
}

#endif

} // namespace prism::core
