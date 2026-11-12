#!/usr/bin/env bash
# Robustness #4 — idle timeout.
# A client is frozen with SIGSTOP (like a hung program or a pulled cable):
# its kernel still ACKs, so TCP never reports an error and no FIN arrives.
# Without an application-level timeout the server child and the player
# slot are held forever.
source "$(dirname "$0")/lib.sh"

run() { # run NAME [extra server args]
    local name=$1; shift
    start_server "$name" "$@"
    "$CLIENT" --bot -p "$PORT" -c 1 -d 60 -i 500 -n "$name" > "$LOGDIR/$name.bot.log" 2>&1 &
    local bot=$!
    sleep 1
    kill -STOP "$bot"
    echo "--- $name: client $bot frozen with SIGSTOP, waiting 5 s ---"
    sleep 5
    local s; s=$(last_stat "$name")
    echo "$s"
    eval "ON_${name#timeout_}=$(field "$s" online); TO_${name#timeout_}=$(field "$s" timeout); CH_${name#timeout_}=$(children)"
    kill -CONT "$bot"; kill "$bot" 2>/dev/null; wait "$bot" 2>/dev/null
    stop_server
}

banner "demo 4: idle timeout  (one client frozen)"
run timeout_without --no-timeout
run timeout_with --idle-timeout 3
echo
printf '%-8s %-7s %-9s %s\n' mode online children timeouts
printf '%-8s %-7s %-9s %s\n' without "$ON_without" "$CH_without" "$TO_without" with "$ON_with" "$CH_with" "$TO_with"
[ "$ON_without" -eq 1 ] && [ "$CH_without" -eq 1 ] && [ "$ON_with" -eq 0 ] && [ "$TO_with" -eq 1 ]
verdict $? "without timeout the frozen client holds a process+slot forever; with it the server reclaims them"
