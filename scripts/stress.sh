#!/usr/bin/env bash
# Concurrency proof: N bot clients against one fork-per-client server.
# usage: scripts/stress.sh [N=200] [SECONDS=20]
source "$(dirname "$0")/lib.sh"
N=${1:-200}
DUR=${2:-20}
OUT=$LOGDIR/stress_proof.txt

start_server stress -W 120 -H 40
"$CLIENT" --bot -p "$PORT" -c "$N" -d "$DUR" -n s > "$LOGDIR/stress.bot.log" 2>&1 &
BOT=$!
sleep $((DUR / 2))
{
    banner "concurrency proof: $N clients, t=$((DUR / 2))s (server pid $SRV_PID, port $PORT)"
    echo "server child processes (one per client): $(children)"
    echo "established TCP connections to :$PORT:   $(ss -Htn state established "( sport = :$PORT )" | wc -l)"
    echo "--- ps (first 8 children) ---"
    ps -o pid,ppid,stat,etime,cmd --ppid "$SRV_PID" | head -9
    echo "--- server status (last 3 s) ---"
    grep '^\[stat\]' "$LOGDIR/stress.server.log" | tail -3
} | tee "$OUT"
wait "$BOT"; BOT_RC=$?
{
    echo "--- bot summary ---"
    tail -2 "$LOGDIR/stress.bot.log"
} | tee -a "$OUT"
stop_server
FINAL=$(last_stat stress)
echo "--- final server status ---"; echo "$FINAL" | tee -a "$OUT"
ERR=$(bot_field "$LOGDIR/stress.bot.log" err); LOST=$(bot_field "$LOGDIR/stress.bot.log" lost)
grep -q "connected $N/$N" "$LOGDIR/stress.bot.log" && [ "$BOT_RC" -eq 0 ]
verdict $? "$N clients served concurrently, client errors=$ERR lost=$LOST (saved to $OUT)"
