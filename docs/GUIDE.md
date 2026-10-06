# Mesh Screensaver — developer guide

Mesh is one screen saver built four times: a `.saver` for macOS, a `.scr` for Windows, an XScreenSaver hack for Linux, and a module for web pages. All four draw through the same C core, so the mesh looks and moves the same everywhere; each one only adds its own window or canvas, 2D drawing, settings, and networking.

## Layout

```
core/
  mesh.h, mesh.c          the simulation and drawing: nodes, links, packets, rings
  meshmonitor.h, .c       settings and presets, API URLs, parsing nodes and messages (Windows, Linux)
  json.h, json.c          a small JSON reader for those responses
  module.modulemap        lets Swift import the core as MeshCore
macos/
  MeshSaverView.swift     the ScreenSaverView: draws the core with Core Graphics, the clock
  MeshMonitorClient.swift polls MeshMonitor (URLSession for https)
  PlainHTTP.swift         plain http:// requests over Network.framework
  ConfigureSheet.swift    the Options sheet, and EditingWindow (copy and paste in the host)
  Settings.swift          settings in ScreenSaverDefaults, presets, the label
  Info.plist, Resources/  the bundle: principal class MeshSaverView, thumbnails
  build.sh                builds build/Mesh.saver, universal and ad-hoc signed
windows/
  mesh_win.c              /s, /p, /c, and the /x test mode; GDI+, WinHTTP, registry, dialog
  mesh.rc, resource.h     the dialog, the name in Screen Saver Settings (string 1), version
  mesh.manifest           modern controls and DPI awareness
  build.sh                cross-compiles build/Mesh.scr with mingw-w64
linux/
  mesh_x11.c              the hack: Xlib + cairo, libcurl, config file and flags, -render test mode
  screensaver-mesh.xml    the XScreenSaver settings page
  Makefile, install.sh, config.example, INSTALL.txt
web/
  bridge.c                the core for JavaScript: exports, and two drawing imports
  index.js                the interface: SETTINGS, cleanSettings, createSaver, …
  build.sh, generate.mjs  builds mesh.wasm, then wasm.js (base64) and presets.js
  mesh.wasm, wasm.js, presets.js   built files (committed, so pages need no build)
  demo.html               a page to try it
package.json              lets a page npm-install web/ from GitHub
tests/                    test_mesh.c (core), test_meshmonitor.c (JSON, settings, parsing),
                          web.test.mjs (the web version, in node)
tools/
  mock_meshmonitor.py     a fake MeshMonitor: 40 nodes and a message every 2 seconds
  preview.swift           loads Mesh.saver like the host does: renders PNGs, the sheet, a paste test
.github/workflows/        build, test, and release
```

## The core

`mesh.c` owns everything that moves. A platform creates a `mesh`, sets its options (colors, reduced motion, compact for previews), tells it the size in points, calls `mesh_step(dt)` every frame, and draws with `mesh_render`, supplying two callbacks: `line` and `circle`. Nothing in the core knows about windows or networking.

- **Simulated mode** is the original saver: node count scales with screen area (14 to 90), links reach 150 points, and a random packet wanders up to seven hops every 0.9 seconds.
- **Live mode** starts when the platform hands over node IDs (`mesh_set_nodes`). Nodes that were already on screen keep their spot. Random packets stop; only `mesh_message(from, to)` sends one:
  - a direct message follows the shortest path over the visible links (breadth-first), or makes one long hop if the two aren't connected
  - a broadcast (`!ffffffff`, or no recipient) sends a packet along every edge of the search tree, three hops out
  - the sender pulses, and nodes within 1.6 × reach get pushed away, then ease back to their own drift
  - unknown senders and recipients are added to the mesh
- In live mode the reach grows with the screen's spacing between nodes (up to 2.2 × 150), so a small mesh still links up.
- **Reduced motion** (from the system) slows drift to 35% and packets to under half speed, and sends simulated packets less often.

## MeshMonitor

Mesh uses MeshMonitor's REST API v1, with `Authorization: Bearer <token>`. Every answer is `{ success, count, data }`.

- `GET /api/v1/sources/{source}/nodes?active=true&sinceDays=7` every 5 minutes. Nodes are sorted by `lastHeard`, newest first, and the first 160 `nodeId`s are kept.
- `GET /api/v1/sources/{source}/messages?limit=50` every 4 seconds. MeshMonitor has no push feed. Each message's `id` is remembered; the first answer only primes that memory, so messages already there aren't replayed. New ones are sent oldest first, 0.35 seconds apart.
- `{source}` is the source ID, or `default` for the first one the token can read.
- After 8 failed polls in a row (about 30 seconds), Mesh goes back to the simulated mesh until MeshMonitor answers again.

## macOS

- Since macOS 14, screen savers run inside `legacyScreenSaver`, a sandboxed host with outgoing network access. It has three quirks Mesh works around:
  - **No App Transport Security exceptions**, so URLSession refuses plain `http://`. `PlainHTTP.swift` speaks HTTP/1.1 over Network.framework instead (including chunked answers), which ATS doesn't cover. `https://` stays on URLSession.
  - **No Edit menu**, so ⌘V, ⌘C, and the rest never reach a text field. `EditingWindow` handles those shortcuts and sends them to its own focused field. The token also has a **Paste** button.
  - **It can keep a saver running after the screen saver ends.** Mesh quits on `com.apple.screensaver.willstop`.
- Settings are in `ScreenSaverDefaults` for `com.maxhayim.screensaver-mesh`.
- The Options sheet uses fixed column widths: NSGridView's own sizing left it lopsided.
- `build.sh` builds arm64 and x86_64, joins them with `lipo`, copies the thumbnails, clears Dropbox's extended attributes (codesign rejects them), and signs ad hoc.

## Windows

- `Mesh.scr /s` covers every monitor with one window over the virtual screen, with the clock on the primary monitor. It quits on a key, a click, the wheel, or the mouse moving more than 8 pixels (ignoring the first 0.4 seconds).
- `/p <HWND>` draws inside Screen Saver Settings' preview and ends when that window closes. `/c` (or no argument) opens the settings dialog.
- `/x <seconds> <out.bmp> [width height] [preview]` is a test mode: it renders off screen, writes a bitmap, and writes `<out.bmp>.txt` with the node count and whether it went live (a windowed program has no console).
- Settings are in `HKEY_CURRENT_USER\Software\maxhayim\screensaver-mesh`, as UTF-16 strings plus `clock` and `24hour` DWORDs.
- Drawing goes into a DIB section with GDI+ (antialiased), scaled by the system DPI. WinHTTP runs on a worker thread and hands results to the window through a locked queue.

## Linux

- XScreenSaver passes its window in `$XSCREENSAVER_WINDOW`; `-root` and `-window-id` work too. With none of them, Mesh opens its own window, which any key closes.
- Settings come from `~/.config/screensaver-mesh/config`, then from flags (`-server`, `-token`, `-preset`, `-label`, …). The XScreenSaver XML exposes the server, presets, clock, and label.
- `-render <seconds> <out.png> [-size WxH] [-preview]` renders off screen with cairo, no X needed.

## Web

`web/` is the same saver for web pages. A page installs it from GitHub and imports `screensaver-mesh`:

```
npm install github:maxhayim/screensaver-mesh#v0.2.0
```

The Starfield screen saver has the same interface, so a page can treat both alike:

```js
export const ID = "mesh";
export const NAME = "Mesh";
export const SETTINGS = [...];     // { key, label, type, default, group?, placeholder?, presets?, options?, maxLength?, showIf? }
export const DEFAULTS = {...};     // key → default
export function cleanSettings(raw) // any saved object → valid settings
export function presetOf(settings) // the matching preset's name, or "Custom"
export function describeError(error, settings) // a plain-language reason a request failed
export function createSaver(canvas, options)   // → { update(settings), destroy() }
```

- **SETTINGS** follows the native Options sheet: Server, API token, Source (group "MeshMonitor"); Preset, Background, Dots, Lines, Packets ("Colors"); Show the clock, 24-hour time, Label, and Text under the clock, which has `showIf: { label: "custom" }` ("Clock"). Types are `preset`, `color`, `toggle`, `choice`, `text`, and `password`. The presets come from `mm_presets` in `core/meshmonitor.c`. Picking a preset sets the four colors; changing a color by hand makes `preset` read "Custom".
- **createSaver(canvas, options)** runs the saver in a canvas the page sizes with CSS. It follows the canvas's size and `devicePixelRatio` (ResizeObserver), and pauses while the page is hidden. Options:
  - `settings`: as from `cleanSettings`; missing keys use `DEFAULTS`
  - `compact`: a small preview, sized like the native thumbnails
  - `reducedMotion`: slower drift and packets (default: the page's `prefers-reduced-motion`)
  - `userName`: the text for "Your name", since a page has no system user
  - `now`: `() => Date`, for the clock
  - `onStatus(status, detail)`: `"simulated"`, `"connecting"`, `"live"`, or `"offline"`; for `"offline"`, `detail` is `describeError`'s message

  `update(settings)` merges changed settings in; a new server, token, or source reconnects. `destroy()` stops everything.
- **Drawing:** `web/bridge.c` wraps the core and draws through two imports from the module `"host"`, `line` and `circle`, which draw into the canvas's 2D context. Each frame: `step(dt)` (seconds, at most 0.1), clear to the background, `render()`, then the clock in the native savers' sizes and margins, in `-apple-system, "Segoe UI", system-ui, sans-serif`, right-to-left when the label is.
- **Live mode** behaves as in [MeshMonitor](#meshmonitor): nodes every 5 minutes and messages every 4 seconds with `fetch` (`Authorization: Bearer`, `credentials: "omit"`), parsed by the same C code as Windows and Linux (`nodes_json`, `messages_json`). A server with no scheme gets `https://`, except `localhost`, `127.0.0.1`, and `[::1]`, which get `http://`. After 8 failed polls it shows the simulated mesh; the first answer after that brings nodes back right away. Messages that arrive while the page is hidden aren't replayed.
- **ALLOWED_ORIGINS:** a browser lets a page read MeshMonitor's answers only if MeshMonitor allows the page's address. Add it (for example `https://example.com`, or `http://localhost:5173` while developing) to MeshMonitor's `ALLOWED_ORIGINS` setting. A page on `https://` also can't reach an `http://` MeshMonitor; give MeshMonitor an `https://` address. `describeError` says which of these happened.
- **The module** is about 60 KB of WebAssembly, carried as base64 in `web/wasm.js`, so a bundler never has to find a `.wasm` file. It's compiled once and instantiated per saver; it imports nothing but the two drawing functions.

To build it, you need [zig](https://ziglang.org/) (CI uses 0.16.0) and node:

```
web/build.sh
```

That compiles `web/mesh.wasm` with `zig cc -target wasm32-wasi -Oz`, then writes `web/wasm.js` and `web/presets.js`. Commit all three. CI rebuilds them and fails if the committed files differ.

To try it in a browser, serve the repo root and open the demo, optionally against the fake MeshMonitor:

```
python3 tools/mock_meshmonitor.py &
python3 -m http.server 8642
# http://localhost:8642/web/demo.html?server=127.0.0.1:8787&token=mm_v1_test
```

## Testing

```
cc -std=c99 -Icore core/mesh.c tests/test_mesh.c -lm -o build/test_mesh && build/test_mesh
cc -std=c99 -Icore core/json.c core/meshmonitor.c tests/test_meshmonitor.c -o build/test_mm && build/test_mm
python3 tools/mock_meshmonitor.py [port] [--chunked] [--backlog N]   # token mm_v1_test, default port 8787
node tests/web.test.mjs                                  # the web version; starts its own fake MeshMonitor
```

macOS, with `build/Mesh.saver` built:

```
swiftc tools/preview.swift -o build/preview -framework ScreenSaver
build/preview build/Mesh.saver shot.png 5 [width height] [--preview]
build/preview build/Mesh.saver sheet.png --sheet
build/preview build/Mesh.saver - --paste-test           # borrows the clipboard and puts it back
```

To point the preview at the fake server, write settings with `defaults -currentHost write com.maxhayim.screensaver-mesh server http://127.0.0.1:8787` (and `token mm_v1_test`), and remove them afterwards with `defaults -currentHost delete com.maxhayim.screensaver-mesh`.

CI (`.github/workflows/build.yml`) runs on every push:
- **macOS:** tests, the build, previews in simulated and live mode, and the paste test
- **Linux:** tests, the build, renders, a run under Xvfb, and a run inside a real XScreenSaver
- **Web:** rebuilds `web/` with zig and checks it matches what's committed, checks `package.json` against `VERSION`, and runs `tests/web.test.mjs`
- **Windows:** the cross-compile, then on a Windows machine the `/x` renders (live included), `/s` full screen, `/p` inside a real parent window, and the `/c` dialog, with screenshots

Screenshots from every run are kept as workflow artifacts.

## Releasing

1. Set `VERSION` and `"version"` in `package.json` (semantic versioning).
2. In `CHANGELOG.md`, rename `## [Unreleased]` to `## [x.y.z] - YYYY-MM-DD`, and add the version to the README's Versioning list.
3. Tag with the release's name as the message, and push:
   ```
   git tag -a vx.y.z -m "Short Title Case Name"
   git push origin vx.y.z
   ```

CI builds and tests everything, then publishes the release as **vx.y.z — Name**: the notes are that version's changelog section plus a Compatibility section, with `screensaver-mesh-<version>-macos.zip`, `-windows.zip`, and `-linux-x86_64.tar.gz` attached.
