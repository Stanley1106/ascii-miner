#!/usr/bin/env bash
# Helpers shared by the stress/demo scripts. Source it, do not run it.
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=${BUILD:-$ROOT/build}
case $BUILD in /*) ;; *) BUILD=$ROOT/$BUILD ;; esac
SERVER=$BUILD/miner_server
CLIENT=$BUILD/miner_client
LOGDIR=${LOGDIR:-$ROOT/logs}
PORT=${PORT:-$((20000 + RANDOM % 20000))}
SRV_PID=""
mkdir -p "$LOGDIR"

[ -x "$SERVER" ] && [ -x "$CLIENT" ] || { echo "build first: make" >&2; exit 2; }

banner() { printf '\n\033[1m=== %s ===\033[0m\n' "$*"; }

# start_server NAME [server args...]  — log goes to $LOGDIR/NAME.server.log
start_server() {
    local name=$1; shift
    "$SERVER" -p "$PORT" "$@" > "$LOGDIR/$name.server.log" 2>&1 &
    SRV_PID=$!
    for _ in $(seq 100); do
        grep -q "listening on port" "$LOGDIR/$name.server.log" 2>/dev/null && return 0
        sleep 0.05
    done
    echo "server did not start, see $LOGDIR/$name.server.log" >&2
    exit 1
}

stop_server() {
    [ -n "$SRV_PID" ] || return 0
    kill -TERM "$SRV_PID" 2>/dev/null
    wait "$SRV_PID" 2>/dev/null
    SRV_PID=""
    sleep 0.2
}

# Last "[stat]" line of a server log, and one key=value field from it.
last_stat()  { grep '^\[stat\]' "$LOGDIR/$1.server.log" | tail -1; }
field()      { sed -n "s/.* $2=\([^ ]*\).*/\1/p" <<<"$1"; }
bot_field()  { grep '^\[bot\] done' "$1" | sed -n "s/.* $2=\([^ ]*\).*/\1/p"; }
children()   { pgrep -P "$SRV_PID" | wc -l; }
zombies()    { ps -o stat= --ppid "$SRV_PID" | grep -c '^Z' || true; }

cleanup() {
    [ -n "$SRV_PID" ] && kill -TERM "$SRV_PID" 2>/dev/null
    jobs -p | xargs -r kill 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

verdict() { # verdict CONDITION_STATUS MESSAGE
    if [ "$1" -eq 0 ]; then printf '\033[32mPASS\033[0m %s\n' "$2"; return 0; fi
    printf '\033[31mFAIL\033[0m %s\n' "$2"; return 1
}
