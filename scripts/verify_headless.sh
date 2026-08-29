#!/bin/zsh
# Headless byte-gate via cairns_serve + NDJSON. Drives 60 deterministic
# render.frame ticks under FixedClock (CAIRNS_DUMP gates FixedClock), then
# dumps `final` and cmp's against tmp/golden_headless_<backend>.png.
#
# NEVER launches sdl-min -- the windowed app steals focus and makes the
# laptop unusable while it is up. This is the canonical byte-gate during
# the #219 allocator sweep and beyond. See
# memory/feedback-headless-only-verify.md.
#
# Usage:
#   scripts/verify_headless.sh metal
#   scripts/verify_headless.sh vk
#   scripts/verify_headless.sh        # both
#
# Regenerate goldens: scripts/regenerate_headless_golden.sh <backend>
set -e
cd "$(dirname "$0")/.."

drive=tmp/_headless_drive.ndjson
mkdir -p tmp
# #269: entity spawn moved off engine init to NDJSON. Spawn loop lives
# in scripts/_headless_spawn.js (editable JS); python3 JSON-encodes it
# into the cairns.script.eval op below.
spawn_op=$(python3 -c 'import json,sys;print(json.dumps({"op":"cairns.script.eval","args":{"code":open(sys.argv[1]).read()}}))' scripts/_headless_spawn.js)
{
  # printf %s (not echo) -- zsh echo interprets \n in the JSON-encoded
  # JS body and would split it across NDJSON lines.
  printf '%s\n' "$spawn_op"
  for i in $(seq 1 60); do echo '{"op":"cairns.render.frame"}'; done
  echo '{"op":"cairns.io.dumpTexture","args":{"name":"final","path":"tmp/_headless_dump.png"}}'
  echo '{"op":"cairns.quit"}'
} > "$drive"

run_backend() {
  local bk="$1"
  local serve
  case "$bk" in
    metal) serve=build/metal/Debug/cairns_serve ;;
    vk)    serve=build/vk/Debug/cairns_serve ;;
    *) echo "usage: $0 [metal|vk]" >&2; return 2 ;;
  esac
  [ -x "$serve" ] || { echo "$bk: cairns_serve missing at $serve" >&2; return 1; }
  local golden="tmp/golden_headless_${bk}.png"
  [ -f "$golden" ] || { echo "$bk: $golden missing -- run regenerate_headless_golden.sh $bk" >&2; return 1; }
  rm -f tmp/_headless_dump.png
  # CAIRNS_DUMP set -> FixedClock so render.frame ticks advance deterministically.
  # The CAIRNS_DUMP path is a sink we discard; the byte-gate runs against the
  # explicit cairns.io.dumpTexture output instead.
  CAIRNS_DUMP=tmp/_headless_unused.png "$serve" < "$drive" > /dev/null 2>&1 || true
  if [ ! -f tmp/_headless_dump.png ]; then
    echo "$bk: HEADLESS: no dump produced" >&2
    return 1
  fi
  if cmp -s tmp/_headless_dump.png "$golden"; then
    echo "$bk: byte-identical to $golden ✓"
  else
    echo "$bk: DIFFERS from $golden ✗  (tmp/_headless_dump.png vs $golden)" >&2
    return 1
  fi
}

if [ -n "$1" ]; then
  run_backend "$1"
else
  run_backend metal
  run_backend vk
fi
