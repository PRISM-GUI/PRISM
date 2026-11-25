# Shared fields (crdt flavor, step 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `prism::crdt::Field<T>` and `crdt::Doc`, syncing scalar fields between replicas through Yrs.

**Architecture:** `crdt::Field<T>` is a marker deriving `core::Field<T>`. `crdt::Doc` owns a
Yrs document (C interface `yffi`), binds fields to keys of a root map, sends local edits out as
update bytes and applies remote ones with `set()`. A reflection walk binds a whole struct.

**Tech Stack:** C++26, Meson, Yrs/yffi built by cargo, doctest, magic_enum.

**Spec:** `doc/design/sync.md`

## Global Constraints

- Opt-in: nothing in `include/prism/core/` changes; apps without `crdt` pay nothing.
- `option('crdt', type: 'feature', value: 'auto')`: on when `cargo` is found and the host is not
  bare metal (`'none'`) nor `'emscripten'`.
- Rust and `libyrs.h` types never appear in `include/prism/crdt/` headers; they live in
  `src/crdt/`.
- Supported `T`: integers, floating point, `bool`, `std::string`, scoped enums. Others: a
  `static_assert` naming these.
- Enums travel by name (magic_enum); integers beyond ±2^53 travel as decimal strings.
- `crdt` re-exports the `core` names it does not replace with using-declarations, never
  `using namespace core`.
- Logic thread only, like every model value.
- No mutable `inline` variables in headers (AGENTS.md); non-template code in `src/crdt/`.
- Builds and tests run on Coder, one at a time (implementer: edit only, no builds, no git).

## Review Focus

1. **Remote apply inside a `TransactionGuard`**: `set()` defers its `on_change` until the guard
   ends, after the Doc's "applying remote" flag is cleared. Expect: still no echo. Test in Task 2.
2. **A peer writes the wrong type** (string into a double key, unknown enum name): expect the
   field keeps its value, nothing throws. Test in Task 2.
3. **Destruction order**: a `Doc` destroyed before its fields, or fields before the `Doc`: no
   crash, no use-after-free under ASAN. Test in Task 2.
4. **Equal value set**: no update emitted (core `set()` skips equal values). Test in Task 2.
5. **Large integers**: `std::int64_t` 2^60 round-trips exactly. Test in Task 2.

---

### Task 1: Build Yrs and link it

**Files:**
- Create: `subprojects/packagefiles/yffi/` (a tiny Cargo crate, `crate-type = ["staticlib"]`,
  depending on a pinned `yffi = "=<version>"`, with `Cargo.lock`), plus how `libyrs.h` is
  obtained (vendored copy matching the pinned version is acceptable; note its source).
- Modify: `meson.options` (the `crdt` option), `meson.build` (`crdt_on` next to the
  `http/io/markdown` loop, with the cargo check), `src/meson.build` (a `custom_target` running
  `cargo build --release --manifest-path ... --target-dir @PRIVATE_DIR@`, a `prism-crdt` static
  library, `prism_crdt_dep`), `tests/meson.build`.
- Test: `tests/test_crdt_smoke.cpp`

**Interfaces:**
- Produces: `prism_crdt_dep` (empty `dependency('', required: false)` when off); `src/crdt/`
  may include `libyrs.h`.

- [ ] **Step 1: Write the smoke test** — `TEST_CASE("yrs links")`: `ydoc_new()`, a root map,
  insert `"k"` = 1.5 in a write transaction, read it back, `== 1.5`; destroy everything.
- [ ] **Step 2: Wire the build.** Rust's linker needs on Linux: `-lpthread -ldl -lm`.
- [ ] **Step 3: Build on Coder** with `-Dcrdt=enabled`: `meson test -C builddir crdt_smoke` PASS.
  Also `-Dcrdt=disabled` configures and the suite is unchanged.
- [ ] **Step 4: Commit** `build(crdt): build Yrs through cargo, behind -Dcrdt`.

### Task 2: `crdt::Field`, `crdt::Doc`, manual binding

**Files:**
- Create: `include/prism/crdt/field.hpp`, `include/prism/crdt/doc.hpp`,
  `src/crdt/doc.cpp`, `tests/test_crdt_doc.cpp`

**Interfaces:**
- Consumes: `prism_crdt_dep`.
- Produces:
  ```cpp
  namespace prism::crdt {
  template <class T> struct Field : core::Field<T> {   // marker, no extra state
      using core::Field<T>::Field; using core::Field<T>::operator=;
  };
  class Doc {
  public:
      Doc(); ~Doc(); Doc(const Doc&) = delete; Doc& operator=(const Doc&) = delete;
      template <class T> void bind(std::string_view key, core::Field<T>& f);   // also takes crdt::Field
      core::SenderHub<std::span<const std::byte>>& on_update();   // a local edit, encoded (Yjs v1)
      void apply_update(std::span<const std::byte> update);
      std::vector<std::byte> state_vector() const;
      std::vector<std::byte> encode_since(std::span<const std::byte> peer_state) const;
  };
  }
  ```
  The template `bind` converts `T` to/from a small non-template `Value` variant
  (`int64, double, bool, string`) and calls a non-template `bind_value(key, getter, setter,
  SenderHub-connection)` in `doc.cpp`. Nested maps: `Doc::map(std::string_view key) -> Doc::Map`
  with the same `bind`, used by Task 3.

- [ ] **Step 1: Write failing tests** (two Docs `a`, `b`, wired `a.on_update → b.apply_update`
  and back):
  - `"set on one replica reaches the other"`: `fa = 2.5` → `fb.get() == 2.5`.
  - `"each supported type round-trips"`: `int`, `std::int64_t` (1LL<<60), `double`, `bool`,
    `std::string`, `enum class Mode {idle, run}` → equal on `b`.
  - `"an enum travels by name"`: `a.to_json()` (`std::string`, the root map as JSON, a public
    `Doc` member) contains `"run"` after `mode = Mode::run`.
  - `"no echo"`: count `b.on_update` emissions while applying `a`'s update → 0; also inside a
    `TransactionGuard`.
  - `"concurrent sets converge"`: unwired `a`,`b`, `fa=1`, `fb=2`, exchange
    `encode_since(state_vector())` both ways → `fa.get() == fb.get()`.
  - `"joining loads the existing state"`: `a` has `gain=3`; fresh `c` binds `gain` (value 0),
    applies `a.encode_since(c.state_vector())` → 3; and a bind on a key already in the map takes
    the map's value.
  - `"wrong type from a peer is ignored"`: write a string into a double key through a Doc bound
    with `Field<std::string>` under the same key → the double field keeps its value.
  - `"equal set emits nothing"`.
  - `"destruction in either order"`: Doc before field and field before Doc, no crash.
  - `"unbound crdt::Field behaves as core::Field"`: observe, set, operators.
- [ ] **Step 2: Build on Coder, run `crdt_doc`: FAIL (doesn't compile / stubs).**
- [ ] **Step 3: Implement.** Echo guard: a `applying_remote_` counter on the Doc checked by the
  field's change handler; because of review item 1, the handler compares the field's value with
  the map's current value and skips the write when equal (that also makes deferred emits safe).
  Field destroyed first: the Doc must not touch it afterwards — store the `Connection` and
  disconnect-on-destroy semantics of `SenderHub` (check `core/connection.hpp`); if a dead field
  can't be detected, document "bind fields that outlive the Doc" with a `simplify:` comment and
  test only Doc-first.
- [ ] **Step 4: Run `crdt_doc` PASS, then the full suite, and the ASAN lane if configured.**
- [ ] **Step 5: Commit** `feat(crdt): shared fields through Yrs`.

### Task 3: Flavor namespace and reflection binding

**Files:**
- Create: `include/prism/crdt/flavor.hpp` (includes `field.hpp`, `doc.hpp`; one
  using-declaration per public `prism::core` name other than `Field`, taken from the headers
  `include/prism/prism.hpp` lists under `// core`; `List` stays re-exported from `core` until
  step 3), `include/prism/crdt/bind.hpp`, `tests/test_crdt_flavor.cpp`,
  `tests/crdt_flavor_model.inc`.

**Interfaces:**
- Produces: `template <class Model> void crdt::bind(Doc&, Model&)` — reflection only
  (`#if __cpp_impl_reflection`): each `crdt::Field` member binds under its member name; a nested
  struct member containing crdt fields binds into `doc.map(name)`; `core::Field` members are
  skipped (stay local).

- [ ] **Step 1: Failing tests:**
  - `"flavor swap"`: one header-style body written the way users write it, included twice:
    `namespace local { namespace model = prism::core; #include "crdt_flavor_model.inc" }` and
    `namespace shared { namespace model = prism::crdt; #include "crdt_flavor_model.inc" }`. The
    `.inc` declares `struct M { model::Field<double> gain; model::Field<int> count; }` and a
    `model::Derived` over them, and a function `check(M&)` doing set/observe/derived asserts;
    the test calls `check` for both flavors.
  - `"bind walks the struct"` (reflection): `struct S { crdt::Field<int> a; core::Field<int>
    local; struct In { crdt::Field<bool> on; } in; }` — two replicas: `a` and `in.on` sync,
    `local` does not.
- [ ] **Step 2: Build, FAIL. Step 3: implement. Step 4: suite PASS; also the no-reflection
  syntax check (memory: check-no-reflection-build) for `flavor.hpp`/`doc.hpp`.**
- [ ] **Step 5: Commit** `feat(crdt): the crdt flavor namespace and reflected binding`.

### Task 4: Proof app and docs

**Files:**
- Create: `examples/crdt_pair/main.cpp` (+ `meson.build` entry, built only when `crdt_on` and
  SDL): one struct (`gain` slider, `mode` dropdown, `name` text, `armed` checkbox, plus a local
  `core::Field<int>` counter), two windows, two Docs wired by passing updates in-process.
- Create: `doc/integrations/crdt.md` (manual page: the alias swap, `Doc`, `bind`, the
  restrictions list from the spec, a complete example) and link it from `doc/index.md`.

- [ ] **Step 1: Write both.** **Step 2:** build on Coder; run the example headless for a few
  frames if it supports it, else note it; `sphinx-build doc <out>` with no warnings.
- [ ] **Step 3: Commit** `feat(examples): crdt_pair, two windows sharing one model`.
