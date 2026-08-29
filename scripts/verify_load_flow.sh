#!/bin/zsh
# #224 L9 verification: the actual user flow.
#
# Drives scripts/_load_flow_test.js via cairns.script.eval against a
# fresh cairns_serve. Asserts:
#   step 1: engine starts EMPTY (count == 0; nothing parsed at boot)
#   step 2: load 3 paths -> count == 3
#   step 3: snapshot prefab handles
#   step 4: load 3 more  -> count == 6
#   step 5: APPEND-only -- first 3's handles unchanged
#
# Usage: scripts/verify_load_flow.sh [metal|vk]   (default: both)
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

    local spawn_op
    spawn_op=$(/usr/bin/python3 -c \
        'import json,sys;print(json.dumps({"op":"cairns.script.eval","args":{"code":open(sys.argv[1]).read()}}))' \
        scripts/_load_flow_test.js)
    local out
    # CAIRNS_DUMP triggers FixedClock so render.frame ticks deterministically.
    /bin/rm -f tmp/_load_flow_dump.png
    out=$(printf '%s\n' "$spawn_op" | CAIRNS_DUMP=tmp/_load_flow_unused.png \
            "$serve" 2>/dev/null \
            | /usr/bin/grep '^{"ok":true' | tail -1)
    if [ -z "$out" ]; then
        echo "$bk: load-flow: no ok response from cairns_serve" >&2
        return 1
    fi
    local fail
    fail=$(printf '%s' "$out" | /usr/bin/python3 -c \
        'import json,sys; r=json.loads(sys.stdin.read())["result"]; payload=json.loads(r["result"] if isinstance(r,dict) and "result" in r else r); print(payload["fail"]);')
    if [ "$fail" = "0" ]; then
        local okc
        okc=$(printf '%s' "$out" | /usr/bin/python3 -c \
            'import json,sys; r=json.loads(sys.stdin.read())["result"]; payload=json.loads(r["result"] if isinstance(r,dict) and "result" in r else r); print(payload["ok"]);')
        echo "$bk: load-flow: ${okc} ok / 0 fail ✓"
    else
        local errs
        errs=$(printf '%s' "$out" | /usr/bin/python3 -c \
            'import json,sys; r=json.loads(sys.stdin.read())["result"]; payload=json.loads(r["result"] if isinstance(r,dict) and "result" in r else r); print("\n".join(payload.get("errors",[])));')
        echo "$bk: load-flow: ${fail} fail ✗" >&2
        printf '%s\n' "$errs" >&2
        return 1
    fi
}

if [ -n "$1" ]; then
    run_backend "$1"
else
    run_backend metal
    run_backend vk
fi
