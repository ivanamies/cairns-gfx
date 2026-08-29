#!/bin/zsh
# #224 L9+: the user's stated sequence, verified at EVERY step.
#
# Drives scripts/_load_flow_test.js (9 steps; see comment block in that
# file). The JS reports {ok, fail, errors[]} -- we assert fail=0. THEN
# we assert the three dump files satisfy:
#
#   tmp/_lf_empty.png    exists, non-trivially sized (particles + bg)
#   tmp/_lf_loaded.png   exists, BYTES differ from _lf_empty.png
#                        (instantiated entities CHANGED pixels)
#   tmp/_lf_animated.png exists, BYTES differ from _lf_loaded.png
#                        (animation drove a per-frame pose delta)
#
# Skipping ANY check is the bug we are guarding against (see
# MISTAKES.md "Committed without verifying").
#
# Usage: scripts/verify_load_flow.sh [metal|vk]   (default: both)
set -e
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
cd "$(dirname "$0")/.."

assert_diff() {
    local a="$1"
    local b="$2"
    local label="$3"
    if [ ! -f "$a" ]; then
        echo "$label: missing $a" >&2; return 1
    fi
    if [ ! -f "$b" ]; then
        echo "$label: missing $b" >&2; return 1
    fi
    if /usr/bin/cmp -s "$a" "$b"; then
        echo "$label: $a == $b (expected to differ)" >&2; return 1
    fi
}

assert_size_gt() {
    local f="$1"
    local min="$2"
    local label="$3"
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

    /bin/rm -f tmp/_lf_empty.png tmp/_lf_loaded.png tmp/_lf_animated.png

    local spawn_op
    spawn_op=$(/usr/bin/python3 -c \
        'import json,sys;print(json.dumps({"op":"cairns.script.eval","args":{"code":open(sys.argv[1]).read()}}))' \
        scripts/_load_flow_test.js)
    local out
    out=$(printf '%s\n' "$spawn_op" | CAIRNS_DUMP=tmp/_lf_unused.png \
            "$serve" 2>/dev/null \
            | /usr/bin/grep '^{"ok":true' | tail -1)
    if [ -z "$out" ]; then
        echo "$bk: load-flow: no ok response from cairns_serve" >&2
        return 1
    fi
    local fail
    fail=$(printf '%s' "$out" | /usr/bin/python3 -c \
        'import json,sys; r=json.loads(sys.stdin.read())["result"]; payload=json.loads(r["result"] if isinstance(r,dict) and "result" in r else r); print(payload["fail"]);')
    if [ "$fail" != "0" ]; then
        local errs
        errs=$(printf '%s' "$out" | /usr/bin/python3 -c \
            'import json,sys; r=json.loads(sys.stdin.read())["result"]; payload=json.loads(r["result"] if isinstance(r,dict) and "result" in r else r); print("\n".join(payload.get("errors",[])));')
        echo "$bk: load-flow: ${fail} fail ✗" >&2
        printf '%s\n' "$errs" >&2
        return 1
    fi

    # ── shell-side pixel asserts (every step has a verification) ──
    assert_size_gt tmp/_lf_empty.png    1024 "$bk: step 2 empty dump"   || return 1
    assert_size_gt tmp/_lf_loaded.png   1024 "$bk: step 8 loaded dump"  || return 1
    assert_size_gt tmp/_lf_animated.png 1024 "$bk: step 9 animated dump"|| return 1
    assert_diff tmp/_lf_empty.png   tmp/_lf_loaded.png   "$bk: instantiate produced no pixel delta" || return 1
    assert_diff tmp/_lf_loaded.png  tmp/_lf_animated.png "$bk: animation produced no pixel delta"  || return 1

    local okc
    okc=$(printf '%s' "$out" | /usr/bin/python3 -c \
        'import json,sys; r=json.loads(sys.stdin.read())["result"]; payload=json.loads(r["result"] if isinstance(r,dict) and "result" in r else r); print(payload["ok"]);')
    echo "$bk: load-flow: ${okc}/9 JS steps + 3 file checks + 2 pixel-delta checks ✓"
}

if [ -n "$1" ]; then
    run_backend "$1"
else
    run_backend metal
    run_backend vk
fi
