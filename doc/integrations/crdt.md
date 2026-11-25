# Shared models

`prism::crdt` is a second flavor of the model types. Two peers hold the same struct, edit their own copy, and merge. The names and the API are the ones you already know: that is the whole trick.

It is opt-in, it needs `cargo` at build time, and nothing in `prism::core` changes for apps that do not use it. The design and its trade-offs are in `doc/design/sync.md` in the repository.

## The one-line swap

```cpp
namespace model = prism::core;   // a local app
namespace model = prism::crdt;   // the same app, shared: one line changes

struct Experiment {
    model::Field<double> gain;   // shared, because the flavor is crdt
    model::Field<Mode>   mode;
    core::Field<int>     tab;    // always local: spelled out, so the exception is visible
};
```

`crdt` re-exports the model vocabulary (`State`, `Derived`, `Shared`, `Channel`, `List`, `Latest`) and replaces `Field`. A `crdt::Field<T>` is a `core::Field<T>` with a marker: same storage, same operators, same deferred notifications, so every widget and every `view()` works unchanged. A `crdt::Field` that no `Doc` binds behaves exactly like a local field.

What a `crdt::Field` holds is a plain value: an integer, a floating point number, a `bool`, a `std::string`, or a scoped enum. Anything else is a compile error naming the supported ones.

## A Doc is one replica

```cpp
model::Doc doc;                          // this peer's document

doc.bind("gain", experiment.gain);       // manual tier: works without reflection
model::bind(doc, experiment);            // reflection tier: every crdt::Field member, by its name
```

The manual tier is one line per field, and `doc.map("in").bind("on", on)` binds under a prefix. The reflection tier walks the struct; a nested struct contributes a dotted prefix (`in.on`, `in.deeper.x`), and a `core::Field` member is skipped and stays local.

What binds is the model field, not the widget, so the field has to hold a value the `Doc` understands. A view that edits a value through a widget-typed field (a slider, a text field) copies the two across, as the example does. A window whose UI comes from reflection (no `view()`) can annotate a number with `[[=core::range<min, max, step>]]` instead, and get a slider bound straight to the shared field.

Binding never writes. A key the map already holds replaces the field's value, so joining a session loads its state. An absent one leaves the field alone: defaults live in the code, every peer starts from the same ones, and a peer that binds before catching up can never push its default over a value somebody edited.

A `Doc` that dies first releases its connections; a field that dies first is left untouched (the `Doc` holds only a token for it).

## Wiring two peers

```cpp
model::Doc a, b;
core::Connection ab = a.on_update().connect(
    [&b](std::span<const std::byte> update) { b.apply_update(update); });
core::Connection ba = b.on_update().connect(
    [&a](std::span<const std::byte> update) { a.apply_update(update); });
```

`on_update()` carries local edits only: applying a peer's update sets the fields with `set()`, so views and `Derived` follow, and nothing is sent back out. Updates are Yjs v1, so a `yjs` or `pycrdt` program joins the same document with an off-the-shelf library.

Catching up on a session you were not part of: send your `state_vector()` to the peer, get back `encode_since(that)`, apply it. `to_json()` shows the document as a flat JSON object, dotted keys included, which is what another language sees of it.

Bytes from a network thread are applied on the logic thread, like every other model change.

## What syncs

| Type | Travels |
|---|---|
| `crdt::Field<T>` | yes, once a `Doc` binds it |
| `Derived` | no: it is recomputed on each peer from what arrived |
| `Shared`, `Channel`, `Latest` | no: fast local streams, cross-thread, never durable |
| `core::Field<T>` | no: local by construction, and the marker that says so |
| `crdt::List`, `Document` | not yet: steps 3 and 4 of the design |

## What to know before you share a field

- An invariant across two fields (`min <= max`) can break after a merge. Keep such a pair in one field, or repair it in an `observe` handler.
- `crdt::Field<std::string>` is last-writer-wins on the whole string. Character-level merging needs `crdt::Document`, which is a later step.
- An enum travels by its enumerator name, so a peer with a different numbering reads the same value.
- Integers beyond 2^53 travel as decimal text, so a JavaScript peer does not lose them.
- Concurrent writes to one field are resolved by Yjs: every peer ends with the same value, not necessarily the last one typed.

## Enabling it

| Part | Meson option | Link with |
|---|---|---|
| The flavor | `-Dcrdt=auto` | `prism_crdt_dep` |
| The engine | (with the flavor) | built by `cargo` from the pinned `yffi` wrap |

`auto` builds it when `cargo` is found and the target is hosted, and it is off on bare metal and in the browser. Everything is behind `prism_crdt_dep`: a program that does not link it never sees Yrs, and no Rust type appears in a PRISM header.

## A complete example

`examples/crdt_pair/` is two windows of one model in one process, each window its own `Doc`, the two Docs wired as above. Its struct is the whole of the crdt API in use:

```cpp
namespace model = prism::crdt;
enum class Mode { idle, run, hold };

struct Mixer {
    model::Field<double> gain{0.5};
    model::Field<Mode> mode{Mode::idle};
    model::Field<std::string> name{"lead"};
    model::Field<bool> armed{false};

    core::Field<int> local_clicks{0};   // never bound: this window alone

    model::Doc doc;

    void attach(app::AppContext&) {
        model::bind(doc, *this);        // without reflection: doc.bind("gain", gain), ...
    }

    void view(app::ViewBuilder& vb);
};
```

Type in the gain's spin box in one window and the other follows; click in both and the two counters
differ, because a `core::Field` is not bound. That difference is the point of the flavor: what is
shared, and what is not, is written in the type.
