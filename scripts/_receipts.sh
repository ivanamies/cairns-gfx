#!/usr/bin/env bash
# #229 allocation-receipts harness (throwaway, like _alloc_capture.js / _diag*.js).
#
# One canonical driver reused after every milestone. Given a cairns_serve
# binary, runs two workloads against it and prints the receipt:
#
#   alloc  -- load 3 GLBs + instantiate + render + reload 1 + clear + unloadAll.
#             Needs a CAIRNS_GFX_ALLOC_TRACE=ON build; extracts the engine's
#             [LOAD]/[RELOAD] fences + [ALLOC-RECEIPT] deltas.
#   perf   -- the boot scene is already 500 actors (run.js loads 100 GLBs x 5
#             slices). Render 120 frames, then cairns.perf.last -> Timer slots
#             (the same numbers the imgui HUD shows).
#
# cairns_serve exits 139 at teardown on a heavy scene (PRE-EXISTING, unrelated
# to receipts -- the non-trace binary does it too); all work + receipts flush
# first, so we ignore the exit code.
#
# Usage: scripts/_receipts.sh <path-to-cairns_serve> [alloc|perf|both]
set -u
SERVE="${1:?usage: _receipts.sh <cairns_serve> [alloc|perf|both]}"
MODE="${2:-both}"
DIR="$(cd "$(dirname "$SERVE")" && pwd)"
BIN="$DIR/$(basename "$SERVE")"

run_alloc() {
  {
    printf '{"op":"cairns.prefab.load","args":{"path":"aatrox.glb"}}\n'
    printf '{"op":"cairns.prefab.load","args":{"path":"ahri.glb"}}\n'
    printf '{"op":"cairns.prefab.load","args":{"path":"aatrox_blood_moon.glb"}}\n'
    printf '{"op":"cairns.scene.instantiate","args":{"prefab":0,"x":-1.5,"y":0,"z":-3,"scale":0.00433}}\n'
    printf '{"op":"cairns.scene.instantiate","args":{"prefab":1,"x":0,"y":0,"z":-3,"scale":0.00433}}\n'
    printf '{"op":"cairns.scene.instantiate","args":{"prefab":2,"x":1.5,"y":0,"z":-3,"scale":0.00433}}\n'
    for _ in $(seq 1 30); do printf '{"op":"cairns.render.frame","args":{}}\n'; done
    printf '{"op":"cairns.prefab.reload","args":{"path":"aatrox.glb"}}\n'
    for _ in $(seq 1 30); do printf '{"op":"cairns.render.frame","args":{}}\n'; done
    printf '{"op":"cairns.scene.clear","args":{}}\n'
    printf '{"op":"cairns.prefab.unloadAll","args":{}}\n'
  } | CAIRNS_SERVE_WATCHDOG_SEC=0 "$BIN" >/dev/null 2>/tmp/_receipts_alloc.err || true
  echo "==== ALLOC RECEIPT ($BIN) ===="
  grep -E '\[LOAD\] (begin|end)|\[RELOAD\]|\[ALLOC-RECEIPT\]' /tmp/_receipts_alloc.err \
    | grep -v '^\[ALLOC\]' || echo "(no receipts -- is this a CAIRNS_GFX_ALLOC_TRACE build?)"
}

run_perf() {
  {
    for _ in $(seq 1 120); do printf '{"op":"cairns.render.frame","args":{}}\n'; done
    printf '{"op":"cairns.perf.last","args":{}}\n'
  } | CAIRNS_SERVE_WATCHDOG_SEC=0 "$BIN" >/tmp/_receipts_perf.out 2>/tmp/_receipts_perf.err || true
  echo "==== PERF RECEIPT (500 actors = 100 GLBs x 5, 120 frames) ===="
  grep '"slots"' /tmp/_receipts_perf.out | tail -1 || echo "(no perf.last response)"
  echo "---- [Timer] lines (tail) ----"
  grep '\[Timer\]' /tmp/_receipts_perf.err | tail -12 || true
}

case "$MODE" in
  alloc) run_alloc ;;
  perf)  run_perf ;;
  both)  run_alloc; echo; run_perf ;;
  *) echo "unknown mode: $MODE (alloc|perf|both)"; exit 2 ;;
esac
