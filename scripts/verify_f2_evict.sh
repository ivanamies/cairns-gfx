#!/bin/zsh
# #228 F2 evict acceptance: the user's prescribed sequence.
#
#   engine starts -> load 3 -> count=3 -> load 3 more -> count=6 ->
#   scene.clear -> unloadAll (returns 6) -> count=0 -> stop engine
#
# Usage: scripts/verify_f2_evict.sh [metal|vk]   (default: both)
set -e
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
cd "$(dirname "$0")/.."

run_backend() {
    local bk="$1"
    local serve
    case "$bk" in
        metal) serve=build/metal/Debug/cairns_serve ;;
        vk)    serve=build/vk/Debug/cairns_serve ;;
        *) echo "usage: $0 [metal|vk]" >&2; return 2 ;;
    esac
    [ -x "$serve" ] || { echo "$bk: cairns_serve missing at $serve" >&2; return 1; }

    local out
    out=$({
        printf '%s\n' '{"op":"cairns.prefab.load","args":{"path":"aatrox.glb"}}'
        printf '%s\n' '{"op":"cairns.prefab.load","args":{"path":"aatrox_blood_moon.glb"}}'
        printf '%s\n' '{"op":"cairns.prefab.load","args":{"path":"aatrox_drx.glb"}}'
        printf '%s\n' '{"op":"cairns.prefab.count"}'
        printf '%s\n' '{"op":"cairns.prefab.load","args":{"path":"ahri.glb"}}'
        printf '%s\n' '{"op":"cairns.prefab.load","args":{"path":"ahri_academy.glb"}}'
        printf '%s\n' '{"op":"cairns.prefab.load","args":{"path":"ahri_arcade.glb"}}'
        printf '%s\n' '{"op":"cairns.prefab.count"}'
        printf '%s\n' '{"op":"cairns.scene.clear"}'
        printf '%s\n' '{"op":"cairns.prefab.unloadAll"}'
        printf '%s\n' '{"op":"cairns.prefab.count"}'
    } | CAIRNS_DUMP=tmp/_f2_unused.png "$serve" 2>/dev/null \
        | /usr/bin/grep -E '"(count|unloaded)"')

    # Expect, in order: count:3, count:6, unloaded:6, count:0.
    local expect=$'{"ok":true,"result":{"count":3}}\n{"ok":true,"result":{"count":6}}\n{"ok":true,"result":{"unloaded":6}}\n{"ok":true,"result":{"count":0}}'
    if [ "$out" = "$expect" ]; then
        echo "$bk: f2-evict: load 3 -> count=3, +3 -> count=6, clear+unloadAll -> 6, count=0 ✓"
    else
        echo "$bk: f2-evict ✗" >&2
        echo "  expected:" >&2
        printf '    %s\n' "$expect" | sed 's/\\n/\n    /g' >&2
        echo "  got:" >&2
        printf '    %s\n' "$out" >&2
        return 1
    fi
}

if [ -n "$1" ]; then
    run_backend "$1"
else
    run_backend metal
    run_backend vk
fi
