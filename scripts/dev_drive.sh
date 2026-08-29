#!/bin/zsh
# Drive a running sdl-min via a FIFO into its stdin (CAIRNS_AGENT_STDIN).
# State is kept in tmp/ across invocations:
#   tmp/_dev_fifo        the named pipe (writer kept alive by a holder)
#   tmp/_dev_app_pid     sdl-min pid
#   tmp/_dev_holder_pid  the writer-holder pid that prevents FIFO EOF
#   tmp/_dev_log         sdl-min stdout+stderr
#
# Usage:
#   scripts/dev_drive.sh start [metal|vk]
#   scripts/dev_drive.sh spawn N
#   scripts/dev_drive.sh relayout
#   scripts/dev_drive.sh clear
#   scripts/dev_drive.sh eval '<js>'
#   scripts/dev_drive.sh screenshot tmp/_shot.png
#   scripts/dev_drive.sh log [N]   # tail last N lines (default 40)
#   scripts/dev_drive.sh stop
#
# The "no flash" relayout (spawn 50 -> spawn 100) is implemented in JS
# (scripts/_drive_spawn.js) -- this script just pipes NDJSON.

set -e
cd "$(dirname "$0")/.."

fifo=tmp/_dev_fifo
app_pid_file=tmp/_dev_app_pid
holder_pid_file=tmp/_dev_holder_pid
log_file=tmp/_dev_log
mkdir -p tmp

# JSON-encode arbitrary text via python3 (stdlib, ships with macOS).
jsenc() {
    python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))'
}

send_op() {
    if [ ! -p "$fifo" ]; then
        echo "fifo missing -- did you 'start'?" >&2
        return 1
    fi
    cat >> "$fifo"
}

eval_js() {
    local js="$1"
    local code
    code=$(printf '%s' "$js" | jsenc)
    printf '%s\n' "{\"op\":\"cairns.script.eval\",\"args\":{\"code\":$code}}" \
        | send_op
}

eval_js_file() {
    local path="$1"
    local code
    code=$(jsenc < "$path")
    printf '%s\n' "{\"op\":\"cairns.script.eval\",\"args\":{\"code\":$code}}" \
        | send_op
}

cmd="${1:-help}"; shift || true

case "$cmd" in
  start)
    backend="${1:-metal}"
    case "$backend" in
      metal) app=build/metal/Debug/sdl-min.app/Contents/MacOS/sdl-min ;;
      vk)    app=build/vk/Debug/sdl-min.app/Contents/MacOS/sdl-min ;;
      *) echo "unknown backend: $backend" >&2; exit 2 ;;
    esac
    [ -x "$app" ] || { echo "$app missing -- build sdl-min first" >&2; exit 1; }
    # Tear down any prior session.
    [ -s "$app_pid_file" ] && kill "$(cat "$app_pid_file")" 2>/dev/null || true
    [ -s "$holder_pid_file" ] && kill "$(cat "$holder_pid_file")" 2>/dev/null || true
    rm -f "$fifo" "$app_pid_file" "$holder_pid_file" "$log_file"
    mkfifo "$fifo"
    # Long-lived writer that holds FIFO open so sdl-min never sees EOF.
    ( exec 9>"$fifo"; while :; do sleep 3600; done ) &
    echo $! > "$holder_pid_file"
    CAIRNS_AGENT_STDIN=1 "$app" < "$fifo" > "$log_file" 2>&1 &
    echo $! > "$app_pid_file"
    # Give sdl-min a moment to come up + parse studio.js.
    sleep 1.0
    # Autoload the spawn-helpers.
    eval_js_file scripts/_drive_spawn.js
    echo "started backend=$backend app_pid=$(cat $app_pid_file) holder_pid=$(cat $holder_pid_file)"
    ;;

  spawn)
    N="${1:-50}"
    eval_js "JSON.stringify(spawnTotal($N))"
    sleep 0.2
    ;;

  relayout)
    eval_js "JSON.stringify(relayout())"
    sleep 0.2
    ;;

  clear)
    eval_js "JSON.stringify(clearAll())"
    sleep 0.2
    ;;

  eval)
    js="${1:?need a js snippet}"
    eval_js "$js"
    sleep 0.2
    ;;

  screenshot)
    path="${1:-tmp/_shot.png}"
    # Queue the window dump for the next presented frame. sdl-min's
    # SDL_AppIterate ticks on its own; we sleep > 1 frame to let
    # Frames::End complete the readback.
    printf '%s\n' "{\"op\":\"cairns.io.dumpTexture\",\"args\":{\"target\":\"window\",\"path\":\"$path\"}}" \
        | send_op
    sleep 0.4
    if [ -f "$path" ]; then
        echo "$path"
    else
        echo "no dump produced -- check $log_file" >&2
        exit 1
    fi
    ;;

  log)
    n="${1:-40}"
    tail -n "$n" "$log_file"
    ;;

  stop)
    [ -s "$app_pid_file" ] && kill "$(cat "$app_pid_file")" 2>/dev/null || true
    [ -s "$holder_pid_file" ] && kill "$(cat "$holder_pid_file")" 2>/dev/null || true
    rm -f "$fifo" "$app_pid_file" "$holder_pid_file"
    echo "stopped"
    ;;

  help|--help|-h|"")
    sed -n '2,/^$/p' "$0"
    ;;

  *)
    echo "unknown command: $cmd (try 'help')" >&2
    exit 2
    ;;
esac
