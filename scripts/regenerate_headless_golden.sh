#!/bin/zsh
# Regenerate the headless byte-gate reference -> tmp/golden_headless_<bk>.png
# (gitignored). Runs cairns_serve under FixedClock + 60 NDJSON-driven
# render.frame ticks, then cairns.io.dumpTexture. NO windowed app.
#
# Usage: scripts/regenerate_headless_golden.sh [metal|vk]
set -e
cd "$(dirname "$0")/.."
backend="${1:-metal}"
mkdir -p tmp

drive=tmp/_headless_drive.ndjson
# #269: spawn shape matches verify_headless.sh. Keep these in sync.
spawn_op=$(python3 -c 'import json,sys;print(json.dumps({"op":"cairns.script.eval","args":{"code":open(sys.argv[1]).read()}}))' scripts/_headless_spawn.js)
{
  printf '%s\n' "$spawn_op"
  for i in $(seq 1 60); do echo '{"op":"cairns.render.frame"}'; done
  echo '{"op":"cairns.io.dumpTexture","args":{"name":"final","path":"tmp/_headless_dump.png"}}'
  echo '{"op":"cairns.quit"}'
} > "$drive"

case "$backend" in
  metal) serve=build/metal/Debug/cairns_serve ;;
  vk)    serve=build/vk/Debug/cairns_serve ;;
  *) echo "usage: $0 [metal|vk]" >&2; exit 2 ;;
esac
[ -x "$serve" ] || { echo "$backend: cairns_serve missing at $serve" >&2; exit 1; }

out="tmp/golden_headless_${backend}.png"
rm -f tmp/_headless_dump.png "$out"
CAIRNS_DUMP=tmp/_headless_unused.png "$serve" < "$drive" > /dev/null 2>&1 || true
[ -f tmp/_headless_dump.png ] || { echo "FAILED: no dump produced" >&2; exit 1; }
mv tmp/_headless_dump.png "$out"
echo "golden_headless ($backend) -> $out"
