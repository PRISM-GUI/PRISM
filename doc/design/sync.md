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
| Engine | **Yrs** over our own C++ CRDTs | Peers in Python and JS for free; we do not own a hard algorithm. Cost: Rust in the build, one toolchain per target. |
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
- `T` in step 1: integers, floating point, `bool`, `std::string`, scoped enums, or a widget type
  whose public `value` member is one of those (only that member travels; the widget's own settings
  stay local). Anything else is a compile error naming the supported types.

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
- Nested structs (reflection tier) become dotted keys in the root map, `in.on`, `in.deeper.x`, not
  nested maps: two replicas each creating the same nested map keep one of the two, and lose the
  other's edits with it, while a dotted key merges like any other key.
- Binding never writes. A key the Yrs map already holds takes the map's value, so joining a session
  loads its state; an absent one leaves the field alone. Defaults live in the code, so every peer
  starts from the same ones, and a peer that binds before catching up can never push its default
  over a value somebody edited. Only a real local change reaches the map.
- Encoding, chosen so a Python or JS peer reads it naturally: numbers as Yjs numbers (integers
  beyond 2^53 as strings), `bool`, strings, **enums by name** (core's enum helpers), not by value.
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
passing updates. A value typed in one window shows in the other.

## Later steps

2. **Network and an agent peer.** Designed below, in "Step 2".
3. **On the board.** Designed below, in "Step 3": the same pieces on a microcontroller, reached
   over its own access point.
4. **`crdt::List<T>`.** A Yrs array with stable ids inside; the index API stays.
5. **`crdt::Document`.** Yrs text with Peritext-style formatting; `{block, byte}` positions
   converted at the edge.
6. **Undo per peer** (Yrs undo manager with tracked origins: undo reverts *my* edits only),
   **presence** (carets, selections: Yjs awareness, not the document), **agent forks** (review an
   agent's edits before merging them).

## Step 2: over the network, and in the browser

Goal: the desktop `crdt_pair` model, a browser tab running the same model as WASM, and a Python
`pycrdt` script all edit one document live. This is the ESP32 demo minus the board: the board
later plays the desktop's part (serve the page, relay updates, run its own replica).

### The protocol: y-sync over WebSocket

The standard Yjs one (`y-protocols/sync`), so `y-websocket` and `pycrdt-websocket` clients join
unchanged. A WebSocket binary message is a var-uint message type, then its body:

- type 0 = sync. Sub-type 0 *SyncStep1* (my state vector), 1 *SyncStep2* (an update: what you
  lack), 2 *Update* (a live edit). Each payload is a var-uint length then the bytes.
- type 1 = awareness (presence): ignored in step 2, never an error.

On connect each side sends SyncStep1; a SyncStep1 is answered with SyncStep2
(`encode_since(their_state)`); SyncStep2 and Update are applied. A local edit goes out as Update.

### Pieces

- **`crdt::SyncSession`** (pure, no socket): wraps a `Doc` and a `send(bytes)` function; `start()`
  sends SyncStep1, `receive(bytes)` handles one message, local updates go out through `send`.
  Encoding and decoding are free functions, unit-tested against byte strings taken from Yjs.
  Any transport (desktop, browser, ESP-IDF) is a thin shell around it.
- **`crdt::Hub`** (desktop, over cpp-httplib's WebSocket server, already a dependency through
  `prism-net`): one HTTP port that serves a static directory (the WASM page, with the
  COOP/COEP headers threads need) and a WebSocket route `/<room>`. It relays each client's
  updates to the others and holds one `Doc` of its own, so a late joiner catches up from it and
  the desktop app's own model is a peer like the others.
- **`crdt::connect(doc, url)`**: a client session. Native: cpp-httplib's WebSocket client. Browser:
  `emscripten/websocket.h`. Same API; the browser's is the one the WASM page uses.
- **Threading:** sockets run on their own threads; received bytes cross to the logic thread through
  a `Channel`, and local updates leave through one. `Doc` stays logic-thread only.

### crdt in the browser build

yffi built by cargo for `wasm32-unknown-emscripten`. PRISM's web build uses threads, so the Rust
objects must be compiled with `+atomics,+bulk-memory` (nightly `-Zbuild-std`), or the link fails
on shared memory. Risky enough to spike first (Task 1).

### The agent

`examples/crdt_pair/agent.py`: `pycrdt` + `pycrdt-websocket` join the hub's room, print the map,
and set `gain` every second. A test runs it against a hub when `pycrdt` is installed.

## Step 3: on the board

The board is a hub with a screen: it runs its own access point, serves the page, relays the room,
and edits the same model from its touchscreen. Nothing in the protocol changes, only the transports.

- **`crdt::Room` is the relay without sockets.** A `Doc` and a map of peer ids to `send(bytes)`
  functions; `join` makes the room speak first (a SyncStep1), `receive` applies the sender's
  message and passes a SyncStep2 or Update on to the other peers, never back to the sender, and
  `leave` forgets a peer so a browser that dropped mid-message costs no further sends. Desktop
  `Hub` drives one over cpp-httplib; the board's server drives one over ESP-IDF's
  `esp_https_server`. The logic-thread rule is unchanged: socket tasks only queue bytes.
- **Its own access point, at 192.168.4.1.** SoftAP, not station mode: a phone or laptop joins the
  board's network and loads the page from the board, with no router in between.
- **HTTPS, with a certificate made at build time.** The page needs threads, and a browser hands the
  threaded WASM build its shared memory only in a secure context; `http://192.168.4.1` is not one.
  The self-signed certificate costs a one-time browser warning, which is fine on the board's own
  network.
- **The page ships in the app image, gzipped.** There is no filesystem to mount a bundle from, so
  the build embeds it and the server answers with it already compressed.
- **Yrs comes from the same wrap, cross-built.** The ESP-IDF cross file names `rust_target`
  (`riscv32imafc-esp-espidf`) and `rust_toolchain=esp`, the toolchain `espup install` puts in
  rustup, and cargo rebuilds std with `-Zbuild-std`. Upstream nightly cannot: its std pins a libc
  whose newlib-4 `stat` layout its own espidf code no longer matches
  (`.superpowers/spike-yrs-s31.md`, sections 3-4).
- **`realpath` is stubbed in the app.** ESP-IDF's newlib does not have it and Rust's `canonicalize`
  references it; the board's app returns NULL from a three-line stub.

## Restrictions users must know

- An invariant across two fields (`min <= max`) can break after a merge. Keep such pairs in one
  field, or repair them in an `observe` handler.
- `crdt::Field<std::string>` is last-writer-wins on the whole string. Character-level merging is
  `crdt::Document` (step 4).
- `crdt::Field<Struct>` replaces the whole struct; per-member merging comes from nesting
  `crdt::Field` members.

## Non-goals

- Making `core` types CRDT-aware, or any cost for apps that do not use `crdt`.
- A server: peers exchange updates; relaying them is the transport's job.

## Left to the builder

The exact `yffi` version and cargo invocation; how the echo guard is held (a flag on the `Doc`
or Yrs transaction origins); Yjs number encoding details; the re-export list for `crdt`.
