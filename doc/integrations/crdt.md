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

What a `crdt::Field` holds is a plain value: an integer, a floating point number, a `bool`, a `std::string`, or a scoped enum. It also takes a widget type: one with a public `value` member of one of those shares that member only, so a slider's number travels while its range and caption stay local. Anything else is a compile error naming the supported ones.

## A Doc is one replica

```cpp
model::Doc doc;                          // this peer's document

doc.bind("gain", experiment.gain);       // manual tier: works without reflection
model::bind(doc, experiment);            // reflection tier: every crdt::Field member, by its name
```

The manual tier is one line per field, and `doc.map("in").bind("on", on)` binds under a prefix. The reflection tier walks the struct; a nested struct contributes a dotted prefix (`in.on`, `in.deeper.x`), and a `core::Field` member is skipped and stays local.

A widget-typed field binds like any other: its `value` member is what the map holds, and the rest of the widget never enters the document. A window whose UI comes from reflection (no `view()`) can annotate a number with `[[=core::range<min, max, step>]]` instead, and get a slider bound straight to the shared field.

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

## Over the network

Step 2 turns a `Doc` into something a network carries (`doc/design/sync.md`). The pieces are
`crdt::Hub` and `crdt::Client`: both speak the y-protocols sync messages, so the other end can be
any Yjs implementation. `Hub` comes with `prism-net`; `Client` exists both natively and in the
browser.

`crdt::Room` is the relay underneath both: a `Doc`, and peers that are each an id and a
`send(bytes)` function. `join` makes the room speak first, `receive` applies the sender's message
and relays it to the other peers, `leave` forgets one. When your transport is not cpp-httplib — a
board's `esp_https_server`, a socket library of your own — drive a `Room` yourself, on the logic
thread.

A hub serves one `Doc` on one port: a WebSocket route at `/<room>`, and, when `static_dir` is set,
the directory the page that joins it was built into.

```cpp
crdt::Hub hub(experiment.doc, {.port = 8000, .static_dir = "builddir-wasm/examples/crdt_pair"});
std::printf("ws://127.0.0.1:%d/room\n", hub.port());   // port 0 picks a free one
```

A peer joins with a `Client`:

```cpp
crdt::Client client(experiment.doc, "ws://127.0.0.1:8000/room");
```

Both carry bytes across threads: socket threads only enqueue what arrived, the `Doc` stays on the
logic thread. So both need draining there, and `set_waker` is how the logic thread learns it has
something to do:

```cpp
const auto drain = [&client] { client.drain(); };
client.set_waker([poster = ctx.poster(), drain] { poster(drain); });
ctx.post_deferred(drain);   // covers what arrived before the waker existed
```

`examples/crdt_pair/model.hpp` has exactly that as `drain_on_logic_thread()`, used by both of its
mains. A `Hub` works the same way: `hub.drain()` applies what clients sent and relays it, and a
hub's own edits leave from that same call.

The other end does not have to be PRISM. `examples/crdt_pair/agent.py` is a short pycrdt program
that joins a room, prints the document as one line of JSON, and can edit one key:

```console
$ python3 examples/crdt_pair/agent.py ws://127.0.0.1:8000/room 0.75
{}
SET gain=0.75
```

The map starts empty because binding never writes, and the agent is a peer like any other: there is
no PRISM code in it.

### What a hub refuses, and what it does not

- It binds `127.0.0.1` unless `Options::host` says otherwise: serving the network is an explicit
  opt-in.
- A browser upgrade must carry an `Origin` the hub knows. An `Origin` equal to the request's own
  `Host` counts only when that host is an IP literal or `localhost` -- a DNS-rebinding page reaches
  the port under its own name, so both agree and are refused. Any other page origin has to be listed
  in `Options::allowed_origins`. A native client (ours, pycrdt) sends no `Origin` at all, and no
  page can forge the absence, so that is accepted.
- A served page carries `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp`, which the threaded wasm build needs for shared
  memory.
- There is no authentication yet: anyone who can reach the port can join. `hub.hpp` marks where the
  missing token goes -- a per-session value in the URL, checked before the upgrade, once a hub is
  exposed beyond one machine.
- A client that stops reading is dropped instead of being allowed to stall the logic thread: its
  outgoing queue is bounded by bytes, and the socket has a write timeout.

### The browser

The browser build can host the same model, but it is an opt-in:

```console
$ meson setup builddir-wasm --cross-file <wasm cross file> -Dcrdt=enabled
$ PRISM_WEB_CRDT=1 scripts/build_web_site.sh builddir-wasm _site
```

`auto` does not turn crdt on for emscripten: yffi there is a wasm cross build whose std has to be
rebuilt with atomics, so it needs a nightly `cargo` with the `rust-src` component
(`subprojects/packagefiles/yffi/`). `-Dcrdt=enabled` says you have one. `examples/crdt_pair` then
builds its page instead of its desktop window, and `scripts/build_web_site.sh` publishes that page
only with `PRISM_WEB_CRDT=1`: a page that joins a hub is only useful next to one.

```console
$ builddir/examples/crdt_pair/crdt_pair --hub 8000 --static _site
$ $BROWSER http://127.0.0.1:8000/
```

The page joins `ws://<page host>/room`, so open it from the hub that served it, and both sides edit
the same `Mixer`. `--listen ADDR` moves the hub off loopback when another machine has to reach it.

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
| The flavor | `-Dcrdt=auto` (the browser: `-Dcrdt=enabled`) | `prism_crdt_dep` |
| The engine | (with the flavor) | built by `cargo` from the pinned `yffi` wrap |

`auto` builds it when `cargo` is found and the target is hosted, and it is off on bare metal. A
browser build opts in with `-Dcrdt=enabled`, because `yffi` there needs a nightly toolchain with
`rust-src` (see [The browser](#the-browser)). Everything is behind `prism_crdt_dep`: a program that
does not link it never sees Yrs, and no Rust type appears in a PRISM header.

## A complete example

`examples/crdt_pair/` is two windows of one model in one process, each window its own `Doc`, the two Docs wired as above. Its struct is the whole of the crdt API in use:

```cpp
namespace model = prism::crdt;
enum class Mode { idle, run, hold };

struct Mixer {
    model::Field<ui::Slider<double>> gain{
        {.value = 0.5, .min = 0.0, .max = 1.0, .step = 0.01, .label = "Gain"}};
    // ch1..ch4: model::Field<ui::Slider<int, ui::Orientation::Vertical>>, 0..100
    model::Field<ui::Dropdown<Mode>> mode{{.value = Mode::idle}};
    model::Field<bool> armed{false};
    model::Field<int> claps{0};

    core::Field<ui::Button> clap{{.text = "Clap"}};   // never bound: this window alone
    core::Field<ui::Label<>> ch1_label{{.value = "1"}};

    model::Doc doc;

    void attach(app::AppContext&) {
        model::bind(doc, *this);        // without reflection: doc.bind("gain", gain), ...
    }

    void view(app::ViewBuilder& vb);
};
```

Drag the gain in one window and the other follows, still labelled with its own range. The Clap
button itself is a `core::Field` and never bound, while the count it increments is shared: a local
control driving a shared value. That is the point of the flavor — what is shared, and what is not,
is written in the type. Two claps at once on two peers count once, since the count is a register.

With `--hub 8000` it is one window instead, and its `Doc` is the room a hub serves: a browser peer
built from the same sources joins it (see [Over the network](#over-the-network)).
