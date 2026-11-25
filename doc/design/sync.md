# Shared models: the `crdt` flavor

## Why

A PRISM app is a struct of fields. Its views and its behavior are derived from that struct.
That data maps almost one to one onto CRDTs, the merge-without-a-server data types behind
Google-Docs-style co-editing. So a model whose fields are CRDTs gives, without new view code:

- live co-editing: several people, several windows, several devices, one model;
- AI agents as peers: an agent edits the same model, through a library in its own language, and
  its edits show up in the ordinary widgets as they happen;
- offline work that merges later.

This is opt-in. A local app pays nothing for it, and nothing in `core` changes.

## The idea: a second flavor of the model types

`prism::crdt` holds shared versions of the model types, with the same names and the same API:

```cpp
using namespace prism;
namespace model = prism::core;     // local app
namespace model = prism::crdt;     // the same app, shared   (one line changes)

struct Experiment {
    model::Field<double> gain;     // shared once the flavor is crdt
    model::Field<Mode>   mode;
    core::Field<int>     tab;      // always local: spelled out, so the exception is visible
    core::Shared<Sample> live;     // fast streams never sync
};
```

- Users write `core::Field` today (about 540 times in `doc/` and `examples/`), so the flavor
  mirrors `core`. The docs recommend the namespace alias: the swap is then one line.
- `crdt` re-exports every `core` name it does not replace (`Derived`, `Shared`, `Channel`,
  `State`, ...) with one using-declaration each. It must not write `using namespace core`:
  using-directives chain, so `Field` would become ambiguous for the user.
- Writing both `using namespace prism::core` and `using namespace prism::crdt` is ambiguous. That
  compile error is the guard against mixing them by accident.
- What syncs is decided by type: `Field`, `List`, `Document` are durable shared state.
  `Shared`/`Channel` are fast local streams. `Derived` is recomputed on each peer.

## Engine: Yrs

[Yrs](https://github.com/y-crdt/y-crdt) is the Rust port of Yjs, used through its C interface
`yffi` (`libyrs.h`). Its update format is the Yjs one, so a JavaScript (`yjs`) or Python
(`pycrdt`) program joins a PRISM model with an off-the-shelf library: that is how an agent
becomes a peer.

## Approaches considered

| | Choice | Why |
|---|---|---|
| Engine | **Yrs** over our own C++ CRDTs | Peers in Python and JS for free; we do not own a hard algorithm. Cost: Rust in the build, no embedded. |
| | Own C++ CRDTs | Runs everywhere, embedded too; but a protocol nobody else speaks, and the algorithms are ours to get right. Rejected for now. |
| First cut | **Fields only** | Smallest thing proving the flavor swap and echo handling. |
| | Fields + lists | List identity (stable ids, moves) is the hardest mapping; it would slow the first proof. Step 3. |
| | Text first | `Document` positions are `{block, byte}`; mapping them onto Yrs is its own project. Step 4. |
| Build | **`cargo` called by Meson**, building `yffi` as a static library | Works with any cargo; one pinned `Cargo.lock`. |
| | Meson's built-in Cargo support | Still experimental; wants `Cargo.toml` at the project root. |
| | A system `libyrs` | Nobody packages it; breaks "dependencies come from wraps". |

## Step 1 (this design): shared fields

### `crdt::Field<T>`

- Derives from `core::Field<T>`. It adds no state. Every widget, `view_builder` call and
  reflection walk that takes a `core::Field<T>&` works unchanged.
- It is a marker: the type says "this field is shared". A `crdt::Field` that no `Doc` binds
  behaves exactly as a local field (tests, no-reflection builds).
- `T` in step 1: integers, floating point, `bool`, `std::string`, scoped enums. Anything else is a
  compile error naming the supported types.

### `crdt::Doc`

Owns one Yrs document and binds fields to keys of one root map.

```cpp
crdt::Doc doc;                          // one replica
doc.bind("gain", model.gain);           // manual tier: works without reflection
crdt::bind(doc, model);                 // reflection tier: every crdt:: member, by its name

doc.on_update([&](std::span<const std::byte> u) { send(u); });   // local edits, encoded
doc.apply_update(bytes);                                          // a peer's edits
auto state = doc.state_vector();  auto missing = doc.encode_since(peer_state);   // catching up
```

- The `Doc` keeps the connections, not the field. A local `set()` reaches it through the field's
  `on_change`; it writes the key in a Yrs transaction and emits the encoded update.
- A remote update sets the field with `set()`, so views and `Derived` follow as for a local edit.
  While the `Doc` applies it, its own change handler ignores the field: no echo back out.
- Nested structs (reflection tier) become nested maps, keyed by member name.
- Binding a key the Yrs map already holds takes the map's value: joining a session loads its
  state. Otherwise the field's current value is written.
- Encoding, chosen so a Python or JS peer reads it naturally: numbers as Yjs numbers (integers
  beyond 2^53 as strings), `bool`, strings, **enums by name** (magic_enum), not by value.
- Logic thread only, like every model value. Bytes from a network thread arrive through a
  `Channel` and are applied on the logic thread (step 2).
- Concurrent writes to one field: Yjs resolves them, every peer ends with the same value.

### Build

- `option('crdt', type: 'feature', value: 'auto')`: on when `cargo` is found. Off on
  bare-metal targets and, in step 1, in the browser build.
- `subprojects/yffi/`: a pinned `yffi` version with its `Cargo.lock`. Meson calls
  `cargo build --release` through a `custom_target` and links the static library into
  a new `prism-crdt` library. Rust types never appear in PRISM headers.
- `include/prism/crdt/` (`field.hpp`, `doc.hpp`, `flavor.hpp`), `src/crdt/doc.cpp`.

### Tests

- Two `Doc`s in one process, updates passed by hand: a set on one reaches the other; each
  supported type survives the round trip; an enum travels by name.
- No echo: applying a remote update emits no local update.
- Concurrent sets on both sides, then exchange: both converge to the same value.
- Joining: a fresh `Doc` catching up from `encode_since` gets the existing values.
- The flavor swap: a test model written against `namespace model = ...` compiles and passes its
  field tests in both flavors.
- Unbound `crdt::Field` behaves as `core::Field`.

### Proof app

`examples/crdt_pair/`: one struct, two windows in one process, each with its own `Doc`, wired by
passing updates. Moving a slider in one moves it in the other.

## Later steps

2. **Network and an agent peer.** Updates over `net`/`io` using the y-sync protocol, and a short
   Python `pycrdt` script that joins and edits: the agent story, end to end.
3. **`crdt::List<T>`.** A Yrs array with stable ids inside; the index API stays.
4. **`crdt::Document`.** Yrs text with Peritext-style formatting; `{block, byte}` positions
   converted at the edge.
5. **Undo per peer** (Yrs undo manager with tracked origins: undo reverts *my* edits only),
   **presence** (carets, selections: Yjs awareness, not the document), **agent forks** (review an
   agent's edits before merging them).

## Restrictions users must know

- An invariant across two fields (`min <= max`) can break after a merge. Keep such pairs in one
  field, or repair them in an `observe` handler.
- `crdt::Field<std::string>` is last-writer-wins on the whole string. Character-level merging is
  `crdt::Document` (step 4).
- `crdt::Field<Struct>` replaces the whole struct; per-member merging comes from nesting
  `crdt::Field` members.

## Non-goals

- Making `core` types CRDT-aware, or any cost for apps that do not use `crdt`.
- Embedded targets.
- A server: peers exchange updates; relaying them is the transport's job.

## Left to the builder

The exact `yffi` version and cargo invocation; how the echo guard is held (a flag on the `Doc`
or Yrs transaction origins); Yjs number encoding details; the re-export list for `crdt`.
