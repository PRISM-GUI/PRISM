#!/usr/bin/env node
// Drives examples/crdt_twins in a real browser: the page shows two Mixers, each its own CRDT
// document, wired through the y-protocols sync. The example logs every change of a synced field as
// `twin <A|B> <field>=<value>`; this script moves a slider on one replica and reads the line the
// other replica prints.
//
// How it finds the sliders: no coordinates. A click on the canvas gives it the keyboard, and Tab
// walks the focus ring, which is the widget tree's depth-first order. The model is built as
// [Peer A: gain, ch1..ch4, mode, armed, Clap] then [Peer B: the same], so ring stop 1 is A's gain
// and stop 10 is B's ch1; ArrowRight on a slider is one step. Those stops are where the layout puts
// them, which is also what a user reaches by pressing Tab.
//
// usage: node tests/web/crdt_twins.mjs <page-url> <chromium-executable>
import { chromium } from 'playwright-core';

const usage = `usage: node tests/web/crdt_twins.mjs <page-url> <chromium-executable>

  node tests/web/crdt_twins.mjs http://127.0.0.1:8080/crdt_twins.html \\
      ~/.cache/ms-playwright/chromium-1254/chrome-linux64/chrome`;

const [url, executablePath] = process.argv.slice(2);
if (!url || !executablePath) {
  console.error(usage);
  process.exit(2);
}

const READY = 'twin ready';
const CHANGE = /^twin (A|B) (\w+)=(.*)$/;
const STEP_WAIT_MS = 3000;
const START_WAIT_MS = 20000;
const SETTLED_MS = 1000; // how long the app must have been up: the service worker reloads the page once

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function until(check, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const found = check();
    if (found != null) return found;
    if (Date.now() > deadline) return null;
    await sleep(50);
  }
}

const lines = []; // { text, at }: every console line, in order
let page;
let browser;

const changesSince = (mark) =>
  lines
    .slice(mark)
    .map(({ text }) => CHANGE.exec(text))
    .filter(Boolean)
    .map(([, peer, field, value]) => ({ peer, field, value }));

// One key press produces two lines, one per replica, but which replica's field setter runs first
// depends on where the change notification is delivered, so look for the value the driven replica
// logged and the same value on the other one, in either order.
function changeOnBoth(mark, field, driven) {
  const other = driven === 'A' ? 'B' : 'A';
  const changes = changesSince(mark);
  const mine = changes.filter((c) => c.field === field && c.peer === driven);
  const theirs = changes.filter((c) => c.field === field && c.peer === other);
  for (const change of mine) {
    if (theirs.some((t) => t.value === change.value)) return change.value;
  }
  return null;
}

async function fail(reason) {
  console.error(`FAIL: ${reason}`);
  if (lines.length > 0) {
    console.error('last lines from the page:');
    for (const { text } of lines.slice(-12)) console.error(`  ${text}`);
  }
  await browser?.close().catch(() => {});
  process.exit(1);
}

// Walks `tabs` steps along the focus ring, then ArrowRight moves that slider one step; both
// replicas must log the same new value within STEP_WAIT_MS.
async function drive(tabs, field, driven, stop) {
  for (let i = 0; i < tabs; i++) {
    await page.keyboard.press('Tab');
    await sleep(60);
  }
  const mark = lines.length;
  await page.keyboard.press('ArrowRight');
  const value = await until(() => changeOnBoth(mark, field, driven), STEP_WAIT_MS);
  if (value === null) {
    await fail(
      `focus ring stop ${stop} (peer ${driven}'s ${field}) did not reach both replicas within ` +
        `${STEP_WAIT_MS} ms`,
    );
  }
  const other = driven === 'A' ? 'B' : 'A';
  console.log(`ok: peer ${driven} ${field}=${value}, and peer ${other} ${field}=${value}`);
  return value;
}

try {
  browser = await chromium.launch({
    executablePath,
    args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'],
  });
  page = await browser.newPage({ viewport: { width: 1200, height: 700 } });
  page.on('console', (message) => lines.push({ text: message.text().trimEnd(), at: Date.now() }));
  page.on('pageerror', (error) => lines.push({ text: `page error: ${error.message}`, at: Date.now() }));

  await page.goto(url, { waitUntil: 'load', timeout: START_WAIT_MS });
  await page.waitForSelector('#canvas', { timeout: START_WAIT_MS });

  // The page loads coi-serviceworker.js, which installs the COOP/COEP headers the threaded build
  // needs and reloads the page once; a ready line that has stood for a second means that is over.
  const running = await until(() => {
    const ready = lines.filter((line) => line.text === READY).at(-1);
    return ready && Date.now() - ready.at > SETTLED_MS ? ready.at : null;
  }, START_WAIT_MS);
  if (running === null) {
    await fail(`the app did not start within ${START_WAIT_MS} ms (no "${READY}" line)`);
  }
  console.log('ok: the app is up');

  // The canvas needs the keyboard before SDL sees Tab and the arrows. The click lands in the top
  // corner, away from every control, so it changes nothing; focus() covers a browser that does not
  // hand the canvas the keyboard on a click.
  await page.locator('#canvas').click({ position: { x: 4, y: 4 } });
  if ((await page.evaluate(() => document.activeElement?.id)) !== 'canvas') {
    await page.evaluate(() => document.getElementById('canvas')?.focus());
  }
  await sleep(250);

  await drive(1, 'gain', 'A', 1); // stop 1: Peer A's gain slider (left half)
  await drive(9, 'ch1', 'B', 10); // stop 10: Peer B's ch1 fader (right half)

  console.log('PASS: each replica followed the other through the sync session');
  await browser.close();
  process.exit(0);
} catch (error) {
  await fail(error.message);
}
