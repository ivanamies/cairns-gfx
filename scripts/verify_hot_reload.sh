#!/bin/zsh
# Hot-reload acceptance test (the ONE verification for hot-reload work).
#
#   engine starts -> load 3 -> count=3 -> load 3 more -> count=6 ->
#   instantiate all 6 + render + dump tmp/_hr_${bk}_loaded.png ->
#   reload one of the resident prefabs in place (count stays 6),
#       render + dump tmp/_hr_${bk}_reloaded.png ->
#   scene.clear -> unloadAll (returns 6) -> count=0 -> stop engine
#
# Both PNG dumps must exist + be non-trivially sized; the reload dump
# proves rendering continues past the in-place pool-slot swap. Inspect
# tmp/_hr_metal_loaded.png to confirm heroes are visible.
#
# Usage: scripts/verify_hot_reload.sh [metal|vk]   (default: both)
set -e
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
cd "$(dirname "$0")/.."

assert_size_gt() {
    local f="$1"; local min="$2"; local label="$3"
    local sz
    sz=$(/usr/bin/stat -f '%z' "$f" 2>/dev/null || echo 0)
    if [ "$sz" -lt "$min" ]; then
        echo "$label: $f size $sz < $min bytes" >&2; return 1
    fi
}

run_backend() {
    local bk="$1"
    local serve
    case "$bk" in
        metal) serve=build/metal/Debug/cairns_serve ;;
        vk)    serve=build/vk/Debug/cairns_serve ;;
        *) echo "usage: $0 [metal|vk]" >&2; return 2 ;;
    esac
    [ -x "$serve" ] || { echo "$bk: cairns_serve missing at $serve" >&2; return 1; }

    /bin/rm -f tmp/_hr_${bk}_loaded.png tmp/_hr_${bk}_reloaded.png
    local SCALE=0.00433
    local SPACING=1.333
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
        # Instantiate all 6 heroes in a row. Same workload shape as the
        # byte-gate + load_flow tests so the picture is recognizable.
        for i in 0 1 2 3 4 5; do
            local x
            x=$(/usr/bin/python3 -c "print(-${SPACING}*2.5 + ${i}*${SPACING})")
            printf '%s\n' "{\"op\":\"cairns.scene.instantiate\",\"args\":{\"prefab\":${i},\"x\":${x},\"y\":0,\"z\":-3,\"scale\":${SCALE},\"time_phase\":0}}"
        done
        for _ in $(seq 1 10); do echo '{"op":"cairns.render.frame"}'; done
        printf '%s\n' "{\"op\":\"cairns.io.dumpTexture\",\"args\":{\"target\":\"final\",\"path\":\"tmp/_hr_${bk}_loaded.png\"}}"
        # Reload one of the resident prefabs in place. count stays 6.
        printf '%s\n' '{"op":"cairns.prefab.reload","args":{"path":"aatrox.glb"}}'
        printf '%s\n' '{"op":"cairns.prefab.count"}'
        # Hot-reload each of the three compute pipelines.
        # KEEP-LAST-GOOD: no visual change since shaders unchanged;
        # render continues with the new (functionally identical) PSO.
        for k in anim_eval skin particle; do
            printf '%s\n' "{\"op\":\"cairns.pipeline.reload\",\"args\":{\"name\":\"${k}\"}}"
        done
        # Reload the JS context (fresh JSContext, same JSRuntime).
        printf '%s\n' '{"op":"cairns.script.reload"}'
        for _ in $(seq 1 10); do echo '{"op":"cairns.render.frame"}'; done
        printf '%s\n' "{\"op\":\"cairns.io.dumpTexture\",\"args\":{\"target\":\"final\",\"path\":\"tmp/_hr_${bk}_reloaded.png\"}}"
        printf '%s\n' '{"op":"cairns.scene.clear"}'
        printf '%s\n' '{"op":"cairns.prefab.unloadAll"}'
        printf '%s\n' '{"op":"cairns.prefab.count"}'
    } | CAIRNS_DUMP=tmp/_hr_unused.png "$serve" 2>/dev/null)

    # Extract per-op markers. Each is the first match of its kind.
    local count3 count6 reload_ok count6_after_reload unloaded6 count0
    count3=$(printf '%s\n' "$out" | /usr/bin/grep -F '"count":3' | head -1)
    count6=$(printf '%s\n' "$out" | /usr/bin/grep -F '"count":6' | head -1)
    reload_ok=$(printf '%s\n' "$out" | /usr/bin/grep -F '"ok":true,"path":"aatrox.glb"' | /usr/bin/grep -v '"prefab":' | head -1)
    count6_after_reload=$(printf '%s\n' "$out" | /usr/bin/grep -F '"count":6' | sed -n '2p')
    unloaded6=$(printf '%s\n' "$out" | /usr/bin/grep -F '"unloaded":6' | head -1)
    count0=$(printf '%s\n' "$out" | /usr/bin/grep -F '"count":0' | head -1)

    local err=0
    [ -n "$count3" ]              || { echo "$bk: missing count:3 ✗" >&2; err=1; }
    [ -n "$count6" ]              || { echo "$bk: missing count:6 (after batch B) ✗" >&2; err=1; }
    [ -n "$reload_ok" ]           || { echo "$bk: missing reload ok:true ✗" >&2; err=1; }
    [ -n "$count6_after_reload" ] || { echo "$bk: missing count:6 (after reload) ✗" >&2; err=1; }
    [ -n "$unloaded6" ]           || { echo "$bk: missing unloaded:6 ✗" >&2; err=1; }
    [ -n "$count0" ]              || { echo "$bk: missing count:0 (final) ✗" >&2; err=1; }
    [ $err -eq 0 ] || return 1

    assert_size_gt tmp/_hr_${bk}_loaded.png   1024 "$bk: loaded dump"   || return 1
    assert_size_gt tmp/_hr_${bk}_reloaded.png 1024 "$bk: reloaded dump" || return 1
    echo "$bk: hot-reload: load 6, reload-in-place, unloadAll, no hang ✓ (dumps: tmp/_hr_${bk}_{loaded,reloaded}.png)"
}

if [ -n "$1" ]; then
    run_backend "$1"
else
    run_backend metal
    run_backend vk
fi
