# A widget for your own type

A widget is a template specialization, not a class you register.
Write `Widget<YourType>` in namespace `prism::ui` and any field of that type draws it.

Nothing else is needed: no factory, no entry in a table, no call at startup.

Every type already has a widget, too. The default one draws a grey rectangle and ignores input,
so a field of a type you have not written yet still places and still lays out.

## The two members you must write

A type is a widget when `Widget<T>` has these two static members:

| Member | Called for |
|---|---|
| `record(DrawList&, const Field<T>&, WidgetNode&)` | drawing, every frame the widget is dirty |
| `handle_input(Field<T>&, const InputEvent&, WidgetNode&)` | keys, clicks, scroll that land on the widget |

`record` gets a const field: it draws what the value is now.
`handle_input` gets a mutable field: it is where the value changes.

Anything else is optional and picked up if present (table below).

## A complete one

A `Level` from 0 to 1, drawn as a track and read with the arrow keys:

```cpp
#include <prism/prism.hpp>
#include <algorithm>

using namespace prism;

struct Level {
    float value{0.5f};

    bool operator==(const Level&) const = default;   // the field needs equality
};

namespace prism::ui {
template <>   // one specialization, and Level works anywhere a Field<T> does
struct Widget<Level> {
    static constexpr FocusPolicy focus_policy = FocusPolicy::tab_and_click;

    // The track is the widget's size and the accent is the level: a leaf is as big as what it draws.
    static void record(DrawList& dl, const Field<Level>& f, WidgetNode& node) {
        const Width w{std::max(200.f, node_allocated(node).w.raw())};
        dl.filled_rect({Point{X{0}, Y{0}}, Size{w, Height{8.f}}}, node_theme(node).track);
        dl.filled_rect({Point{X{0}, Y{0}}, Size{Width{w.raw() * value(f)}, Height{8.f}}},
                       node_theme(node).accent);
    }

    static void handle_input(Field<Level>& f, const InputEvent& ev, WidgetNode& node) {
        const auto* kp = std::get_if<KeyPress>(&ev);
        if (!kp || (kp->key != keys::left && kp->key != keys::right)) return;
        set_value(f, std::clamp(value(f) + (kp->key == keys::right ? 0.1f : -0.1f), 0.f, 1.f));
        node.dirty = true;
    }
};
} // namespace prism::ui
```

Four rules are what makes it fit:

1. `record` draws in the widget's own coordinates, origin at its top-left.
2. `record` must stay inside `node_allocated(node)`, the size the layout gave it.
3. `set_value` notifies the field's listeners; `node.dirty` asks for the next frame.
4. `value()` reads, `set_value()` writes — never the struct's members directly.

`operator==` matters: a field compares before it notifies, and a value that compares equal
notifies nobody.

## Placing it

The type is all the placement needs:

```cpp
struct Meter {
    core::Field<Level> level{};

    void view(app::ViewBuilder& vb) { vb.widget(level); }
};
```

With reflection, `core::Field<Level> level;` in a model struct becomes the widget with no `view()`
at all.

The same field also works inside a container, a card, a table row, or a node board port.

## Optional members

Add the ones you want. Each is found only if it exists.

| Member | Gives the widget |
|---|---|
| `static constexpr FocusPolicy focus_policy` | Tab and click focus. `none` by default. |
| `intrinsic_width(const Field<T>&, const WidgetNode&) -> float` | A width from its content, for a leaf that should not stretch. |
| `access_role` | Its role for assistive technology. |
| `describe(const Field<T>&, AccessInfo&)` | What a screen reader reads: value and state. |
| `static constexpr bool expand` | Row or column expansion. False by default. |
| `static constexpr ExpandAxis expand_axis` | Which way it expands, when `expand` is set. |
| `try_shortcut(const Field<T>&, const KeyPress&, WidgetNode&) -> bool` | A shortcut key it handles itself. |
| `mnemonic_key(const Field<T>&) -> std::int32_t` | An Alt+key mnemonic. 0 means none. |
| `static constexpr bool opens_menu` | It is a menu title, so sibling menus form a bar. |
| `static constexpr bool context_menu` | A right click in its parent opens a menu. |
| `static constexpr bool popup_anchor` | It draws nothing in place, only popups. |

## Where to go next

`examples/tour/04_custom_widget.cpp` is this page's program, ready to run.
`examples/widget_gallery/knob.hpp` is a knob in one header: dragging, a tooltip, and its own size.
`examples/model_task_runner/` draws its table rows the same way, one `Widget<T>` per row shape.
