#!/usr/bin/env bash
# Two-layer debug log control.
#   layer 1, compile time: `make` vs `make DEBUG=1` (-DMINER_DEBUG)
#   layer 2, run time:     --log-level / -v / MINER_LOG=debug
cd "$(dirname "$0")/.."
make -s && make -s DEBUG=1 || exit 1
export LOGDIR=${LOGDIR:-$PWD/logs}
mkdir -p "$LOGDIR"

count_debug() { # BUILD_DIR LEVEL -> number of DEBUG lines a short session logs
    local port=$((20000 + RANDOM % 20000)) log=$LOGDIR/log_$1_$2.log
    MINER_LOG=$2 "$1/miner_server" -q -p "$port" > "$log" 2>&1 &
    local srv=$!
    sleep 0.3
    "$1/miner_client" --bot -p "$port" -c 2 -d 1 > /dev/null 2>&1
    kill "$srv"; wait "$srv" 2>/dev/null
    grep -c ' DEBUG ' "$log"
}

printf '\n\033[1m=== two-layer debug log control ===\033[0m\n'
echo "layer 1: debug format strings compiled into the binary"
printf '  %-28s %s\n' "build/       (make)" "$(strings build/libminer.so build/miner_server | grep -c 'frame %s seq')"
printf '  %-28s %s\n' "build-debug/ (make DEBUG=1)" "$(strings build-debug/libminer.so build-debug/miner_server | grep -c 'frame %s seq')"
echo
echo "layer 2: DEBUG lines logged by a 1-second, 2-client session"
printf '  %-14s %-16s %s\n' build MINER_LOG=info MINER_LOG=debug
printf '  %-14s %-16s %s\n' release "$(count_debug build info)" "$(count_debug build debug)"
printf '  %-14s %-16s %s\n' debug "$(count_debug build-debug info)" "$(count_debug build-debug debug)"
echo
echo "sample debug lines:"
grep -m4 ' DEBUG ' "$LOGDIR/log_build-debug_debug.log" | cut -c1-110
