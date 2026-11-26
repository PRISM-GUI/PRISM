# CRDT on the board — Implementation Plan

**Goal:** the ESP32-S31-Korvo-1 shows the crdt_pair Mixer on its screen, runs its own Wi-Fi
access point, serves the browser build over HTTPS and relays CRDT updates over WSS, so a phone
or laptop that joins the board's network edits the same model live.

**Architecture:** the hub's relay logic leaves cpp-httplib and becomes `crdt::Room` (pure, no
sockets: peers are ids and send functions). The desktop Hub and the board's server both drive
a Room. Yrs is cross-built for `riscv32imafc-esp-espidf` by the existing yffi wrap, steered by
the Meson cross file. The board serves the gzipped bundle embedded in the app, with ESP-IDF's
`esp_https_server` (WebSocket on), a certificate made at build time, SoftAP at 192.168.4.1.

**Spec:** doc/design/sync.md (Step 2, plus the new "Step 3: on the board" section from Task 1).
Spikes: .superpowers/spike-yrs-s31.md, .superpowers/spike-yrs-wasm.md.

## Global Constraints

- User decisions (2026-11-26): HTTPS with a self-signed certificate (browser threads need a
  secure context); the board's own access point, no station mode.
- Rust for the S31: the esp-rs toolchain installed by `espup` (`RUSTUP_TOOLCHAIN=esp`), because
  upstream nightly's std pins a libc whose newlib `stat` layout does not build for espidf.
  `-Zbuild-std=std,panic_abort`. ESP-IDF's newlib lacks `realpath()`: the board app stubs it.
- Same model keys on both sides: the board includes PRISM's `examples/crdt_pair/model.hpp`.
- Every Doc and Room call stays on the logic thread; socket tasks only queue bytes.
- Builds and tests run on Coder; implementers edit only.

## Review Focus

1. A browser that disconnects mid-message (Wi-Fi drop): the Room must forget it on `leave`, and
   no send may reach a closed socket.
2. A second and third browser joining late: each catches up (step1/step2) and sees later edits.
3. Five browsers at once: the access point's station limit and httpd's socket limit must allow
   it (`max_open_sockets`, SoftAP max connections).
4. The board's own edit (touch) reaches every browser; a browser edit reaches the screen.
5. A page reload: the old socket closes, the new one joins; no leaked session.

### Task 1 (PRISM): crdt::Room, the Hub on top, yffi for ESP-IDF
Brief: .superpowers/task-board-room.md

### Task 2 (prism-esp32): the crdt firmware
Brief: .superpowers/task-board-esp.md

### Task 3: on the board
Build on Coder, flash, join the AP from a phone and a laptop, check the Review Focus by hand,
record numbers (flash, RAM, latency) in doc/design/sync.md.
