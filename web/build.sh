#!/bin/sh
# Builds the web version: web/mesh.wasm from the C core, then web/wasm.js (the
# module as base64, so a page's bundler never has to find a .wasm file) and
# web/presets.js (the color presets, read from core/meshmonitor.c).
# Needs zig (it bundles a libc for wasm) and node. CI uses zig 0.16.0.
set -eu
cd "$(dirname "$0")/.."

zig cc -target wasm32-wasi -Oz -s -mexec-model=reactor -I core \
  web/bridge.c core/mesh.c core/meshmonitor.c core/json.c -o web/mesh.wasm
node web/generate.mjs
echo "Built web/mesh.wasm ($(wc -c < web/mesh.wasm | tr -d ' ') bytes), web/wasm.js, web/presets.js"
