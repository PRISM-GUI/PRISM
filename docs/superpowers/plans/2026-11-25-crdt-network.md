# Shared models over the network (crdt step 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** desktop, browser (WASM) and a Python `pycrdt` agent edit one PRISM model live.

**Architecture:** a pure y-sync `SyncSession` over a `Doc`; transports are thin shells: a
desktop `Hub` (cpp-httplib: static files + WebSocket relay + its own Doc), a native client, a
browser client (emscripten WebSocket). yffi is also built for the web.

**Tech Stack:** C++26, Yrs/yffi, cpp-httplib 0.66 (WebSocket), emscripten websocket API, pycrdt.

**Spec:** `doc/design/sync.md`, section "Step 2".

## Global Constraints

- Protocol: y-protocols sync exactly (var-uint type; 0 = sync with sub-types 0 SyncStep1,
  1 SyncStep2, 2 Update; 1 = awareness, ignored). Interop with `pycrdt-websocket` is the test.
- `Doc` stays logic-thread only; socket threads hand bytes over through `core::Channel`.
- Hub and native client live in `prism-crdt` only when `prism_net_dep` is found; the browser
  client only under emscripten. `SyncSession` is always built with `prism-crdt`.
- Hub serves its static directory with `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp`.
- Builds/tests on Coder, one at a time; implementers edit only.

## Review Focus

1. A client that disconnects mid-session: the hub drops it, others keep syncing. Task 3.
2. A late joiner gets the full state from the hub's Doc, not from other clients. Task 3.
3. Malformed or truncated messages (bad var-uint, length past the end): ignored, no crash. Task 2.
4. An awareness message (type 1) from a JS/Python peer: ignored, session continues. Task 2.
5. Hub shutdown with clients connected: joins its threads, no hang (TSAN lane). Task 3.

---

### Task 1: Spike — yffi in the web build

Throwaway, on Coder in `~/spike-yrs-wasm`: build yffi for `wasm32-unknown-emscripten` with
`-C target-feature=+atomics,+bulk-memory` (nightly `-Zbuild-std=std,panic_abort`), link it into a
tiny emscripten program built with PRISM's web flags (`-pthread`, JSPI: copy from `meson.build`
and `scripts/build_web_site.sh`), run it under node: `ydoc_new` + a map write/read. Report
`.superpowers/spike-yrs-wasm.md`: commands that work, size added, verdict. If yes: Task 5 wires
it into Meson (`build.py` takes a `--target` and extra RUSTFLAGS from the cross build).

### Task 2: `SyncSession` and the y-sync codec

**Files:** Create `include/prism/crdt/sync.hpp`, `src/crdt/sync.cpp`, `tests/test_crdt_sync.cpp`.

**Interfaces — Produces:**
```cpp
namespace prism::crdt {
enum class SyncKind : std::uint8_t { step1 = 0, step2 = 1, update = 2 };
std::vector<std::byte> encode_sync(SyncKind, std::span<const std::byte> payload);
struct SyncMessage { SyncKind kind; std::vector<std::byte> payload; };
std::optional<SyncMessage> decode_sync(std::span<const std::byte> message);   // nullopt: not sync, or malformed
class SyncSession {
public:
    SyncSession(Doc& doc, std::function<void(std::span<const std::byte>)> send);
    void start();                                  // sends SyncStep1
    void receive(std::span<const std::byte> message);
};
}
```
- [ ] Tests first: `encode_sync(step1, {})` == bytes `{0, 0, 0}`; a round trip per kind;
  decode of `{1, ...}` (awareness) → nullopt; truncated length / overlong var-uint → nullopt;
  two Docs each with a SyncSession whose `send` feeds the other's `receive`: `start()` both,
  then a set on one reaches the other; a session receiving step1 answers with step2.
  Var-uint values ≥ 128 (a 300-byte payload) round-trip.
- [ ] Build red, implement, suite green, commit `feat(crdt): y-sync sessions`.

### Task 3: `Hub` and native `connect`

**Files:** Create `include/prism/crdt/hub.hpp`, `src/crdt/hub.cpp`, `tests/test_crdt_hub.cpp`.

**Interfaces — Produces:**
```cpp
namespace prism::crdt {
class Hub {
public:
    struct Options { std::string host = "0.0.0.0"; std::uint16_t port = 0; std::string static_dir; };
    explicit Hub(Doc& doc, Options);   // starts serving; port 0 picks one
    ~Hub();                            // stops and joins
    std::uint16_t port() const;
    void drain();                      // logic thread: apply what clients sent, relay, send local edits
};
class Client {
public:
    Client(Doc& doc, std::string url);  // ws://host:port/room ; connects on its own thread
    ~Client();
    void drain();                       // logic thread
    bool connected() const;
};
}
```
`drain()` is how bytes reach the logic thread (Channel inside); a `simplify:` comment if it stays
a manual call instead of the app's tick.
- [ ] Tests first: hub + two clients on localhost: a set on client A's Doc reaches B (drain all
  in a loop with a deadline); a client joining after edits gets them; a client destroyed
  mid-session leaves the other syncing; the hub's own Doc edits reach clients; `static_dir`
  file served with COOP/COEP headers (cpp-httplib client GET).
- [ ] Build red, implement, suite green (also TSAN if the lane is configured), commit
  `feat(crdt): a WebSocket hub and client`.

### Task 4: The Python agent

**Files:** Create `examples/crdt_pair/agent.py`, `tests/test_crdt_agent.py` (+ meson test
registered only when `python3 -c "import pycrdt, pycrdt_websocket"` works).
- [ ] agent.py: connect to `ws://host:port/room` with pycrdt-websocket, print the root map
  `model`, set `gain` to a value given on the command line, exit.
- [ ] The test starts a small C++ hub program (tests/crdt_hub_fixture.cpp: a Doc with a bound
  `gain`, runs a Hub, prints the port, waits until `gain` changes, prints it, exits), runs
  agent.py against it, and checks the printed value. Commit `feat(crdt): a Python agent peer`.

### Task 5: The browser

Depends on Task 1's verdict.
- [ ] Meson: crdt in the emscripten build (cargo target + RUSTFLAGS from Task 1).
- [ ] `Client` under emscripten through `emscripten/websocket.h` (same header API).
- [ ] `examples/crdt_pair`: a `--hub PORT --static DIR` mode on desktop (one window + Hub) and a
  web build (one window + Client to the page's own origin). `scripts/build_web_site.sh` builds it.
- [ ] Check (Claude, not the implementer): desktop hub + headless Chromium page + agent.py, all
  three converge. Commit `feat(examples): crdt_pair across desktop, browser and Python`.
- [ ] Manual page `doc/integrations/crdt.md`: the network section.
