#!/bin/zsh
# #225 R5: studio_js smoke harness. Loads scripts/_studio_js_smoke.js
# via cairns.script.eval and asserts the printed result is {ok:N, fail:0}.
#
# Usage: scripts/verify_studio_js.sh [metal|vk]   (default: both)
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
        scripts/_studio_js_smoke.js)
    local out
    out=$(printf '%s\n' "$spawn_op" | "$serve" 2>/dev/null | /usr/bin/grep '^{"ok":true' | tail -1)
    if [ -z "$out" ]; then
        echo "$bk: studio_js smoke: no ok response from cairns_serve" >&2
        return 1
    fi
    # The eval result is a stringified {ok:N, fail:N, errors:[...]} -- ok iff fail===0.
    local fail
    fail=$(printf '%s' "$out" | /usr/bin/python3 -c \
        'import json,sys; r=json.loads(sys.stdin.read())["result"]; payload=json.loads(r["result"] if isinstance(r,dict) and "result" in r else r); print(payload["fail"]);')
    if [ "$fail" = "0" ]; then
        local okc
        okc=$(printf '%s' "$out" | /usr/bin/python3 -c \
            'import json,sys; r=json.loads(sys.stdin.read())["result"]; payload=json.loads(r["result"] if isinstance(r,dict) and "result" in r else r); print(payload["ok"]);')
        echo "$bk: studio_js smoke: ${okc} ok / 0 fail ✓"
    else
        local errs
        errs=$(printf '%s' "$out" | /usr/bin/python3 -c \
            'import json,sys; r=json.loads(sys.stdin.read())["result"]; payload=json.loads(r["result"] if isinstance(r,dict) and "result" in r else r); print("\n".join(payload.get("errors",[])));')
        echo "$bk: studio_js smoke: ${fail} fail ✗" >&2
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
