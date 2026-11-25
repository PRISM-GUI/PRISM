#pragma once

// Small form widgets: radio group, progress bar, spin box.

#include <prism/ui/delegate.hpp>
#include <prism/ui/widget_node.hpp>

// simplify: kept for spin_detail::format<T> below, the one template here that formats (fmt's
// to_string and "{:.2f}"). Move it to non-template helpers in the .cpp -- a std::to_chars pair,
// or fmt::format living in the .cpp -- to drop fmt from every file that shows a spin box.
#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace prism::ui {

// ---- Radio group ----

// Pick one of a list of strings. For an enum field, use Radio<E> instead.
struct RadioGroup {
    std::size_t selected = 0;
    std::vector<std::string> options{};
    bool operator==(const RadioGroup&) const = default;
};

// An enum field shown as radio buttons; Dropdown<E> (or a bare enum) shows it as a dropdown.
template <ScopedEnum E>
struct Radio {
    E value{};
    bool operator==(const Radio&) const = default;
};

namespace radio_detail {

constexpr float pad = 4.f;
constexpr float row_h = 26.f;
constexpr float dot_r = 8.f;
constexpr float label_x = 2 * dot_r + 16.f;
constexpr float font_size = 14.f;

inline float height(std::size_t n) { return 2 * pad + row_h * static_cast<float>(n); }

// Defined in src/delegates/form_delegates.cpp.
float width(const std::vector<std::string>& labels, const WidgetNode& n);

void draw(DrawList& dl, const std::vector<std::string>& labels, std::size_t selected, WidgetNode& node);

// The index an event selects, if it changes the selection.
std::optional<std::size_t> pick(const InputEvent& ev, std::size_t selected, std::size_t n);

template <ScopedEnum E>
std::vector<std::string> enum_labels() {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < enum_count<E>(); ++i) out.push_back(enum_label<E>(i));
    return out;
}

} // namespace radio_detail

template <>
struct Widget<RadioGroup> {
    static constexpr FocusPolicy focus_policy = FocusPolicy::tab_and_click;   // one tab stop; arrows choose
    static constexpr Role access_role = Role::RadioGroup;
    static void describe(const Field<RadioGroup>& f, AccessInfo& a);
    static float intrinsic_width(const Field<RadioGroup>& f, const WidgetNode& n) {
        return radio_detail::width(f.get().options, n);
    }
    static void record(DrawList& dl, const Field<RadioGroup>& f, WidgetNode& node);
    static void handle_input(Field<RadioGroup>& f, const InputEvent& ev, WidgetNode&);
};

template <ScopedEnum E>
struct Widget<Radio<E>> {
    static constexpr FocusPolicy focus_policy = FocusPolicy::tab_and_click;
    static constexpr Role access_role = Role::RadioGroup;
    static void describe(const Field<Radio<E>>& f, AccessInfo& a) { a.value = enum_label<E>(enum_index(f.get().value)); }
    static float intrinsic_width(const Field<Radio<E>>&, const WidgetNode& n) {
        return radio_detail::width(radio_detail::enum_labels<E>(), n);
    }
    static void record(DrawList& dl, const Field<Radio<E>>& f, WidgetNode& node) {
        radio_detail::draw(dl, radio_detail::enum_labels<E>(), enum_index(f.get().value), node);
    }
    static void handle_input(Field<Radio<E>>& f, const InputEvent& ev, WidgetNode&) {
        if (auto to = radio_detail::pick(ev, enum_index(f.get().value), enum_count<E>()))
            f.set(Radio<E>{enum_from_index<E>(*to)});
    }
};

// ---- Progress bar ----

// Display only. `busy`: the work's length is unknown -- a segment slides back and forth
// instead of the value (Qt's indeterminate progress).
struct ProgressBar {
    double value = 0.0;
    double min = 0.0;
    double max = 1.0;
    std::string label{};   // drawn over the bar when not empty
    bool busy = false;
    bool operator==(const ProgressBar&) const = default;
};

namespace progress_detail {
constexpr float bar_row_h = 16.f;
constexpr float label_h = 18.f;   // the label sits above the bar
constexpr float track_h = 8.f;
constexpr float label_size = 12.f;

// Defined in src/delegates/form_delegates.cpp.
float fraction(const ProgressBar& p);

constexpr double busy_period_s = 1.4;   // there and back
constexpr auto busy_frame = std::chrono::milliseconds(33);

// Where the busy segment is (x, width) on a track `w` wide, `seconds` into its animation.
std::pair<float, float> busy_segment(float w, double seconds);
} // namespace progress_detail

template <>
struct Widget<ProgressBar> {
    static constexpr FocusPolicy focus_policy = FocusPolicy::none;
    static constexpr Role access_role = Role::ProgressIndicator;
    static constexpr bool expand = true;
    static constexpr ExpandAxis expand_axis = ExpandAxis::Horizontal;
    static void describe(const Field<ProgressBar>& f, AccessInfo& a);
    static void record(DrawList& dl, const Field<ProgressBar>& f, WidgetNode& node);
    static void handle_input(Field<ProgressBar>&, const InputEvent&, WidgetNode&) {}
};

// ---- Spin box ----

// A number with up/down steppers; typed digits commit on Enter. Mirrors Slider's data.
template <Numeric T>
struct SpinBox {
    T value{};
    T min = T{0};
    T max = T{100};
    T step = T{1};
    bool operator==(const SpinBox&) const = default;
};

// What is being typed, until Enter commits it or Escape (or losing focus) drops it.
struct SpinBoxEditState {
    std::optional<std::string> typed;
};

namespace spin_detail {
constexpr float widget_h = 30.f;
constexpr float stepper_w = 18.f;
constexpr float pad = 8.f;
constexpr float font_size = 14.f;

template <typename T>
std::string format(T v) {
    if constexpr (std::is_floating_point_v<T>) return fmt::format("{:.2f}", v);
    else return fmt::to_string(v);
}

// Into [min, max]; a range with max below min holds everything at min.
template <typename T>
T clamped(const SpinBox<T>& s, T v) {
    return std::clamp(v, s.min, std::max(s.min, s.max));
}

// One step (> 0) up (dir > 0) or down, within [min, max]. Not snapped to a step grid: like
// QSpinBox, a step adds `step` to whatever the value is. Integers step in their own type, so
// 64-bit values near the limits stay exact: the distance to the limit fits the unsigned type
// (the value is already inside the range), and a step that would pass it lands on it.
template <typename T>
T stepped(const SpinBox<T>& s, int dir) {
    T lo = s.min, hi = std::max(s.min, s.max), v = clamped(s, s.value);
    if constexpr (std::is_integral_v<T>) {
        using U = std::make_unsigned_t<T>;
        U room = dir > 0 ? U(U(hi) - U(v)) : U(U(v) - U(lo));
        if (room < U(s.step)) return dir > 0 ? hi : lo;
        return dir > 0 ? T(v + s.step) : T(v - s.step);
    } else {
        return clamped(s, dir > 0 ? v + s.step : v - s.step);
    }
}

template <typename T>
bool accepts(const SpinBox<T>& s, const std::string& typed, char c) {
    if (c >= '0' && c <= '9') return true;
    if (c == '-') return typed.empty() && s.min < T{0};
    if (c == '.') return std::is_floating_point_v<T> && typed.find('.') == std::string::npos
                         && !typed.empty() && typed.back() >= '0' && typed.back() <= '9';
    return false;
}

template <typename T>
std::optional<T> parse(const std::string& s) {
    T v{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

// One step up (dir > 0) or down, or nothing when the step is zero or the value cannot move.
template <typename T>
std::optional<T> stepped_to(const SpinBox<T>& s, int dir) {
    if (s.step <= T{0}) return std::nullopt;
    T next = stepped(s, dir);
    if (next == s.value) return std::nullopt;
    return next;
}

// The whole range of the type, one step at a time: what a plain numeric field edits.
template <Numeric T>
SpinBox<T> whole_range(T value) {
    return SpinBox<T>{value, std::numeric_limits<T>::lowest(), std::numeric_limits<T>::max(), T{1}};
}

// The box a spin box needs around text `text_px` wide: its padding, its number and its steppers.
inline float width_for(float text_px) { return 2 * pad + std::max(text_px, 32.f) + stepper_w; }

void draw(DrawList& dl, std::string_view shown, bool typing, WidgetNode& node);

// The box showing `s`: the text being typed while there is some (dropped when the box is not
// focused), else the formatted value.
template <typename T>
void record(DrawList& dl, const SpinBox<T>& s, WidgetNode& node) {
    auto& es = node.get_or_create<SpinBoxEditState>();
    if (!node_vs(node).focused) es.typed.reset();   // simplify: losing focus drops what was typed, like Escape
    draw(dl, es.typed.value_or(format(s.value)), es.typed.has_value(), node);
}

// The value one event gives the box, if any. Typed digits live in the node's edit state (they
// append, Backspace removes the last); Enter commits them, Escape drops them. Up, Down, the wheel
// and the stepper column step the value itself.
template <typename T>
std::optional<T> event(const SpinBox<T>& s, const InputEvent& ev, WidgetNode& node) {
    auto& es = node.get_or_create<SpinBoxEditState>();
    if (auto* ti = std::get_if<TextInput>(&ev)) {
        for (char c : ti->text)
            if (accepts(s, es.typed.value_or(""), c)) es.typed = es.typed.value_or("") + c;
        node.dirty = true;
        return std::nullopt;
    }
    if (auto* kp = std::get_if<KeyPress>(&ev)) {
        if (es.typed) {
            std::optional<T> committed;
            if (kp->key == keys::enter) {
                if (auto v = parse<T>(*es.typed)) committed = clamped(s, *v);
                es.typed.reset();
            } else if (kp->key == keys::escape) {
                es.typed.reset();
            } else if (kp->key == keys::backspace && !es.typed->empty()) {
                es.typed->pop_back();
            }
            node.dirty = true;
            return committed;
        }
        // Page keys never arrive: routing scrolls the focused widget's container with them.
        if (kp->key == keys::up) return stepped_to(s, 1);
        if (kp->key == keys::down) return stepped_to(s, -1);
        return std::nullopt;
    }
    if (auto* ms = std::get_if<MouseScroll>(&ev); ms && ms->dy.raw() != 0.f)
        return stepped_to(s, ms->dy.raw() > 0.f ? 1 : -1);
    if (auto* mb = std::get_if<MouseButton>(&ev); mb && mb->pressed && mb->button == buttons::left
                                                    && mb->position.x.raw() >= delegate_detail::widget_w(node).raw() - stepper_w)
        return stepped_to(s, mb->position.y.raw() < widget_h / 2 ? 1 : -1);
    return std::nullopt;
}

template <typename T>
void describe(const SpinBox<T>& s, AccessInfo& a) {
    a.value = format(s.value);
    a.numeric_value = static_cast<double>(s.value);
    a.min_value = static_cast<double>(s.min);
    a.max_value = static_cast<double>(s.max);
    if (s.step > T{0}) a.step = static_cast<double>(s.step);
}

// A SpinBox field stores the number an event gives, keeping its range and step.
template <typename T>
void apply(Field<SpinBox<T>>& f, const InputEvent& ev, WidgetNode& node) {
    if (auto v = event(f.get(), ev, node)) {
        auto s = f.get();
        s.value = *v;
        f.set(s);
    }
}

// A plain numeric field stores the number itself, over its type's whole range.
template <typename T>
void apply_plain(Field<T>& f, const InputEvent& ev, WidgetNode& node) {
    if (auto v = event(whole_range(f.get()), ev, node)) f.set(*v);
}

} // namespace spin_detail

template <Numeric T>
struct Widget<SpinBox<T>> {
    static constexpr FocusPolicy focus_policy = FocusPolicy::tab_and_click;
    static constexpr Role access_role = Role::SpinButton;
    static void describe(const Field<SpinBox<T>>& f, AccessInfo& a) { spin_detail::describe(f.get(), a); }
    static float intrinsic_width(const Field<SpinBox<T>>& f, const WidgetNode& n) {
        using namespace spin_detail;
        const auto* m = node_theme(n).measurer;
        return width_for(std::max(text_width(m, format(f.get().min), font_size),
                                  text_width(m, format(f.get().max), font_size)));
    }

    static void record(DrawList& dl, const Field<SpinBox<T>>& f, WidgetNode& node) {
        spin_detail::record(dl, f.get(), node);
    }

    // simplify: typed text has no caret or selection -- digits append, Backspace removes the last;
    // reuse the TextField editing core if spin boxes need in-place editing.
    static void handle_input(Field<SpinBox<T>>& f, const InputEvent& ev, WidgetNode& node) {
        spin_detail::apply(f, ev, node);
    }
};

// --- Widget<T> for a plain number: the spin box over the whole range of its type ---

template <Numeric T>
void Widget<T>::describe(const Field<T>& f, AccessInfo& a) {
    spin_detail::describe(spin_detail::whole_range(f.get()), a);
}

template <Numeric T>
float Widget<T>::intrinsic_width(const Field<T>& f, const WidgetNode& n) {
    return spin_detail::width_for(
        text_width(node_theme(n).measurer, spin_detail::format(f.get()), spin_detail::font_size));
}

template <Numeric T>
void Widget<T>::record(DrawList& dl, const Field<T>& f, WidgetNode& node) {
    spin_detail::record(dl, spin_detail::whole_range(f.get()), node);
}

template <Numeric T>
void Widget<T>::handle_input(Field<T>& f, const InputEvent& ev, WidgetNode& node) {
    spin_detail::apply_plain(f, ev, node);
}

} // namespace prism::ui
