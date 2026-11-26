#!/bin/bash
# Assemble the GitHub Pages site from an emscripten build of the examples.
# Usage: scripts/build_web_site.sh [wasm-builddir] [outdir]
# A crdt build (meson -Dcrdt=enabled) also has the browser peer: set PRISM_WEB_CRDT=1 here to
# publish it. It is a page that joins a hub, so it needs the desktop `crdt_hub` to talk to.
set -euo pipefail

builddir="${1:-builddir-wasm}"
outdir="${2:-_site}"
# page:screenshot (the system monitor's capture keeps its README name)
examples=(widget_gallery:widget_gallery model_notes:model_notes model_system_monitor:system_monitor perf_lab:perf_lab model_lab_bench:lab_bench model_synth_rack:synth_rack model_data_studio:data_studio model_task_runner:task_runner)
if [ "${PRISM_WEB_CRDT:-0}" = 1 ]; then
    examples+=(crdt_pair:)
    crdt_card='<a class="card" href="crdt_pair.html"><div><h2>Browser peer</h2><span>One window whose model is a crdt::Doc synced over a WebSocket. Start the desktop crdt_hub (it serves ./web), open this page from that hub, and edit either side.</span></div></a>'
fi

coi_version=0.1.7
coi_sha256=03d2f35be3249f5eeb4e214c5daf2be8067e2b24630aaef26a1247b723f2d05d

rm -rf "$outdir"
mkdir -p "$outdir/screenshots"
cp examples/web/index.html "$outdir/"
if [ "${PRISM_WEB_CRDT:-0}" = 1 ]; then
    sed -i "s|<!-- crdt:card -->|${crdt_card}|" "$outdir/index.html"
fi

curl -sfL "https://cdn.jsdelivr.net/npm/coi-serviceworker@${coi_version}/coi-serviceworker.min.js" \
    -o "$outdir/coi-serviceworker.js"
echo "${coi_sha256}  $outdir/coi-serviceworker.js" | sha256sum -c --quiet

for entry in "${examples[@]}"; do
    name="${entry%%:*}" shot="${entry##*:}"
    cp "$builddir/examples/$name/$name".{html,js,wasm,data} "$outdir/"
    if [ -n "$shot" ]; then
        cp "doc/screenshots/$shot.svg" "$outdir/screenshots/"
    fi
done

# prism-trace's page: reads a board's profile dump over Web Serial, hands it to Perfetto.
cp tools/prism-trace/web/trace.html "$outdir/"
cp "$builddir/tools/prism-trace/web/prism-trace-web".{js,wasm} "$outdir/"

echo "Site assembled in $outdir/"
