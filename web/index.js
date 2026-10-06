// Mesh for web pages: the native savers' C core, compiled to WebAssembly (web/bridge.c),
// drawn into a canvas. No dependencies. See docs/GUIDE.md ("Web").
import WASM from "./wasm.js";
import PRESETS from "./presets.js";

export const ID = "mesh";
export const NAME = "Mesh";

const BASE = PRESETS[0].colors;

// The settings, in the order and with the labels of the native Options sheet
// (macos/ConfigureSheet.swift). A page builds its settings UI from this.
export const SETTINGS = [
  { key: "server", label: "Server", type: "text", default: "", group: "MeshMonitor", placeholder: "https://meshmonitor.example.com" },
  { key: "token", label: "API token", type: "password", default: "", group: "MeshMonitor", placeholder: "mm_v1_…" },
  { key: "source", label: "Source", type: "text", default: "default", group: "MeshMonitor", placeholder: "default" },
  { key: "preset", label: "Preset", type: "preset", presets: PRESETS, default: PRESETS[0].name, group: "Colors" },
  { key: "background", label: "Background", type: "color", default: BASE.background, group: "Colors" },
  { key: "dots", label: "Dots", type: "color", default: BASE.dots, group: "Colors" },
  { key: "lines", label: "Lines", type: "color", default: BASE.lines, group: "Colors" },
  { key: "packets", label: "Packets", type: "color", default: BASE.packets, group: "Colors" },
  { key: "clock", label: "Show the clock", type: "toggle", default: true, group: "Clock" },
  { key: "use24Hour", label: "24-hour time", type: "toggle", default: false, group: "Clock" },
  { key: "label", label: "Label", type: "choice", options: [["mesh", "Mesh"], ["user", "Your name"], ["custom", "Custom text"]], default: "mesh", group: "Clock" },
  { key: "labelText", label: "Text under the clock", type: "text", maxLength: 60, default: "", showIf: { label: "custom" }, group: "Clock" },
];

export const DEFAULTS = Object.fromEntries(SETTINGS.map((s) => [s.key, s.default]));

const COLOR_KEYS = ["background", "dots", "lines", "packets"];
const HEX = /^#[0-9a-f]{6}$/i;

/** The matching preset's name, or "Custom". Picking a preset sets the color keys. */
export function presetOf(settings) {
  const match = PRESETS.find((p) => COLOR_KEYS.every((k) => String(settings?.[k] ?? "").toLowerCase() === p.colors[k].toLowerCase()));
  return match ? match.name : "Custom";
}

/** Any saved object → valid settings. Unknown keys are dropped; bad values fall back to DEFAULTS. */
export function cleanSettings(raw) {
  const input = raw && typeof raw === "object" ? raw : {};
  const out = {};
  for (const s of SETTINGS) {
    const v = input[s.key];
    switch (s.type) {
      case "color":
        out[s.key] = typeof v === "string" && HEX.test(v.trim()) ? v.trim().toLowerCase() : s.default;
        break;
      case "toggle":
        out[s.key] = typeof v === "boolean" ? v : s.default;
        break;
      case "choice":
        out[s.key] = s.options.some(([id]) => id === v) ? v : s.default;
        break;
      case "text":
      case "password": {
        let text = typeof v === "string" ? v.trim() : s.default;
        if (s.maxLength) text = [...text].slice(0, s.maxLength).join("");
        out[s.key] = text;
        break;
      }
      default:
        out[s.key] = s.default;
    }
  }
  if (!out.source) out.source = "default";
  // A preset named with no colors of its own means "use that preset".
  const named = PRESETS.find((p) => p.name === input.preset);
  if (named && COLOR_KEYS.every((k) => input[k] === undefined)) Object.assign(out, named.colors);
  out.preset = presetOf(out);
  return out;
}

// ---------- MeshMonitor ----------

const NODES_EVERY = 300_000; // ms, as the native savers
const MESSAGES_EVERY = 4_000;
const OFFLINE_AFTER = 8; // failed polls in a row
const MESSAGE_SPACING = 350;
const TIMEOUT = 10_000;

class HttpError extends Error {
  constructor(status) {
    super(`MeshMonitor answered ${status}`);
    this.status = status;
  }
}

/** "{server}/api/v1/sources/{source}/{what}?…". No scheme means https://, except for this computer. */
function apiUrl(settings, what) {
  let server = settings.server.trim().replace(/\/+$/, "");
  if (!/^[a-z][a-z0-9+.-]*:\/\//i.test(server)) {
    const local = /^(localhost|127\.0\.0\.1|\[::1\])(:\d+)?(\/|$)/i.test(server);
    server = (local ? "http://" : "https://") + server;
  }
  const query = what === "nodes" ? "?active=true&sinceDays=7" : "?limit=50";
  return `${server}/api/v1/sources/${encodeURIComponent(settings.source || "default")}/${what}${query}`;
}

/** A plain-language reason a MeshMonitor request failed, for a page to show. */
export function describeError(error, settings = {}) {
  const status = error?.status;
  if (status === 401) return "MeshMonitor turned down the API token. Check it in the settings.";
  if (status === 403) return "This API token can't read nodes and messages. Give its user read access in MeshMonitor.";
  if (status === 404) return "MeshMonitor has no such source. Leave Source as \"default\", or check its ID.";
  if (status) return `MeshMonitor answered ${status}.`;
  if (error?.name === "AbortError" || error?.name === "TimeoutError") return "MeshMonitor didn't answer within 10 seconds.";
  if (error?.name === "SyntaxError" || error?.badResponse) return "That address didn't answer like MeshMonitor's API. Check the server address.";
  const origin = globalThis.location?.origin;
  let server = "";
  try {
    server = settings.server ? new URL(apiUrl(cleanSettings(settings), "nodes")).origin : "";
  } catch {
    return "The MeshMonitor address isn't valid.";
  }
  if (origin?.startsWith("https:") && server.startsWith("http:"))
    return `This page is on https://, so the browser won't connect to ${server}. Give MeshMonitor an https:// address.`;
  const where = server ? ` at ${server}` : "";
  const allow = origin && origin !== "null" ? ` add this page's address (${origin}) to MeshMonitor's ALLOWED_ORIGINS` : " add this page's address to MeshMonitor's ALLOWED_ORIGINS";
  return `Couldn't reach MeshMonitor${where}. If the address is right,${allow}.`;
}

// ---------- the module ----------

let compiled; // WebAssembly.Module, compiled once; each saver gets its own instance

function moduleBytes() {
  const text = atob(WASM);
  const bytes = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) bytes[i] = text.charCodeAt(i);
  return bytes;
}

async function instantiate(host) {
  compiled ??= WebAssembly.compile(moduleBytes());
  const instance = await WebAssembly.instantiate(await compiled, { host });
  instance.exports._initialize?.();
  return instance.exports;
}

const encoder = new TextEncoder();
const decoder = new TextDecoder();

// ---------- the clock ----------

const FONT = '-apple-system, "Segoe UI", system-ui, sans-serif';
const RTL = /^[\s\d\p{P}\p{S}]*[֐-ࣿיִ-﷿ﹰ-﻿]/u;

function formatTime(date, use24Hour) {
  const h = date.getHours();
  const m = String(date.getMinutes()).padStart(2, "0");
  if (use24Hour) return `${String(h).padStart(2, "0")}:${m}`;
  return `${h % 12 || 12}:${m} ${h < 12 ? "AM" : "PM"}`;
}

function rgba(r, g, b, a) {
  return `rgba(${Math.round(r * 255)},${Math.round(g * 255)},${Math.round(b * 255)},${a})`;
}

function hexToRgb(hex) {
  const v = parseInt(hex.slice(1), 16);
  return [((v >> 16) & 255) / 255, ((v >> 8) & 255) / 255, (v & 255) / 255];
}

// ---------- the saver ----------

/*
 * Run the saver in a canvas. The canvas is sized by the page (CSS); the saver follows its size and
 * devicePixelRatio (ResizeObserver) and pauses while the page is hidden.
 * options:
 *   settings       – as from cleanSettings (missing keys use DEFAULTS)
 *   compact        – a small preview
 *   reducedMotion  – slower drift and packets (default: the page's prefers-reduced-motion)
 *   userName       – the text for "Your name" (a page has no system user)
 *   now            – () => Date, for the clock
 *   onStatus       – (status, detail) => void, with "simulated" | "connecting" | "live" | "offline";
 *                    for "offline", detail is describeError's message
 * Returns { update(settings), destroy() }.
 */
export function createSaver(canvas, options = {}) {
  let settings = cleanSettings({ ...DEFAULTS, ...options.settings });
  const ctx = canvas.getContext("2d");
  const now = options.now ?? (() => new Date());
  const reducedMotion = options.reducedMotion ??
    Boolean(globalThis.matchMedia?.("(prefers-reduced-motion: reduce)").matches);
  const raf = globalThis.requestAnimationFrame?.bind(globalThis) ?? ((f) => setTimeout(() => f(performance.now()), 16));
  const cancelRaf = globalThis.cancelAnimationFrame?.bind(globalThis) ?? clearTimeout;

  let wasm = null;
  let core = 0;
  let tracker = 0;
  let destroyed = false;
  let frame = 0;
  let last = 0;
  let width = 0, height = 0, ratio = 1;
  let status = "";
  let observer = null;

  const host = {
    line(x0, y0, x1, y1, w, r, g, b, a) {
      ctx.strokeStyle = rgba(r, g, b, a);
      ctx.lineWidth = w;
      ctx.beginPath();
      ctx.moveTo(x0, y0);
      ctx.lineTo(x1, y1);
      ctx.stroke();
    },
    circle(x, y, radius, filled, w, r, g, b, a) {
      ctx.beginPath();
      ctx.arc(x, y, radius, 0, Math.PI * 2);
      if (filled) {
        ctx.fillStyle = rgba(r, g, b, a);
        ctx.fill();
      } else {
        ctx.strokeStyle = rgba(r, g, b, a);
        ctx.lineWidth = w;
        ctx.stroke();
      }
    },
  };

  // Copies a JS string into the module's memory, NUL-terminated. Caller frees.
  function cString(text) {
    const bytes = encoder.encode(text);
    const ptr = wasm.alloc(bytes.length + 1);
    const mem = new Uint8Array(wasm.memory.buffer, ptr, bytes.length + 1);
    mem.set(bytes);
    mem[bytes.length] = 0;
    return ptr;
  }

  function readString(ptr) {
    const mem = new Uint8Array(wasm.memory.buffer);
    let end = ptr;
    while (mem[end]) end++;
    return decoder.decode(mem.subarray(ptr, end));
  }

  // Hands a response body to a parser in the module: fn(…before, ptr, length).
  function withBody(text, fn) {
    const bytes = encoder.encode(text);
    const ptr = wasm.alloc(bytes.length);
    new Uint8Array(wasm.memory.buffer, ptr, bytes.length).set(bytes);
    try {
      return fn(ptr, bytes.length);
    } finally {
      wasm.dealloc(ptr);
    }
  }

  function setStatus(next, detail) {
    if (next === status) return;
    status = next;
    options.onStatus?.(next, detail);
  }

  function applyOptions() {
    if (!wasm) return;
    const [b, d, l, p] = [settings.background, settings.dots, settings.lines, settings.packets].map(hexToRgb);
    wasm.set_options(core, ...b, ...d, ...l, ...p, reducedMotion ? 1 : 0, options.compact ? 1 : 0);
  }

  // ----- size -----

  function measure() {
    const w = canvas.clientWidth || canvas.width || 0;
    const h = canvas.clientHeight || canvas.height || 0;
    const dpr = globalThis.devicePixelRatio || 1;
    if (w === width && h === height && dpr === ratio) return;
    width = w;
    height = h;
    ratio = dpr;
    canvas.width = Math.max(1, Math.round(w * dpr));
    canvas.height = Math.max(1, Math.round(h * dpr));
    if (wasm && w > 0 && h > 0) wasm.resize(core, w, h);
  }

  // ----- live mode -----

  const live = { timers: [], failures: 0, queue: [], controllers: new Set(), discardNext: false };

  async function fetchText(what) {
    const controller = new AbortController();
    live.controllers.add(controller);
    const timer = setTimeout(() => controller.abort(), TIMEOUT);
    try {
      const response = await fetch(apiUrl(settings, what), {
        headers: { Authorization: `Bearer ${settings.token}`, Accept: "application/json" },
        credentials: "omit",
        cache: "no-store",
        signal: controller.signal,
      });
      if (!response.ok) throw new HttpError(response.status);
      return await response.text();
    } finally {
      clearTimeout(timer);
      live.controllers.delete(controller);
    }
  }

  function failed(error) {
    live.failures++;
    if (live.failures === OFFLINE_AFTER) {
      wasm.set_live(core, 0);
      live.queue = [];
      setStatus("offline", describeError(error, settings));
    }
  }

  function succeeded() {
    const wasOffline = live.failures >= OFFLINE_AFTER;
    live.failures = 0;
    // Back from offline: ask for nodes now rather than in up to five minutes.
    if (wasOffline && !wasm.is_live(core)) pollNodes();
  }

  async function pollNodes() {
    try {
      const body = await fetchText("nodes");
      if (destroyed) return;
      const wasLive = wasm.is_live(core);
      if (!wasLive) wasm.set_live(core, 1);
      const n = withBody(body, (ptr, len) => wasm.nodes_json(core, ptr, len));
      if (n < 0) throw Object.assign(new Error("not MeshMonitor"), { badResponse: true });
      if (n === 0 && !wasLive) wasm.set_live(core, 0);
      live.failures = 0;
      if (n > 0) setStatus("live");
    } catch (error) {
      if (!destroyed && live.timers.length) failed(error);
    }
  }

  async function pollMessages() {
    try {
      const body = await fetchText("messages");
      if (destroyed) return;
      const n = withBody(body, (ptr, len) => wasm.messages_json(tracker, ptr, len));
      if (n < 0) throw Object.assign(new Error("not MeshMonitor"), { badResponse: true });
      succeeded();
      if (live.discardNext) {
        live.discardNext = false; // came back from a hidden page: don't replay what was missed
        return;
      }
      const start = performance.now();
      for (let i = 0; i < n; i++) {
        live.queue.push({
          due: start + i * MESSAGE_SPACING,
          from: readString(wasm.message_from(i)),
          to: readString(wasm.message_to(i)),
        });
      }
    } catch (error) {
      if (!destroyed && live.timers.length) failed(error);
    }
  }

  function startLive() {
    stopLive();
    if (tracker) wasm.tracker_destroy(tracker);
    tracker = wasm.tracker_create();
    live.failures = 0;
    live.queue = [];
    wasm.set_live(core, 0);
    if (!settings.server || !settings.token) {
      setStatus("simulated");
      return;
    }
    setStatus("connecting");
    live.timers = [setInterval(pollNodes, NODES_EVERY), setInterval(pollMessages, MESSAGES_EVERY)];
    pollNodes();
    pollMessages();
  }

  function stopLive() {
    live.timers.forEach(clearInterval);
    live.timers = [];
    live.controllers.forEach((c) => c.abort());
    live.controllers.clear();
    live.queue = [];
  }

  function sendDueMessages(t) {
    while (live.queue.length && live.queue[0].due <= t) {
      const m = live.queue.shift();
      if (!wasm.is_live(core)) continue;
      const from = cString(m.from);
      const to = cString(m.to);
      wasm.message(core, from, to);
      wasm.dealloc(from);
      wasm.dealloc(to);
    }
  }

  // ----- drawing -----

  function labelText() {
    const kind = settings.label === "user" ? 1 : settings.label === "custom" ? 2 : 0;
    const text = cString(settings.labelText);
    const user = cString(options.userName ?? "");
    const out = readString(wasm.label(kind, text, user, wasm.is_live(core)));
    wasm.dealloc(text);
    wasm.dealloc(user);
    return out;
  }

  // As macos/MeshSaverView.swift: bottom-left, the time at 48pt with the label at 12pt under it.
  function drawClock() {
    const [r, g, b] = hexToRgb(settings.dots);
    const k = options.compact ? Math.max(0.25, height / 900) : 1;
    const margin = 32 * k;
    const label = labelText();
    ctx.save();
    ctx.textBaseline = "alphabetic";
    ctx.textAlign = "left";
    ctx.direction = RTL.test(label) ? "rtl" : "ltr";
    const labelBaseline = height - margin - 3 * k;
    if (label) {
      ctx.font = `400 ${12 * k}px ${FONT}`;
      ctx.fillStyle = rgba(r, g, b, 0.5);
      if (ctx.direction === "rtl") {
        ctx.textAlign = "right";
        ctx.fillText(label, margin + ctx.measureText(label).width, labelBaseline);
      } else {
        ctx.fillText(label, margin, labelBaseline);
      }
    }
    ctx.direction = "ltr";
    ctx.textAlign = "left";
    ctx.font = `600 ${48 * k}px ${FONT}`;
    if ("letterSpacing" in ctx) ctx.letterSpacing = `${-1 * k}px`;
    if ("fontVariantNumeric" in ctx) ctx.fontVariantNumeric = "tabular-nums";
    ctx.fillStyle = rgba(r, g, b, 0.8);
    ctx.fillText(formatTime(now(), settings.use24Hour), margin - 2 * k, labelBaseline - 17 * k);
    ctx.restore();
  }

  function draw() {
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    ctx.fillStyle = settings.background;
    ctx.fillRect(0, 0, width, height);
    ctx.lineCap = "round";
    wasm.render(core);
    if (settings.clock) drawClock();
  }

  function tick(t) {
    if (destroyed) return;
    frame = raf(tick);
    if (!observer) measure();
    const dt = last ? Math.min(0.1, (t - last) / 1000) : 0;
    last = t;
    sendDueMessages(performance.now());
    wasm.step(core, dt);
    if (width > 0 && height > 0) draw();
  }

  // ----- visibility -----

  const doc = globalThis.document;
  function onVisibility() {
    if (doc.visibilityState === "hidden") {
      cancelRaf(frame);
      frame = 0;
      stopLive();
    } else if (!frame && wasm && !destroyed) {
      last = 0;
      if (settings.server && settings.token) {
        live.discardNext = true;
        live.timers = [setInterval(pollNodes, NODES_EVERY), setInterval(pollMessages, MESSAGES_EVERY)];
        pollMessages();
      }
      frame = raf(tick);
    }
  }

  // ----- start -----

  instantiate(host).then((exports) => {
    if (destroyed) return;
    wasm = exports;
    core = wasm.create((Math.random() * 0xffffffff) >>> 0 || 1);
    applyOptions();
    if (typeof ResizeObserver === "function") {
      observer = new ResizeObserver(measure);
      observer.observe(canvas);
    }
    width = height = 0;
    measure();
    startLive();
    doc?.addEventListener("visibilitychange", onVisibility);
    if (width > 0 && height > 0) draw(); // something to see even if the page starts hidden
    if (doc?.visibilityState !== "hidden") frame = raf(tick);
  });

  return {
    update(next) {
      const before = settings;
      settings = cleanSettings({ ...settings, ...next });
      if (!wasm) return;
      applyOptions();
      if (before.server !== settings.server || before.token !== settings.token || before.source !== settings.source) startLive();
    },
    destroy() {
      if (destroyed) return;
      destroyed = true;
      cancelRaf(frame);
      stopLive();
      observer?.disconnect();
      doc?.removeEventListener("visibilitychange", onVisibility);
      if (wasm) {
        if (tracker) wasm.tracker_destroy(tracker);
        wasm.destroy(core);
      }
    },
  };
}
