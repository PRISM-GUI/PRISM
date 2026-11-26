# Browser tests

Scripts that drive a built example page in a real browser. They are not part of `meson test`: they
need a wasm build, a static server and a Chromium binary.

## crdt_twins.mjs

`examples/crdt_twins` shows two Mixers, each with its own CRDT document, kept in step by the
y-protocols sync. The example logs every change of a synced field as one line on stdout, and the wasm
shell sends stdout to `console.log`:

```console
twin A gain=0.51
twin B gain=0.51
```

The script moves one slider, then asserts that both replicas logged the same new value, `twin A
gain=…` and `twin B gain=…` within 3 s and in either order, then does the other direction on the
right-hand replica.

### Build and serve

```console
$ meson setup builddir-wasm --cross-file cross/wasm32-emscripten.ini
$ ninja -C builddir-wasm
$ scripts/build_web_site.sh builddir-wasm _site
$ python3 -m http.server -d _site 8080
```

`crdt_twins.html` is published by `build_web_site.sh` whenever the build has it (crdt is on by
default). The page loads `coi-serviceworker.js`, which adds the `Cross-Origin-Opener-Policy` and
`Cross-Origin-Embedder-Policy` headers the threaded wasm build needs and reloads the page once on
the first visit, so any static file server works -- no special headers to configure.

### Run

The script needs `playwright-core` (no browser download: pass the executable) and a Chromium build:

```console
$ npm install playwright-core
$ node tests/web/crdt_twins.mjs http://127.0.0.1:8080/crdt_twins.html \
      ~/.cache/ms-playwright/chromium-1254/chrome-linux64/chrome
```

`--use-angle=swiftshader --enable-unsafe-swiftshader` are set for it, so it renders without a GPU.
It also needs real time: give it up to 20 s to see the app come up.

### What it does

- clicks the canvas, because that is what gives it the keyboard,
- presses Tab to walk the focus ring, which is the widget tree's depth-first order: stop 1 is Peer
  A's gain slider, stop 10 is Peer B's first channel fader,
- presses ArrowRight at those two stops (a slider moves one step), and
- reads the console: for each press it wants the same new value logged by both replicas within 3 s,
  in either order -- the local field and the one written by the arriving update are two separate
  notifications.

Exit codes: `0` both directions synced, `1` a step failed (the last console lines are printed),
`2` bad arguments.
