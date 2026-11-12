#!/usr/bin/env bash
# Robustness #5 — robust mutex recovery.
# The 30th dig kills its child process (SIGKILL) while it holds the shared
# world lock. A plain process-shared mutex stays locked forever, so every
# other player freezes on their next dig. A robust mutex hands the next
# locker EOWNERDEAD; it calls pthread_mutex_consistent() and play goes on.
source "$(dirname "$0")/lib.sh"

run() { # run NAME [extra server args]
    local name=$1; shift
    start_server "$name" --crash-at-dig 30 "$@"
    "$CLIENT" --bot -p "$PORT" -c 20 -d 5 -i 50 -n "$name" > "$LOGDIR/$name.bot.log" 2>&1
    echo "--- $name ---"
    grep -m3 -E "crash test|EOWNERDEAD|released player slot" "$LOGDIR/$name.server.log" | cut -c1-120
    grep '^\[bot\] t=' "$LOGDIR/$name.bot.log" | tail -1
    stop_server
}

banner "demo 5: robust mutex  (a child dies holding the lock)"
run robust_without --no-robust
run robust_with
echo
printf '%-8s %-9s %-14s %s\n' mode lockfix last_rps stalled_bots
for m in without with; do
    last=$(grep '^\[bot\] t=' "$LOGDIR/robust_$m.bot.log" | tail -1)
    printf '%-8s %-9s %-14s %s\n' "$m" "$(field "$(last_stat "robust_$m")" lockfix)" \
        "$(field "$last" rps)" "$(field "$last" stalled)"
    eval "RPS_$m=$(field "$last" rps); FIX_$m=$(field "$(last_stat "robust_$m")" lockfix)"
done
[ "$RPS_without" -eq 0 ] && [ "$RPS_with" -gt 0 ] && [ "$FIX_with" -eq 1 ]
verdict $? "plain mutex deadlocked all players; robust mutex recovered and play continued"
