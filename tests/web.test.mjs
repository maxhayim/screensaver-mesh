// The web version, in node with a stub canvas: node tests/web.test.mjs
// Starts tools/mock_meshmonitor.py for the live test (needs python3).
import { spawn } from "node:child_process";
import assert from "node:assert/strict";
import { setTimeout as sleep } from "node:timers/promises";
import { ID, NAME, SETTINGS, DEFAULTS, cleanSettings, presetOf, describeError, createSaver } from "../web/index.js";
import PRESETS from "../web/presets.js";

let failures = 0;
async function test(name, fn) {
  try {
    await fn();
    console.log(`ok   ${name}`);
  } catch (error) {
    failures++;
    console.log(`FAIL ${name}\n     ${error.message}`);
  }
}

// A canvas whose 2D context records what each frame draws.
function stubCanvas(width = 800, height = 500) {
  const frames = [];
  let current = null;
  const ctx = {
    fillStyle: "", strokeStyle: "", lineWidth: 1, lineCap: "butt", font: "", textAlign: "left",
    textBaseline: "alphabetic", direction: "ltr", letterSpacing: "0px",
    setTransform() {
      current = { at: performance.now(), lines: 0, filled: {}, rings: 0, text: [] };
      frames.push(current);
    },
    fillRect() {}, beginPath() {}, moveTo() {}, lineTo() {}, arc() {}, save() {}, restore() {},
    stroke() { if (current) current.lines++; },
    fill() { if (current) current.filled[this.fillStyle] = (current.filled[this.fillStyle] ?? 0) + 1; },
    fillText(text) { current?.text.push(text); },
    measureText(text) { return { width: text.length * 6 }; },
  };
  return { canvas: { clientWidth: width, clientHeight: height, width: 0, height: 0, getContext: () => ctx }, frames };
}

const RED = "rgba(255,0,0,1)"; // the packets in the live test

await test("exports the shared interface", () => {
  assert.equal(ID, "mesh");
  assert.equal(NAME, "Mesh");
  assert.equal(typeof createSaver, "function");
  assert.deepEqual(Object.keys(DEFAULTS), SETTINGS.map((s) => s.key));
  for (const s of SETTINGS) assert.ok(["preset", "color", "range", "toggle", "choice", "text", "password"].includes(s.type), s.key);
});

await test("defaults match the native savers", () => {
  assert.equal(DEFAULTS.background, "#0b0b0a");
  assert.equal(DEFAULTS.dots, "#eeebe4");
  assert.equal(DEFAULTS.lines, "#eeebe4");
  assert.equal(DEFAULTS.packets, "#f06a2a");
  assert.equal(DEFAULTS.clock, true);
  assert.equal(DEFAULTS.use24Hour, false);
  assert.equal(DEFAULTS.label, "mesh");
  assert.equal(DEFAULTS.source, "default");
  assert.equal(DEFAULTS.preset, "Default");
});

await test("every preset is in SETTINGS", () => {
  const entry = SETTINGS.find((s) => s.type === "preset");
  assert.deepEqual(entry.presets.map((p) => p.name), ["Default", "Meshtastic", "Green terminal", "Amber terminal", "Paper"]);
  assert.deepEqual(entry.presets, PRESETS);
  for (const p of PRESETS) assert.equal(presetOf(p.colors), p.name);
});

await test("cleanSettings fixes bad input", () => {
  assert.deepEqual(cleanSettings(null), DEFAULTS);
  assert.deepEqual(cleanSettings("nope"), DEFAULTS);
  const s = cleanSettings({
    background: "red", dots: "#ABCDEF", clock: "yes", use24Hour: true, label: "weird",
    labelText: "x".repeat(200), source: "  ", server: "  mesh.example.com  ", extra: 1,
  });
  assert.equal(s.background, DEFAULTS.background);
  assert.equal(s.dots, "#abcdef");
  assert.equal(s.clock, true);
  assert.equal(s.use24Hour, true);
  assert.equal(s.label, "mesh");
  assert.equal(s.labelText.length, 60);
  assert.equal(s.source, "default");
  assert.equal(s.server, "mesh.example.com");
  assert.equal(s.preset, "Custom");
  assert.equal("extra" in s, false);
  const picked = cleanSettings({ preset: "Paper" });
  assert.equal(picked.background, "#f4f1ea");
  assert.equal(picked.preset, "Paper");
});

await test("describeError explains failures", () => {
  assert.match(describeError({ status: 401 }), /token/);
  assert.match(describeError({ status: 404 }), /source/i);
  assert.match(describeError(new TypeError("Failed to fetch"), { server: "http://10.0.0.5:8080" }), /ALLOWED_ORIGINS/);
});

await test("simulated: draws nodes, links, and the clock", async () => {
  const { canvas, frames } = stubCanvas();
  const statuses = [];
  const saver = createSaver(canvas, { settings: {}, onStatus: (s) => statuses.push(s), now: () => new Date(2026, 0, 1, 19, 49) });
  await sleep(1200);
  saver.destroy();
  const f = frames.at(-1);
  assert.ok(frames.length > 20, `only ${frames.length} frames`);
  assert.ok(f.lines > 10, `only ${f.lines} lines`);
  assert.ok((f.filled["rgba(238,235,228,0.75)"] ?? 0) >= 14, "dots missing");
  assert.deepEqual(f.text, ["Mesh", "7:49 PM"]);
  assert.deepEqual(statuses, ["simulated"]);
  assert.equal(canvas.width, 800 * (globalThis.devicePixelRatio || 1));
});

await test("label and 24-hour clock", async () => {
  const { canvas, frames } = stubCanvas();
  const saver = createSaver(canvas, {
    settings: { use24Hour: true, label: "user" }, userName: "Max Hayim", now: () => new Date(2026, 0, 1, 7, 5),
  });
  await sleep(300);
  saver.update({ label: "custom", labelText: "KO4XYZ" });
  await sleep(200);
  saver.destroy();
  assert.ok(frames.some((f) => f.text.join("|") === "Max Hayim|07:05"));
  assert.deepEqual(frames.at(-1).text, ["KO4XYZ", "07:05"]);
});

await test("live: goes live, animates new messages, doesn't replay old ones", async () => {
  const port = 8795;
  const mock = spawn("python3", ["tools/mock_meshmonitor.py", String(port), "--backlog", "5"], { stdio: "ignore" });
  try {
    await sleep(800);
    const { canvas, frames } = stubCanvas();
    const statuses = [];
    const start = performance.now();
    const saver = createSaver(canvas, {
      settings: { server: `127.0.0.1:${port}`, token: "mm_v1_test", packets: "#ff0000" },
      onStatus: (s) => statuses.push(s),
    });
    await sleep(7500);
    saver.destroy();
    assert.deepEqual(statuses, ["connecting", "live"]);
    const early = frames.filter((f) => f.at - start < 3500);
    const late = frames.filter((f) => f.at - start > 4000);
    assert.ok(early.length && late.length);
    assert.equal(Math.max(...early.map((f) => f.filled[RED] ?? 0)), 0, "replayed messages from the first answer");
    assert.ok(Math.max(...late.map((f) => f.filled[RED] ?? 0)) > 0, "no packets for new messages");
    assert.equal(Math.max(...late.map((f) => f.filled["rgba(238,235,228,0.75)"] ?? 0)), 40, "not the mock's 40 nodes");
    assert.ok(late.at(-1).text[0].endsWith("· live"));
  } finally {
    mock.kill();
  }
});

await test("offline: a server that isn't there stays simulated", async () => {
  const { canvas } = stubCanvas();
  const statuses = [];
  const saver = createSaver(canvas, { settings: { server: "http://127.0.0.1:9", token: "x" }, onStatus: (s) => statuses.push(s) });
  await sleep(500);
  saver.destroy();
  assert.deepEqual(statuses, ["connecting"]);
});

if (failures) {
  console.log(`${failures} failure(s)`);
  process.exit(1);
}
console.log("all web tests passed");
process.exit(0);
